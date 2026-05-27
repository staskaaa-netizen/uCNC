#ifndef LEANCAM_TEXT_H
#define LEANCAM_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lc_text_command_is(const char *line, const char *cmd);
bool lc_text_get_field_text(const char *line, const char *key, char *out, size_t out_sz);
bool lc_text_get_field_float(const char *line, const char *key, float *out);

#ifdef __cplusplus
}
#endif

#endif
