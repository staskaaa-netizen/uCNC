#include "grbl_port_win32.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

static bool win32_open(void *ctx, const char *device, unsigned baud)
{
    grbl_port_win32_t *port = ctx;
    char path[64];
    DCB dcb;
    COMMTIMEOUTS timeouts;
    HANDLE handle;

    if (!port || !device || !*device)
        return false;
    /* COMn above 9 needs the \\.\ prefix; keep it when the caller already used
       a full path. */
    if (strncmp(device, "\\\\.\\", 4u) == 0)
        snprintf(path, sizeof(path), "%s", device);
    else
        snprintf(path, sizeof(path), "\\\\.\\%s", device);

    handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        CloseHandle(handle);
        return false;
    }
    dcb.BaudRate = baud ? baud : 115200u;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(handle, &dcb)) {
        CloseHandle(handle);
        return false;
    }

    /* Non-blocking reads: hand back whatever is waiting. */
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutConstant = 500;
    if (!SetCommTimeouts(handle, &timeouts)) {
        CloseHandle(handle);
        return false;
    }
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    port->handle = handle;
    port->opened_ms = (unsigned long long)GetTickCount64();
    snprintf(port->device, sizeof(port->device), "%s", path);
    return true;
}

static void win32_close(void *ctx)
{
    grbl_port_win32_t *port = ctx;

    if (!port)
        return;
    if (port->handle && port->handle != INVALID_HANDLE_VALUE)
        CloseHandle((HANDLE)port->handle);
    port->handle = NULL;
}

static int win32_write(void *ctx, const char *data, size_t len)
{
    grbl_port_win32_t *port = ctx;
    DWORD written = 0;

    if (!port || !port->handle || !data || len == 0u)
        return -1;
    if (!WriteFile((HANDLE)port->handle, data, (DWORD)len, &written, NULL))
        return -1;
    return (int)written;
}

static int win32_read(void *ctx, char *buf, size_t len)
{
    grbl_port_win32_t *port = ctx;
    DWORD got = 0;

    if (!port || !port->handle || !buf || len == 0u)
        return -1;
    if (!ReadFile((HANDLE)port->handle, buf, (DWORD)len, &got, NULL))
        return -1;
    return (int)got;
}

static unsigned win32_elapsed_ms(void *ctx)
{
    grbl_port_win32_t *port = ctx;

    if (!port)
        return 0u;
    return (unsigned)((unsigned long long)GetTickCount64() - port->opened_ms);
}

static void win32_sleep_ms(void *ctx, unsigned ms)
{
    (void)ctx;
    Sleep(ms);
}

static const grbl_port_t g_win32_port = {
    win32_open,
    win32_close,
    win32_write,
    win32_read,
    win32_elapsed_ms,
    win32_sleep_ms
};

const grbl_port_t *grbl_port_win32(grbl_port_win32_t *port)
{
    if (port)
        memset(port, 0, sizeof(*port));
    return &g_win32_port;
}

#else

const grbl_port_t *grbl_port_win32(grbl_port_win32_t *port)
{
    (void)port;
    return NULL;
}

#endif
