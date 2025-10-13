#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "../libs/markdown.h"

#define MAX_HEADING_LEVEL 3 // Maximum heading level is ###
#define MAX_HEADING_LEN (MAX_HEADING_LEVEL + 2) // "### " + null terminator
#define MAX_HEADING_BUF (MAX_HEADING_LEN + 2) // Buffer for optional newline + heading + null

#define LINK_SUFFIX_FORMAT_LEN 4  // Length for "](<url>)" + null terminator

#define LIST_PREFIX_LEN 3 // "1. " up till "9. "
#define LIST_PREFIX_DOT_OFFSET 1 // Position of '.' in list prefix
#define LIST_PREFIX_SPACE_OFFSET 2 // Position of space after '.' in list prefix
#define MAX_LIST_ITEM_NUMBER 9 // Ordered list supports items 1-9

#define BUF_SIZE 16 // Generic small buffer size
#define INSERT_FAILED -4 // Fallback error code for failed insert

// HELPER FUNCTIONS

// Finds the chunk containing a position and returns its local offset
chunk *find_chunk(document *doc, size_t pos, size_t *local_offset) {
    chunk *current = doc->head;
    size_t i = 0;

    while (current && i + current->length <= pos) {
        i += current->length;
        current = current->next;
    }

    if (current) {
        *local_offset = pos - i;
    }

    return current;
}

// Checks if the position is near an existing ordered list prefix (e.g., "1. ")
bool is_near_list_prefix(const char *text, size_t pos, size_t doc_length) {
    // Check before cursor
    if (pos >= LIST_PREFIX_LEN &&
        isdigit(text[pos - LIST_PREFIX_LEN]) &&
        text[pos - LIST_PREFIX_LEN + LIST_PREFIX_DOT_OFFSET] == '.' &&
        text[pos - LIST_PREFIX_LEN + LIST_PREFIX_SPACE_OFFSET] == ' ') {
        return true;
    }

    // Check at cursor
    if (pos + LIST_PREFIX_LEN - 1 < doc_length &&
        isdigit(text[pos]) &&
        text[pos + LIST_PREFIX_DOT_OFFSET] == '.' &&
        text[pos + LIST_PREFIX_SPACE_OFFSET] == ' ') {
        return true;
    }

    return false;
}

// Returns true if the [start, end) range is entirely within a deleted region.
bool is_fully_within_deleted(size_t start, size_t end, range *r) {
    while (r) {
        if (start >= r->start && end <= r->end) {
            return true;
        }
        r = r->next;
    }
    return false;
}

// Adjusts start/end positions if they fall within a deleted region
void adjust_partially_deleted(size_t *start, size_t *end, range *r) {
    while (r) {
        // If start is inside a deleted range, snap to the closer edge
        if (*start >= r->start && *start < r->end) {
            *start = (*start - r->start <= r->end - *start) ? r->start : r->end;
        }
        // If end is inside a deleted range, snap to the closer edge
        if (*end >= r->start && *end < r->end) {
            *end = (*end - r->start <= r->end - *end) ? r->start : r->end;
        }
        r = r->next;
    }
}

// Adjusts a single position if it falls within a deleted range
size_t adjust_single_position_if_deleted(size_t pos, range *deleted) {
    for (range *r = deleted; r; r = r->next) {
        // If position is inside a deleted range, move to the start of that range
        if (pos >= r->start && pos < r->end) {
            return r->start;
        }
    }
    return pos;
}

// Builds a list of deleted ranges from the pending edit queue
range *build_deleted_ranges(edit *pending) {
    range *deleted = NULL;
    for (edit *e = pending; e; e = e->next) {
        if (e->type == EDIT_DELETE) {
            range *r = malloc(sizeof(range));
            r->start = e->pos;
            r->end = e->pos + e->del_len;
            r->next = deleted;
            deleted = r;
        }
    }
    return deleted;
}

// Frees a list of deleted ranges created by build_deleted_ranges()
void free_deleted_ranges(range *deleted) {
    while (deleted) {
        range *next = deleted->next;
        free(deleted);
        deleted = next;
    }
}

// Returns true if the character before the given position is not a newline
bool needs_preceding_newline(const document *doc, size_t pos) {
    if (pos == 0) {
        return false;
    }
    char *flat = markdown_flatten(doc);
    bool result = (flat[pos - 1] != '\n');
    free(flat);
    return result;
}

// INITIALISATION AND FREE

// Initialises and returns a new, empty document
document *markdown_init(void) {
    document *doc = malloc(sizeof(document));
    doc->version = 0; // Start at version 0
    doc->length = 0; // Empty document
    doc->head = NULL; // No chunks yet
    doc->tail = NULL;
    doc->pending = NULL;
    return doc;
}

void markdown_free(document *doc) {
    // Free all chunks in the document
    chunk *cur = doc->head;
    while (cur) {
        chunk *next = cur->next;
        free(cur);
        cur = next;
    }
    // Free all pending edits
    edit *e = doc->pending;
    while (e) {
        edit *next = e->next;
        if (e->text) {
            free(e->text);
        }
        free(e);
        e = next;
    }
    free(doc); // Free the document itself
}

// EDITING COMMANDS

// Queues an insert edit at the given position with the specified text
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *text) {
    if (!doc || !text || pos > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }
    // Create new edit node
    edit *e = malloc(sizeof(edit));
    e->type = EDIT_INSERT;
    e->pos = pos;
    e->text = strdup(text);
    e->del_len = 0;
    e->next = NULL;
    // Append to end of pending edit queue
    if (!doc->pending) {
        doc->pending = e;
    } else {
        edit *cur = doc->pending;
        while (cur->next) {
            cur = cur->next;
        }
        cur->next = e;
    }
    return SUCCESS;
}

// Applies an insert edit immediately to the document content
void apply_insert(document *doc, size_t pos, const char *text) {
    size_t len = strlen(text);
    size_t inserted = 0;
    size_t offset;
    chunk *cur = find_chunk(doc, pos, &offset);
    // If inserting at the end and no chunk exists, create one
    if (!cur && pos == doc->length) {
        cur = malloc(sizeof(chunk));
        cur->length = 0;
        cur->next = NULL;
        cur->prev = doc->tail;
        if (doc->tail) {
            doc->tail->next = cur;
        } else {
            doc->head = cur;
        }
        doc->tail = cur;
        offset = 0;
    }
    // Insert the text across one or more chunks
    while (inserted < len) {
         // If no chunk exists (e.g., middle insertion caused split), create one
        if (!cur) {
            cur = malloc(sizeof(chunk));
            cur->length = 0;
            cur->next = NULL;
            cur->prev = doc->tail;
            if (doc->tail) {
                doc->tail->next = cur;
            } else {
                doc->head = cur;
            }
            doc->tail = cur;
            offset = 0;
        }
        size_t space = CHUNK_SIZE - cur->length;
        size_t to_copy = (len - inserted < space) ? (len - inserted) : space;

        // Shift existing data to make room, if inserting in the middle
        if (offset < cur->length) {
            memmove(cur->data + offset + to_copy,
                    cur->data + offset,
                    cur->length - offset);
        }

        // Copy new data into the chunk
        memcpy(cur->data + offset, text + inserted, to_copy);
        cur->length += to_copy;
        doc->length += to_copy;
        inserted += to_copy;

        // If the chunk is full, move to the next one (create if necessary)
        if (cur->length == CHUNK_SIZE) {
            offset = 0;
            if (!cur->next) {
                chunk *next = malloc(sizeof(chunk));
                next->length = 0;
                next->prev = cur;
                next->next = NULL;
                cur->next = next;
                doc->tail = next;
            }
            cur = cur->next;
        } else {
            // Continue in same chunk
            offset += to_copy;
        }
    }
}

// Queues a delete operation starting at the given position for the specified length
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (!doc || pos > doc->length || len == 0) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }
    // Create and populate the delete edit node
    edit *e = malloc(sizeof(edit));
    e->type = EDIT_DELETE;
    e->pos = pos;
    e->text = NULL;
    e->del_len = len;
    e->next = NULL;
    // Append to the pending edit queue
    if (!doc->pending) doc->pending = e;
    else {
        edit *cur = doc->pending;
        while (cur->next) {
            cur = cur->next;
        }
        cur->next = e;
    }
    return SUCCESS;
}

// Applies a delete operation by removing characters from the document starting at the given position
void apply_delete(document *doc, size_t pos, size_t len) {
    size_t offset;
    chunk *cur = find_chunk(doc, pos, &offset);
    size_t to_delete = len;
    while (cur && to_delete > 0) {
        // Determine how many characters can be deleted from this chunk
        size_t can_delete = cur->length - offset;
        if (can_delete > to_delete) can_delete = to_delete;

        // Shift remaining data left to overwrite deleted portion
        memmove(cur->data + offset,
                cur->data + offset + can_delete,
                cur->length - offset - can_delete);

        cur->length -= can_delete;
        doc->length -= can_delete;
        to_delete -= can_delete;

        chunk *next = cur->next;
        
        // If the current chunk is now empty, remove it from the list
        if (cur->length == 0) {
            if (cur->prev) {
                cur->prev->next = cur->next;
            } else {
                doc->head = cur->next;
            }
            if (cur->next) {
                cur->next->prev = cur->prev;
            } else {
                doc->tail = cur->prev;
            }
            free(cur);
            cur = next;
            offset = 0;
        } else {
            // Move to the next chunk
            cur = next;
            offset = 0;
        }
    }
}

// FORMATTING COMMANDS

// Inserts a newline at the given position, adjusting if the position is inside a deleted range
int markdown_newline(document *doc, uint64_t version, size_t pos) {
    if (doc == NULL || pos > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }
    range *deleted = build_deleted_ranges(doc->pending);
    pos = adjust_single_position_if_deleted(pos, deleted);
    free_deleted_ranges(deleted);

    return markdown_insert(doc, version, pos, "\n");
}

// Inserts a Markdown heading (e.g., "# ", "## ", "### ") at the specified position and level
int markdown_heading(document *doc, uint64_t version, int level, size_t pos) {
    if (!doc || pos > doc->length || level < 1 || level > MAX_HEADING_LEVEL)
        return INVALID_CURSOR_POS;
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }
    range *deleted = build_deleted_ranges(doc->pending);
    size_t adjusted = adjust_single_position_if_deleted(pos, deleted);
    free_deleted_ranges(deleted);

    // Build heading prefix based on level
    char heading[MAX_HEADING_LEN];
    memset(heading, '#', level);
    heading[level] = ' ';
    heading[level + 1] = '\0';

    // Insert with newline prefix if needed
    if (needs_preceding_newline(doc, adjusted)) {
        char buf[MAX_HEADING_BUF];
        snprintf(buf, sizeof(buf), "\n%s", heading);
        return markdown_insert(doc, version, adjusted, buf);
    } else {
        return markdown_insert(doc, version, adjusted, heading);
    }
}

// Applies bold formatting by inserting "**" around the specified range
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    if (doc == NULL || start > end || end > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);
    if (is_fully_within_deleted(start, end, deleted)) {
        free_deleted_ranges(deleted);
        return DELETED_POSITION;
    }
    adjust_partially_deleted(&start, &end, deleted);
    free_deleted_ranges(deleted);

    // Insert closing tag first to preserve offsets
    if (markdown_insert(doc, version, end, "**") != 0)  {
        return INSERT_FAILED;
    }
    if (markdown_insert(doc, version, start, "**") != 0) {
        return INSERT_FAILED;
    }

    return SUCCESS;
}

// Applies italic formatting by inserting "*" around the specified range
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end) {
    if (doc == NULL || start > end || end > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);

    if (is_fully_within_deleted(start, end, deleted)) {
        free_deleted_ranges(deleted);
        return DELETED_POSITION;
    }
    adjust_partially_deleted(&start, &end, deleted);
    free_deleted_ranges(deleted);
    // Insert closing tag first to preserve original offsets
    if (markdown_insert(doc, version, end, "*") != 0)  {
        return INSERT_FAILED;
    }
    if (markdown_insert(doc, version, start, "*") != 0) {
        return INSERT_FAILED;
    }

    return SUCCESS;
}

// Inserts a blockquote prefix "> " at the given position
int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    if (doc == NULL || pos > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);
    size_t adjusted = adjust_single_position_if_deleted(pos, deleted);
    free_deleted_ranges(deleted);

    // Add newline before "> " if inserting mid-line
    if (needs_preceding_newline(doc, adjusted)) {
        if (markdown_insert(doc, version, adjusted, "\n> ") != 0) {
            return INSERT_FAILED;
        }
    } else {
        if (markdown_insert(doc, version, adjusted, "> ") != 0) {
            return INSERT_FAILED;
        }
    }

    return SUCCESS;
}

// Applies ordered list formatting at the given position in the document, with newline if needed
int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    if (doc == NULL || pos > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    // Adjust for deleted regions
    range *deleted = build_deleted_ranges(doc->pending);
    pos = adjust_single_position_if_deleted(pos, deleted);
    free_deleted_ranges(deleted);

    char *text = markdown_flatten(doc);
    // Reject insertion if already near an existing list prefix
    if (is_near_list_prefix(text, pos, doc->length)) {
        free(text);
        return -1;
    }

    // Step 1: Determine what number this list item should be
    size_t line_start = pos;
    while (line_start > 0 && text[line_start - 1] != '\n') {
        line_start--;
    }
    int number = 1;
    // Scan upward to find the most recent ordered list number
    size_t scan = line_start;
    while (scan > 0) {
        size_t line = scan;
        while (line > 0 && text[line - 1] != '\n') {
            line--;
        }
        if (isdigit(text[line]) && 
            text[line + LIST_PREFIX_DOT_OFFSET] == '.' && 
            text[line + LIST_PREFIX_SPACE_OFFSET] == ' ') {
            number = text[line] - '0' + 1;
            break;
        }
        if (line == 0) {
            break;
        }
        scan = line - 1;
    }
    if (number > MAX_LIST_ITEM_NUMBER) {
        free(text);
        return -1;
    }
    char prefix[BUF_SIZE];
    if (needs_preceding_newline(doc, pos)) {
        snprintf(prefix, sizeof(prefix), "\n%d. ", number);
    } else {
        snprintf(prefix, sizeof(prefix), "%d. ", number);
    }

    if (markdown_insert(doc, version, pos, prefix) != 0) {
        free(text);
        return INSERT_FAILED;
    }

    // Step 3: Re-flatten document to reflect new insertion
    free(text);
    text = markdown_flatten(doc);

    // Step 4: Walk forward to renumber any subsequent list items
    size_t cursor = pos + strlen(prefix);
    int renumber = number + 1;

    while (renumber <= MAX_LIST_ITEM_NUMBER && cursor < doc->length) {
        // Find start of next line
        size_t line_start = cursor;
        while (line_start < doc->length && text[line_start] != '\n') {
            line_start++;
        }
        if (line_start >= doc->length) break;

        size_t next_line = line_start + 1;
        if (next_line + LIST_PREFIX_SPACE_OFFSET >= doc->length) {
            break;
        }

        // Check if the next line starts with a valid list prefix
        if (isdigit(text[next_line]) && 
            text[next_line + LIST_PREFIX_DOT_OFFSET] == '.' && 
            text[next_line + LIST_PREFIX_SPACE_OFFSET] == ' ') {
            // Overwrite current prefix with new renumbered one
            if (markdown_delete(doc, doc->version, next_line, LIST_PREFIX_LEN) != 0) {
                break;
            }

            char new_prefix[BUF_SIZE];
            snprintf(new_prefix, sizeof(new_prefix), "%d. ", renumber);
            if (markdown_insert(doc, doc->version, next_line, new_prefix) != 0) {
                break;
            }

            renumber++;
            cursor = next_line + strlen(new_prefix);

            free(text);
            text = markdown_flatten(doc);
        } else {
            break; // No more list items to renumber
        }
    }

    free(text);
    return SUCCESS;
}

// Inserts an unordered list prefix "- " at the given position, with newline if needed
int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    if (!doc || pos > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);
    size_t adjusted = adjust_single_position_if_deleted(pos, deleted);
    free_deleted_ranges(deleted);

    char *flat = markdown_flatten(doc);
    if (!flat) {
        return -1;
    }
    bool need_newline = (adjusted > 0 && flat[adjusted - 1] != '\n');
    free(flat);
    if (need_newline) {
        if (markdown_insert(doc, version, adjusted, "\n- ") != 0) {
            return INSERT_FAILED;
        }
        adjusted++;  // Adjust position since newline is prepended
    } else {
        if (markdown_insert(doc, version, adjusted, "- ") != 0) {
            return INSERT_FAILED;
        }
    }

    return SUCCESS;
}

// Applies inline code formatting by wrapping the given range in "`"
int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    if (doc == NULL || start > end || end > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);
    if (is_fully_within_deleted(start, end, deleted)) {
        free_deleted_ranges(deleted);
        return DELETED_POSITION;
    }
    adjust_partially_deleted(&start, &end, deleted);
    free_deleted_ranges(deleted);

    if (markdown_insert(doc, version, end, "`") != 0) {
        return INSERT_FAILED;
    }
    if (markdown_insert(doc, version, start, "`") != 0) {
        return INSERT_FAILED;
    }

    return SUCCESS;
}

// Inserts a horizontal rule ("---") at the given position, adding newlines as needed
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    if (!doc || pos > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);
    size_t adjusted = adjust_single_position_if_deleted(pos, deleted);
    free_deleted_ranges(deleted);

    char *flat = markdown_flatten(doc);
    bool need_prefix_newline = (adjusted > 0 && flat[adjusted - 1] != '\n');
    bool need_suffix_newline = (adjusted == doc->length || flat[adjusted] != '\n');
    free(flat);

    // Build appropriate string to insert based on surrounding content (i.e. add newlines if required)
    char buffer[BUF_SIZE];
    if (need_prefix_newline && need_suffix_newline) {
        strcpy(buffer, "\n---\n");
    } else if (need_prefix_newline) {
        strcpy(buffer, "\n---");
    } else if (need_suffix_newline) {
        strcpy(buffer, "---\n");
    } else {
        strcpy(buffer, "---");
    }

    if (markdown_insert(doc, version, adjusted, buffer) != 0) {
        return INSERT_FAILED;
    }
    
    return SUCCESS;
}

// Wraps a text range in a markdown link using the provided URL
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url) {
    if (doc == NULL || url == NULL || start > end || end > doc->length) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) {
        return OUTDATED_VERSION;
    }

    range *deleted = build_deleted_ranges(doc->pending);
    if (is_fully_within_deleted(start, end, deleted)) {
        free_deleted_ranges(deleted);
        return DELETED_POSITION;
    }
    adjust_partially_deleted(&start, &end, deleted);
    free_deleted_ranges(deleted);

    // Create closing string: "](url)"
    size_t url_len = strlen(url);
    size_t link_buf_size = url_len + LINK_SUFFIX_FORMAT_LEN;
    char *closing = malloc(link_buf_size);
    snprintf(closing, link_buf_size, "](%s)", url);
    // Insert link markers
    if (markdown_insert(doc, version, start, "[") != 0) {
        free(closing);
        return INSERT_FAILED;
    }
    if (markdown_insert(doc, version, end, closing) != 0) {
        free(closing);
        return INSERT_FAILED;
    }

    free(closing);
    return SUCCESS;
}

// UTILITIES

// Prints the current state of the document to the given output stream
void markdown_print(const document *doc, FILE *stream) {
    char *flattened = markdown_flatten(doc);
    if (flattened == NULL) {
        return;
    }
    fprintf(stream, "%s", flattened);
    free(flattened);
}

// Returns a newly allocated string containing the full document text as a flat buffer
char *markdown_flatten(const document *doc) {
    char *buf = malloc(doc->length + 1); // +1 for null terminator
    if (buf == NULL) {
        return NULL;
    }
    size_t offset = 0;
    for (chunk *c = doc->head; c; c = c->next) {
        memcpy(buf + offset, c->data, c->length);
        offset += c->length;
    }
    buf[doc->length] = '\0';
    return buf;
}

// VERSIONING
// Applies all pending edits (in the form of inserts and deletes) to the document and increments its version
void markdown_increment_version(document *doc) {
    edit *e = doc->pending;

    // Separate deletes and inserts into two lists
    edit *deletes = NULL;
    edit *inserts = NULL;
    while (e) {
        edit *next = e->next;
        if (e->type == EDIT_DELETE) {
            e->next = deletes;
            deletes = e;
        } else if (e->type == EDIT_INSERT) {
            e->next = inserts;
            inserts = e;
        }
        e = next;
    }

    // Apply all delete edits
    for (edit *d = deletes; d; d = d->next) {
        apply_delete(doc, d->pos, d->del_len);
    }

    // Count insert operations
    int count = 0;
    for (edit *i = inserts; i; i = i->next) count++;

    // Copy insert edits into an array for sorting
    edit **insert_array = malloc(sizeof(edit *) * count);
    int idx = 0;
    for (edit *i = inserts; i; i = i->next) {
        insert_array[idx++] = i;
    }

    // Sort inserts by position in ascending order
    for (int i = 1; i < count; i++) {
        edit *key = insert_array[i];
        int j = i - 1;
        while (j >= 0 && insert_array[j]->pos > key->pos) {
            insert_array[j + 1] = insert_array[j];
            j--;
        }
        insert_array[j + 1] = key;
    }

    // Apply inserts, adjusting for position shifts due to prior inserts
    size_t offset = 0;
    for (int i = 0; i < count; i++) {
        size_t adjusted_pos = insert_array[i]->pos + offset;
        apply_insert(doc, adjusted_pos, insert_array[i]->text);
        offset += strlen(insert_array[i]->text);
    }

    // Free delete edits
    for (edit *d = deletes; d;) {
        edit *next = d->next;
        free(d);
        d = next;
    }
    // Free insert edits and their text
    for (int i = 0; i < count; i++) {
        free(insert_array[i]->text);
        free(insert_array[i]);
    }
    free(insert_array);

    doc->pending = NULL;
    doc->version++; // Increment the document version
}