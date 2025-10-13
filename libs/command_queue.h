#ifndef COMMAND_QUEUE_H
#define COMMAND_QUEUE_H

#include <stdint.h>
#include <time.h>

/*
 * Represents a single command from a client.
 * Used to queue edits until applied by the broadcast thread.
 */
typedef struct queued_command {
    char *username; // Name of client issuing command
    char *role; // Client's role (i.e. "read" or "write")
    char *command_str; // The actual command text
    uint64_t client_version; // Document version of client when sending
    struct timespec timestamp; // Time when command was received
    struct queued_command *next; // Pointer to next command in queue
} queued_command;

/*
 * Adds a new command to the end of the command queue (stores user info, command, client version and timestamp)
 */ 
void enqueue_command(queued_command **head, const char *user, const char *role, const char *cmd, uint64_t version);

/*
 * Frees all memory in the command queue
 */
void free_command_queue(queued_command **head);

/*
 * Sorts the command queue by timestamp (earliest first) to ensure consistent processing order
 * Uses an in-place bubble sort by swapping fields
 */
void sort_command_queue(queued_command **head);

#endif