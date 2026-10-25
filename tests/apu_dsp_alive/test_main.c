/* DirectSound's DoWork reads one word of EP memory and, when it is not
 * 0xCCCCCC, "recovers" the DSP: sample counter off, GP/EP reset, a 10 ms
 * stall. The model runs no DSP program, so the word has to be answered.
 * Run three ways (see CMakeLists.txt): default, =0, and a moved address. */
#include "apu_state.h"
#include "apu_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(int argc, char **argv)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    const char *mode = argc > 1 ? argv[1] : "default";
    uint64_t at;

    if (!d) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!strcmp(mode, "off")) {
        check(apu_dsp_alive_offset() < 0, "=0: feature off");
        check(mcpx_apu_mmio_read(d, 0x5A018, 4) == 0, "=0: word reads 0");
    } else {
        at = !strcmp(mode, "addr") ? 0x5B020 : 0x5A018;
        check(apu_dsp_alive_offset() == (int64_t)at, "offset resolved");
        check(mcpx_apu_mmio_read(d, at, 4) == 0xCCCCCC, "dword reads 0xCCCCCC");
        check(mcpx_apu_mmio_read(d, at, 2) == 0xCCCC, "word reads 0xCCCC");
        check(mcpx_apu_mmio_read(d, at + 2, 1) == 0xCC, "byte +2 reads 0xCC");
        check(mcpx_apu_mmio_read(d, at + 3, 1) == 0, "byte +3 reads 0");
        check(mcpx_apu_mmio_read(d, at + 4, 4) == 0, "next word reads 0");
        check(mcpx_apu_mmio_read(d, at - 4, 4) == 0, "previous word reads 0");
        if (!strcmp(mode, "addr"))
            check(mcpx_apu_mmio_read(d, 0x5A018, 4) == 0,
                  "old address no longer answered");
    }
    {   /* a storm warns once and keeps running */
        int i;
        for (i = 0; i < 100; i++)
            apu_note_counter_off(1000 + i);
        check(1, "counter-off storm path runs");
    }
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
