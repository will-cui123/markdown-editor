#include "helper.h"
#include <string.h>
#include <stdio.h>

// Helper function used by server and client to parse and process commands
int process_command(document *doc, const char *command_str, uint64_t client_version) {
    size_t pos;
    size_t start; 
    size_t end;
    int level;
    size_t len;
    char arg[MAX_COMMAND_SIZE];
    char url[MAX_COMMAND_SIZE];

    // Handle all markdown editing and formatting commands
    if (sscanf(command_str, "INSERT %zu %[^\n]", &pos, arg) == 2) {
        return markdown_insert(doc, client_version, pos, arg);
    } else if (sscanf(command_str, "DEL %zu %zu", &pos, &len) == 2) {
        return markdown_delete(doc, client_version, pos, len);
    } else if (sscanf(command_str, "NEWLINE %zu", &pos) == 1) {
        return markdown_newline(doc, client_version, pos);
    } else if (sscanf(command_str, "HEADING %d %zu", &level, &pos) == 2) {
        return markdown_heading(doc, client_version, level, pos);
    } else if (sscanf(command_str, "BOLD %zu %zu", &start, &end) == 2) {
        return markdown_bold(doc, client_version, start, end);
    } else if (sscanf(command_str, "ITALIC %zu %zu", &start, &end) == 2) {
        return markdown_italic(doc, client_version, start, end);
    } else if (sscanf(command_str, "BLOCKQUOTE %zu", &pos) == 1) {
        return markdown_blockquote(doc, client_version, pos);
    } else if (sscanf(command_str, "ORDERED_LIST %zu", &pos) == 1) {
        return markdown_ordered_list(doc, client_version, pos);
    } else if (sscanf(command_str, "UNORDERED_LIST %zu", &pos) == 1) {
        return markdown_unordered_list(doc, client_version, pos);
    } else if (sscanf(command_str, "CODE %zu %zu", &start, &end) == 2) {
        return markdown_code(doc, client_version, start, end);
    } else if (sscanf(command_str, "HORIZONTAL_RULE %zu", &pos) == 1) {
        return markdown_horizontal_rule(doc, client_version, pos);
    } else if (sscanf(command_str, "LINK %zu %zu %[^\n]", &start, &end, url) == 3) {
        return markdown_link(doc, client_version, start, end, url);
    }

    return UNKNOWN_COMMAND;
}