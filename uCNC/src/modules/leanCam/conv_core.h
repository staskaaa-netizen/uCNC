#ifndef CONV_CORE_H
#define CONV_CORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINES 32
#define MAX_LEN   160

typedef struct {
    char lines[MAX_LINES][MAX_LEN];
    int count;
} program_t;

void prog_init(program_t *p);
bool prog_add(program_t *p, const char *line);
bool prog_insert_after(program_t *p, int after_idx, const char *line);
bool prog_delete(program_t *p, int idx);

bool find_field(const char *line, int start_from, int *s, int *e);
bool set_field(char *line, int s, int e, const char *val);

bool field_is_required_or_unresolved(const char *line, int s, int e);
bool find_first_required_or_unresolved(const char *line, int *s, int *e);
bool find_next_required_or_unresolved(const char *line, int from, int *s, int *e);

bool line_has_unresolved_required(const char *line);

#ifdef __cplusplus
}
#endif

#endif