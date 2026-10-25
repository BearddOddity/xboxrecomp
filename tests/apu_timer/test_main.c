/* RECOMP_SAGE=1 asks Windows for 1 ms timers while the APU is up (timeBeginPeriod)
 * and gives them back at shutdown. At the default 15.6 ms tick Sleep(1) takes
 * about 15 ms, which starves the audio device queue; Hulk: Ultimate
 * Destruction went from 650 to 72 underruns a minute on this alone.
 *
 * A fake device stands in for XAudio2, as in apu_lifecycle. Windows prints
 * what it measured; the assertion is only that with SAGE the APU's lifetime
 * runs at 1 ms, and that without it the APU does not change the tick. If
 * another program already holds a 1 ms tick the baseline is already fast and
 * the SAGE assertion is skipped, since there is nothing to improve. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int dev_open;
int  xa2_init(void)            { dev_open = 1; return 1; }
void xa2_shutdown(void)        { dev_open = 0; }
int  xa2_is_active(void)       { return dev_open; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 6; }
unsigned xa2_underruns(void)   { return 0; }
int  xa2_queued_samples(void)  { return 0; }
uint64_t xa2_samples_played(void) { return 0; }
void xa2_expect_drain(void)    {}
int  xa2_submit_samples(const int16_t *s, int n) { (void)s; (void)n; return 1; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double mean_sleep1_ms(int n)
{
    LARGE_INTEGER f, a, b;
    int i;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (i = 0; i < n; i++) Sleep(1);
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / n;
}

int main(int argc, char **argv)
{
    int sage = argc > 1 && !strcmp(argv[1], "on");
    static uint8_t ram[256u << 10];
    double before, during, after;
    MCPXAPUState *d;

    setvbuf(stdout, NULL, _IONBF, 0);
    before = mean_sleep1_ms(40);
    d = mcpx_apu_init_standalone(ram);
    if (!d) { puts("mcpx_apu_init_standalone failed"); return 2; }
    during = mean_sleep1_ms(40);
    mcpx_apu_shutdown(d);
    after = mean_sleep1_ms(40);
    printf("  Sleep(1) mean: before %.2f ms, APU up %.2f ms, after shutdown %.2f ms\n",
           before, during, after);

    if (sage) {
        if (before < 3.0)
            puts("  (baseline already 1 ms: another program holds the tick; skipped)");
        else {
            check(during < 3.0, "with SAGE the APU's lifetime runs at about 1 ms");
            check(after > during * 2.0, "and the tick is given back at shutdown");
        }
    } else {
        check(during > before * 0.5 || before < 3.0,
              "without SAGE the APU does not change the tick");
    }
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
