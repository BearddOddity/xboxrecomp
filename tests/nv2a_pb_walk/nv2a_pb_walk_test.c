/* The pushbuffer walk: nv2a_pb_scan() follows the ring the way the GPU's DMA
 * engine does -- JUMP, CALL / RETURN, and a resync when the title moves GET --
 * and hands every method it meets to the executor, in order.
 *
 * The executor and the guest memory are stubbed: the memory is a small buffer
 * standing in for the contiguous window (physical address P is read at
 * 0x80000000 | P, as the walk does), and the executor records what it was
 * given. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_BYTES 0x10000u

static uint8_t s_mem[MEM_BYTES];

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return (ptrdiff_t)s_mem - (ptrdiff_t)0x80000000u;
}

/* DMA_GET, as the walk publishes it. */
static volatile uint32_t s_get_reg;

/* DMA_PUT, as the title's writer leaves it. The walk re-reads it every
 * command; the test keeps it where kick() says unless the executor hook
 * moves it, which is the title writing while the walk is in progress. */
static volatile uint32_t s_put_reg;

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    if (offset == 0x800044u)
        return &s_get_reg;
    if (offset == 0x800040u)
        return &s_put_reg;
    return NULL;
}

static struct { uint32_t subch, method, param, get; } s_seen[64];
static int s_n;
static uint32_t s_title_moves_get_at;       /* a method that makes the title write GET */
static uint32_t s_title_get;
static uint32_t s_title_moves_put_at;       /* ... and DMA_PUT (more work, or a wrap) */
static uint32_t s_title_put;

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    if (s_n < 64) {
        s_seen[s_n].subch = subch;
        s_seen[s_n].method = method;
        s_seen[s_n].param = param;
        s_seen[s_n].get = s_get_reg;
    }
    s_n++;
    if (method == s_title_moves_get_at)
        s_get_reg = s_title_get;
    if (method == s_title_moves_put_at)
        s_put_reg = s_title_put;
}
void nv2a_pb_exec_report(void) {}
void xbox_Nv2aAckBusyBits(void) {}
void nv2a_pb_exec_walk_begin(void) {}       /* the flip hold's vblank sample */

void nv2a_pb_resync(uint32_t get_phys);
void nv2a_pb_scan(uint32_t put_phys);
int  nv2a_pb_caught_up(void);

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static void put(uint32_t at, uint32_t w) { memcpy(s_mem + at, &w, 4); }

/* increasing-method header: count words follow */
static uint32_t hdr(uint32_t count, uint32_t subch, uint32_t method)
{
    return (count << 18) | (subch << 13) | method;
}

static void expect(int i, uint32_t method, uint32_t param)
{
    CHECK(i < s_n && s_seen[i].method == method && s_seen[i].param == param);
}

/* What the poll thread sees when the title writes DMA_GET: the register holds
 * the value, and it tells the walk. */
static void title_sets_get(uint32_t get)
{
    s_get_reg = get;
    nv2a_pb_resync(get);
}

/* The poll thread's pass: it read DMA_PUT at `put_phys` and calls the walk.
 * The walk re-reads the register, so the test sets it the same way: the
 * title moves PUT mid-walk by the s_title_moves_put_at hook, or by setting
 * the register between passes. */
static void kick(uint32_t put_phys)
{
    s_put_reg = put_phys;
    nv2a_pb_scan(put_phys);
}

int main(void)
{
#ifdef _WIN32
    _putenv_s("RECOMP_PB_EXEC", "1");
    _putenv_s("RECOMP_PB_WALK", "dma");
#else
    setenv("RECOMP_PB_EXEC", "1", 1);
    setenv("RECOMP_PB_WALK", "dma", 1);
#endif

    /* 0x100: two methods, then CALL 0x200 (one method, RETURN), one more
     * method, JUMP 0x400 (one method). PUT is the end of that. */
    title_sets_get(0x100);
    put(0x100, hdr(2, 0, 0x0100)); put(0x104, 11); put(0x108, 12);
    put(0x10C, 0x200u | 2u);                               /* CALL */
    put(0x110, hdr(1, 0, 0x0304)); put(0x114, 14);
    put(0x118, 0x400u | 1u);                               /* JUMP */
    put(0x200, hdr(1, 0, 0x0300)); put(0x204, 13);
    put(0x208, 0x00020000u);                               /* RETURN */
    put(0x400, hdr(1, 0, 0x0308)); put(0x404, 15);

    kick(0x408);
    CHECK(s_n == 5);
    expect(0, 0x0100, 11);
    expect(1, 0x0104, 12);      /* the count walks the method up by 4 */
    expect(2, 0x0300, 13);      /* inside the CALL ... */
    expect(3, 0x0304, 14);      /* ... and back after it */
    expect(4, 0x0308, 15);      /* past the JUMP */

    /* GET is published before each command, so it never runs ahead of the
     * executor (it is the address of the header being executed), and it ends
     * on PUT. */
    CHECK(s_seen[0].get == 0x100);
    CHECK(s_seen[2].get == 0x200);
    CHECK(s_seen[3].get == 0x110);
    CHECK(s_seen[4].get == 0x400);
    CHECK(s_get_reg == 0x408);

    /* A finished segment: the poll thread may mirror the fence. */
    CHECK(nv2a_pb_caught_up() == 1);

    /* Reaching PUT again does nothing: the walk keeps its own GET. */
    kick(0x408);
    CHECK(s_n == 5);

    /* Non-increasing: the same method for every parameter. */
    put(0x408, 0x40000000u | (3u << 18) | 0x0200u);
    put(0x40C, 1); put(0x410, 2); put(0x414, 3);
    kick(0x418);
    CHECK(s_n == 8);
    expect(5, 0x0200, 1);
    expect(6, 0x0200, 2);
    expect(7, 0x0200, 3);

    /* The title resets the ring: the walk starts where GET now is, not where
     * it had got to. */
    put(0x800, hdr(1, 0, 0x0314)); put(0x804, 21);
    title_sets_get(0x800);
    kick(0x808);
    CHECK(s_n == 9);
    expect(8, 0x0314, 21);

    /* The title writes GET while the walk is in the middle of a kick (XDK D3D's
     * ring reset): the walk follows it instead of overwriting it, and the
     * commands it had been about to read are not executed. */
    put(0xA00, hdr(1, 0, 0x0400)); put(0xA04, 31);
    put(0xA08, hdr(1, 0, 0x0404)); put(0xA0C, 32);        /* abandoned */
    put(0xB00, hdr(1, 0, 0x0408)); put(0xB04, 33);
    title_sets_get(0xA00);
    s_title_moves_get_at = 0x0400;
    s_title_get = 0xB00;
    kick(0xB08);
    s_title_moves_get_at = 0;
    CHECK(s_n == 11);
    expect(9, 0x0400, 31);
    expect(10, 0x0408, 33);
    CHECK(s_get_reg == 0xB08);

    /* A jump out of RAM means the walk is reading data as commands: GET snaps
     * to PUT and nothing past it is executed. */
    put(0x900, 0xFFFFFFFCu | 1u);                          /* JUMP 0xFFFFFFFC */
    title_sets_get(0x900);
    kick(0x904);
    CHECK(s_n == 11);
    /* A resync skipped commands it never read: not caught up. */
    CHECK(nv2a_pb_caught_up() == 0);

    put(0x904, hdr(1, 0, 0x0318)); put(0x908, 22);
    kick(0x90C);                                   /* in step again */
    CHECK(s_n == 12);
    expect(11, 0x0318, 22);

    /* A method whose parameters run past PUT stops at PUT, and finishes when
     * PUT moves. Header at 0xC00 promises four parameters and PUT is after the
     * first. The walk must neither carry on through what the ring held before
     * (a replayed lap of old commands) nor forget the method: the words that
     * come next are its parameters, here deliberately shaped like headers, and
     * reading them as headers would decode vertex data as commands. */
    {
        int n0 = s_n;

        title_sets_get(0xC00);
        put(0xC00, hdr(4, 0, 0x0500)); put(0xC04, 41);
        put(0xC08, hdr(1, 0, 0x0600)); put(0xC0C, 43);
        put(0xC10, 0x00000000u);       put(0xC14, hdr(1, 0, 0x0700)); put(0xC18, 71);
        kick(0xC08);
        CHECK(s_n == n0 + 1);
        expect(n0, 0x0500, 41);
        CHECK(s_get_reg == 0xC08);
        /* Stalled inside a method: the segment is not finished, and a fence
         * mirrored now would be a lie. */
        CHECK(nv2a_pb_caught_up() == 0);
        kick(0xC08);                               /* nothing moves */
        CHECK(s_n == n0 + 1);

        kick(0xC14);                               /* PUT moves: three more */
        CHECK(s_n == n0 + 4);
        expect(n0 + 1, 0x0504, hdr(1, 0, 0x0600));
        expect(n0 + 2, 0x0508, 43);
        expect(n0 + 3, 0x050C, 0);
        CHECK(s_get_reg == 0xC14);
        CHECK(nv2a_pb_caught_up() == 1);           /* the method came in whole */

        kick(0xC1C);                               /* and a header after it */
        CHECK(s_n == n0 + 5);
        expect(n0 + 4, 0x0700, 71);
        CHECK(nv2a_pb_caught_up() == 1);
    }


    /* A CALL inside a CALL is an error on the hardware, and the walk stops at
     * PUT rather than overwrite the return address. */
    {
        int n0 = s_n;

        title_sets_get(0xD00);
        put(0xD00, 0xD40u | 2u);                           /* CALL 0xD40 */
        put(0xD04, hdr(1, 0, 0x0708)); put(0xD08, 53);     /* after the call */
        put(0xD40, hdr(1, 0, 0x0700)); put(0xD44, 51);
        put(0xD48, 0xD80u | 2u);                           /* CALL inside CALL */
        put(0xD80, hdr(1, 0, 0x0704)); put(0xD84, 52);     /* never reached */
        put(0xD88, 0x00020000u);
        kick(0xD0C);
        CHECK(s_n == n0 + 1);
        expect(n0, 0x0700, 51);
        CHECK(s_get_reg == 0xD0C);
    }

    /* A jump to itself never reaches PUT; the runaway guard ends the walk. */
    {
        int n0 = s_n;

        title_sets_get(0xE00);
        put(0xE00, 0xE00u | 1u);                           /* JUMP 0xE00 */
        kick(0xE04);
        CHECK(s_n == n0);
        CHECK(s_get_reg == 0xE04);
        put(0xE04, hdr(1, 0, 0x0710)); put(0xE08, 61);
        kick(0xE0C);                               /* in step again */
        CHECK(s_n == n0 + 1);
        expect(n0, 0x0710, 61);
    }

    /* The title queues more work while the walk is in the segment: DMA_PUT
     * moves forward past the value the poll thread read. The walk does not
     * chase it (a walk that chases a writer faster than it rasterises ends
     * its word budget mid-chase and snaps GET over commands it never read);
     * it finishes the segment it was given, and the next pass -- the poll
     * thread sees PUT != last_put -- reads on from there. */
    {
        int n0 = s_n;

        title_sets_get(0xE80);
        put(0xE80, hdr(1, 0, 0x0720)); put(0xE84, 71);
        put(0xE88, hdr(1, 0, 0x0724)); put(0xE8C, 72);
        s_title_moves_put_at = 0x0720;
        s_title_put = 0xE90;                    /* queued during the walk */
        kick(0xE88);                             /* the aim the poll read */
        s_title_moves_put_at = 0;
        CHECK(s_n == n0 + 1);                    /* the walk stopped at it */
        expect(n0, 0x0720, 71);
        CHECK(s_get_reg == 0xE88);
        put(0xE90, 0xE00u | 1u);         /* stale, and past the live writer */
        kick(0xE90);                            /* the next pass reads on */
        CHECK(s_n == n0 + 2);
        expect(n0 + 1, 0x0724, 72);
        CHECK(s_get_reg == 0xE90);
    }

    /* The title wraps the ring while the walk is in the segment: the tail
     * JUMP goes to the ring's start, and DMA_PUT names a low place there.
     * Walking to the stale PUT replays a lap of ring the title is already
     * writing -- half-written words decode as commands (a pair of 16-bit
     * vertex indices, low bits 1 or 2, is a JUMP or CALL): the garage
     * desync, measured as [PB] bad target with the ring rewritten under
     * the walk. The walk must follow the JUMP round, take the new lap's
     * commands, and stop at the live PUT without reading the stale word
     * past it. */
    {
        int n0 = s_n;

        title_sets_get(0xF00);
        put(0xF00, hdr(1, 0, 0x0730)); put(0xF04, 81);
        put(0xF08, 0xE00u | 1u);                           /* tail JUMP */
        put(0xE00, hdr(1, 0, 0x0734)); put(0xE04, 82);
        put(0xE08, 0x282F9D4Du);            /* stale: JUMP out of RAM */
        s_title_moves_put_at = 0x0730;                    /* the wrap lands */
        s_title_put = 0xE08;
        kick(0xF20);                                      /* the stale aim */
        s_title_moves_put_at = 0;
        CHECK(s_n == n0 + 2);
        expect(n0, 0x0730, 81);
        expect(n0 + 1, 0x0734, 82);
        CHECK(s_get_reg == 0xE08);          /* stopped at the live writer */
        CHECK(nv2a_pb_caught_up() == 1);    /* the lap was read whole */
    }


    if (!s_fail)
        printf("nv2a pushbuffer walk: ok\n");
    return s_fail;
}
