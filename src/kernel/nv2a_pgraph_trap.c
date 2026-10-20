/*
 * PGRAPH's software-method trap, and the interrupt registers it needs.
 *
 * NO_OPERATION (0x0100) with a non-zero parameter is not something the GPU
 * performs. XDK D3D writes one into the pushbuffer when it wants its graphics
 * interrupt at that point in the stream -- the software method that queues a
 * swap is one. On hardware PGRAPH stops at the method, latches it in
 * TRAPPED_ADDR / TRAPPED_DATA_LOW, sets the NOTIFICATION source and the ERROR
 * bit in PGRAPH_INTR -- which, enabled in PGRAPH_INTR_EN, shows as the PGRAPH
 * bit in PMC_INTR and asserts the GPU's interrupt line -- and holds there until
 * the driver has acknowledged the ERROR bit and re-enabled FIFO access.
 *
 * The acknowledgement is the part plain memory cannot do. PGRAPH_INTR is
 * write-1-to-clear: XDK D3D's handler reads it, writes the value it read back,
 * and reads it again, and its deferred routine loops for as long as that second
 * read is non-zero. Against plain memory the write changes nothing, so the
 * deferred routine runs the same software method over and over at
 * DISPATCH_LEVEL, on the thread that delivers every other device interrupt --
 * the swap is queued many times, and the USB pad is starved because the guest
 * never drops below DISPATCH_LEVEL.
 *
 * So while a trap is outstanding the PGRAPH register page is read-only. Reads
 * run at full speed and see plain memory; a write faults, is single-stepped,
 * and then has the register's semantics applied: write-1-to-clear for
 * PGRAPH_INTR, with the PMC_INTR bit and the interrupt line following
 * PGRAPH_INTR & PGRAPH_INTR_EN. This is the AC'97 bus-master trap in
 * xbox_memory_layout.c applied to a second device. The page is protected only
 * for the length of a trap, so the register aperture is plain memory at every
 * other time, exactly as before.
 *
 * The walk waits on the trap the way PGRAPH does -- the next method is not
 * processed until the driver is done with this one -- which also means a
 * second trap can never overwrite TRAPPED_DATA before the handler has read the
 * first. The wait is bounded: a handler that never runs must not stop the
 * pushbuffer for good.
 *
 * RECOMP_PB_NOP_TRAP=0 turns the trap off; the method is then counted as
 * unhandled, as it was before this existed.
 *
 * Windows, and x86-64 Linux / macOS. On Windows the write is single-stepped
 * from a VEH. On x86-64 POSIX the title's SIGSEGV handler hands the fault to
 * nv2a_pgraph_trap_posix_fault (through xbox_PosixMmioFault), which emulates
 * the store with the decoder the APU uses. Elsewhere the trap is off and the
 * method falls through. Only PGRAPH_INTR has register semantics --
 * the other registers on the page are plain stores, which is all a handler of a
 * software method needs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */
#include "nv2a_pgraph_trap.h"

/* Non-zero while a trap is outstanding. PMC_INTR and PGRAPH_INTR then hold a
 * real pending bit, and the busy-bit pass (xbox_Nv2aAckBusyBits), which
 * otherwise holds them at zero, leaves them alone. The lock orders the two. */
volatile LONG g_nv2a_intr_hold;
SRWLOCK g_nv2a_intr_lock = SRWLOCK_INIT;

#if defined(_WIN32) || defined(__x86_64__)
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "../platform/mmio_decode.h"
#endif

extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern void xbox_set_irq_line(uint32_t vector, int level);
extern void xbox_Nv2aAckBusyBits(void);

#define GPU_VECTOR               3u

#define PMC_INTR                 0x000100u
#define PMC_INTR_PGRAPH          0x00001000u   /* bit 12 */
#define PGRAPH_PAGE              0x400000u     /* the 4 KB page with all of these */
#define PGRAPH_PAGE_BYTES        0x1000u
#define PGRAPH_INTR              0x400100u
#define PGRAPH_INTR_ERROR        0x00100000u   /* bit 20 */
#define PGRAPH_NSOURCE           0x400108u
#define PGRAPH_NSOURCE_NOTIFY    0x00000001u
#define PGRAPH_INTR_EN           0x400140u
#define PGRAPH_TRAPPED_ADDR      0x400704u     /* method [12:2], subchannel [18:16] */
#define PGRAPH_TRAPPED_DATA_LOW  0x400708u
#define PGRAPH_FIFO              0x400720u     /* bit 0: FIFO access enabled */

#define TRAP_TIMEOUT_MS          1000u

static volatile uint8_t *s_regs;        /* host address of register offset 0 */
static void   *s_page;                  /* host address of the PGRAPH page   */
static volatile LONG s_armed;           /* page is read-only for a trap      */
static volatile LONG s_fifo_written;    /* the handler touched PGRAPH_FIFO   */

#if defined(_WIN32)
static HANDLE  s_done;                  /* auto-reset: the trap is answered  */
#define DONE_SET()    SetEvent(s_done)
#define DONE_RESET()  ResetEvent(s_done)
#define DONE_WAIT1()  (WaitForSingleObject(s_done, 1) == WAIT_OBJECT_0)
#else
/* A flag, not an event: it is set from inside a signal handler. The wait
 * already polls every millisecond, so nothing is lost. */
static volatile int s_done;
#define DONE_SET()    __atomic_store_n(&s_done, 1, __ATOMIC_RELEASE)
#define DONE_RESET()  __atomic_store_n(&s_done, 0, __ATOMIC_RELEASE)
static int DONE_WAIT1(void)
{
    if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE))
        return 1;
    usleep(1000);
    return __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
}
#endif

/* One write is stepped at a time: the page is writable while it is, and a
 * second thread's write in that window would skip the semantics. Taken on the
 * fault, released on the single step that follows it, on the same thread. */
#if defined(_WIN32)
static SRWLOCK s_step_lock = SRWLOCK_INIT;
#define STEP_LOCK()   AcquireSRWLockExclusive(&s_step_lock)
#define STEP_UNLOCK() ReleaseSRWLockExclusive(&s_step_lock)
#else
/* A spin lock: the fault side runs in a signal handler. Held for one
 * emulated store or one mprotect. */
static volatile int s_step_lock;
#define STEP_LOCK()   while (__atomic_exchange_n(&s_step_lock, 1, __ATOMIC_ACQUIRE)) ;
#define STEP_UNLOCK() __atomic_store_n(&s_step_lock, 0, __ATOMIC_RELEASE)
#endif

static int page_protect(int read_only)
{
#if defined(_WIN32)
    DWORD old;
    return VirtualProtect(s_page, PGRAPH_PAGE_BYTES,
                          read_only ? PAGE_READONLY : PAGE_READWRITE, &old) != 0;
#else
    return mprotect(s_page, PGRAPH_PAGE_BYTES,
                    read_only ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
#endif
}

#if defined(_WIN32)
static RECOMP_TLS struct {
    int      on;
    int      w1c;          /* the write may have touched PGRAPH_INTR  */
    uint32_t intr_before;  /* PGRAPH_INTR before it                   */
} s_step;
#endif

static volatile uint32_t *reg(uint32_t off)
{
    return (volatile uint32_t *)(s_regs + off);
}

/* Does a write of up to 8 bytes at off overlap the 4-byte register at r? */
static int touches(uint32_t off, uint32_t r)
{
    return off + 8u > r && off < r + 4u;
}

/* PMC_INTR's PGRAPH bit and the interrupt line follow what is pending and
 * enabled; the trap is answered once ERROR is clear and, if the handler
 * touched it, FIFO access is back on. */
static void pgraph_update(void)
{
    uint32_t intr = *reg(PGRAPH_INTR);

    if (intr & *reg(PGRAPH_INTR_EN)) {
        InterlockedOr((volatile LONG *)reg(PMC_INTR), (LONG)PMC_INTR_PGRAPH);
    } else {
        InterlockedAnd((volatile LONG *)reg(PMC_INTR), (LONG)~PMC_INTR_PGRAPH);
        xbox_set_irq_line(GPU_VECTOR, 0);
    }
    if (!(intr & PGRAPH_INTR_ERROR)
            && (!s_fifo_written || (*reg(PGRAPH_FIFO) & 1u)))
        DONE_SET();
}

#if defined(_WIN32)

static LONG CALLBACK pgraph_write_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    /* Second half: the write has executed. A write-1-to-clear register was
     * zeroed before it, so what is there now is exactly the bits written. */
    if (code == EXCEPTION_SINGLE_STEP && s_step.on) {
        if (s_step.w1c) {
            uint32_t wrote = *reg(PGRAPH_INTR);
            *reg(PGRAPH_INTR) = s_step.intr_before & ~wrote;
        }
        s_step.on = 0;
        pgraph_update();
        VirtualProtect(s_page, PGRAPH_PAGE_BYTES, PAGE_READONLY, &old);
        ReleaseSRWLockExclusive(&s_step_lock);
        ep->ContextRecord->EFlags &= ~0x100u;         /* clear TF */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION && s_page
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t off;

        if (fault < (uintptr_t)s_page
                || fault >= (uintptr_t)s_page + PGRAPH_PAGE_BYTES)
            return EXCEPTION_CONTINUE_SEARCH;

        AcquireSRWLockExclusive(&s_step_lock);
        if (!s_armed) {
            /* The trap ended while this thread waited: the page is plain
             * memory again, so just run the write. */
            ReleaseSRWLockExclusive(&s_step_lock);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (!VirtualProtect(s_page, PGRAPH_PAGE_BYTES, PAGE_READWRITE, &old)) {
            ReleaseSRWLockExclusive(&s_step_lock);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        off = PGRAPH_PAGE + (uint32_t)(fault - (uintptr_t)s_page);
        s_step.w1c = touches(off, PGRAPH_INTR);
        if (s_step.w1c) {
            s_step.intr_before = *reg(PGRAPH_INTR);
            *reg(PGRAPH_INTR) = 0;
        }
        if (touches(off, PGRAPH_FIFO))
            InterlockedExchange(&s_fifo_written, 1);
        s_step.on = 1;
        ep->ContextRecord->EFlags |= 0x100u;          /* TF: step the write */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else  /* x86-64 POSIX */

static uint64_t page_rd(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    memcpy(&v, (char *)dev + off, size > 8 ? 8 : size);
    return v;
}

static void page_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    memcpy((char *)dev + off, &v, size > 8 ? 8 : size);
}

/* The title's SIGSEGV handler, through xbox_PosixMmioFault. Returns 1 when
 * the fault was a write to the trapped page and the thread may resume. The
 * store is emulated onto the page, opened for the moment, and the register
 * semantics applied as the Windows single step does. */
int nv2a_pgraph_trap_posix_fault(void *ucv, uintptr_t fault)
{
    static int warned;
    mmio_ctx_t c;
    uint32_t off, intr_before = 0;
    int w1c, ok;

    if (!s_page || fault < (uintptr_t)s_page
            || fault >= (uintptr_t)s_page + PGRAPH_PAGE_BYTES)
        return 0;

    STEP_LOCK();
    if (!s_armed) {
        /* The trap ended while this thread waited: the page is plain
         * memory again, so just run the write. */
        STEP_UNLOCK();
        return 1;
    }
    if (!page_protect(0)) {
        STEP_UNLOCK();
        return 0;
    }
    off = (uint32_t)(fault - (uintptr_t)s_page);
    w1c = touches(PGRAPH_PAGE + off, PGRAPH_INTR);
    if (w1c) {
        intr_before = *reg(PGRAPH_INTR);
        *reg(PGRAPH_INTR) = 0;
    }
    if (touches(PGRAPH_PAGE + off, PGRAPH_FIFO))
        InterlockedExchange(&s_fifo_written, 1);

    mmio_ctx_from_ucontext(&c, (ucontext_t *)ucv);
    ok = mmio_emulate(&c, off, s_page, page_rd, page_wr);
    if (!ok) {
        /* An instruction the decoder does not know. Leave the page open and
         * let it run as a plain store; this trap then times out. */
        if (w1c)
            *reg(PGRAPH_INTR) = intr_before;
        if (!warned++) {
            const uint8_t *ip = (const uint8_t *)c.Rip;
            fprintf(stderr, "  [GPU] software-method trap: undecoded write"
                            " at PGRAPH +0x%03X (bytes %02X %02X %02X %02X"
                            " %02X %02X); left as a plain store\n", off,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        }
        STEP_UNLOCK();
        return 1;
    }
    mmio_ctx_to_ucontext((ucontext_t *)ucv, &c);
    if (w1c) {
        uint32_t wrote = *reg(PGRAPH_INTR);
        *reg(PGRAPH_INTR) = intr_before & ~wrote;
    }
    pgraph_update();
    page_protect(1);
    STEP_UNLOCK();
    return 1;
}
#endif

/* The page and the handler, once. A failure turns the trap off for the run:
 * a software method left unhandled is the old behaviour, a trap that cannot
 * see the acknowledgement is a one-second stall per method. */
static int pgraph_trap_init(void)
{
    static int state;   /* 0 not yet, 1 ready, -1 failed */

    if (state)
        return state > 0;
    state = -1;
    s_regs = (volatile uint8_t *)xbox_Nv2aRegPtr(0);
    if (!s_regs)
        return 0;
    s_page = (void *)(s_regs + PGRAPH_PAGE);
#if defined(_WIN32)
    s_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    /* First, so it runs before a title's own crash reporter, which would
     * otherwise print the write as an access violation. */
    if (!s_done || !AddVectoredExceptionHandler(1, pgraph_write_veh)) {
        fprintf(stderr, "  [GPU] software-method trap unavailable; "
                        "software methods stay unhandled\n");
        s_page = NULL;
        return 0;
    }
#else
    /* The title's fault handler has to route the write here; the page must
     * also be whole host pages, which it is with 4 KB pages. */
    if (((uintptr_t)s_page & ((uintptr_t)sysconf(_SC_PAGESIZE) - 1)) != 0
            || sysconf(_SC_PAGESIZE) > (long)PGRAPH_PAGE_BYTES) {
        fprintf(stderr, "  [GPU] software-method trap unavailable (host page"
                        " size); software methods stay unhandled\n");
        s_page = NULL;
        return 0;
    }
#endif
    state = 1;
    return 1;
}

static void set_armed(int on)
{
    STEP_LOCK();
    InterlockedExchange(&s_armed, on);
    page_protect(on);
    STEP_UNLOCK();
}

int nv2a_pgraph_trap_nop(uint32_t subch, uint32_t param)
{
    static int on = -1, verbose = -1;
    static unsigned logged, timeouts;
    unsigned long long t0;
    int done = 0;

    if (on < 0) {
        const char *e = getenv("RECOMP_PB_NOP_TRAP");
        on = !(e && !strcmp(e, "0"));
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    }
    if (!on || !xbox_GetConnectedInterrupt(GPU_VECTOR) || !pgraph_trap_init())
        return 0;
    /* A driver that has not enabled the ERROR interrupt would never be told;
     * on hardware the stream would stop here for good. Leave it unhandled. */
    if (!(*reg(PGRAPH_INTR_EN) & PGRAPH_INTR_ERROR))
        return 0;

    /* The busy-bit pass holds PMC_INTR and PGRAPH_INTR at zero; hold it off
     * them, under its own lock, before anything is made pending. */
    AcquireSRWLockExclusive(&g_nv2a_intr_lock);
    InterlockedExchange(&g_nv2a_intr_hold, 1);
    ReleaseSRWLockExclusive(&g_nv2a_intr_lock);

    *reg(PGRAPH_TRAPPED_ADDR)     = 0x100u | ((subch & 7u) << 16);
    *reg(PGRAPH_TRAPPED_DATA_LOW) = param;
    *reg(PGRAPH_NSOURCE)          = PGRAPH_NSOURCE_NOTIFY;
    *reg(PGRAPH_INTR)             = PGRAPH_INTR_ERROR;   /* the only source modelled */
    InterlockedExchange(&s_fifo_written, 0);
    DONE_RESET();
    set_armed(1);

    t0 = GetTickCount64();
    pgraph_update();
    xbox_set_irq_line(GPU_VECTOR, 1);
    /* The walk's thread is also the one that answers the GPU's other busy and
     * status bits (xbox_Nv2aAckBusyBits) -- kickoffs, and the vertical blank's
     * pending bit in PMC_INTR. It keeps doing that while it waits here: XDK
     * D3D's deferred routine acknowledges a vblank and then spins until
     * PMC_INTR bit 24 drops, and if this thread stopped clearing it, that
     * routine would never get as far as the trapped method. */
    for (;;) {
        done = DONE_WAIT1();
        if (done || GetTickCount64() - t0 >= TRAP_TIMEOUT_MS)
            break;
        xbox_Nv2aAckBusyBits();
    }

    set_armed(0);
    if (!done) {
        /* Name the state: a masked PMC_INTR_EN says the handler's deferred
         * routine never finished, a set ERROR bit that nobody acknowledged,
         * FIFO off that it stopped half-way. */
        if (++timeouts <= 4 || timeouts % 64 == 0)
            fprintf(stderr, "  [GPU] software method 0x%08X not acknowledged "
                            "in %u ms; dropped (#%u: PGRAPH_INTR %08X "
                            "PMC_INTR %08X PMC_INTR_EN %08X FIFO %08X%s)\n",
                    param, TRAP_TIMEOUT_MS, timeouts, *reg(PGRAPH_INTR),
                    *reg(PMC_INTR), *reg(0x000140), *reg(PGRAPH_FIFO),
                    s_fifo_written ? ", FIFO written" : "");
        *reg(PGRAPH_INTR) &= ~PGRAPH_INTR_ERROR;
    }
    *reg(PGRAPH_NSOURCE) = 0;
    pgraph_update();            /* PMC_INTR and the line follow what is left */

    AcquireSRWLockExclusive(&g_nv2a_intr_lock);
    InterlockedExchange(&g_nv2a_intr_hold, 0);
    ReleaseSRWLockExclusive(&g_nv2a_intr_lock);

    if (verbose && logged++ < 8)
        fprintf(stderr, "  [GPU] software method 0x%08X (subch %u) "
                        "acknowledged in %llu ms\n",
                param, subch, GetTickCount64() - t0);
    return 1;
}

#else  /* neither Windows nor x86-64 */

int nv2a_pgraph_trap_posix_fault(void *ucv, uintptr_t fault)
{
    (void)ucv; (void)fault;
    return 0;
}

int nv2a_pgraph_trap_nop(uint32_t subch, uint32_t param)
{
    (void)subch; (void)param;
    return 0;
}

#endif
