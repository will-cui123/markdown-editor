#ifndef HELPER_H
#define HELPER_H

#include "markdown.h"
#include <stdint.h>

// Shared constants between server and client
#define FIFO_NAME_LEN 64
#define MAX_INPUT_SIZE 256
#define ROLE_LEN 16 // Role is either "read" or "write"
#define MAX_COMMAND_SIZE 256 // Maximum command size is 256 bytes
#define LINE_LEN 256
#define UNKNOWN_COMMAND -5 // Fallback error code for unrecognised command

/*
 * Parses and applies a markdown editing command to the given document.
 * This function ensures consistent command handling logic between the client-side
 * local copy and the server copy of the document.
*/
int process_command(document *doc, const char *command_str, uint64_t client_version);

#endif