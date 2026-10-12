/* ioctl_trace.c - which serial IOCTLs does SetCommState send, and which of
 * them makes usbser.sys send the RTS state to the device?
 *
 * Patches kernelbase.dll's import of ntdll!NtDeviceIoControlFile in this
 * process only (x64), calls SetCommState once with RTS lowered, prints the
 * IOCTL function numbers in order, then replays each one after an
 * IOCTL_SERIAL_SET_RTS or CLR_RTS and prints the echo board's line reports
 * (firmware/ysp_echo version 2, "l" mode). docs/serial.md, "RTS on
 * usbser.sys", has the result.
 *
 *   ioctl_trace COM3
 *
 * Build (Visual C++): cl /O2 /W3 ioctl_trace.c
 * Windows only. Public domain / MIT-0, as the rest of ysp. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

typedef LONG (NTAPI *nt_ioctl_fn)(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG);
static nt_ioctl_fn real_ioctl;
static volatile int logging;
static ULONG log_code[64];
static ULONG log_inlen[64];
static unsigned char log_in[64][64];
static int nlog;

static LONG NTAPI hook(HANDLE f, HANDLE e, PVOID apc, PVOID ctx, PVOID iosb, ULONG code,
                       PVOID in, ULONG inlen, PVOID out, ULONG outlen) {
    if (logging && nlog < 64) {
        log_code[nlog] = code;
        log_inlen[nlog] = inlen;
        if (in && inlen <= 64) memcpy(log_in[nlog], in, inlen);
        nlog++;
    }
    return real_ioctl(f, e, apc, ctx, iosb, code, in, inlen, out, outlen);
}

/* Patches kernelbase.dll's import of NtDeviceIoControlFile, so the hook
 * chains to whatever the import pointed at. */
static void install(void) {
    void* want = (void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDeviceIoControlFile");
    unsigned char* base = (unsigned char*)GetModuleHandleA("kernelbase.dll");
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    int found = 0;
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA* th = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; th->u1.Function; th++) {
            if ((void*)th->u1.Function != want) continue;
            DWORD old;
            VirtualProtect(&th->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            real_ioctl = (nt_ioctl_fn)want;
            th->u1.Function = (ULONG_PTR)(void*)hook;
            VirtualProtect(&th->u1.Function, sizeof(void*), old, &old);
            found++;
        }
    }
    printf("patched %d import(s)\n", found);
    if (!found) ExitProcess(1);
}

static HANDLE h;

static void put(const char* s) { DWORD n; WriteFile(h, s, (DWORD)strlen(s), &n, NULL); }

static void reports(const char* label) {
    char buf[256];
    DWORD n = 0, tot = 0;
    DWORD end = GetTickCount() + 60;
    while (GetTickCount() < end) {
        if (ReadFile(h, buf + tot, sizeof buf - 1 - tot, &n, NULL)) tot += n;
        if (tot > 200) break;
    }
    buf[tot] = '\0';
    for (char* p = buf; *p; p++) if (*p == '\n') *p = '|';
    printf("  %-40s board: %s\n", label, tot ? buf : "none");
}

int main(int argc, char** argv) {
    char path[64];
    if (argc < 2) return 2;
    snprintf(path, sizeof path, "\\\\.\\%s", argv[1]);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("open failed %lu\n", GetLastError()); return 1; }
    DCB d; memset(&d, 0, sizeof d); d.DCBlength = sizeof d;
    GetCommState(h, &d);
    d.BaudRate = 115200; d.ByteSize = 8; d.Parity = NOPARITY; d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE; d.fDtrControl = DTR_CONTROL_ENABLE; d.fRtsControl = RTS_CONTROL_ENABLE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = d.fOutX = d.fInX = FALSE;
    SetCommState(h, &d);
    COMMTIMEOUTS t; memset(&t, 0, sizeof t);
    t.ReadIntervalTimeout = MAXDWORD; t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = 10; t.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(h, &t);
    put("x\nl 0\n"); Sleep(50); PurgeComm(h, PURGE_RXCLEAR);
    put("l 1\n");
    reports("start");

    install();
    GetCommState(h, &d);
    d.fDtrControl = DTR_CONTROL_ENABLE;
    d.fRtsControl = RTS_CONTROL_DISABLE;
    nlog = 0; logging = 1;
    BOOL ok = SetCommState(h, &d);
    logging = 0;
    printf("SetCommState(RTS off) ok=%d, %d IOCTLs:\n", ok, nlog);
    int n = nlog;
    ULONG codes[64], lens[64];
    unsigned char ins[64][64];
    memcpy(codes, log_code, sizeof codes); memcpy(lens, log_inlen, sizeof lens); memcpy(ins, log_in, sizeof ins);
    for (int i = 0; i < n; i++)
        printf("  %2d: function %lu (code 0x%08lx), in %lu bytes\n", i,
               (codes[i] >> 2) & 0xFFF, codes[i], lens[i]);
    reports("after SetCommState(RTS off)");

    /* Replay: RTS high with IOCTL_SERIAL_SET_RTS, then each logged IOCTL of
     * the sequence alone (with its logged input), and look for a report. */
    DWORD set_rts = CTL_CODE(FILE_DEVICE_SERIAL_PORT, 12, METHOD_BUFFERED, FILE_ANY_ACCESS);
    DWORD clr_rts = CTL_CODE(FILE_DEVICE_SERIAL_PORT, 13, METHOD_BUFFERED, FILE_ANY_ACCESS);
    int level = 0;
    for (int i = 0; i < n; i++) {
        DWORD k;
        level = !level;
        DeviceIoControl(h, level ? set_rts : clr_rts, NULL, 0, NULL, 0, &k, NULL);
        reports(level ? "SET_RTS alone" : "CLR_RTS alone");
        unsigned char out[256];
        BOOL r = DeviceIoControl(h, codes[i], lens[i] ? ins[i] : NULL, lens[i], out, sizeof out, &k, NULL);
        char lab[80];
        snprintf(lab, sizeof lab, "then function %lu again (rc %d)", (codes[i] >> 2) & 0xFFF, r);
        reports(lab);
    }
    put("l 0\n");
    CloseHandle(h);
    return 0;
}
