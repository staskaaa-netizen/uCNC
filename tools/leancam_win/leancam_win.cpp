#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "leancam_gcode.h"
}

#define IDC_LCAM     1001
#define IDC_GCODE    1002
#define IDC_CANVAS   1003
#define IDC_PROGRAM  1004
#define IDC_NEW      1101
#define IDC_OPEN     1102
#define IDC_SAVE     1103
#define IDC_GEN      1104
#define IDC_RUN      1105
#define IDC_PAUSE    1106
#define IDC_STOP     1107
#define IDC_STATUS   1108
#define IDC_CONNECT  1109
#define IDC_COMPORT  1110
#define IDC_GRBL     1111
#define IDC_SAVE_NC  1112
#define IDC_TAB_LCAM 1113
#define IDC_TAB_GCODE 1114
#define RUN_TIMER       1
#define UCNC_TIMER      2
#define RUN_TIMER_MS   24
#define UCNC_TIMER_MS  40

static COLORREF ra565_color(unsigned short c)
{
    unsigned r = (unsigned)((c >> 11) & 0x1f);
    unsigned g = (unsigned)((c >> 5) & 0x3f);
    unsigned b = (unsigned)(c & 0x1f);
    return RGB((r * 255u) / 31u, (g * 255u) / 63u, (b * 255u) / 31u);
}

static const COLORREF RA_WIN_BLACK      = ra565_color(0x0000);
static const COLORREF RA_WIN_WHITE      = ra565_color(0xFFFF);
static const COLORREF RA_WIN_RED        = ra565_color(0xF800);
static const COLORREF RA_WIN_BLUE       = ra565_color(0x001F);
static const COLORREF RA_WIN_YELLOW     = ra565_color(0xFFE0);
static const COLORREF RA_WIN_CYAN       = ra565_color(0x07FF);
static const COLORREF RA_WIN_GRAY       = ra565_color(0x8410);
static const COLORREF RA_WIN_DGRAY      = ra565_color(0x4208);
static const COLORREF RA_WIN_LGRAY      = ra565_color(0xC618);
static const COLORREF RA_WIN_AMBER_SOFT = ra565_color(0xFCA0);

static const COLORREF UI_WIN_BG     = RA_WIN_BLACK;
static const COLORREF UI_WIN_TOP    = RA_WIN_BLUE;
static const COLORREF UI_WIN_BOTTOM = RA_WIN_DGRAY;
static const COLORREF UI_WIN_FRAME  = RA_WIN_GRAY;
static const COLORREF UI_WIN_TEXT   = RA_WIN_WHITE;
static const COLORREF UI_WIN_WARN   = RA_WIN_YELLOW;
static const COLORREF UI_WIN_VALUE  = RA_WIN_AMBER_SOFT;
static const COLORREF SIM_WIN_STOCK = RA_WIN_GRAY;
static const COLORREF SIM_WIN_AXIS  = RA_WIN_WHITE;
static const COLORREF SIM_WIN_DIM   = RA_WIN_CYAN;
static const COLORREF SIM_WIN_HI    = RA_WIN_YELLOW;
static const COLORREF SIM_WIN_CHUCK = RA_WIN_LGRAY;
static const COLORREF SIM_WIN_TOOL  = RA_WIN_RED;

struct FieldMap {
    std::vector<std::pair<std::string, std::string>> values;
};

struct Setup {
    double length = 100.0;
    double od = 50.0;
    double id = 0.0;
    double clamp = 0.0;
    double extra = 0.0;
};

struct GMove {
    double x = NAN;
    double z = NAN;
    double feed = 0.0;
    bool rapid = false;
    bool motion = false;
};

struct SimRun {
    std::vector<GMove> moves;
    size_t index = 0;
    double startX = 50.0;
    double startZ = 5.0;
    double x = 50.0;
    double z = 5.0;
    double feed = 0.0;
    double t = 0.0;
    bool running = false;
    bool paused = false;
    bool realUcnc = false;
};

struct UcncBridge {
    HANDLE port = INVALID_HANDLE_VALUE;
    bool connected = false;
    bool running = false;
    bool waitingOk = false;
    bool programDone = false;
    DWORD lastStatusMs = 0;
    std::string rx;
    std::vector<std::string> txLines;
    size_t txIndex = 0;
    std::string state = "Disconnected";
};

static HINSTANCE g_inst;
static HWND g_main;
static HWND g_lcam;
static HWND g_gcode;
static HWND g_canvas;
static HWND g_program;
static HWND g_status;
static HWND g_comport;
static HWND g_grbl;
static HWND g_tab_lcam;
static HWND g_tab_gcode;
static std::string g_path;
static std::string g_error;
static std::vector<GMove> g_generated_moves;
static Setup g_setup;
static SimRun g_run;
static UcncBridge g_ucnc;
static int g_selected_line = 0;
static int g_program_scroll = 0;
static int g_code_tab = 0;
static bool g_edit_active = false;
static int g_edit_line = -1;
static int g_edit_field = 0;
static std::string g_edit_draft;
static std::string g_edit_input;

struct FieldSpan;

static std::string edit_display_line();
static std::string current_preview_line();
static void commit_line_edit();
static void draw_text(HDC dc, int x, int y, COLORREF c, const char *fmt, ...);
static std::string effective_edit_input(const FieldSpan &field);
static std::string trim(const std::string &s);
static std::string get_text(HWND hwnd);

static const char *kStarter =
    "SETUP|L{100}|OD{50}|ID{0}|CLAMP{12}|EXTRA{5}|CLR{1}|MAT{ST45}|WOFF{G54}\r\n"
    "TOOL|T{1}|D{6}|S{800}|R_FEED{120}|FIN_FEED{60}|R_DOC{2.0}|FIN_DOC{0.5}\r\n"
    "FACE|D{50}|Z1{1}|Z{0}|DOC{1.0}|CLR{1}\r\n"
    "OD|D1{50}|Z1{0}|Z2{-80}|D2{42}|CLR{1}\r\n"
    "GROOVE|D1{42}|D2{34}|Z1{-30}|Z2{-36}|WIDTH{3}|CLR{1}\r\n";

static const char *kCycleTemplates[] = {
    "TOOL|T{1}|D{6}|S{800}|R_FEED{120}|FIN_FEED{60}|R_DOC{2.0}|FIN_DOC{0.5}",
    "OD|D1{50}|Z1{0}|Z2{-50}|D2{42}|CLR{1}",
    "ID|D1{10}|Z1{0}|Z2{-50}|D2{20}|CLR{1}",
    "FACE|D{50}|Z1{1}|Z{0}|DOC{1.0}|CLR{1}",
    "DRILL|Z1{0}|DEPTH{-30}|PECK{0}|FEED{120}|S{800}",
    "TAP|Z1{0}|DEPTH{-20}|PITCH{1.0}|RPM{300}",
    "CUT|D{0}|Z{-50}|WIDTH{3}|DOC{1.0}|CLR{1}",
    "CHAMFER|D{42}|Z{-10}|SIZE{1.0}|CLR{1}",
    "THREAD_OD|D{42}|Z2{-50}|PITCH{1.5}|THR_DEPTH{0.6}|Z1{0}|CLR{1}",
    "THREAD_ID|D{20}|Z2{-50}|PITCH{1.5}|THR_DEPTH{0.6}|Z1{0}|CLR{1}",
};

static std::string get_text(HWND hwnd)
{
    int len = GetWindowTextLengthA(hwnd);
    std::vector<char> buf((size_t)len + 1u, '\0');
    if (len > 0)
        GetWindowTextA(hwnd, buf.data(), len + 1);
    std::string text(buf.data(), (size_t)len);
    return text;
}

static void set_text(HWND hwnd, const std::string &text)
{
    SetWindowTextA(hwnd, text.c_str());
}

static void set_status(const std::string &text)
{
    SetWindowTextA(g_status, text.c_str());
}

static std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static bool grbl_mode_enabled()
{
    return g_grbl && SendMessageA(g_grbl, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static std::string grbl_sender_gcode(const std::string &gcode)
{
    std::string out;
    for (std::string line : split_lines(gcode)) {
        std::string t = trim(line);

        if (t == "G7" || t == "G8")
            continue;

        out += line;
        out += "\r\n";
    }
    return out;
}

static std::string sender_gcode()
{
    std::string gcode = get_text(g_gcode);
    return grbl_mode_enabled() ? grbl_sender_gcode(gcode) : gcode;
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    return s.substr(a, b - a + 1);
}

static std::string module_name(const std::string &line)
{
    size_t p = line.find('|');
    return trim(p == std::string::npos ? line : line.substr(0, p));
}

static bool is_cycle_line(const std::string &line)
{
    std::string m = module_name(line);
    return m == "OD" || m == "ID" || m == "FACE" || m == "DRILL" ||
           m == "CUT" || m == "PART" || m == "GROOVE";
}

struct TableLine {
    std::string header;
    std::string value;
    bool tableLike = false;
    bool hasHi = false;
    int hiStart = 0;
    int hiEnd = 0;
};

struct FieldSpan {
    size_t nameStart = 0;
    size_t open = 0;
    size_t close = 0;
    std::string name;
    std::string value;
};

static int trimmed_len(const std::string &s)
{
    int n = (int)s.size();
    while (n > 0 && s[(size_t)n - 1u] == ' ')
        --n;
    return n;
}

static int column_width(const std::string &name, const std::string &value)
{
    int width = std::max(trimmed_len(name), trimmed_len(value));
    if (width < 2)
        width = 2;
    return width + 2;
}

static void append_padded(std::string &dst, const std::string &txt, int width)
{
    dst.append(txt);
    if ((int)txt.size() < width)
        dst.append((size_t)(width - (int)txt.size()), ' ');
}

static std::vector<FieldSpan> parse_field_spans(const std::string &line)
{
    std::vector<FieldSpan> out;
    size_t bar = line.find('|');
    if (bar == std::string::npos)
        return out;

    size_t p = bar + 1;
    while (p < line.size()) {
        size_t next = line.find('|', p);
        if (next == std::string::npos)
            next = line.size();
        size_t open = line.find('{', p);
        if (open == std::string::npos || open > next)
            break;
        size_t close = line.find('}', open + 1);
        if (close == std::string::npos || close > next)
            break;

        FieldSpan f;
        f.nameStart = p;
        f.open = open;
        f.close = close;
        f.name = trim(line.substr(p, open - p));
        f.value = line.substr(open + 1, close - open - 1);
        out.push_back(f);

        p = next + 1;
    }

    return out;
}

static TableLine build_table_line(const std::string &line, int activeField = -1)
{
    TableLine out;
    size_t bar = line.find('|');
    if (bar == std::string::npos) {
        out.header = line;
        return out;
    }

    append_padded(out.header, line.substr(0, bar), 10);
    append_padded(out.value, "", 10);
    out.header.push_back(' ');
    out.value.push_back(' ');

    size_t p = bar + 1;
    int field = 0;
    while (p < line.size() && out.header.size() < 126) {
        size_t next = line.find('|', p);
        if (next == std::string::npos)
            next = line.size();
        if (next <= p)
            break;

        size_t open = line.find('{', p);
        if (open == std::string::npos || open > next)
            open = next;
        size_t close = open < next ? line.find('}', open + 1) : std::string::npos;
        if (close == std::string::npos || close > next)
            close = next;

        std::string name = trim(line.substr(p, open - p));
        std::string value;
        if (open < next && close > open)
            value = line.substr(open + 1, close - open - 1);

        int width = column_width(name, value);
        if ((int)out.header.size() + width + 3 >= 128)
            width = 124 - (int)out.header.size();
        if (width <= 0)
            break;

        int colStart = (int)out.value.size();
        append_padded(out.header, name, width);
        append_padded(out.value, value, width);
        if (field == activeField) {
            out.hasHi = true;
            out.hiStart = colStart;
            out.hiEnd = (int)out.value.size();
        }
        out.header.push_back(' ');
        out.value.push_back(' ');

        p = next + 1;
        ++field;
    }

    out.tableLike = true;
    return out;
}

static FieldMap parse_fields(const std::string &line)
{
    FieldMap out;
    size_t p = 0;
    while ((p = line.find('|', p)) != std::string::npos) {
        size_t name_start = p + 1;
        size_t open = line.find('{', name_start);
        size_t close = open == std::string::npos ? std::string::npos : line.find('}', open + 1);
        if (open == std::string::npos || close == std::string::npos)
            break;
        out.values.push_back({line.substr(name_start, open - name_start),
                              line.substr(open + 1, close - open - 1)});
        p = close + 1;
    }
    return out;
}

static bool field_text(const FieldMap &fields, const char *a, const char *b, std::string &out)
{
    for (const auto &kv : fields.values) {
        if (kv.first == a || (b && kv.first == b)) {
            out = kv.second;
            return true;
        }
    }
    return false;
}

static bool field_num(const FieldMap &fields, const char *a, const char *b, double &out)
{
    std::string s;
    char *endp = nullptr;
    if (!field_text(fields, a, b, s))
        return false;
    s = trim(s);
    if (!s.empty() && s.front() == '(' && s.back() == ')')
        s = s.substr(1, s.size() - 2);
    if (s.empty() || s == "*")
        return false;
    double v = std::strtod(s.c_str(), &endp);
    if (endp == s.c_str())
        return false;
    out = v;
    return true;
}

static Setup setup_from_text(const std::string &text)
{
    Setup s;
    for (const std::string &raw : split_lines(text)) {
        std::string line = trim(raw);
        if (module_name(line) != "SETUP")
            continue;
        FieldMap f = parse_fields(line);
        field_num(f, "L", "LENGTH", s.length);
        field_num(f, "OD", "OUTER_DIAMETER", s.od);
        field_num(f, "ID", "INNER_DIAMETER", s.id);
        field_num(f, "CLAMP", "CLAMP_LENGTH", s.clamp);
        field_num(f, "EXTRA", "EXTRA_LENGTH", s.extra);
    }
    if (s.length <= 0.0) s.length = 100.0;
    if (s.od <= 0.0) s.od = 50.0;
    if (s.id < 0.0) s.id = 0.0;
    if (s.clamp < 0.0) s.clamp = 0.0;
    if (s.extra < 0.0) s.extra = 0.0;
    return s;
}

static int emit_line(const char *line, void *user)
{
    std::string *out = (std::string *)user;
    if (!line || !out)
        return 0;
    out->append(line);
    out->append("\r\n");
    return 1;
}

static bool generate_gcode()
{
    std::string lcam = get_text(g_lcam);
    std::vector<std::string> lines = split_lines(lcam);
    std::string setup;
    std::string tool;
    std::string out;
    char err[160];
    int line_no = 0;

    g_error.clear();
    g_setup = setup_from_text(lcam);

    if (!leancam_gcode_emit_program_header(emit_line, &out)) {
        g_error = "Could not emit program header";
        return false;
    }

    for (const std::string &raw : lines) {
        ++line_no;
        std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (module_name(line) == "SETUP") {
            setup = line;
            continue;
        }
        if (module_name(line) == "TOOL") {
            tool = line;
            continue;
        }
        if (!is_cycle_line(line))
            continue;

        err[0] = 0;
        lc_gcode_result_t r = leancam_gcode_run_program_line_ex(line.c_str(),
                                                                 setup.empty() ? nullptr : setup.c_str(),
                                                                 tool.empty() ? nullptr : tool.c_str(),
                                                                 emit_line,
                                                                 &out,
                                                                 err,
                                                                 sizeof(err));
        if (r != LC_GCODE_OK) {
            char buf[240];
            std::snprintf(buf, sizeof(buf), "Line %d: %s", line_no, err[0] ? err : "LeanCam generation failed");
            g_error = buf;
            return false;
        }
    }

    if (!leancam_gcode_emit_program_footer(emit_line, &out)) {
        g_error = "Could not emit program footer";
        return false;
    }

    set_text(g_gcode, out);
    set_status("Generated G-code from LeanCam source");
    return true;
}

static bool read_file(const char *path, std::string &out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.assign((size_t)std::max<long>(n, 0), '\0');
    if (n > 0)
        std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    return true;
}

static bool write_file(const char *path, const std::string &text)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return true;
}

static std::string serial_path_from_name(std::string name)
{
    name = trim(name);
    if (name.empty())
        name = "COM14";
    if (name.rfind("\\\\.\\", 0) == 0)
        return name;
    return "\\\\.\\" + name;
}

static void ucnc_close()
{
    if (g_ucnc.port != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ucnc.port);
        g_ucnc.port = INVALID_HANDLE_VALUE;
    }
    g_ucnc.connected = false;
    g_ucnc.running = false;
    g_ucnc.waitingOk = false;
    g_ucnc.programDone = false;
    g_ucnc.state = "Disconnected";
    KillTimer(g_main, UCNC_TIMER);
}

static bool ucnc_write_raw(const char *text)
{
    DWORD written = 0;
    if (!g_ucnc.connected || g_ucnc.port == INVALID_HANDLE_VALUE || !text)
        return false;
    return WriteFile(g_ucnc.port, text, (DWORD)std::strlen(text), &written, nullptr) && written == std::strlen(text);
}

static bool ucnc_connect(const std::string &portName)
{
    ucnc_close();
    std::string path = serial_path_from_name(portName);
    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g_ucnc.state = "Could not open " + path;
        set_status(g_ucnc.state);
        return false;
    }

    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        g_ucnc.state = "Could not read COM settings";
        set_status(g_ucnc.state);
        return false;
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        g_ucnc.state = "Could not configure COM port";
        set_status(g_ucnc.state);
        return false;
    }

    COMMTIMEOUTS timeouts;
    ZeroMemory(&timeouts, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(h, &timeouts);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    g_ucnc.port = h;
    g_ucnc.connected = true;
    g_ucnc.running = false;
    g_ucnc.waitingOk = false;
    g_ucnc.programDone = false;
    g_ucnc.rx.clear();
    g_ucnc.state = "Connected " + path;
    set_status(g_ucnc.state + " - real uCNC parser/planner mode");
    SetTimer(g_main, UCNC_TIMER, UCNC_TIMER_MS, nullptr);
    ucnc_write_raw("\r\n");
    return true;
}

static bool parse_status_axis(const std::string &line, const char *tag, double &x, double &z)
{
    size_t p = line.find(tag);
    if (p == std::string::npos)
        return false;
    p += std::strlen(tag);
    char *endp = nullptr;
    double a0 = std::strtod(line.c_str() + p, &endp);
    if (endp == line.c_str() + p || *endp != ',')
        return false;
    double a1 = std::strtod(endp + 1, &endp);
    (void)a1;
    if (*endp != ',')
        return false;
    double a2 = std::strtod(endp + 1, &endp);
    x = a0;
    z = a2;
    return true;
}

static void parse_ucnc_status_line(const std::string &line)
{
    if (line.empty())
        return;

    if (line == "ok") {
        g_ucnc.waitingOk = false;
        return;
    }
    if (line.rfind("error:", 0) == 0 || line.rfind("ALARM:", 0) == 0) {
        g_ucnc.waitingOk = false;
        g_ucnc.running = false;
        g_run.running = false;
        g_error = line;
        set_status("uCNC: " + line);
        return;
    }
    if (line.front() == '<') {
        size_t bar = line.find('|');
        if (bar != std::string::npos)
            g_ucnc.state = line.substr(1, bar - 1);

        double x = 0.0;
        double z = 0.0;
        if (parse_status_axis(line, "WPos:", x, z) || parse_status_axis(line, "MPos:", x, z)) {
            g_run.x = x;
            g_run.z = z;
        }

        size_t fs = line.find("|FS:");
        if (fs != std::string::npos) {
            fs += 4;
            char *endp = nullptr;
            g_run.feed = std::strtod(line.c_str() + fs, &endp);
        }

        if (g_ucnc.programDone && g_ucnc.state.find("Idle") != std::string::npos) {
            g_ucnc.running = false;
            g_run.running = false;
            set_status("uCNC virtual run complete");
        }
        return;
    }
}

static void ucnc_send_next_line()
{
    while (g_ucnc.running && !g_ucnc.waitingOk && g_ucnc.txIndex < g_ucnc.txLines.size()) {
        std::string line = trim(g_ucnc.txLines[g_ucnc.txIndex++]);
        if (line.empty())
            continue;
        line.push_back('\n');
        if (!ucnc_write_raw(line.c_str())) {
            g_error = "uCNC COM write failed";
            set_status(g_error);
            g_ucnc.running = false;
            g_run.running = false;
            return;
        }
        g_ucnc.waitingOk = true;
        return;
    }

    if (g_ucnc.running && g_ucnc.txIndex >= g_ucnc.txLines.size())
        g_ucnc.programDone = true;
}

static void ucnc_poll()
{
    if (!g_ucnc.connected || g_ucnc.port == INVALID_HANDLE_VALUE)
        return;

    char buf[256];
    DWORD got = 0;
    while (ReadFile(g_ucnc.port, buf, sizeof(buf), &got, nullptr) && got > 0) {
        g_ucnc.rx.append(buf, buf + got);
        size_t eol;
        while ((eol = g_ucnc.rx.find_first_of("\r\n")) != std::string::npos) {
            std::string line = trim(g_ucnc.rx.substr(0, eol));
            g_ucnc.rx.erase(0, eol + 1);
            if (!line.empty())
                parse_ucnc_status_line(line);
        }
    }

    DWORD now = GetTickCount();
    if (now - g_ucnc.lastStatusMs >= 120) {
        g_ucnc.lastStatusMs = now;
        ucnc_write_raw("?");
    }

    ucnc_send_next_line();
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static bool choose_file(bool save, char *path, DWORD path_len)
{
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFile = path;
    ofn.nMaxFile = path_len;
    ofn.lpstrFilter = "LeanCam (*.lcam)\0*.lcam\0All files\0*.*\0";
    ofn.lpstrDefExt = "lcam";
    ofn.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
}

static bool choose_nc_file(char *path, DWORD path_len)
{
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFile = path;
    ofn.nMaxFile = path_len;
    ofn.lpstrFilter = "G-code (*.nc;*.gcode)\0*.nc;*.gcode\0All files\0*.*\0";
    ofn.lpstrDefExt = "nc";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    return GetSaveFileNameA(&ofn);
}

static void save_generated_nc()
{
    if (!generate_gcode())
    {
        set_status(g_error);
        return;
    }

    char path[MAX_PATH] = "";
    if (!g_path.empty())
    {
        std::strncpy(path, g_path.c_str(), sizeof(path) - 1);
        char *dot = std::strrchr(path, '.');
        if (dot)
            std::strcpy(dot, ".nc");
        else
            std::strcat(path, ".nc");
    }

    if (!choose_nc_file(path, MAX_PATH))
        return;

    std::string text = sender_gcode();
    if (write_file(path, text))
        set_status(std::string(grbl_mode_enabled() ? "Saved Grbl-filtered NC " : "Saved uCNC NC ") + path);
    else
        set_status("Save NC failed");
}

static double word_value(const std::string &line, char word, bool &found)
{
    found = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if ((char)std::toupper((unsigned char)line[i]) != word)
            continue;
        char *endp = nullptr;
        double v = std::strtod(line.c_str() + i + 1, &endp);
        if (endp != line.c_str() + i + 1) {
            found = true;
            return v;
        }
    }
    return 0.0;
}

static std::vector<GMove> parse_gcode_moves(const std::string &gcode)
{
    std::vector<GMove> moves;
    double curX = g_setup.od;
    double curZ = 5.0;
    double feed = 0.0;
    for (std::string line : split_lines(gcode)) {
        size_t comment = line.find('(');
        if (comment != std::string::npos)
            line = line.substr(0, comment);
        line = trim(line);
        if (line.empty())
            continue;

        bool hasG = false, hasX = false, hasZ = false, hasF = false;
        double g = word_value(line, 'G', hasG);
        double x = word_value(line, 'X', hasX);
        double z = word_value(line, 'Z', hasZ);
        double f = word_value(line, 'F', hasF);
        if (hasF)
            feed = f;
        if (hasG && (std::fabs(g) < 0.01 || std::fabs(g - 1.0) < 0.01)) {
            if (hasX) curX = x;
            if (hasZ) curZ = z;
            GMove m;
            m.x = curX;
            m.z = curZ;
            m.feed = feed;
            m.rapid = std::fabs(g) < 0.01;
            m.motion = true;
            moves.push_back(m);
        }
    }
    return moves;
}

struct View {
    RECT rc;
    int stockLeft;
    int stockRight;
    int stockTop;
    int stockBottom;
    int z0x;
    double scale;
};

static View make_view(RECT rc)
{
    View v;
    v.rc = rc;
    int w = std::max(10, (int)(rc.right - rc.left));
    int h = std::max(10, (int)(rc.bottom - rc.top));
    int leftPad = 54;
    int rightPad = 42;
    int topPad = 48;
    int bottomPad = 58;
    double visibleLen = std::max(1.0, g_setup.length + g_setup.extra);
    double sx = (double)(w - leftPad - rightPad) / visibleLen;
    double sy = (double)(h - topPad - bottomPad - 28) / g_setup.od;
    v.scale = std::max(1.0, std::min(sx, sy));
    int stockW = (int)(visibleLen * v.scale + 0.5);
    int stockH = (int)(g_setup.od * v.scale + 0.5);
    v.stockLeft = rc.left + leftPad;
    v.stockTop = rc.top + topPad;
    v.stockRight = std::min((int)rc.right - rightPad, v.stockLeft + stockW);
    v.stockBottom = std::min((int)rc.bottom - bottomPad, v.stockTop + stockH);
    v.z0x = v.stockLeft + (int)(g_setup.length * v.scale + 0.5);
    return v;
}

static int z_px(const View &v, double z)
{
    int x = v.z0x + (int)(z * v.scale + (z >= 0 ? 0.5 : -0.5));
    return std::clamp(x, v.stockLeft, v.stockRight);
}

static int d_py(const View &v, double d)
{
    int y = v.stockTop + (int)(d * v.scale + 0.5);
    return std::clamp(y, v.stockTop, v.stockBottom);
}

static void draw_corner_label(HDC dc, const View &v, int x, int y, const std::string &label, int dx, int dy)
{
    if (label.empty())
        return;

    int tx = std::clamp(x + dx, (int)v.rc.left + 2, (int)v.rc.right - 86);
    int ty = std::clamp(y + dy, (int)v.rc.top + 2, (int)v.rc.bottom - 18);
    draw_text(dc, tx, ty, SIM_WIN_DIM, "%s", label.c_str());
}

static std::string fmt_label(const char *name, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %.2f", name ? name : "", v);
    return buf;
}

static void draw_dz_corner_labels(HDC dc,
                                  const View &v,
                                  int startX,
                                  int startY,
                                  const std::string &z1,
                                  const std::string &d1,
                                  int endX,
                                  int endY,
                                  const std::string &z2,
                                  const std::string &d2)
{
    draw_corner_label(dc, v, startX, startY, z1, -48, -30);
    draw_corner_label(dc, v, startX, startY, d1, -48, -2);
    draw_corner_label(dc, v, endX, endY, z2, -48, -30);
    draw_corner_label(dc, v, endX, endY, d2, -48, -2);
}

static void fill_rect(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    RECT rc = {l, t, r, b};
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void draw_text(HDC dc, int x, int y, COLORREF c, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, buf, (int)std::strlen(buf));
}

static void draw_text_plain(HDC dc, int x, int y, COLORREF fg, COLORREF bg, const std::string &s)
{
    SetBkMode(dc, bg == CLR_INVALID ? TRANSPARENT : OPAQUE);
    SetTextColor(dc, fg);
    if (bg != CLR_INVALID)
        SetBkColor(dc, bg);
    TextOutA(dc, x, y, s.c_str(), (int)s.size());
}

static void draw_value_line(HDC dc, int x, int y, const TableLine &tl)
{
    const int charW = 8;
    COLORREF value = UI_WIN_VALUE;
    COLORREF warn = UI_WIN_WARN;
    COLORREF bg = UI_WIN_BG;

    if (!tl.hasHi || tl.hiEnd <= tl.hiStart) {
        draw_text_plain(dc, x, y, value, CLR_INVALID, tl.value);
        return;
    }

    int a = std::clamp(tl.hiStart, 0, (int)tl.value.size());
    int b = std::clamp(tl.hiEnd, a, (int)tl.value.size());
    draw_text_plain(dc, x, y, value, CLR_INVALID, tl.value.substr(0, (size_t)a));
    draw_text_plain(dc, x + a * charW, y, bg, warn, tl.value.substr((size_t)a, (size_t)(b - a)));
    draw_text_plain(dc, x + b * charW, y, value, CLR_INVALID, tl.value.substr((size_t)b));
}

static int draw_table_line(HDC dc, int x, int y, int rowH, int right, const std::string &line, bool selected, int activeField)
{
    TableLine tl = build_table_line(line, activeField);
    COLORREF headerColor = selected ? UI_WIN_WARN : UI_WIN_TEXT;

    if (tl.tableLike) {
        if (selected)
            fill_rect(dc, 2, y - 2, right - 2, y + rowH * 2 - 4, UI_WIN_BOTTOM);
        draw_text(dc, x, y, headerColor, "%s", tl.header.c_str());
        draw_value_line(dc, x, y + rowH, tl);
        return 2;
    }

    if (selected)
        fill_rect(dc, 2, y - 2, right - 2, y + rowH - 4, UI_WIN_BOTTOM);
    draw_text(dc, x, y, headerColor, "%s", line.c_str());
    return 1;
}

static void draw_program(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    fill_rect(dc, rc.left, rc.top, rc.right, rc.bottom, UI_WIN_BG);

    HFONT font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    std::vector<std::string> lines = split_lines(get_text(g_lcam));

    int y = 8;
    int rowH = 26;
    int physical = 0;
    int maxPhysical = std::max(1, (int)(rc.bottom - rc.top - 116) / rowH);
    int stickySetup = -1;
    int stickyTool = -1;

    draw_text(dc, 8, y, UI_WIN_TEXT, "LeanCam");
    draw_text(dc, 104, y, UI_WIN_FRAME, "labels");
    draw_text(dc, 104, y + rowH, UI_WIN_FRAME, "values");
    y += rowH * 2;

    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string line = trim(lines[(size_t)i]);
        if (module_name(line) == "SETUP")
            stickySetup = i;
        if (i <= g_selected_line && module_name(line) == "TOOL")
            stickyTool = i;
    }

    if (stickySetup >= 0) {
        std::string line = trim(lines[(size_t)stickySetup]);
        y += draw_table_line(dc, 8, y, rowH, rc.right, line, stickySetup == g_selected_line, -1) * rowH;
    }
    if (stickyTool >= 0 && stickyTool != stickySetup) {
        std::string line = trim(lines[(size_t)stickyTool]);
        y += draw_table_line(dc, 8, y, rowH, rc.right, line, stickyTool == g_selected_line, -1) * rowH;
    }

    HPEN sep = CreatePen(PS_SOLID, 1, UI_WIN_FRAME);
    HPEN oldPen = (HPEN)SelectObject(dc, sep);
    MoveToEx(dc, 4, y + 2, nullptr);
    LineTo(dc, rc.right - 4, y + 2);
    SelectObject(dc, oldPen);
    DeleteObject(sep);
    y += 8;

    int first = std::clamp(g_program_scroll, 0, std::max(0, (int)lines.size() - 1));
    for (int i = first; i < (int)lines.size() && physical < maxPhysical; ++i) {
        if (i == stickySetup || i == stickyTool)
            continue;
        std::string line = trim(lines[(size_t)i]);
        if (line.empty())
            continue;

        bool editingThis = g_edit_active && i == g_edit_line;
        if (editingThis)
            line = edit_display_line();

        bool selected = (i == g_selected_line);
        int used = draw_table_line(dc, 8, y, rowH, rc.right, line, selected, editingThis ? g_edit_field : -1);
        if (physical + used > maxPhysical)
            break;
        y += used * rowH;
        physical += used;
    }

    int helperY = rc.bottom - 48;
    if (g_edit_active) {
        std::vector<FieldSpan> fields = parse_field_spans(g_edit_draft);
        const char *name = (g_edit_field >= 0 && g_edit_field < (int)fields.size()) ? fields[(size_t)g_edit_field].name.c_str() : "";
        int fieldCount = (int)fields.size();
        draw_text(dc,
                  8,
                  helperY,
                  UI_WIN_TEXT,
                  "F%d/%d %s  input='%s'",
                  std::min(g_edit_field + 1, fieldCount),
                  fieldCount,
                  name,
                  g_edit_input.c_str());
        draw_text(dc,
                  8,
                  helperY + 22,
                  UI_WIN_TEXT,
                  "Enter accept | last Enter commits | End commit | Backspace | Esc cancel");
    } else {
        draw_text(dc,
                  8,
                  helperY,
                  UI_WIN_TEXT,
                  "0 tool | 1 OD | 2 ID | 3 FACE | 4 DRILL | 6 CUT/PART | 7/8/9 draft only");
        draw_text(dc,
                  8,
                  helperY + 22,
                  UI_WIN_TEXT,
                  "Up/Down move | Enter edit | Delete line | End run/send | Save NC exports");
    }
    SelectObject(dc, oldFont);
}

static void draw_cycle_preview(HDC dc, const View &v, const std::string &line)
{
    if (line.empty())
        return;
    std::string m = module_name(line);
    if (!is_cycle_line(line))
        return;

    FieldMap f = parse_fields(line);
    double d1 = 0, d2 = 0, z1 = 0, z2 = 0, d = g_setup.od, z = 0, width = 0;
    HBRUSH br = CreateSolidBrush(SIM_WIN_HI);
    HBRUSH old = (HBRUSH)SelectObject(dc, br);

    if ((m == "OD" || m == "ID") &&
        field_num(f, "D1", "DIAMETER_1", d1) &&
        field_num(f, "D2", "DIAMETER_2", d2) &&
        field_num(f, "Z1", "Z_1", z1) &&
        field_num(f, "Z2", "Z_2", z2)) {
        int x1 = z_px(v, z1);
        int x2 = z_px(v, z2);
        int y1 = d_py(v, d1);
        int y2 = d_py(v, d2);
        int l = std::min(x1, x2);
        int r = std::max(x1, x2);
        int t = std::min(y1, y2);
        int b = std::max(y1, y2);
        Rectangle(dc, l, t, std::max(l + 2, r), std::max(t + 2, b));
        draw_dz_corner_labels(dc,
                              v,
                              x1,
                              y1,
                              fmt_label("Z1", z1),
                              fmt_label("D1", d1),
                              x2,
                              y2,
                              fmt_label("Z2", z2),
                              fmt_label("D2", d2));
    } else if (m == "FACE" && field_num(f, "Z", "Z_2", z)) {
        field_num(f, "D", "OD", d);
        field_num(f, "Z1", "Z_1", z1);
        int x1 = z_px(v, z1);
        int x2 = z_px(v, z);
        int y0 = d_py(v, 0.0);
        int yd = d_py(v, d);
        int l = std::min(x1, x2);
        int r = std::max(x1, x2);
        Rectangle(dc, l, v.stockTop, std::max(l + 2, r), d_py(v, d));
        draw_dz_corner_labels(dc,
                              v,
                              x1,
                              yd,
                              fmt_label("Z1", z1),
                              fmt_label("D", d),
                              x2,
                              y0,
                              fmt_label("Z", z),
                              "");
    } else if (m == "DRILL" && field_num(f, "Z1", "Z_START", z1) && field_num(f, "DEPTH", nullptr, z2)) {
        double target = z2 <= 0.0 ? z2 : z1 - z2;
        int l = std::min(z_px(v, z1), z_px(v, target));
        int r = std::max(z_px(v, z1), z_px(v, target));
        int yTool = d_py(v, g_setup.od * 0.12);
        Rectangle(dc, l, v.stockTop, std::max(l + 2, r), yTool);
        draw_corner_label(dc, v, z_px(v, z1), yTool, fmt_label("Z1", z1), 6, 4);
        draw_corner_label(dc, v, z_px(v, target), yTool, fmt_label("Z", target), -54, 4);
    } else if ((m == "CUT" || m == "PART") &&
               field_num(f, "D", "DIAMETER", d) &&
               field_num(f, "Z", nullptr, z) &&
               field_num(f, "WIDTH", nullptr, width)) {
        int x = z_px(v, z);
        Rectangle(dc, x, d_py(v, d), std::max(x + 2, z_px(v, z - width)), v.stockBottom);
        draw_corner_label(dc, v, x, d_py(v, d), fmt_label("Z", z), -48, -30);
        draw_corner_label(dc, v, x, d_py(v, d), fmt_label("D", d), -48, -2);
    } else if (m == "GROOVE" &&
               field_num(f, "D1", nullptr, d1) &&
               field_num(f, "D2", nullptr, d2) &&
               field_num(f, "Z1", nullptr, z1) &&
               field_num(f, "Z2", nullptr, z2)) {
        int x1 = z_px(v, z1);
        int x2 = z_px(v, z2);
        int y1 = d_py(v, d1);
        int y2 = d_py(v, d2);
        Rectangle(dc, std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
        draw_corner_label(dc, v, x1, y1, fmt_label("D1", d1), -54, -18);
        draw_corner_label(dc, v, x2, y2, fmt_label("D2", d2), 6, 4);
    }

    SelectObject(dc, old);
    DeleteObject(br);
}

static std::string first_preview_cycle()
{
    return current_preview_line();
}

static void draw_canvas(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    fill_rect(dc, rc.left, rc.top, rc.right, rc.bottom, UI_WIN_BG);
    g_setup = setup_from_text(get_text(g_lcam));
    View v = make_view(rc);

    fill_rect(dc, v.stockLeft, v.stockTop, v.stockRight, v.stockBottom, SIM_WIN_STOCK);
    HPEN axis = CreatePen(PS_SOLID, 1, SIM_WIN_AXIS);
    HPEN oldPen = (HPEN)SelectObject(dc, axis);
    MoveToEx(dc, v.stockLeft - 22, v.stockTop, nullptr);
    LineTo(dc, v.stockRight + 22, v.stockTop);
    MoveToEx(dc, v.z0x, v.stockTop - 24, nullptr);
    LineTo(dc, v.z0x, v.stockBottom + 24);
    SelectObject(dc, oldPen);
    DeleteObject(axis);

    if (g_setup.clamp > 0.0) {
        int cw = std::clamp((int)(g_setup.clamp * v.scale + 0.5), 8, 90);
        fill_rect(dc, v.stockLeft, v.stockBottom + 1, v.stockLeft + cw, v.stockBottom + 28, SIM_WIN_CHUCK);
        draw_text(dc, v.stockLeft + 4, v.stockBottom + 7, UI_WIN_BG, "CL %.1f", g_setup.clamp);
    }

    draw_text(dc, v.z0x - 16, v.stockTop - 28, SIM_WIN_DIM, "Z0");
    draw_text(dc, v.stockLeft, v.stockBottom + 34, SIM_WIN_DIM, "L %.1f  OD %.1f  ID %.1f", g_setup.length, g_setup.od, g_setup.id);

    draw_cycle_preview(dc, v, first_preview_cycle());

    std::vector<GMove> moves = g_generated_moves;
    if (moves.empty())
        moves = parse_gcode_moves(get_text(g_gcode));
    if (!moves.empty()) {
        HPEN rapid = CreatePen(PS_DOT, 1, SIM_WIN_DIM);
        HPEN cut = CreatePen(PS_SOLID, 2, SIM_WIN_TOOL);
        double px = g_setup.od;
        double pz = 5.0;
        for (const GMove &m : moves) {
            HPEN use = m.rapid ? rapid : cut;
            SelectObject(dc, use);
            MoveToEx(dc, z_px(v, pz), d_py(v, px), nullptr);
            LineTo(dc, z_px(v, m.z), d_py(v, m.x));
            px = m.x;
            pz = m.z;
        }
        SelectObject(dc, oldPen);
        DeleteObject(rapid);
        DeleteObject(cut);
    }

    double markerX = g_run.running || g_run.paused ? g_run.x : g_setup.od;
    double markerZ = g_run.running || g_run.paused ? g_run.z : 5.0;
    int mx = z_px(v, markerZ);
    int my = d_py(v, markerX);
    HBRUSH marker = CreateSolidBrush(SIM_WIN_TOOL);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, marker);
    Ellipse(dc, mx - 5, my - 5, mx + 6, my + 6);
    SelectObject(dc, oldBrush);
    DeleteObject(marker);

    draw_text(dc, rc.left + 12,
              rc.top + 12,
              UI_WIN_TEXT,
              g_run.realUcnc ? "Preview and real uCNC virtual-core run" : "Preview and local visual fallback run");
    if (g_ucnc.connected)
        draw_text(dc, rc.left + 12, rc.top + 30, SIM_WIN_DIM, "uCNC: %s", g_ucnc.state.c_str());
    if (!g_error.empty())
        draw_text(dc, rc.left + 12, rc.top + 48, UI_WIN_WARN, "%s", g_error.c_str());
}

static void start_run()
{
    if (!generate_gcode())
        return;
    g_generated_moves = parse_gcode_moves(get_text(g_gcode));

    if (g_ucnc.connected) {
        g_run = SimRun();
        g_run.moves = g_generated_moves;
        g_run.x = g_setup.od;
        g_run.z = 5.0;
        g_run.startX = g_run.x;
        g_run.startZ = g_run.z;
        g_run.running = true;
        g_run.realUcnc = true;

        g_ucnc.txLines = split_lines(sender_gcode());
        g_ucnc.txIndex = 0;
        g_ucnc.waitingOk = false;
        g_ucnc.programDone = false;
        g_ucnc.running = true;
        g_ucnc.lastStatusMs = 0;
        g_error.clear();

        set_status(grbl_mode_enabled()
                       ? "Running Grbl sender mode over COM"
                       : "Running through real uCNC virtual COM parser/planner");
        SetTimer(g_main, UCNC_TIMER, UCNC_TIMER_MS, nullptr);
        ucnc_write_raw("$X\n");
        ucnc_write_raw("?");
        InvalidateRect(g_canvas, nullptr, FALSE);
        return;
    }

    g_run = SimRun();
    g_run.moves = g_generated_moves;
    g_run.x = g_setup.od;
    g_run.z = 5.0;
    g_run.startX = g_run.x;
    g_run.startZ = g_run.z;
    g_run.running = !g_run.moves.empty();
    g_run.paused = false;
    SetTimer(g_main, RUN_TIMER, RUN_TIMER_MS, nullptr);
    set_status(g_run.running ? "Running local visual fallback - not real uCNC core" : "No G0/G1 moves to run");
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void stop_run()
{
    g_run.running = false;
    g_run.paused = false;
    g_ucnc.running = false;
    g_ucnc.waitingOk = false;
    g_ucnc.programDone = false;
    KillTimer(g_main, RUN_TIMER);
    if (g_ucnc.connected)
        ucnc_write_raw("!\n");
    set_status("Run stopped");
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void tick_run()
{
    if (!g_run.running || g_run.paused || g_run.index >= g_run.moves.size())
        return;
    const GMove &m = g_run.moves[g_run.index];
    double dx = m.x - g_run.startX;
    double dz = m.z - g_run.startZ;
    double dist = std::sqrt(dx * dx + dz * dz);
    double mm_per_tick = m.rapid ? 3.0 : std::max(0.2, (m.feed > 0.0 ? m.feed : 120.0) / 60.0 * (RUN_TIMER_MS / 1000.0) * 10.0);
    double step = dist <= 0.001 ? 1.0 : mm_per_tick / dist;
    g_run.t += step;
    if (g_run.t >= 1.0) {
        g_run.x = m.x;
        g_run.z = m.z;
        g_run.feed = m.feed;
        ++g_run.index;
        g_run.t = 0.0;
        g_run.startX = g_run.x;
        g_run.startZ = g_run.z;
        if (g_run.index >= g_run.moves.size()) {
            g_run.running = false;
            KillTimer(g_main, RUN_TIMER);
            set_status("Virtual run complete");
        }
    } else {
        g_run.x = g_run.startX + dx * g_run.t;
        g_run.z = g_run.startZ + dz * g_run.t;
    }
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int toolbar = 36;
    int status = 24;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int leftW = std::max(360, w / 2);
    int codePaneH = std::max(140, (h - toolbar - status) / 5);
    int tabH = 28;
    int y = toolbar;
    int codeY = h - status - codePaneH;

    int x = 6;
    int bw = 74;
    HWND buttons[] = {
        GetDlgItem(hwnd, IDC_NEW), GetDlgItem(hwnd, IDC_OPEN), GetDlgItem(hwnd, IDC_SAVE),
        GetDlgItem(hwnd, IDC_GEN), GetDlgItem(hwnd, IDC_SAVE_NC), GetDlgItem(hwnd, IDC_RUN), GetDlgItem(hwnd, IDC_PAUSE),
        GetDlgItem(hwnd, IDC_STOP), GetDlgItem(hwnd, IDC_CONNECT)
    };
    for (HWND b : buttons) {
        MoveWindow(b, x, 6, bw, 24, TRUE);
        x += bw + 6;
    }
    MoveWindow(g_comport, x, 6, 78, 24, TRUE);
    x += 84;
    MoveWindow(g_grbl, x, 8, 58, 20, TRUE);

    MoveWindow(g_program, 6, y, leftW - 10, codeY - y - 6, TRUE);

    MoveWindow(g_tab_lcam, 6, codeY, 112, 24, TRUE);
    MoveWindow(g_tab_gcode, 124, codeY, 112, 24, TRUE);
    MoveWindow(g_lcam, 6, codeY + tabH, leftW - 10, codePaneH - tabH - 4, TRUE);
    MoveWindow(g_gcode, 6, codeY + tabH, leftW - 10, codePaneH - tabH - 4, TRUE);
    ShowWindow(g_lcam, g_code_tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_gcode, g_code_tab == 1 ? SW_SHOW : SW_HIDE);

    MoveWindow(g_canvas, leftW + 2, y, w - leftW - 8, h - toolbar - status - 4, TRUE);
    MoveWindow(g_status, 0, h - status, w, status, TRUE);
}

static void set_code_tab(int tab)
{
    g_code_tab = tab ? 1 : 0;
    if (g_lcam)
        ShowWindow(g_lcam, g_code_tab == 0 ? SW_SHOW : SW_HIDE);
    if (g_gcode)
        ShowWindow(g_gcode, g_code_tab == 1 ? SW_SHOW : SW_HIDE);
    if (g_tab_lcam)
        SendMessageA(g_tab_lcam, BM_SETCHECK, g_code_tab == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_tab_gcode)
        SendMessageA(g_tab_gcode, BM_SETCHECK, g_code_tab == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void set_lcam_lines(const std::vector<std::string> &lines)
{
    std::string text;
    for (const std::string &line : lines) {
        text += line;
        text += "\r\n";
    }
    set_text(g_lcam, text);
    InvalidateRect(g_program, nullptr, FALSE);
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void clamp_selected_line()
{
    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    if (lines.empty()) {
        g_selected_line = 0;
        return;
    }
    g_selected_line = std::clamp(g_selected_line, 0, (int)lines.size() - 1);
}

static void ensure_selected_visible()
{
    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    if (lines.empty()) {
        g_program_scroll = 0;
        return;
    }

    int maxVisibleLines = 8;
    if (g_program) {
        RECT rc;
        GetClientRect(g_program, &rc);
        maxVisibleLines = std::max(3, ((int)(rc.bottom - rc.top) - 170) / 52);
    }

    if (g_selected_line < g_program_scroll)
        g_program_scroll = g_selected_line;
    if (g_selected_line >= g_program_scroll + maxVisibleLines)
        g_program_scroll = g_selected_line - maxVisibleLines + 1;

    g_program_scroll = std::clamp(g_program_scroll, 0, std::max(0, (int)lines.size() - 1));
}

static void move_selected_line(int delta)
{
    if (g_edit_active)
        return;
    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    if (lines.empty())
        return;

    int i = std::clamp(g_selected_line, 0, (int)lines.size() - 1);
    for (;;) {
        i += delta;
        if (i < 0 || i >= (int)lines.size())
            break;
        if (!trim(lines[(size_t)i]).empty()) {
            g_selected_line = i;
            ensure_selected_visible();
            break;
        }
    }

    InvalidateRect(g_program, nullptr, FALSE);
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static bool begin_line_edit()
{
    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    clamp_selected_line();
    if (g_selected_line < 0 || g_selected_line >= (int)lines.size())
        return false;
    std::string line = trim(lines[(size_t)g_selected_line]);
    if (line.empty() || line.find('|') == std::string::npos)
        return false;

    g_edit_active = true;
    g_edit_line = g_selected_line;
    g_edit_field = 0;
    g_edit_draft = line;
    g_edit_input.clear();
    set_status("LeanCam edit: type value, Enter accepts field, End commits");
    InvalidateRect(g_program, nullptr, FALSE);
    InvalidateRect(g_canvas, nullptr, FALSE);
    return true;
}

static void cancel_line_edit()
{
    g_edit_active = false;
    g_edit_line = -1;
    g_edit_field = 0;
    g_edit_draft.clear();
    g_edit_input.clear();
    set_status("Edit canceled");
    InvalidateRect(g_program, nullptr, FALSE);
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void accept_edit_field()
{
    if (!g_edit_active)
        return;
    std::vector<FieldSpan> fields = parse_field_spans(g_edit_draft);
    if (g_edit_field < 0 || g_edit_field >= (int)fields.size())
        return;

    if (!g_edit_input.empty()) {
        FieldSpan f = fields[(size_t)g_edit_field];
        g_edit_draft.replace(f.open + 1, f.close - f.open - 1, effective_edit_input(f));
        g_edit_input.clear();
    }

    fields = parse_field_spans(g_edit_draft);
    if (g_edit_field + 1 < (int)fields.size())
    {
        ++g_edit_field;
        set_status("LeanCam field accepted");
    }
    else
    {
        commit_line_edit();
        return;
    }

    InvalidateRect(g_program, nullptr, FALSE);
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static bool field_value_is_negative(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 && value.front() == '(' && value.back() == ')')
        value = trim(value.substr(1, value.size() - 2));
    return !value.empty() && value.front() == '-';
}

static std::string effective_edit_input(const FieldSpan &field)
{
    if (g_edit_input.empty())
        return field.value;

    if (g_edit_input.front() == '-')
        return g_edit_input;

    if (g_edit_input.front() == '+')
        return g_edit_input.substr(1);

    if (field_value_is_negative(field.value))
        return "-" + g_edit_input;

    return g_edit_input;
}

static std::string edit_display_line()
{
    if (!g_edit_active)
        return "";

    std::string line = g_edit_draft;
    if (g_edit_input.empty())
        return line;

    std::vector<FieldSpan> fields = parse_field_spans(line);
    if (g_edit_field < 0 || g_edit_field >= (int)fields.size())
        return line;

    FieldSpan f = fields[(size_t)g_edit_field];
    line.replace(f.open + 1, f.close - f.open - 1, effective_edit_input(f));
    return line;
}

static void commit_line_edit()
{
    if (!g_edit_active)
        return;

    if (!g_edit_input.empty())
    {
        accept_edit_field();
        if (!g_edit_active)
            return;
    }

    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    if (g_edit_line >= 0 && g_edit_line < (int)lines.size()) {
        lines[(size_t)g_edit_line] = g_edit_draft;
        set_lcam_lines(lines);
        generate_gcode();
        set_status("LeanCam line committed");
    }

    g_edit_active = false;
    g_edit_line = -1;
    g_edit_field = 0;
    g_edit_draft.clear();
    g_edit_input.clear();
    InvalidateRect(g_program, nullptr, FALSE);
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void delete_selected_line()
{
    if (g_edit_active)
        return;
    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    clamp_selected_line();
    if (g_selected_line < 0 || g_selected_line >= (int)lines.size())
        return;
    lines.erase(lines.begin() + g_selected_line);
    if (g_selected_line >= (int)lines.size())
        g_selected_line = std::max(0, (int)lines.size() - 1);
    ensure_selected_visible();
    set_lcam_lines(lines);
    generate_gcode();
    set_status("LeanCam line deleted");
}

static void insert_cycle_template(int index)
{
    if (g_edit_active)
        return;
    if (index < 0 || index >= (int)(sizeof(kCycleTemplates) / sizeof(kCycleTemplates[0])))
        return;

    std::vector<std::string> lines = split_lines(get_text(g_lcam));
    if (lines.empty()) {
        lines.push_back(kCycleTemplates[index]);
        g_selected_line = 0;
    } else {
        clamp_selected_line();
        lines.insert(lines.begin() + g_selected_line + 1, kCycleTemplates[index]);
        ++g_selected_line;
    }

    ensure_selected_visible();
    set_lcam_lines(lines);
    generate_gcode();
    begin_line_edit();
    set_status("Inserted new LeanCam cycle. Edit fields, End commits.");
}

static void handle_leancam_key(UINT vk)
{
    switch (vk) {
    case VK_UP:
        move_selected_line(-1);
        return;
    case VK_DOWN:
        move_selected_line(1);
        return;
    case VK_RETURN:
        if (g_edit_active)
            accept_edit_field();
        else
            begin_line_edit();
        return;
    case VK_END:
        if (g_edit_active)
            commit_line_edit();
        else
            start_run();
        return;
    case VK_DELETE:
        delete_selected_line();
        return;
    case VK_ESCAPE:
        if (g_edit_active)
            cancel_line_edit();
        return;
    case VK_BACK:
        if (g_edit_active && !g_edit_input.empty()) {
            g_edit_input.pop_back();
            InvalidateRect(g_program, nullptr, FALSE);
        }
        return;
    default:
        if (vk >= '0' && vk <= '9') {
            insert_cycle_template((int)(vk - '0'));
            return;
        }
        return;
    }
}

static void handle_leancam_char(WPARAM ch)
{
    if (!g_edit_active)
        return;
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' ||
        (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_') {
        if (g_edit_input.size() < 31) {
            g_edit_input.push_back((char)ch);
            InvalidateRect(g_program, nullptr, FALSE);
        }
    }
}

static std::string current_preview_line()
{
    std::vector<std::string> lines = split_lines(get_text(g_lcam));

    if (g_edit_active && !g_edit_draft.empty())
        return edit_display_line();

    if (g_selected_line >= 0 && g_selected_line < (int)lines.size()) {
        std::string selected = trim(lines[(size_t)g_selected_line]);
        if (is_cycle_line(selected))
            return selected;
    }

    for (const std::string &raw : lines) {
        std::string line = trim(raw);
        if (is_cycle_line(line))
            return line;
    }

    return "";
}

static LRESULT CALLBACK program_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        draw_program(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        int y = HIWORD(lp);
        int rowH = 26;
        std::vector<std::string> lines = split_lines(get_text(g_lcam));
        int row = (y - 8 - rowH * 2 - 8) / (rowH * 2);
        int target = g_program_scroll + row;
        if (target >= 0 && target < (int)lines.size()) {
            g_selected_line = target;
            ensure_selected_visible();
            InvalidateRect(hwnd, nullptr, FALSE);
            InvalidateRect(g_canvas, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        std::vector<std::string> lines = split_lines(get_text(g_lcam));
        g_program_scroll += (delta < 0) ? 1 : -1;
        g_program_scroll = std::clamp(g_program_scroll, 0, std::max(0, (int)lines.size() - 1));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        handle_leancam_key((UINT)wp);
        return 0;
    case WM_CHAR:
        handle_leancam_char(wp);
        return 0;
    case WM_LBUTTONDBLCLK:
        SetFocus(g_lcam);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK canvas_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        draw_canvas(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_program = CreateWindowExA(WS_EX_CLIENTEDGE, "LeanCamProgram", "",
                                    WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    hwnd, (HMENU)IDC_PROGRAM, g_inst, nullptr);
        g_lcam = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", kStarter,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                                     ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
                                 0, 0, 0, 0, hwnd, (HMENU)IDC_LCAM, g_inst, nullptr);
        g_gcode = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                                      ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
                                  0, 0, 0, 0, hwnd, (HMENU)IDC_GCODE, g_inst, nullptr);
        g_canvas = CreateWindowExA(0, "LeanCamCanvas", "",
                                   WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                   hwnd, (HMENU)IDC_CANVAS, g_inst, nullptr);
        g_status = CreateWindowExA(0, "STATIC", "Ready", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, g_inst, nullptr);
        CreateWindowA("BUTTON", "New", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_NEW, g_inst, nullptr);
        CreateWindowA("BUTTON", "Open", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_OPEN, g_inst, nullptr);
        CreateWindowA("BUTTON", "Save", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE, g_inst, nullptr);
        CreateWindowA("BUTTON", "Generate", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_GEN, g_inst, nullptr);
        CreateWindowA("BUTTON", "Run", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RUN, g_inst, nullptr);
        CreateWindowA("BUTTON", "Pause", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_PAUSE, g_inst, nullptr);
        CreateWindowA("BUTTON", "Stop", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_STOP, g_inst, nullptr);
        CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_CONNECT, g_inst, nullptr);
        CreateWindowA("BUTTON", "Save NC", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_NC, g_inst, nullptr);
        g_tab_lcam = CreateWindowA("BUTTON", "Lean code",
                                   WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | BS_PUSHLIKE,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_TAB_LCAM, g_inst, nullptr);
        g_tab_gcode = CreateWindowA("BUTTON", "G-code",
                                    WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                    0, 0, 0, 0, hwnd, (HMENU)IDC_TAB_GCODE, g_inst, nullptr);
        g_comport = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "COM14",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, hwnd, (HMENU)IDC_COMPORT, g_inst, nullptr);
        g_grbl = CreateWindowA("BUTTON", "Grbl",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_GRBL, g_inst, nullptr);
        SendMessageA(g_lcam, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        SendMessageA(g_gcode, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        SendMessageA(g_comport, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        SendMessageA(g_grbl, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageA(g_tab_lcam, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageA(g_tab_gcode, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        layout(hwnd);
        set_code_tab(0);
        generate_gcode();
        SetFocus(g_program);
        return 0;

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_SETFOCUS:
        SetFocus(g_program);
        return 0;

    case WM_KEYDOWN:
        handle_leancam_key((UINT)wp);
        return 0;

    case WM_CHAR:
        handle_leancam_char(wp);
        return 0;

    case WM_TIMER:
        if (wp == RUN_TIMER)
            tick_run();
        if (wp == UCNC_TIMER)
            ucnc_poll();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_LCAM) {
            g_error.clear();
            InvalidateRect(g_program, nullptr, FALSE);
            InvalidateRect(g_canvas, nullptr, FALSE);
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDC_NEW:
            set_text(g_lcam, kStarter);
            g_path.clear();
            generate_gcode();
            InvalidateRect(g_program, nullptr, FALSE);
            InvalidateRect(g_canvas, nullptr, FALSE);
            return 0;
        case IDC_OPEN: {
            char path[MAX_PATH] = "";
            std::string text;
            if (choose_file(false, path, MAX_PATH) && read_file(path, text)) {
                g_path = path;
                set_text(g_lcam, text);
                generate_gcode();
                InvalidateRect(g_program, nullptr, FALSE);
                InvalidateRect(g_canvas, nullptr, FALSE);
                set_status(std::string("Opened ") + path);
            }
            return 0;
        }
        case IDC_SAVE: {
            char path[MAX_PATH] = "";
            if (!g_path.empty())
                std::strncpy(path, g_path.c_str(), sizeof(path) - 1);
            if ((g_path.empty() && choose_file(true, path, MAX_PATH)) || !g_path.empty()) {
                if (g_path.empty())
                    g_path = path;
                write_file(g_path.c_str(), get_text(g_lcam));
                set_status(std::string("Saved ") + g_path);
            }
            return 0;
        }
        case IDC_GEN:
            if (!generate_gcode()) {
                set_status(g_error);
                InvalidateRect(g_canvas, nullptr, FALSE);
            } else {
                g_generated_moves = parse_gcode_moves(get_text(g_gcode));
                InvalidateRect(g_program, nullptr, FALSE);
                InvalidateRect(g_canvas, nullptr, FALSE);
            }
            return 0;
        case IDC_SAVE_NC:
            save_generated_nc();
            return 0;
        case IDC_RUN:
            start_run();
            return 0;
        case IDC_GRBL:
            set_status(grbl_mode_enabled()
                           ? "Grbl sender mode: G7/G8 filtered on send/export"
                           : "uCNC sender mode: generated G-code sent unchanged");
            return 0;
        case IDC_TAB_LCAM:
            set_code_tab(0);
            return 0;
        case IDC_TAB_GCODE:
            set_code_tab(1);
            return 0;
        case IDC_PAUSE:
            if (g_run.realUcnc && g_ucnc.connected) {
                g_run.paused = !g_run.paused;
                ucnc_write_raw(g_run.paused ? "!" : "~");
                set_status(g_run.paused ? "uCNC feed hold requested" : "uCNC cycle start requested");
            } else {
                g_run.paused = !g_run.paused;
                set_status(g_run.paused ? "Paused local visual fallback" : "Running local visual fallback");
            }
            return 0;
        case IDC_STOP:
            stop_run();
            return 0;
        case IDC_CONNECT:
            if (g_ucnc.connected) {
                ucnc_close();
                set_status("Disconnected from uCNC virtual COM");
            } else {
                ucnc_connect(get_text(g_comport));
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, RUN_TIMER);
        ucnc_close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    g_inst = hInstance;

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "LeanCamWin";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = program_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "LeanCamProgram";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = canvas_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "LeanCamCanvas";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClassA(&wc);

    g_main = CreateWindowA("LeanCamWin", "LeanCamWin - LeanCam editor and virtual lathe run",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
                           nullptr, nullptr, hInstance, nullptr);
    ShowWindow(g_main, nCmdShow);
    UpdateWindow(g_main);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
