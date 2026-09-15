#ifndef NC_TOOLS_H
#define NC_TOOLS_H

#include "nc.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int t;
    float r;
    int orient;
    float feed;
    float finish_feed;
    float doc;
    float finish_doc;
    float rpm;
    float xoff;
    float zoff;
    bool valid;
} nc_tool_t;

const char *nc_tool_default_line(int t);
bool nc_tool_line_is_tool(const char *line);
bool nc_tool_orient_valid(int orient);
bool nc_tool_from_line(const char *line, nc_tool_t *tool);
bool nc_tool_field_text(const char *line, char letter, char *out, size_t out_sz);
bool nc_tool_number_for_line(const nc_document_t *doc, size_t before_or_at, int *tool_no);
bool nc_tool_by_number(const nc_document_t *tool_doc, int tool_no, nc_tool_t *tool);
bool nc_tool_active_for_line(const nc_document_t *doc, size_t before_or_at, nc_tool_t *tool);
bool nc_tool_active_from_table(const nc_document_t *program_doc,
                               size_t before_or_at,
                               const nc_document_t *tool_doc,
                               nc_tool_t *tool);
bool nc_tool_active_from_file(const nc_document_t *program_doc,
                              size_t before_or_at,
                              const char *tool_path,
                              nc_tool_t *tool);
nc_result_t nc_insert_tool_default(nc_document_t *doc);

#ifdef __cplusplus
}
#endif

#endif
