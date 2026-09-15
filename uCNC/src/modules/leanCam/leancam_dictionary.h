#ifndef LEANCAM_DICTIONARY_H
#define LEANCAM_DICTIONARY_H

/* LeanCam vocabulary: NC command, NC word, conversational meaning.
 * Keep this as data/constant text only. Parsing and behavior live elsewhere.
 */
#define LC_DICT_COMMAND_TOOL "T"
#define LC_DICT_COMMAND_G71 "G71"
#define LC_DICT_COMMAND_G72 "G72"
#define LC_DICT_COMMAND_G76 "G76"
#define LC_DICT_COMMAND_G970 "G970"
#define LC_DICT_COMMAND_G971 "G971"
#define LC_DICT_COMMAND_G972 "G972"
#define LC_DICT_COMMAND_G973 "G973"

#define LC_DICT_WORD_T "T"
#define LC_DICT_WORD_R "R"
#define LC_DICT_WORD_O "O"
#define LC_DICT_WORD_F "F"
#define LC_DICT_WORD_FF "FF"
#define LC_DICT_WORD_DOC "DOC"
#define LC_DICT_WORD_FDOC "FDOC"
#define LC_DICT_WORD_S "S"
#define LC_DICT_WORD_XO "XO"
#define LC_DICT_WORD_ZO "ZO"

#define LC_LEANCAM_DICTIONARY(X) \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_T, "tool number") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_R, "tool nose radius") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_O, "tool insert orientation") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_F, "roughing feed") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_FF, "finishing feed") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_DOC, "roughing depth of cut") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_FDOC, "finishing depth of cut") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_S, "spindle rpm") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_XO, "measured X offset") \
    X(LC_DICT_COMMAND_TOOL, LC_DICT_WORD_ZO, "measured Z offset") \
    X(LC_DICT_COMMAND_G71, "U", "X roughing depth") \
    X(LC_DICT_COMMAND_G71, LC_DICT_WORD_R, "retract amount") \
    X(LC_DICT_COMMAND_G71, "X", "X finish allowance") \
    X(LC_DICT_COMMAND_G71, "Z", "Z finish allowance") \
    X(LC_DICT_COMMAND_G71, LC_DICT_WORD_F, "roughing feed") \
    X(LC_DICT_COMMAND_G72, "W", "Z roughing depth") \
    X(LC_DICT_COMMAND_G72, LC_DICT_WORD_R, "retract amount") \
    X(LC_DICT_COMMAND_G72, "X", "X finish allowance") \
    X(LC_DICT_COMMAND_G72, "Z", "Z finish allowance") \
    X(LC_DICT_COMMAND_G72, LC_DICT_WORD_F, "roughing feed") \
    X(LC_DICT_COMMAND_G76, "P", "thread pitch") \
    X(LC_DICT_COMMAND_G76, "K", "full thread depth") \
    X(LC_DICT_COMMAND_G76, "J", "first cut depth") \
    X(LC_DICT_COMMAND_G76, "H", "spring passes") \
    X(LC_DICT_COMMAND_G76, "Q", "compound angle") \
    X(LC_DICT_COMMAND_G76, LC_DICT_WORD_R, "thread degression") \
    X(LC_DICT_COMMAND_G970, "X", "preview minimum X") \
    X(LC_DICT_COMMAND_G970, "U", "preview maximum X") \
    X(LC_DICT_COMMAND_G970, "Z", "preview minimum Z") \
    X(LC_DICT_COMMAND_G970, "W", "preview maximum Z") \
    X(LC_DICT_COMMAND_G971, "X", "stock outer diameter") \
    X(LC_DICT_COMMAND_G971, "Z", "stock length") \
    X(LC_DICT_COMMAND_G971, "I", "stock inner diameter") \
    X(LC_DICT_COMMAND_G971, "E", "extra stock") \
    X(LC_DICT_COMMAND_G972, "C", "clamp length") \
    X(LC_DICT_COMMAND_G973, "P", "preview mode")

#endif
