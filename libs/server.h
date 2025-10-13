#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

/*
 * Holds the PID of a newly connecting client (passed to handler thread)
 */ 
typedef struct {
    pid_t client_pid; // PID of connecting client
} client_arg;


/*
 * Represents a single line in the version log
 */
typedef struct log_entry {
    char *line; // Log line content
    struct log_entry *next; // Pointer to next log entry
} log_entry;

/*
 *A complete log of one document version's changes
 */ 
typedef struct version_log {
    int version_number; // Version number associated with changes
    log_entry *entries; // Linked list of log entries for this version
    struct version_log *next; // Pointer to next log version
} version_log;

/*
 * Represents a client currently connected to the server (for broadcasting)
 */ 
typedef struct client_pipe {
    int fd; // File descriptor for the server-to-client FIFO
    struct client_pipe *next; // Pointer to next client in the list
} client_pipe;