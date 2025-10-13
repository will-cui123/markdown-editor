#ifndef MARKDOWN_H
#define MARKDOWN_H
#include <stdio.h>
#include <stdint.h>
#include "document.h"  

// Return codes for the editing and formatting markdown functions
#define SUCCESS 0
#define INVALID_CURSOR_POS -1
#define DELETED_POSITION -2
#define OUTDATED_VERSION -3

// Initialise and free a document
document * markdown_init(void);
void markdown_free(document *doc);

// === Edit Commands ===
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content);
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len);

// === Formatting Commands ===
int markdown_newline(document *doc, uint64_t version, size_t pos);
int markdown_heading(document *doc, uint64_t version, int level, size_t pos);
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end);
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end);
int markdown_blockquote(document *doc, uint64_t version, size_t pos);
int markdown_ordered_list(document *doc, uint64_t version, size_t pos);
int markdown_unordered_list(document *doc, uint64_t version, size_t pos);
int markdown_code(document *doc, uint64_t version, size_t start, size_t end);
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos);
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url);

// === Utilities ===
void markdown_print(const document *doc, FILE *stream);
char *markdown_flatten(const document *doc);

// === Versioning ===
void markdown_increment_version(document *doc);
#endif // MARKDOWN_H
