// Native checks for the pure parts of include/settings_store.h.
//
//   cc -I../include -o /tmp/sstest test_settings_store.c && /tmp/sstest
//
// The flash access itself needs a board, but the two things that decide
// whether stored settings are trusted - the checksum and the range rules -
// are plain arithmetic and are exactly where a silent mistake would hurt:
// a checksum that ignores a field turns a corrupted save into settings the
// board happily boots with.

#include <stdio.h>
#include <string.h>

#include "settings_store.h"

static int failures;

static void expect(const char *name, int ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

static StandSettings sample(void)
{
    StandSettings s;
    s.maxThrottle     = 1000;
    s.rampPerS        = 500;
    s.motorPoles      = 14;
    s.vbatScaleM      = 8300;
    s.currentScaleM   = 92600;
    s.currentOffsetMv = 40;
    s.currentLimitMa  = 0;
    s.tempLimitC      = 0;
    return s;
}

static void testChecksumCoversEveryField(void)
{
    const StandSettings base = sample();
    const uint32_t      want = settingsChecksum(&base);

    expect("checksum is stable for identical settings", settingsChecksum(&base) == want);

    // Walk every byte of the struct and flip it. A checksum that misses a
    // field would let a corrupted value through as valid stored settings.
    int missed = 0;
    for (unsigned i = 0; i < sizeof(StandSettings); i++) {
        StandSettings  altered = base;
        unsigned char *bytes   = (unsigned char *)&altered;
        bytes[i] ^= 0xFFu;
        if (settingsChecksum(&altered) == want) {
            missed++;
        }
    }
    expect("every byte of the struct changes the checksum", missed == 0);

    // No padding: the struct is written to flash word by word, and padding
    // would carry whatever happened to be on the stack into the checksum,
    // making two identical saves compare as different.
    expect("the struct has no padding bytes", sizeof(StandSettings) == 8 * 4);
}

static void testClampRules(void)
{
    StandSettings s = sample();
    s.maxThrottle   = 5000;
    s.rampPerS      = 0;
    s.motorPoles    = 0;
    s.vbatScaleM    = 0;
    s.currentScaleM = 0;
    s.tempLimitC    = -40;
    settingsClamp(&s);

    expect("throttle cap cannot exceed full scale", s.maxThrottle == 1000);
    expect("ramp cannot be zero", s.rampPerS >= 10);
    expect("a motor has at least one pole pair", s.motorPoles >= 2);
    // A scale of zero would peg a whole measurement channel at zero without
    // anything looking broken, which is worse than an obviously wrong number.
    expect("voltage scale cannot be zero", s.vbatScaleM > 0);
    expect("current scale cannot be zero", s.currentScaleM > 0);
    expect("a negative temperature cutoff is disabled, not armed", s.tempLimitC == 0);

    StandSettings many = sample();
    many.motorPoles    = 200;
    settingsClamp(&many);
    expect("an absurd pole count is capped", many.motorPoles == 64);

    StandSettings sane   = sample();
    StandSettings before = sane;
    settingsClamp(&sane);
    expect("valid settings pass through untouched",
           memcmp(&sane, &before, sizeof(sane)) == 0);
}

int main(void)
{
    testChecksumCoversEveryField();
    testClampRules();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
