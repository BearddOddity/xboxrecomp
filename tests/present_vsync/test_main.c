/* RECOMP_PRESENT_VSYNC: unset -> on; empty -> off; starting with '0' -> off;
 * anything else -> on. Both presenters call present_vsync_on(). */
#include "present_vsync.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void set(const char *value)
{
#ifdef _WIN32
    if (value) _putenv_s("RECOMP_PRESENT_VSYNC", value);
    else _putenv_s("RECOMP_PRESENT_VSYNC", "");
#else
    if (value) setenv("RECOMP_PRESENT_VSYNC", value, 1);
    else unsetenv("RECOMP_PRESENT_VSYNC");
#endif
}

static void check(const char *value, int want)
{
    int got;

    set(value);
    got = present_vsync_on();
    printf("  %-12s -> %s  %s\n", value ? (*value ? value : "(empty)") : "(unset)",
           got ? "on " : "off", got == want ? "PASS" : "FAIL");
    if (got != want) failures++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    check(NULL, 1);      /* unset: on */
#ifdef _WIN32
    check("", 1);        /* Windows removes an empty variable: it reads as unset */
#else
    check("", 0);        /* POSIX: empty is set, so off */
#endif
    check("0", 0);
    check("00", 0);
    check("1", 1);
    check("yes", 1);
    printf(failures ? "FAILED\n" : "all passed\n");
    return failures ? 1 : 0;
}
