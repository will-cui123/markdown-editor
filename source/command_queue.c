#define _POSIX_C_SOURCE 200809L
#include "command_queue.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Add a new command to the end of the queue
void enqueue_command(queued_command **head, const char *user, const char *role, const char *cmd, uint64_t version) {
    queued_command *new_node = malloc(sizeof(queued_command));
    new_node->username = strdup(user);
    new_node->role = strdup(role);
    new_node->command_str = strdup(cmd);
    new_node->client_version = version;
    // Capture the timestamp at the moment the command was received
    clock_gettime(CLOCK_MONOTONIC, &new_node->timestamp);
    new_node->next = NULL;

    // Append to the end of the queue
    if (*head == NULL) {
        *head = new_node;
    } else {
        queued_command *curr = *head;
        while (curr->next) curr = curr->next;
        curr->next = new_node;
    }
}

// Frees all memory in the command queue
void free_command_queue(queued_command **head) {
    while (*head) {
        queued_command *temp = *head;
        *head = (*head)->next;
        free(temp->username);
        free(temp->role);
        free(temp->command_str);
        free(temp);
    }
}

// Helper function to compare two command timestamps
int compare_timestamps(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) {
        return -1;
    }
    if (a->tv_sec > b->tv_sec) {
        return 1;
    }
    if (a->tv_nsec < b->tv_nsec) {
        return -1;
    }
    if (a->tv_nsec > b->tv_nsec) {
        return 1;
    }
    return 0;
}

// Sort the queue by timestamp
void sort_command_queue(queued_command **head) {
    if (!*head || !(*head)->next) {
        return;
    }

    for (queued_command *i = *head; i; i = i->next) {
        for (queued_command *j = i->next; j; j = j->next) {
            if (compare_timestamps(&i->timestamp, &j->timestamp) > 0) {
                // Swap all data fields
                char *tmp_user = i->username;
                char *tmp_role = i->role;
                char *tmp_cmd = i->command_str;
                uint64_t tmp_ver = i->client_version;
                struct timespec tmp_time = i->timestamp;

                i->username = j->username;
                i->role = j-> role;
                i->command_str = j->command_str;
                i->client_version = j->client_version;
                i->timestamp = j->timestamp;

                j->username = tmp_user;
                j->role = tmp_role;
                j->command_str = tmp_cmd;
                j->client_version = tmp_ver;
                j->timestamp = tmp_time;
            }
        }
    }
}
