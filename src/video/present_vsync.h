/*
 * RECOMP_PRESENT_VSYNC, read the same way by both presenters: the SDL window
 * (fb_present_sdl.c) and the D3D11 path (gpu_present.c).
 *
 * Unset: vsync on. Starting with '0': off. Anything else: on.
 *
 * An empty value is off where the platform can hold one (POSIX). On Windows an
 * empty variable is removed from the environment (_putenv_s and `set NAME=`
 * both do it), so it reads as unset, and vsync stays on.
 */
#ifndef RECOMP_PRESENT_VSYNC_H
#define RECOMP_PRESENT_VSYNC_H

#include <stdlib.h>

static inline int present_vsync_on(void)
{
    const char *e = getenv("RECOMP_PRESENT_VSYNC");

    if (!e)
        return 1;
    return *e && *e != '0';   /* empty: off (POSIX; Windows never sees it) */
}

#endif
