#define _GNU_SOURCE
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>

#include "../libs/server.h"
#include "../libs/markdown.h"
#include "../libs/command_queue.h"
#include "../libs/helper.h"

#define USERNAME_LEN 128

// Server state and document versioning
version_log *log_head = NULL;
int current_version = 0;
queued_command *cmd_queue = NULL;
document *doc = NULL;

// Thread-safety for shared data
pthread_mutex_t client_count_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t doc_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t client_list_lock = PTHREAD_MUTEX_INITIALIZER;

// Track number of connected clients and their output pipes
int client_count = 0;
client_pipe *client_list = NULL;

/*
 *Read roles.txt to verify a user's role(i.e. "read" or "write")
 */
bool check_user_role(const char *username, char *out_role) {
    FILE *file = fopen("roles.txt", "r");
    if (!file) {
        return false;
    }
    char line[LINE_LEN];
    while (fgets(line, sizeof(line), file)) {
        char user[USERNAME_LEN];
        char role[ROLE_LEN];
        
        // Parse a line into username and role
        if (sscanf(line, "%127s %7s", user, role) == 2) {
            if (strcmp(user, username) == 0) {
                strcpy(out_role, role);
                fclose(file);
                return true; // Match found
            }
        }
    }
    fclose(file);
    return false; // Match not found
}

/*
 *Frees all allocated memory for logs when server shuts down
 */
void free_logs() {
    version_log *vlog = log_head;
    while (vlog) {
        log_entry *e = vlog->entries;
        while (e) {
            log_entry *tmp = e;
            e = e->next;
            free(tmp->line);
            free(tmp);
        }
        version_log *tmpv = vlog;
        vlog = vlog->next;
        free(tmpv);
    }
}

/*
 * Broadcast thread function that runs every TIME_INTERVAL. 
 * - Locks the document and processes all queued commands in order of arrival. 
 * - Increments document version for successful edits.
 * - Broadcasts the results of all commands (success or reject) to all clients. 
 * This ensures synchronisation across all clients and avoids race conditions resulting from concurrent edits.
 */
void *broadcast_thread(void *arg) {
    int interval = *((int *)arg); // Interval in ms between broadcasts

    while (1) {
        usleep(interval * 1000);
        // Lock document while processing updates
        pthread_mutex_lock(&doc_lock);

        // Prepare log entry list for this broadcast
        log_entry *entry_head = NULL;
        log_entry **entry_tail = &entry_head;

        // Process all queued commands
        if (cmd_queue != NULL) {
            // Ensure commands are ordered by timestamp
            sort_command_queue(&cmd_queue); 

            // Process each command in the queue
            while (cmd_queue) {
                char *username = cmd_queue->username;
                char *role = cmd_queue->role;
                char *command = cmd_queue->command_str;
                uint64_t version = cmd_queue->client_version;
                char log_line[LINE_LEN];

                // Reject edit if user has read-only permissions
                if (strcmp(role, "read") == 0) {
                    snprintf(log_line, sizeof(log_line), "EDIT %s %s Reject UNAUTHORISED", username, command);
                } else {
                    // Process the command and determine outcome
                    int result = process_command(doc, command, version);

                    if (result == SUCCESS) {
                        markdown_increment_version(doc); // Commit the changes
                        current_version = doc->version;
                        snprintf(log_line, sizeof(log_line), "EDIT %s %s SUCCESS", username, command);
                    } else if (result == INVALID_CURSOR_POS) {
                        snprintf(log_line, sizeof(log_line), "EDIT %s %s Reject INVALID_POSITION", username, command);
                    } else if (result == DELETED_POSITION) {
                        snprintf(log_line, sizeof(log_line), "EDIT %s %s Reject DELETED_POSITION", username, command);
                    } else if (result == OUTDATED_VERSION) {
                        snprintf(log_line, sizeof(log_line), "EDIT %s %s Reject OUTDATED_VERSION", username, command);
                    }
                }
                // Build the log entry for this command
                log_entry *entry = malloc(sizeof(log_entry));
                entry->line = strdup(log_line);
                entry->next = NULL;
                *entry_tail = entry;
                entry_tail = &entry->next;

                // Free the processed command
                queued_command *old = cmd_queue;
                cmd_queue = cmd_queue->next;
                free(username);
                free(role);
                free(command);
                free(old);
            }
        }
        // Determine broadcast version after processing all commands
        int broadcast_version = doc->version;

        // Build version_log for this broadcast
        version_log *new_log = malloc(sizeof(version_log));
        new_log->version_number = broadcast_version;
        new_log->entries = entry_head;
        new_log->next = NULL;

        // Send the updates to all currently connected clients
        pthread_mutex_lock(&client_list_lock);
        client_pipe *curr = client_list;
        while (curr) {
            dprintf(curr->fd, "VERSION %d\n", broadcast_version);
            log_entry *e = new_log->entries;
            while (e) {
                dprintf(curr->fd, "%s\n", e->line);
                e = e->next;
            }
            dprintf(curr->fd, "END\n");
            curr = curr->next;
        }
        pthread_mutex_unlock(&client_list_lock);

        // Save the log in the server's version history
        if (!log_head) {
            log_head = new_log;
        } else {
            version_log *tail_log = log_head;
            while (tail_log->next) tail_log = tail_log->next;
            tail_log->next = new_log;
        }
        // Unlock document after processing
        pthread_mutex_unlock(&doc_lock);
    }

    return NULL;
}

/*
 * Handles a new client connection:
 * - Creates and manages client-specific FIFOs
 * - Authenticates user using roles.txt
 * - Sends role and current document to client upon successful authentication.
 * - Enqueues commands for processing. 
 */
void *client_handler(void *arg) {
    // Retrieve client PID from argument and free the struct
    client_arg *c_arg = (client_arg *)arg;
    pid_t pid = c_arg->client_pid;
    free(c_arg);

    // Create FIFOs using client PID
    char fifo_c2s[FIFO_NAME_LEN];
    char fifo_s2c[FIFO_NAME_LEN];
    snprintf(fifo_c2s, FIFO_NAME_LEN, "FIFO_C2S_%d", pid);
    snprintf(fifo_s2c, FIFO_NAME_LEN, "FIFO_S2C_%d", pid);

    // Ensure old FIFOs are removed before creating new ones
    unlink(fifo_c2s);
    unlink(fifo_s2c);

    // Create the FIFOs for client-to-server and server-to-client communication
    if (mkfifo(fifo_c2s, 0666) == -1 || mkfifo(fifo_s2c, 0666) == -1) {
        perror("mkfifo");
        pthread_exit(NULL);
    }

    // Notify client that connection has been received 
    kill(pid, SIGRTMIN + 1);

    // Open FIFO file descriptors
    int fd_c2s = open(fifo_c2s, O_RDONLY);
    int fd_s2c = open(fifo_s2c, O_WRONLY);

    // Read username from client
    char username[USERNAME_LEN];
    ssize_t n = read(fd_c2s, username, sizeof(username) - 1);
    username[n] = '\0';
    username[strcspn(username, "\n")] = '\0';

    // Check user's role
    char role[ROLE_LEN];
    bool found = check_user_role(username, role);

    // Reject connection if username is not found
    if (!found) {
        dprintf(fd_s2c, "Reject UNAUTHORISED\n");
        sleep(1);
        close(fd_c2s);
        close(fd_s2c);
        unlink(fifo_c2s);
        unlink(fifo_s2c);
        pthread_exit(NULL);
    }

    // Add client pipe to broadcast list
    pthread_mutex_lock(&client_list_lock);
    client_pipe *new_client = malloc(sizeof(client_pipe));
    new_client->fd = fd_s2c;
    new_client->next = client_list;
    client_list = new_client;
    pthread_mutex_unlock(&client_list_lock);

    // Increment client count
    pthread_mutex_lock(&client_count_lock);
    client_count++;
    pthread_mutex_unlock(&client_count_lock);

    // Send role to client
    dprintf(fd_s2c, "%s\n", role);
    // Send current document version to client
    dprintf(fd_s2c, "%llu\n", (unsigned long long)doc->version);
    // Flatten document and send its contents
    char *flat = markdown_flatten(doc);
    size_t doc_len = strlen(flat);
    dprintf(fd_s2c, "%zu\n", doc_len);
    write(fd_s2c, flat, doc_len);
    free(flat);

    // Wrap fd_c2s in a FILE* for simpler line-based reading
    FILE *c2s = fdopen(fd_c2s, "r");
    if (!c2s) {
        pthread_exit(NULL);
    }

    char command_line[LINE_LEN];
    while (fgets(command_line, sizeof(command_line), c2s)) {
        command_line[strcspn(command_line, "\n")] = '\0';

        // Client wants to disconnect
        if (strcmp(command_line, "DISCONNECT") == 0) {
            break;
        }

        uint64_t client_version = doc->version;

        // Queue command to be stored in log although it will be rejected
        if (strcmp(role, "read") == 0 &&
            (strncmp(command_line, "INSERT", 6) == 0 || // 6 = strlen("INSERT")
             strncmp(command_line, "DEL", 3) == 0 || // 3 = strlen("DEL")
             strncmp(command_line, "NEWLINE", 7) == 0 || // 7 = strlen("NEWLINE")
             strncmp(command_line, "HEADING", 7) == 0 || // 7 = strlen("HEADING")
             strncmp(command_line, "BOLD", 4) == 0 || // 4 = strlen("BOLD")
             strncmp(command_line, "ITALIC", 6) == 0 || // 6 = strlen("ITALIC")
             strncmp(command_line, "BLOCKQUOTE", 10) == 0 || // 10 = strlen("BLOCKQUOTE")
             strncmp(command_line, "ORDERED_LIST", 12) == 0 || // 12 = strlen("ORDERED_LIST")
             strncmp(command_line, "UNORDERED_LIST", 14) == 0 || // 14 = strlen("UNORDERED_LIST")
             strncmp(command_line, "CODE", 4) == 0 || // 4 = strlen("CODE")
             strncmp(command_line, "HORIZONTAL_RULE", 15) == 0 || // 15 = strlen("HORIZONTAL_RULE")
             strncmp(command_line, "LINK", 4) == 0)) { // 4 = strlen("LINK")

            pthread_mutex_lock(&doc_lock);
            enqueue_command(&cmd_queue, username, role, command_line, doc->version);
            pthread_mutex_unlock(&doc_lock);
            continue;
        }

        // Queue the command for processing in the broadcast thread
        pthread_mutex_lock(&doc_lock);
        enqueue_command(&cmd_queue, username, role, command_line, client_version);
        pthread_mutex_unlock(&doc_lock);
    }

    // Handle client disconnection
    pthread_mutex_lock(&client_count_lock);
    client_count--;
    pthread_mutex_unlock(&client_count_lock);

    // Remove client from broadcast list
    pthread_mutex_lock(&client_list_lock);
    client_pipe **curr = &client_list;
    while (*curr) {
        if ((*curr)->fd == fd_s2c) {
            client_pipe *to_remove = *curr;
            *curr = (*curr)->next;
            free(to_remove);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&client_list_lock);

    // Cleanup: close and unlink FIFOs
    fclose(c2s);
    close(fd_c2s);
    close(fd_s2c);
    unlink(fifo_c2s);
    unlink(fifo_s2c);

    pthread_exit(NULL);
}

/*
 * Signal-waiting thread that handles new client connections. 
 * - Waits for SIGRTMIN from new clients (blocks on sigwaitinfo() for signal safety).
 * - On receiving SIGRTMIN, the server spawns a new thread to handle the corresponding client.
 * This approach ensures thread-safe client acceptance and avoids race conditions during signal handling.
 */
void *sigwait_thread(void *arg) {
    (void)arg;
    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGRTMIN);

    siginfo_t si;
    while (1) {
        // Block until a SIGRTMIN signal is received (signal-safe client thread creation)
        if (sigwaitinfo(&waitset, &si) > 0) {
            // Extract client PID from signal
            pid_t client_pid = si.si_pid;
            
            // Allocate memory for client_arg struct to pass PID to handler thread
            client_arg *carg = malloc(sizeof(client_arg));
            carg->client_pid = client_pid;

            // Spawn a new thread to handle this client
            pthread_t tid;
            if (pthread_create(&tid, NULL, client_handler, carg) != 0) {
                perror("pthread_create");
                free(carg);
            }
            pthread_detach(tid);
        }
    }
    return NULL;
}

/* 
 * Entry point of server program.
 * - Parses the TIME_INTERVAL command line argument.
 * - Initialises:
 *     - sigwait_thread for accepting client connections through signals
 *     - bcast_thread for processing and broadcasting edits. 
 *     - shared document between clients
 * - Enters a blocking command loop for server-side debugging.
 */
int main(int argc, char *argv[]) {
    int time_interval;
    if (argc != 2) {
        perror("Invalid number of arguments\n");
        return 0;
    }
    // Convert time interval to integer
    time_interval = atoi(argv[1]);
    printf("Server PID: %d\n", getpid());

    // Block SIGRTMIN in all threads so only sigwait_thread can handle it
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGRTMIN);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    // Create the thread to wait for client SIGRTMIN signals
    pthread_t sig_thread;
    if (pthread_create(&sig_thread, NULL, sigwait_thread, NULL) != 0) {
        perror("pthread_create sigwait_thread");
        return 1;
    }

    // Create and detach broadcast thread that handles periodic updates
    pthread_t bcast_thread;
    pthread_create(&bcast_thread, NULL, broadcast_thread, &time_interval);
    pthread_detach(bcast_thread);

    // Initialise shared document
    doc = markdown_init();

    // Server terminal loop for user commands
    char input[MAX_INPUT_SIZE];
    while (1) {
        while (fgets(input, sizeof(input), stdin) != NULL) {
            input[strcspn(input, "\n")] = '\0';
            if (strcmp(input, "DOC?") == 0) {
                // Print current document content to terminal
                char *flat = markdown_flatten(doc);
                printf("%s\n", flat);
                free(flat);
            } else if (strcmp(input, "LOG?") == 0) {
                // Print full edit history (including successes and rejections)
                version_log *vlog = log_head;
                while (vlog) {
                    printf("VERSION %d\n", vlog->version_number);
                    log_entry *e = vlog->entries;
                    while (e) {
                        printf("%s\n", e->line);
                        e = e->next;
                    }
                    printf("END\n");
                    vlog = vlog->next;
                }
            } else if (strcmp(input, "QUIT") == 0) {
                // Only allow server to shutdown if no clients are connected
                pthread_mutex_lock(&client_count_lock);
                if (client_count == 0) {
                    // Final commit and save document to doc.md
                    markdown_increment_version(doc);
                    FILE *outfile = fopen("doc.md", "w");
                    if (outfile) {
                        markdown_print(doc, outfile);
                        fclose(outfile);
                    }
                    
                    // Clean up: free any remaining queued commands, document and logs
                    free_command_queue(&cmd_queue);
                    markdown_free(doc);
                    free_logs();
                    pthread_mutex_unlock(&client_count_lock);
                    
                    // Destroy all mutexes before exit
                    pthread_mutex_destroy(&client_count_lock);
                    pthread_mutex_destroy(&doc_lock);
                    pthread_mutex_destroy(&client_list_lock);
                    exit(0);
                } else {
                    // Prevent shutdown if clients are still connected
                    printf("QUIT rejected, %d clients still connected\n", client_count);
                    pthread_mutex_unlock(&client_count_lock);
                }
            }
        }
    }
    return 0;
}