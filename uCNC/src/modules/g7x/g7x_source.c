/* Numbered-block source cursor and bounded serial history for P/Q cycles.
   Plain C: no file IO, no allocation, no NC dependency. */
#include "g7x_source.h"

#include <string.h>

g7x_source_status_t g7x_source_next(const g7x_source_t *source,
                                    const g7x_source_pos_t *from,
                                    g7x_source_pos_t *out,
                                    char *text,
                                    size_t text_sz)
{
    if (!source || !source->next || !from || !out || !text || text_sz == 0)
        return G7X_SOURCE_ERROR;
    text[0] = '\0';
    return source->next(source->user, from, out, text, text_sz);
}

void g7x_history_reset(g7x_history_t *history)
{
    if (history)
        memset(history, 0, sizeof(*history));
}

g7x_result_t g7x_history_add(g7x_history_t *history,
                             uint32_t number,
                             const char *text)
{
    g7x_retained_block_t *slot;
    size_t len;

    if (!history || !text || !*text)
        return G7X_BAD_FIELD;
    len = strlen(text);
    if (len >= G7X_RETAINED_TEXT_LEN)
        return G7X_WRITE_FAILED;

    slot = &history->blocks[history->next];
    if (slot->used) {
        /* The ring is full: this block is dropped and must not be resolved
           later by accident. */
        history->evicted = true;
    } else {
        history->count++;
    }
    slot->number = number;
    slot->used = true;
    memcpy(slot->text, text, len + 1u);
    history->next = (history->next + 1u) % G7X_MAX_RETAINED_BLOCKS;
    return G7X_OK;
}

const char *g7x_history_find(const g7x_history_t *history, uint32_t number)
{
    unsigned i;

    if (!history)
        return NULL;
    for (i = 0; i < G7X_MAX_RETAINED_BLOCKS; i++) {
        const g7x_retained_block_t *slot = &history->blocks[i];
        if (slot->used && slot->number == number)
            return slot->text;
    }
    return NULL;
}

bool g7x_history_evicted(const g7x_history_t *history)
{
    return history && history->evicted;
}

/* Retained entries are stored in a ring; expose them in stream order. */
static unsigned g7x_history_index(const g7x_history_t *history, unsigned offset)
{
    if (history->count < G7X_MAX_RETAINED_BLOCKS)
        return offset;
    return (history->next + offset) % G7X_MAX_RETAINED_BLOCKS;
}

g7x_result_t g7x_history_visit_range(const g7x_history_t *history,
                                     uint32_t p,
                                     uint32_t q,
                                     g7x_history_visit_fn visit,
                                     void *user,
                                     unsigned *visited)
{
    unsigned i;
    unsigned start = G7X_MAX_RETAINED_BLOCKS;
    unsigned end = G7X_MAX_RETAINED_BLOCKS;
    unsigned found = 0;

    if (visited)
        *visited = 0;
    if (!history || !visit)
        return G7X_BAD_FIELD;
    if (q < p)
        return G7X_RANGE_AMBIGUOUS;

    /* Stream order is ring order starting at the oldest retained entry. */
    for (i = 0; i < G7X_MAX_RETAINED_BLOCKS; i++) {
        const g7x_retained_block_t *slot = &history->blocks[g7x_history_index(history, i)];
        if (!slot->used)
            continue;
        if (start == G7X_MAX_RETAINED_BLOCKS) {
            if (slot->number == p)
                start = i;
        } else if (end == G7X_MAX_RETAINED_BLOCKS) {
            if (slot->number == q)
                end = i;
        }
    }
    if (start == G7X_MAX_RETAINED_BLOCKS || end == G7X_MAX_RETAINED_BLOCKS)
        return G7X_RANGE_MISSING;

    /* Two blocks with the same number inside the range make it ambiguous. */
    for (i = start; i <= end; i++) {
        unsigned j;
        const g7x_retained_block_t *a = &history->blocks[g7x_history_index(history, i)];
        if (!a->used)
            continue;
        for (j = i + 1u; j <= end; j++) {
            const g7x_retained_block_t *b = &history->blocks[g7x_history_index(history, j)];
            if (b->used && b->number == a->number)
                return G7X_RANGE_AMBIGUOUS;
        }
    }

    for (i = start; i <= end; i++) {
        const g7x_retained_block_t *slot = &history->blocks[g7x_history_index(history, i)];
        if (!slot->used)
            continue;
        visit(user, slot->number, slot->text);
        found++;
    }
    if (visited)
        *visited = found;
    return G7X_OK;
}
