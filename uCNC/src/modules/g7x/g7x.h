#ifndef G7X_H
#define G7X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    G7X_OK = 0,
    G7X_BAD_FIELD,
    G7X_UNSUPPORTED,
    G7X_WRITE_FAILED
} g7x_result_t;

typedef enum {
    G7X_CYCLE_NONE = 0,
    G7X_CYCLE_G71,
    G7X_CYCLE_G72
} g7x_cycle_t;

typedef enum {
    G7X_CONTOUR_NONE = 0,
    G7X_CONTOUR_RAPID,
    G7X_CONTOUR_LINE,
    G7X_CONTOUR_ARC_CW,
    G7X_CONTOUR_ARC_CCW,
    G7X_CONTOUR_END
} g7x_contour_cmd_t;

typedef enum {
    G7X_AXIS_X = 0,
    G7X_AXIS_Z = 1
} g7x_axis_t;

typedef enum {
    G7X_CORNER_NONE = 0,
    G7X_CORNER_RND,
    G7X_CORNER_CHMF
} g7x_corner_kind_t;

typedef enum {
    G7X_SEGMENT_LINE = 0,
    G7X_SEGMENT_ARC
} g7x_segment_kind_t;

typedef struct {
    uint8_t units;
    uint8_t distance;
} g7x_modal_t;

typedef struct {
    g7x_cycle_t cycle;
    g7x_axis_t pass_axis;
    g7x_axis_t cut_axis;
    g7x_axis_t contour_monotonic_axis;
    char rough_doc_word;
    const char *name;
} g7x_cycle_profile_t;

#ifndef G7X_MAX_CONTOUR_ELEMENTS
#define G7X_MAX_CONTOUR_ELEMENTS 48
#endif

typedef struct {
    g7x_segment_kind_t kind;
    float d;
    float z;
    float r;
    int cw;
    int gcode_cw;
    float i;
    float k;
    int has_center;
    g7x_corner_kind_t outgoing_kind;
    float outgoing_amount;
    size_t source_line;
} g7x_contour_element_t;

typedef struct {
    int active;
    g7x_cycle_t cycle;
    float retract;
    float x_allow;
    float z_allow;
    g7x_contour_element_t elements[G7X_MAX_CONTOUR_ELEMENTS];
    unsigned count;
} g7x_contour_region_t;

typedef enum {
    G7X_STEP_LINE = 0,
    G7X_STEP_DONE,
    G7X_STEP_ERROR
} g7x_step_result_t;

typedef struct {
    bool active;
    bool started;
    bool finish;
    unsigned stage;
    unsigned finish_i;
    int dir;
    float pass;
    float final_pass;
    float start_x;
    float start_z;
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    float feed;
    float doc;
    size_t pending_source_line;
    size_t last_source_line;
    g7x_contour_region_t region;
} g7x_stream_t;

typedef struct {
    uint8_t motion;
    bool has_x;
    bool has_z;
    bool has_r;
    bool has_i;
    bool has_k;
    bool has_f;
    size_t source_line;
    float x;
    float z;
    float r;
    float i;
    float k;
    float f;
} g7x_motion_block_t;

typedef enum {
    G7X_EVENT_NONE = 0,
    G7X_EVENT_COMMENT,
    G7X_EVENT_MOTION
} g7x_event_type_t;

typedef struct {
    g7x_event_type_t type;
    char comment[48];
    g7x_motion_block_t motion;
} g7x_event_t;

enum {
    G7X_DISTANCE_ABSOLUTE = 0,
    G7X_DISTANCE_INCREMENTAL = 1,
    G7X_UNITS_INCH = 0,
    G7X_UNITS_MM = 1
};

void g7x_modal_default(g7x_modal_t *modal);
void g7x_modal_from_ucnc_modes(g7x_modal_t *modal, const uint8_t *modalgroups);
bool g7x_cycle_profile(g7x_cycle_t cycle, g7x_cycle_profile_t *profile);

void g7x_stream_reset(g7x_stream_t *stream);
g7x_result_t g7x_stream_begin_parsed(g7x_stream_t *stream,
                                     g7x_cycle_t cycle,
                                     float retract,
                                     float x_allow,
                                     float z_allow,
                                     float feed,
                                     float doc);
g7x_result_t g7x_stream_add_parsed(g7x_stream_t *stream,
                                   g7x_contour_cmd_t cmd,
                                   float x,
                                   bool has_x,
                                   float z,
                                   bool has_z,
                                   float r,
                                   bool has_r,
                                   float i,
                                   bool has_i,
                                   float k,
                                   bool has_k,
                                   g7x_corner_kind_t corner_kind,
                                   float corner_amount,
                                   bool *done);
g7x_step_result_t g7x_stream_next(g7x_stream_t *stream, char *out, size_t out_sz);
g7x_step_result_t g7x_stream_next_event(g7x_stream_t *stream, g7x_event_t *event);
g7x_step_result_t g7x_stream_next_block(g7x_stream_t *stream, g7x_motion_block_t *block);
bool g7x_parser_busy(void);

const char *g7x_result_text(g7x_result_t result);

#ifdef __cplusplus
}
#endif

#endif
