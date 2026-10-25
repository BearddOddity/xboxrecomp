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

    {   /* RECOMP_APU_MIXSTAT is set by CMake, so the stage has been counting */
        MCPXMixStats st;
        int k;

        for (k = 0; k < 3; k++) mcpx_apu_note_frame(APU_FRAME_FULL);
        for (k = 0; k < 2; k++) mcpx_apu_note_frame(APU_FRAME_SILENT_COUNTER_OFF);
        mcpx_apu_note_frame(-1);                     /* out of range: ignored */
        mcpx_apu_note_frame(APU_FRAME_KINDS);
        mcpx_apu_mix_stats(&st, 0);
        check(st.peak >= 2.0f, "stats: raw peak is read before the limiter");
        check(st.over_full_scale > 0, "stats: samples over full scale are counted");
        if (c > 0.0f)
            check(st.limited > 0, "stats: limited samples are counted");
        else
            check(st.limited == 0, "stats: nothing limited when the limiter is off");
        check(st.frames[APU_FRAME_FULL] == 3 &&
              st.frames[APU_FRAME_SILENT_COUNTER_OFF] == 2 &&
              st.frames[APU_FRAME_TRAPPED] == 0, "stats: frames counted by kind");
        mcpx_apu_mix_stats(&st, 1);                  /* read and clear */
        mcpx_apu_mix_stats(&st, 0);
        check(st.samples == 0 && st.peak == 0.0f && st.frames[APU_FRAME_FULL] == 0,
              "stats: reset clears everything");
    }

    mcpx_apu_set_master_gain(1.0f);                  /* the setters are safe to call */
    mcpx_apu_set_class_gain(9, 2.0f);                /* out-of-range class ignored */
    mcpx_apu_set_voice_class(-1, 3);
    check(1, "gain setters accept any argument");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
