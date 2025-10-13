#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>

#include "../libs/client.h"
#include "../libs/markdown.h"
#include "../libs/helper.h"

#define MAX_RESPONSE_LEN 512 // Max size of a broadcast line
#define VERSION_BUF_SIZE 32 // Buffer size for document version string
#define VERSION_PREFIX_LEN 8 // Length of "VERSION " prefix in broadcasts
#define LENGTH_BUF_SIZE 32 // Buffer size for document length string
#define ASCII_PRINT_MIN 32
#define ASCII_PRINT_MAX 126
#define BASE_DECIMAL 10

// Local copy of document and log of all broadcasts
document *doc = NULL;
log_line *log_head = NULL;
log_line *log_tail = NULL;

/*
 * Appends a line from the server broadcast (VERSION, EDIT, or END)
 * to the client's local broadcast log. This will be used to implement
 * the LOG? command and maintain a record of all changes received.
 */
void append_log_line(const char *line) {
    log_line *new_node = malloc(sizeof(log_line));
    new_node->line = strdup(line);
    new_node->next = NULL;

    if (!log_head) {
        log_head = log_tail = new_node;
    } else {
        log_tail->next = new_node;
        log_tail = new_node;
    }
}

/*
 * Frees all memory used by the client's local broadcast log.
 * Called on client shutdown to prevent memory leaks.
 */
void free_log() {
    log_line *curr = log_head;
    while (curr) {
        log_line *next = curr->next;
        free(curr->line);
        free(curr);
        curr = next;
    }
    log_head = log_tail = NULL;
}

/*
 * Checks the server-to-client FIFO for any pending broadcasts.
 * - Parses and logs each VERSION, EDIT result and END marker.
 * - Only applies successful EDIT commands to the local document.
 * - Commits changes by incrementing the local document version.
 * This function is made to be non-blocking to prevent blocking the user input loop.
 */
void apply_broadcasts(FILE *s2c) {
    // Temporarily enable non-blocking mode to check for available broadcasts without hanging
    char resp[MAX_RESPONSE_LEN];
    int fd_raw = fileno(s2c);
    int old_flags = fcntl(fd_raw, F_GETFL);
    fcntl(fd_raw, F_SETFL, old_flags | O_NONBLOCK);

    while (fgets(resp, sizeof(resp), s2c)) {
        if (strncmp(resp, "VERSION", 7) == 0) { // 7 = strlen("VERSION")
            uint64_t new_version = strtoull(resp + VERSION_PREFIX_LEN, NULL, BASE_DECIMAL);
            char edit_line[MAX_RESPONSE_LEN];

            // Store VERSION line in log
            append_log_line(resp);

            // Process all command results for this version
            while (fgets(edit_line, sizeof(edit_line), s2c)) {
                // Log all edits and END
                append_log_line(edit_line); 

                if (strncmp(edit_line, "END", 3) == 0) { // 3 = strlen("END")
                    break;
                }

                if (strstr(edit_line, "SUCCESS")) {
                    // Extract the command string by skipping two spaces
                    char *cmd_start = strchr(edit_line, ' ');
                    if (!cmd_start) {
                        continue;
                    }
                    cmd_start = strchr(cmd_start + 1, ' ');
                    if (!cmd_start) {
                        continue;
                    }
                    cmd_start++;

                    char *end = strstr(cmd_start, " SUCCESS");
                    if (end) {
                        *end = '\0';
                    }

                    // Apply the command to the local document
                    process_command(doc, cmd_start, doc->version);
                }
            }
            // Commit changes and update document version
            markdown_increment_version(doc);
            doc->version = new_version;
        } else {
            // Not a VERSION block, but still kept for LOG?
            append_log_line(resp);
        }
    }
    // Restore blocking mode
    fcntl(fd_raw, F_SETFL, old_flags);
}

/*
 * Entry point of client program.
 * - Performs a signal-based handshake with the server.
 * - Opens FIFO pipes for communication between client and server.
 * - Authenticates and receives initial document state.
 * - Enters a loop to process user commands and apply server broadcasts to sync local document.
*/
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: ./client <server_pid> <username>\n");
        return 1;
    }

    pid_t server_pid = (pid_t)atoi(argv[1]);
    const char *username = argv[2];
    pid_t client_pid = getpid();

    // Block SIGRTMIN+1 before initiating handshake
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN + 1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    // Send handshake signal, SIGRTMIN, to server
    kill(server_pid, SIGRTMIN);
    
    // Wait for SIGRTMIN+1 from server to continue
    int sig;
    sigwait(&mask, &sig);

    // Construct FIFO names
    char fifo_c2s[FIFO_NAME_LEN];
    char fifo_s2c[FIFO_NAME_LEN];
    snprintf(fifo_c2s, FIFO_NAME_LEN, "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, FIFO_NAME_LEN, "FIFO_S2C_%d", client_pid);

    // Open FIFOs
    int fd_c2s = open(fifo_c2s, O_WRONLY);
    int fd_s2c = open(fifo_s2c, O_RDONLY);

    // Send username to server
    dprintf(fd_c2s, "%s\n", username);

    // Wrap server-to-client FIFO in FILE* for reading
    FILE *s2c = fdopen(fd_s2c, "r");
    if (!s2c) {
        perror("fdopen");
        close(fd_c2s);
        close(fd_s2c);
        return 1;
    }

    // 1. Read role from server
    char role_line[ROLE_LEN];
    if (!fgets(role_line, sizeof(role_line), s2c)) {
        fprintf(stderr, "Failed to read role from server.\n");
        close(fd_c2s);
        fclose(s2c);
        return 1;
    }

    // Check for rejection
    if (strncmp(role_line, "Reject UNAUTHORISED", 19) == 0) { // 19 = strlen("Reject UNAUTHORISED")
        printf("%s", role_line);
        close(fd_c2s);
        fclose(s2c);
        return 1;
    }

    // Correctly format role line
    role_line[strcspn(role_line, "\n")] = '\0';
    char client_role[ROLE_LEN];
    strncpy(client_role, role_line, sizeof(client_role));
    client_role[sizeof(client_role) - 1] = '\0';

    // 2. Read document version from server
    char version_buf[VERSION_BUF_SIZE];
    if (!fgets(version_buf, sizeof(version_buf), s2c)) {
        fprintf(stderr, "Failed to read version.\n");
        close(fd_c2s);
        fclose(s2c);
        return 1;
    }
    size_t doc_version = strtoull(version_buf, NULL, BASE_DECIMAL);

    // 3. Read document length from server
    char length_buf[LENGTH_BUF_SIZE];
    if (!fgets(length_buf, sizeof(length_buf), s2c)) {
        fprintf(stderr, "Failed to read document length.\n");
        close(fd_c2s);
        fclose(s2c);
        return 1;
    }
    size_t doc_length = strtoull(length_buf, NULL, BASE_DECIMAL);

    // 4. Read document content from server
    char *document = malloc(doc_length + 1);
    size_t total_read = fread(document, 1, doc_length, s2c);
    if (total_read < doc_length) {
        fprintf(stderr, "Partial document read.\n");
    }
    document[doc_length] = '\0';

    // Initialise local document
    doc = markdown_init();
    markdown_insert(doc, 0, 0, document);
    doc->version = doc_version;
    free(document);

    // Client command Loop
    char input[MAX_INPUT_SIZE];
    while (1) {
        fflush(stdout);

        // Read a line of user input
        if (!fgets(input, sizeof(input), stdin)) {
            // EOF or error
            break;
        }

        // Strip trailing newline
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') {
            input[len - 1] = '\0';
            --len;
        }

        // Enforce max command size (255 chars + '\n')
        if (len + 1 > MAX_COMMAND_SIZE) {
            fprintf(stderr, "Error: command too long (max 255 chars)\n");
            continue;
        }

        // Enforce printable ASCII characters (32–126)
        bool bad_input = false;
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)input[i] < ASCII_PRINT_MIN || (unsigned char)input[i] > ASCII_PRINT_MAX) {
                bad_input = true;
                break;
            }
        }
        if (bad_input) {
            fprintf(stderr, "Error: non-ASCII or non-printable character in command\n");
            continue;
        }

        // Send DISCONNECT to server, then break
        if (strcmp(input, "DISCONNECT") == 0) {
            dprintf(fd_c2s, "%s\n", input);
            break;
        // Handle PERM?, LOG? and DOC? commands locally
        } else if (strcmp(input, "PERM?") == 0) {
            printf("%s\n", role_line);

        } else if (strcmp(input, "LOG?") == 0) {
            apply_broadcasts(s2c);
            for (log_line *curr = log_head; curr != NULL; curr = curr->next) {
                printf("%s", curr->line);
            }
        } else if (strcmp(input, "DOC?") == 0) {
            apply_broadcasts(s2c);
            char *flat = markdown_flatten(doc);
            printf("%s\n", flat);
            free(flat);
        // Otherwise, a normal editing command has been inputted and will be sent to the server
        } else {
            if (dprintf(fd_c2s, "%s\n", input) < 0) {
                perror("Failed to send command");
            }
            apply_broadcasts(s2c);
        }
    }
    // Clean up resources
    markdown_free(doc);
    close(fd_c2s);
    fclose(s2c);
    close(fd_s2c);
    free_log();
    return 0;
}