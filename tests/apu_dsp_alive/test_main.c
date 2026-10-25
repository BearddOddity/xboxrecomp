/* DirectSound's DoWork reads one word of EP memory and, when it is not
 * 0xCCCCCC, "recovers" the DSP: sample counter off, GP/EP reset, a 10 ms
 * stall. GP and EP memory are plain RAM in the aperture, so the word has to
 * be stored there when the aperture is made. Run three ways (CMakeLists.txt):
 * default, =0, and a moved address. */
#include "apu_state.h"
#include "apu_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

#define APERTURE (512u << 10)

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t word(const uint8_t *ap, uint32_t off)
{
    uint32_t w;
    memcpy(&w, ap + off, 4);
    return w;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    uint8_t *ap = (uint8_t *)calloc(1, APERTURE);
    uint32_t at;
    int stored;

    if (!ap) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);

    stored = mcpx_apu_seed_dsp_memory(ap, APERTURE);
    if (!strcmp(mode, "off")) {
        check(apu_dsp_alive_offset() < 0, "=0: feature off");
        check(!stored, "=0: nothing stored");
        check(word(ap, 0x5A018) == 0, "=0: word stays 0");
    } else {
        at = !strcmp(mode, "addr") ? 0x5B020 : 0x5A018;
        check(apu_dsp_alive_offset() == (int64_t)at, "offset resolved");
        check(stored, "word stored");
        check(word(ap, at) == 0xCCCCCC, "word is 0xCCCCCC");
        check(word(ap, at + 4) == 0 && word(ap, at - 4) == 0,
              "neighbours untouched");
        if (!strcmp(mode, "addr"))
            check(word(ap, 0x5A018) == 0, "old address left alone");
    }
    check(!mcpx_apu_seed_dsp_memory(NULL, APERTURE), "NULL aperture refused");
    check(!mcpx_apu_seed_dsp_memory(ap, 0x2000), "aperture too small refused");

    printf("%s\n", failures ? "FAILED" : "all passed");
    free(ap);
    return failures ? 1 : 0;
}
