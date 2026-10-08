/* PGRAPH's software-method trap, against real page faults.
 *
 * The register aperture is a buffer laid out like the NV2A's. A second thread
 * plays the title's interrupt handler, in the order XDK D3D's does it: FIFO
 * access off, read PGRAPH_INTR, write the value it read back to acknowledge,
 * read the trapped method, read PGRAPH_INTR again, FIFO access on. Its writes
 * are ordinary stores, so they fault into the trap's handler exactly as the
 * title's do.
 *
 * What matters most is the second read of PGRAPH_INTR. XDK D3D's deferred
 * routine loops for as long as it is non-zero; against plain memory it never
 * is, and the routine runs the same software method for ever at
 * DISPATCH_LEVEL. Write-1-to-clear makes it zero.
 *
 * Also checked: the trap waits for FIFO access to come back on, PMC_INTR's
 * PGRAPH bit and the interrupt line follow PGRAPH_INTR, a partial-width write
 * clears only the bits it carries, the page is plain memory again once the
 * trap is over, the busy bits a handler may be waiting on keep being cleared
 * during the trap, an unacknowledged trap is dropped after its bound, and a
 * trap that could not be delivered is not taken at all. */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include "nv2a_pgraph_trap.h"

#define APERTURE_BYTES 0x401000u
static volatile uint8_t *s_ap;
#define R(off) (*(volatile uint32_t *)(s_ap + (off)))

/* ---- what the trap expects the runtime to provide ---------------------- */
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t off)
{
    return off + 4u <= APERTURE_BYTES ? (volatile uint32_t *)(s_ap + off) : NULL;
}
static uint32_t s_connected = 0x1234u;
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return vector == 3 ? s_connected : 0;
}
static volatile LONG s_line;
static HANDLE s_raised;
void xbox_set_irq_line(uint32_t vector, int level)
{
    if (vector != 3)
        return;
    InterlockedExchange(&s_line, level);
    if (level)
        SetEvent(s_raised);
}

/* The busy-bit pass: here it clears the vertical blank's bit in PMC_INTR,
 * when the test says the runtime would. */
static volatile int s_busy_clears_vblank;
void xbox_Nv2aAckBusyBits(void)
{
    if (s_busy_clears_vblank)
        InterlockedAnd((volatile LONG *)(s_ap + 0x000100), ~0x01000000L);
}

/* ---- the title's handler ------------------------------------------------ */
enum { ACK_XDK, ACK_HALFWORD, ACK_NEVER, ACK_AFTER_VBLANK };
static volatile int s_mode;
static volatile int s_quit;
static struct {
    uint32_t intr, nsource, addr, data, intr_after, pmc_after;
    volatile LONG ran;
} s_seen;

static DWORD WINAPI handler_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(s_raised, INFINITE);
        if (s_quit)
            return 0;
        if (s_mode == ACK_NEVER)
            continue;
        if (s_mode == ACK_AFTER_VBLANK) {
            /* XDK D3D's deferred routine acknowledges a vblank and spins until
             * PMC_INTR bit 24 drops before it looks at PGRAPH. Bounded here so
             * a failure is a failed check, not a hung test. */
            unsigned long long t0 = GetTickCount64();
            while ((R(0x000100) & 0x01000000u) && GetTickCount64() - t0 < 3000)
                Sleep(0);
        }
        R(0x400720) = 0;                              /* FIFO access off   */
        s_seen.intr    = R(0x400100);
        s_seen.nsource = R(0x400108);
        s_seen.addr    = R(0x400704);
        if (s_mode == ACK_HALFWORD)                   /* ERROR is bit 20   */
            *(volatile uint16_t *)(s_ap + 0x400102) = 0x0010;
        else
            R(0x400100) = s_seen.intr;                /* write-1-to-clear  */
        s_seen.data       = R(0x400708);
        s_seen.intr_after = R(0x400100);
        s_seen.pmc_after  = R(0x000100);
        InterlockedIncrement(&s_seen.ran);            /* before FIFO on:   */
        R(0x400720) = 1;                              /* the trap waits for it */
    }
}

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static int trap(uint32_t subch, uint32_t param, unsigned long long *ms)
{
    unsigned long long t0 = GetTickCount64();
    int r = nv2a_pgraph_trap_nop(subch, param);
    *ms = GetTickCount64() - t0;
    return r;
}

int main(void)
{
    unsigned long long ms;
    HANDLE th;

    s_ap = (volatile uint8_t *)VirtualAlloc(NULL, APERTURE_BYTES,
                                            MEM_RESERVE | MEM_COMMIT,
                                            PAGE_READWRITE);
    s_raised = CreateEventA(NULL, FALSE, FALSE, NULL);
    th = CreateThread(NULL, 0, handler_thread, NULL, 0, NULL);
    if (!s_ap || !s_raised || !th) {
        printf("FAIL: setup\n");
        return 1;
    }
    /* What XDK D3D's initialisation leaves: every PGRAPH interrupt enabled,
     * FIFO access on. PMC_INTR carries an unrelated pending vblank. */
    R(0x400140) = 0xFFFFFFFFu;
    R(0x400720) = 1;
    R(0x000100) = 0x01000000u;

    /* The XDK handler's acknowledgement clears the bit, and the trap waits
     * for the whole handler, FIFO access included. */
    s_mode = ACK_XDK;
    CHECK(trap(5, 0x9, &ms) == 1);
    CHECK(s_seen.ran == 1);
    CHECK(s_seen.intr == 0x00100000u);                /* ERROR, nothing else */
    CHECK(s_seen.nsource == 1u);                      /* NOTIFICATION        */
    CHECK(s_seen.addr == (0x100u | (5u << 16)));      /* method, subchannel  */
    CHECK(s_seen.data == 0x9u);
    CHECK(s_seen.intr_after == 0);                    /* the loop ends       */
    CHECK(s_seen.pmc_after == 0x01000000u);           /* PGRAPH bit gone, vblank kept */
    CHECK(s_line == 0);
    CHECK(R(0x400100) == 0 && R(0x400108) == 0);
    CHECK(ms < 500);

    /* Once the trap is over the page is plain memory again: no semantics,
     * no fault. */
    R(0x400100) = 0x5u;
    CHECK(R(0x400100) == 0x5u);
    R(0x400100) = 0;

    /* A narrower write clears only the bits it carries. */
    s_mode = ACK_HALFWORD;
    CHECK(trap(0, 0x28, &ms) == 1);
    CHECK(s_seen.ran == 2 && s_seen.data == 0x28u);
    CHECK(s_seen.intr_after == 0);
    CHECK(s_line == 0);

    /* The walk's thread keeps clearing the GPU's other busy bits while it
     * waits: a handler stuck behind a vertical blank's pending bit gets to the
     * trapped method well inside the bound. */
    s_mode = ACK_AFTER_VBLANK;
    R(0x000100) = 0x01000000u;
    s_busy_clears_vblank = 1;
    CHECK(trap(0, 0x42980021u, &ms) == 1);
    CHECK(s_seen.ran == 3 && s_seen.data == 0x42980021u);
    CHECK(s_seen.intr_after == 0);
    CHECK(ms < 500);
    s_busy_clears_vblank = 0;

    /* A handler that never acknowledges costs the bound, not the run. */
    s_mode = ACK_NEVER;
    CHECK(trap(0, 0x5, &ms) == 1);
    CHECK(ms >= 900 && ms < 3000);
    CHECK(R(0x400100) == 0);
    CHECK(s_line == 0);
    s_mode = ACK_XDK;

    /* Not delivered, not taken: no ERROR interrupt enabled, or nothing
     * connected on the GPU vector. The method then stays unhandled. */
    R(0x400140) = 0;
    CHECK(trap(0, 0x5, &ms) == 0);
    R(0x400140) = 0xFFFFFFFFu;
    s_connected = 0;
    CHECK(trap(0, 0x5, &ms) == 0);
    s_connected = 0x1234u;
    CHECK(s_seen.ran == 3 && R(0x400100) == 0);

    s_quit = 1;
    SetEvent(s_raised);
    WaitForSingleObject(th, 1000);

    if (!s_fail)
        printf("nv2a pgraph software-method trap: ok\n");
    return s_fail;
}
