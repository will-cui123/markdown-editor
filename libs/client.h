#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 *Represents a single line in the client-side log of broadcasts
 */ 
typedef struct log_line {
    char *line; // Text of the log entry (e.g., VERSION, EDIT, END)
    struct log_line *next; // Pointer to the next log entry
} log_line;