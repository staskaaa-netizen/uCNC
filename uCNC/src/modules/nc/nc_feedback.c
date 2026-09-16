#include "../../cnc.h"
#include "nc_feedback.h"

const char *nc_feedback_lock(uint8_t settings_error, uint16_t state, bool alarm)
{
    if (settings_error & SETTINGS_READ_ERROR)
        return "Settings invalid: back up $$; $RST=* resets settings/offsets";
    if (settings_error & SETTINGS_WRITE_ERROR)
        return "Settings save failed: check storage before restarting";
    if ((state & (EXEC_KILL | EXEC_LIMITS)) || alarm)
        return "Controller alarm: check cause/status (?) before unlocking";
    if (state & EXEC_POSITION_MAYBE_LOST)
        return "Position untrusted: home ($H), or intentional unlock ($X)";
    if (state & EXEC_DOOR) return "Door open: close door before resuming";
    if (state & EXEC_JOG) return "Jog active: finish or cancel jog before RUN";
    return "";
}

const char *nc_feedback_error(uint8_t error)
{
    switch (error) {
    case STATUS_BAD_NUMBER_FORMAT: return "Invalid number";
    case STATUS_INVALID_STATEMENT: return "Invalid parameters or cycle contour";
    case STATUS_NEGATIVE_VALUE: return "Negative value not allowed";
    case STATUS_SYSTEM_GC_LOCK: return "Controller locked or cycle canceled";
    case STATUS_SOFT_LIMIT_ERROR: return "Target exceeds travel limits";
    case STATUS_GCODE_UNSUPPORTED_COMMAND: return "Unsupported command";
    case STATUS_GCODE_MODAL_GROUP_VIOLATION: return "Conflicting modal commands";
    case STATUS_GCODE_UNDEFINED_FEED_RATE: return "Set a valid feed rate (F)";
    case STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER: return "Integer value required";
    case STATUS_GCODE_VALUE_WORD_MISSING: return "Required parameter missing";
    case STATUS_GCODE_UNUSED_WORDS: return "Unexpected parameter";
    case STATUS_INVALID_PLANE_SELECTED: return "Wrong plane for this command";
    case STATUS_SPINDLE_RPM_ERROR: return "Spindle feedback/synchronization error";
    default: return "Command rejected; check parameters";
    }
}
