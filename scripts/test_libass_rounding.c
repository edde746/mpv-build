// Cross-compile against the patched libass headers and run on ARMv7.
// VCVT (without R), float narrowing, or dropping the overflow fallback changes
// outline coordinates. Compare with the device's lrint in every rounding mode.
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ass_utils.h"

#if !defined(__arm__) || !defined(__ARM_FP) || !(__ARM_FP & 8)
#error This regression must run on 32-bit ARM with double-precision VFP.
#endif

int main(void)
{
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    const double edges[] = {
        0, -0.0, .5, -.5, 1.5, -1.5, 2.5, -2.5,
        2147483647.49, 2147483647.5, 2147483648., -2147483648.5,
        -2147483649., INFINITY, -INFINITY, NAN, 0x1p-1074, -0x1p-1074,
    };
    long (*volatile reference)(double) = lrint;
    uint64_t random = 0x65391c064b10a17;
    unsigned checks = 0;
    fenv_t original;
    if (fegetenv(&original))
        return 1;
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); mode++) {
        if (fesetround(modes[mode]))
            goto fail;
        for (unsigned i = 0; i < 100000; i++) {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            double x;
            if (i < sizeof(edges) / sizeof(*edges))
                x = edges[i];
            else if (i & 1)
                x = (int32_t)random + ((random >> 32) & 15) / 16.;
            else
                memcpy(&x, &random, sizeof(x));

            // Preserve pre-existing exceptions, too, not just a clear FPSCR.
            int prior = (i & 2) ? FE_DIVBYZERO : 0;
            feclearexcept(FE_ALL_EXCEPT);
            feraiseexcept(prior);
            long expected = reference(x);
            int expected_flags = fetestexcept(FE_ALL_EXCEPT);
            feclearexcept(FE_ALL_EXCEPT);
            feraiseexcept(prior);
            long actual = ass_lrint(x);
            int actual_flags = fetestexcept(FE_ALL_EXCEPT);
            if (expected != actual || expected_flags != actual_flags) {
                fprintf(stderr, "FAIL mode=%u x=%a expected=%ld/%x actual=%ld/%x\n",
                        mode, x, expected, expected_flags, actual, actual_flags);
                goto fail;
            }
            checks++;
        }
    }
    fesetenv(&original);
    printf("PASS: %u exact rounding values and exception states on ARMv7\n", checks);
    return 0;
fail:
    fesetenv(&original);
    return 1;
}
