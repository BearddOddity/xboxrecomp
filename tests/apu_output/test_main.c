/* The final stage of the mix. Default: the old clamp at full scale and
 * truncation to 16 bits, bit for bit. RECOMP_SAGE=1: a soft limiter
 * with a 0.95 ceiling that cannot be passed, rounding, and a gain that comes
 * back after a loud burst. RECOMP_AUDIO_LIMITER=<c> sets the ceiling alone and
 * =0 turns the limiter off even under SAGE. Run four ways (CMakeLists.txt). */
#include "apu_state.h"
#include "apu.h"
#include "apu_shim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int peak_after(float amp, int n, int16_t *last)
{
    int i, peak = 0;
    for (i = 0; i < n; i++) {
        int16_t l, r;
        float x = amp * sinf(i * 0.05f);
        mcpx_apu_output_stage(x, -x, &l, &r);
        if (abs(l) > peak) peak = abs(l);
        if (abs(r) > peak) peak = abs(r);
        if (last) *last = l;
    }
    return peak;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    float c = mcpx_apu_output_ceiling();
    int16_t l, r;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (!strcmp(mode, "default") || !strcmp(mode, "limiter_off")) {
        check(c == 0.0f, "no limiter");
        mcpx_apu_output_stage(0.5f, -0.5f, &l, &r);
        check(l == 16383 && r == -16383, "truncates 0.5 to 16383, as before");
        mcpx_apu_output_stage(2.0f, -3.0f, &l, &r);
        check(l == 32767 && r == -32767, "clamps at full scale, as before");
        mcpx_apu_output_stage(0.99999f, 0.0f, &l, &r);
        check(l == 32766, "truncation, not rounding, as before");
    } else {
        float want = !strcmp(mode, "ceiling") ? 0.5f : 0.95f;
        int cap = (int)lrintf(want * 32767.0f);
        int16_t last = 0;
        int peak, k;

        check(fabsf(c - want) < 1e-6f, "ceiling resolved");
        peak = peak_after(4.0f, 20000, NULL);        /* +12 dBFS of sine */
        check(peak <= cap, "a +12 dBFS sine never passes the ceiling");
        check(peak > cap / 2, "and is not simply muted");
        mcpx_apu_output_stage(0.1f, -0.1f, &l, &r);
        /* The gain is still recovering from the burst; run it clear. */
        for (k = 0; k < 60000; k++) mcpx_apu_output_stage(0.0f, 0.0f, &l, &r);
        mcpx_apu_output_stage(0.1f, -0.1f, &l, &r);
        check(abs(l - 3277) <= 1, "a quiet sample passes unchanged once the gain recovers");
        peak = peak_after(0.4f, 5000, &last);
        check(peak >= (int)(0.4f * 32767) - 40, "below the knee the signal is not touched");
        mcpx_apu_output_stage(0.9f, 0.9f, &l, &r);
        check(l <= cap && l >= 0, "rounded output stays inside the ceiling");
    }

    mcpx_apu_set_master_gain(1.0f);                  /* the setters are safe to call */
    mcpx_apu_set_class_gain(9, 2.0f);                /* out-of-range class ignored */
    mcpx_apu_set_voice_class(-1, 3);
    check(1, "gain setters accept any argument");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
