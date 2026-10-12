/* rts_probe.c - which host calls change RTS on a USB CDC device, and when.
 *
 * Drives RTS on the echo board (firmware/ysp_echo version 2, "l" line
 * reports) through every Win32 path that sets it, with DTR held low and
 * high, and prints what the board reported after each step and what the
 * driver says the line state is (IOCTL_SERIAL_GET_DTRRTS). A DTR change
 * after the RTS steps shows whether a held-back RTS state goes out with it.
 *
 *   rts_probe COM3
 *
 * Paths:
 *   esc    EscapeCommFunction(SETRTS / CLRRTS)
 *   ioctl  DeviceIoControl(IOCTL_SERIAL_SET_RTS / IOCTL_SERIAL_CLR_RTS)
 *   dcb    SetCommState with fRtsControl RTS_CONTROL_ENABLE / DISABLE, the
 *          rest of the DCB from GetCommState (fDtrControl included)
 *   dcbd   the same, with fDtrControl set from the driver's DTR state
 *   hflow  IOCTL_SERIAL_GET_HANDFLOW, then IOCTL_SERIAL_SET_HANDFLOW with the
 *          RTS bits of FlowReplace set and the DTR bits of ControlHandShake
 *          set from the driver's DTR state
 *   escd   EscapeCommFunction(SETRTS / CLRRTS), then SETDTR or CLRDTR to the
 *          DTR level the driver already has (IOCTL_SERIAL_GET_DTRRTS)
 *   k_baud, k_lc, k_chars, k_hf
 *          IOCTL_SERIAL_SET_RTS / CLR_RTS, then one more request that sets
 *          what is already set: the baud rate, the line control, the special
 *          characters or the handflow. Which of them makes usbser.sys send
 *          the line state?
 *
 * Build (Visual C++): cl /O2 /W4 /I..\..\..\include rts_probe.c
 * Windows only. Public domain / MIT-0, as the rest of ysp. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ntddser.h values, so the probe needs no DDK header. */
#define P_SER(fn) CTL_CODE(FILE_DEVICE_SERIAL_PORT, fn, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define P_IOCTL_SET_DTR    P_SER(9)
#define P_IOCTL_CLR_DTR    P_SER(10)
#define P_IOCTL_SET_RTS    P_SER(12)
#define P_IOCTL_CLR_RTS    P_SER(13)
#define P_IOCTL_GET_DTRRTS P_SER(30)
#define P_IOCTL_SET_BAUD   P_SER(1)
#define P_IOCTL_SET_LC     P_SER(3)
#define P_IOCTL_GET_BAUD   P_SER(20)
#define P_IOCTL_GET_LC     P_SER(21)
#define P_IOCTL_GET_CHARS  P_SER(22)
#define P_IOCTL_SET_CHARS  P_SER(23)
#define P_IOCTL_GET_HANDFLOW P_SER(24)
#define P_IOCTL_SET_HANDFLOW P_SER(25)
#define P_DTR_MASK    0x03u  /* ControlHandShake */
#define P_DTR_CONTROL 0x01u
#define P_RTS_MASK    0xC0u  /* FlowReplace */
#define P_RTS_CONTROL 0x40u
typedef struct { ULONG ControlHandShake, FlowReplace; LONG XonLimit, XoffLimit; } p_handflow;

/* The driver's DTR state, or -1. */
static int driver_dtr(void);

static HANDLE h;
static LARGE_INTEGER f0, t0;

static double now_ms(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - t0.QuadPart) * 1e3 / (double)f0.QuadPart;
}

static void die(const char* what) {
    fprintf(stderr, "rts_probe: %s failed (os error %lu)\n", what, (unsigned long)GetLastError());
    exit(1);
}

static void put(const char* s) {
    DWORD n = 0;
    if (!WriteFile(h, s, (DWORD)strlen(s), &n, NULL) || n != strlen(s)) die("WriteFile");
}

/* The driver's view of DTR and RTS: "d r" or "?" when the IOCTL fails. */
static void driver_state(char* out) {
    ULONG v = 0;
    DWORD n = 0;
    if (DeviceIoControl(h, P_IOCTL_GET_DTRRTS, NULL, 0, &v, sizeof v, &n, NULL))
        sprintf(out, "%d %d", (int)(v & 1), (int)((v >> 1) & 1));
    else
        sprintf(out, "? (err %lu)", (unsigned long)GetLastError());
}

static int driver_dtr(void) {
    ULONG v = 0;
    DWORD n = 0;
    return DeviceIoControl(h, P_IOCTL_GET_DTRRTS, NULL, 0, &v, sizeof v, &n, NULL) ? (int)(v & 1) : -1;
}

/* Every "L t dtr rts" line the board sends within ms, as "dtr rts" pairs. */
static int collect(int ms, char* out, size_t cap) {
    char buf[512];
    int len = 0, nrep = 0;
    double end = now_ms() + ms;
    out[0] = '\0';
    while (now_ms() < end) {
        DWORD n = 0;
        if (!ReadFile(h, buf + len, (DWORD)(sizeof buf - 1 - len), &n, NULL)) die("ReadFile");
        len += (int)n;
        buf[len] = '\0';
        char* nl;
        while ((nl = strchr(buf, '\n')) != NULL) {
            *nl = '\0';
            if (buf[0] == 'L' && strlen(buf) == 16) {
                size_t u = strlen(out);
                snprintf(out + u, cap - u, "%s[%c %c]", nrep ? " " : "", buf[13], buf[15]);
                nrep++;
            }
            int rest = len - (int)(nl + 1 - buf);
            memmove(buf, nl + 1, (size_t)rest);
            len = rest;
            buf[len] = '\0';
        }
    }
    if (!nrep) snprintf(out, cap, "none");
    return nrep;
}

static int set_line(const char* path, int rts, int on) {
    if (strcmp(path, "escd") == 0) {
        int dtr = driver_dtr();
        if (!rts || dtr < 0) return -1;
        if (!EscapeCommFunction(h, on ? SETRTS : CLRRTS)) return -2;
        return EscapeCommFunction(h, dtr ? SETDTR : CLRDTR) ? 0 : -3;
    }
    if (strcmp(path, "esc") == 0)
        return EscapeCommFunction(h, rts ? (on ? SETRTS : CLRRTS) : (on ? SETDTR : CLRDTR)) ? 0 : -1;
    if (strcmp(path, "ioctl") == 0) {
        DWORD code = rts ? (on ? P_IOCTL_SET_RTS : P_IOCTL_CLR_RTS) : (on ? P_IOCTL_SET_DTR : P_IOCTL_CLR_DTR);
        DWORD n = 0;
        return DeviceIoControl(h, code, NULL, 0, NULL, 0, &n, NULL) ? 0 : -1;
    }
    if (strncmp(path, "k_", 2) == 0) {
        unsigned char buf[64];
        DWORD n = 0, get, set;
        if (!rts) return -1;
        if (!DeviceIoControl(h, on ? P_IOCTL_SET_RTS : P_IOCTL_CLR_RTS, NULL, 0, NULL, 0, &n, NULL))
            return -1;
        if (strcmp(path, "k_baud") == 0)       { get = P_IOCTL_GET_BAUD; set = P_IOCTL_SET_BAUD; }
        else if (strcmp(path, "k_lc") == 0)    { get = P_IOCTL_GET_LC; set = P_IOCTL_SET_LC; }
        else if (strcmp(path, "k_chars") == 0) { get = P_IOCTL_GET_CHARS; set = P_IOCTL_SET_CHARS; }
        else                                   { get = P_IOCTL_GET_HANDFLOW; set = P_IOCTL_SET_HANDFLOW; }
        if (!DeviceIoControl(h, get, NULL, 0, buf, sizeof buf, &n, NULL)) return -2;
        DWORD len = n;
        return DeviceIoControl(h, set, buf, len, NULL, 0, &n, NULL) ? 0 : -3;
    }
    if (strcmp(path, "hflow") == 0) {
        p_handflow hf;
        DWORD n = 0;
        int dtr = driver_dtr();
        if (dtr < 0) return -1;
        if (!DeviceIoControl(h, P_IOCTL_GET_HANDFLOW, NULL, 0, &hf, sizeof hf, &n, NULL)) return -1;
        hf.FlowReplace = (hf.FlowReplace & ~P_RTS_MASK) | (on ? P_RTS_CONTROL : 0u);
        hf.ControlHandShake = (hf.ControlHandShake & ~P_DTR_MASK) | (dtr ? P_DTR_CONTROL : 0u);
        return DeviceIoControl(h, P_IOCTL_SET_HANDFLOW, &hf, sizeof hf, NULL, 0, &n, NULL) ? 0 : -1;
    }
    /* dcb, dcbd */
    DCB d;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (!GetCommState(h, &d)) return -1;
    if (rts) d.fRtsControl = on ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    if (rts && strcmp(path, "dcbd") == 0) {
        int dtr = driver_dtr();
        if (dtr < 0) return -1;
        d.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    }
    else d.fDtrControl = on ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    return SetCommState(h, &d) ? 0 : -1;
}

static void step(const char* label, const char* path, int rts, int on) {
    char rep[256], st[32];
    double a = now_ms();
    int rc = set_line(path, rts, on);
    double b = now_ms();
    collect(60, rep, sizeof rep);
    driver_state(st);
    printf("%-34s %-5s %s=%d rc=%d call=%.3f ms | driver d r: %s | board: %s\n",
           label, path, rts ? "RTS" : "DTR", on, rc, b - a, st, rep);
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: rts_probe COM3\n"); return 2; }
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    char path[64];
    snprintf(path, sizeof path, "\\\\.\\%s", argv[1]);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) die("CreateFile");
    DCB d;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (!GetCommState(h, &d)) die("GetCommState");
    d.BaudRate = 115200; d.ByteSize = 8; d.Parity = NOPARITY; d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE; d.fParity = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE; d.fRtsControl = RTS_CONTROL_ENABLE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = d.fOutX = d.fInX = FALSE;
    if (!SetCommState(h, &d)) die("SetCommState");
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof t);
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = 10;
    t.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(h, &t)) die("SetCommTimeouts");
    char rep[256], st[32];
    put("x\nl 0\n");
    Sleep(50);
    PurgeComm(h, PURGE_RXCLEAR);
    {
        /* The board must be ysp_echo 2 or later: "l" is a version 2 report. */
        char v[64];
        DWORD n = 0;
        put("v\n");
        Sleep(50);
        if (!ReadFile(h, v, sizeof v - 1, &n, NULL)) die("ReadFile");
        v[n] = '\0';
        if (strncmp(v, "V ysp-echo ", 11) != 0) {
            fprintf(stderr, "rts_probe: no ysp_echo 2 board: \"%s\"\n", v);
            return 1;
        }
        printf("board: %s", v);
    }
    put("l 1\n");
    collect(100, rep, sizeof rep);
    driver_state(st);
    printf("start: DTR=1 RTS=1 set by SetCommState | driver d r: %s | board: %s\n", st, rep);

    static const char* const paths[10] = { "esc", "ioctl", "dcb", "dcbd", "hflow",
                                           "k_baud", "k_lc", "k_chars", "k_hf", "escd" };
    for (int dtr = 0; dtr <= 1; dtr++) {
        for (int k = 0; k < 10; k++) {
            printf("--- path %s, DTR held at %d\n", paths[k], dtr);
            step("DTR to held level", "esc", 0, dtr);
            step("RTS low", paths[k], 1, 0);
            step("RTS high", paths[k], 1, 1);
            step("RTS low", paths[k], 1, 0);
            step("RTS low again", paths[k], 1, 0);
            step("RTS high", paths[k], 1, 1);
            /* Left low, so the board's last report (RTS 1) differs from the
             * driver's state, and a request with no DTR change shows. */
            step("RTS low, left there", paths[k], 1, 0);
            step("DTR set to the same level", "esc", 0, dtr);
            step("DTR changed", "esc", 0, !dtr);
            step("DTR back", "esc", 0, dtr);
        }
    }
    put("l 0\n");
    CloseHandle(h);
    return 0;
}
