#ifndef PARSER_G7_G8_H
#define PARSER_G7_G8_H

#include "../../cnc.h"
#include <stdbool.h>

#ifdef ENABLE_PARSER_MODULES

bool g7_g8_is_diameter_mode(void);
void g7_g8_program_words_to_motion(const parser_cmd_explicit_t *cmd, parser_words_t *words);
void g7_g8_motion_words_to_program(const parser_cmd_explicit_t *cmd, parser_words_t *words);

#endif

#endif
