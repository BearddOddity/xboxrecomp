/*
 * PsCreateSystemThreadEx's CreateSuspended argument, through the thunk
 * dispatcher like a title's kernel calls. Runs the real memory layout on the
 * synthetic XBE from tools/conformance; no title is needed.
 *
 *   default  Today's behaviour, pinned: the worker runs at once whatever
 *            CreateSuspended says.
 *   honor    RECOMP_HONOR_SUSPEND=1: a worker created suspended does not run
 *            until NtResumeThread; one created running still runs at once.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static volatile LONG g_ran, g_done;
static HANDLE g_gate;     /* parked mode: the worker waits here after it ran */

static void worker(void)
{
    InterlockedIncrement(&g_ran);
    if (g_gate) {
        WaitForSingleObject(g_gate, 10000);
        InterlockedIncrement(&g_done);
    }
}

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { return xbox_va == 0x00030000u ? worker : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;

enum { S_CREATE, S_RESUME, S_SUSPEND, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = { 255 /* PsCreateSystemThreadEx */, 224 /* NtResumeThread */,
                                       231 /* NtSuspendThread */ };

static uint32_t scratch;
#define THUNK_VA (scratch)
#define STACK_VA (scratch + 0x1000)
#define H1_VA    (scratch + 0x2000)
#define H2_VA    (scratch + 0x2010)
#define H3_VA    (scratch + 0x2020)

static uint32_t slot_va[N_SLOTS];
static int failures;

static uint32_t *G(uint32_t va) { return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset()); }

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = G(STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++) sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    return g_eax;
}

static uint32_t create(uint32_t handle_va, uint32_t suspended)
{
    uint32_t a[10] = { handle_va, 0, 0, 0, 0, 0, 0, suspended, 0, 0x00030000u };
    return call(S_CREATE, 10, a);
}

static int wait_ran(LONG n, DWORD ms)
{
    DWORD t0 = GetTickCount();
    while (g_ran < n && GetTickCount() - t0 < ms) Sleep(5);
    return g_ran >= n;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int guard = !strcmp(mode, "guard");
    int parked = !strcmp(mode, "parked");   /* resumed, then parked by the title */
    int honor = !strcmp(mode, "honor") || guard || parked;
    int zero = !strcmp(mode, "zero");     /* =0 is off, like every RECOMP_* switch */
    size_t xbe_len = 0;
    unsigned char *xbe;
    uint32_t args[2];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!honor && !zero && strcmp(mode, "default")) { printf("unknown mode '%s'\n", mode); return 2; }
    _putenv_s("RECOMP_HONOR_SUSPEND", honor ? "1" : zero ? "0" : "");
    if (!honor && !zero) _putenv_s("RECOMP_HONOR_SUSPEND", "");
    if (guard || parked) _putenv_s("RECOMP_HONOR_SUSPEND_MS", "300");

    xbe = load_file(path, &xbe_len);
    if (!xbe) { printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path); return 2; }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) { puts("xbox_MemoryLayoutInit failed"); return 2; }

    scratch = xbox_HeapAlloc(0x10000, 4096);
    for (i = 0; i < N_SLOTS; i++) *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *G(THUNK_VA + 4 * i);
        if (!recomp_lookup_kernel(slot_va[i])) { printf("ordinal %u not bridged\n", ORD[i]); return 2; }
    }
    printf("mode: %s\n", mode);

    /* The first create is the title's main thread and runs inline. */
    create(H1_VA, 0);
    g_ran = 0;
    /* After the main thread, which runs the same routine inline. */
    if (parked) g_gate = CreateEvent(NULL, TRUE, FALSE, NULL);

    check(create(H2_VA, 1) == 0, "create suspended: STATUS_SUCCESS");
    if (guard) {
        /* The title never resumes it: the guard starts it after 300 ms. */
        check(!wait_ran(1, 100), "suspended worker has not run yet");
        check(wait_ran(1, 3000), "never-resumed worker is started by the guard");
    } else if (parked) {
        /* The title resumes the worker, then suspends it again itself (50
         * Cent's handshake). The guard must not mistake that for a thread
         * that was never resumed and start it. */
        args[0] = *G(H2_VA); args[1] = 0;
        check(call(S_RESUME, 2, args) == 0, "NtResumeThread succeeds");
        check(wait_ran(1, 2000), "worker runs after NtResumeThread");
        check(call(S_SUSPEND, 2, args) == 0, "title suspends it again");
        Sleep(800);                       /* well past the 300 ms guard window */
        SetEvent(g_gate);
        Sleep(200);
        check(g_done == 0, "guard did not restart the title-parked worker");
        check(call(S_RESUME, 2, args) == 0, "title resumes it");
        DWORD t0 = GetTickCount();
        while (g_done < 1 && GetTickCount() - t0 < 2000) Sleep(5);
        check(g_done == 1, "worker finishes after the title's own resume");
    } else if (honor) {
        check(!wait_ran(1, 200), "suspended worker has not run");
        args[0] = *G(H2_VA); args[1] = 0;
        check(call(S_RESUME, 2, args) == 0, "NtResumeThread on its handle succeeds");
        check(wait_ran(1, 2000), "worker runs after NtResumeThread");
        Sleep(400);   /* the guard (3 s) is far off; make sure nothing runs it again */
        check(g_ran == 1, "worker ran exactly once");
    } else {
        check(wait_ran(1, 2000), "worker runs at once (CreateSuspended ignored)");
    }

    g_ran = 0;
    check(create(H3_VA, 0) == 0, "create running: STATUS_SUCCESS");
    check(wait_ran(1, 2000), "a worker created running runs at once");

    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
