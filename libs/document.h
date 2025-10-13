#ifndef DOCUMENT_H

#define DOCUMENT_H

#include <stdint.h>
#include <stdlib.h>

#define CHUNK_SIZE 256 // Each chunk holds up to 256 characters of document content

/*
 * Represents a block of text in the document, forming a doubly linked list
 */ 
typedef struct chunk {
    char data[CHUNK_SIZE]; // Buffer of characters in this chunk
    size_t length; // Number of characters currently in use
    struct chunk *prev; // Pointer to previous chunk
    struct chunk *next; // Pointer to next chunk
} chunk;

/*
 * Type of edit (insert or delete)
 */
typedef enum {
    EDIT_INSERT,
    EDIT_DELETE
} edit_type;

/*
 * Represents a single pending edit (insert or delete)
 */
typedef struct edit {
    edit_type type; // Type of edit
    size_t pos; // Position in the document
    char *text; // Text to insert (for inserts only)
    size_t del_len; // Length of text to delete (for deletes only)
    struct edit *next; // Pointer to next edit in queue
} edit;

/*
 * Represents a deleted range (used for validation and adjustments)
 */ 
typedef struct range {
    size_t start; // Starting index of deleted region
    size_t end; // Ending index (exclusive) of deleted region
    struct range *next; // Pointer to next range
} range;

/*
 * Represents the entire document, including content and pending edits
 */
typedef struct {
    uint64_t version; // Current version of the document
    size_t length; // Total number of characters in the document
    chunk *head; // Pointer to the first chunk
    chunk *tail; // Pointer to the last chunk
    edit *pending; // Linked list of pending edits
} document;

#endif
