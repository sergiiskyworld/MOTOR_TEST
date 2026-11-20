// Native checks for the sense low-pass in include/sense_filter.h.
//
//   cc -I../include -o /tmp/sftest test_sense_filter.c && /tmp/sftest
//
// The bug these guard against cost an evening of blaming the ESC. An integer
// low-pass that rounds its state down on every step never reaches a small
// signal at all: the current sensor read a flat zero below 0.37 A, so the
// calibration had nothing to divide by and kept refusing. Nothing about that
// is visible on hardware - a current sensor reading zero looks exactly like a
// current sensor that is not connected.

#include <stdio.h>

#include "sense_filter.h"

static int failures;

static void expect(const char *name, int ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

// Enough samples that any converging filter has long since settled.
#define SETTLE 4000

static int32_t settled(int32_t sample)
{
    int32_t accum = 0;
    int32_t out   = 0;

    for (int i = 0; i < SETTLE; i++) {
        out = senseFilterStep(&accum, sample);
    }
    return out;
}

static void testSmallSignalsSurvive(void)
{
    // One ADC step is 0.8 mV, so 1 mV is the smallest reading worth asking
    // about, and 5 mV is what 0.5 A looks like through a 92.6 A/V sensor.
    expect("a 1 mV signal is not swallowed", settled(1) == 1);
    expect("0.5 A worth of sense voltage settles exactly", settled(5) == 5);

    // Sweeping the low end states the property itself rather than the few
    // values someone happened to think of.
    int32_t worst = 0;

    for (int32_t mv = 0; mv <= 200; mv++) {
        const int32_t got = settled(mv);
        const int32_t err = got > mv ? got - mv : mv - got;

        if (err > worst) {
            worst = err;
        }
    }
    expect("no signal below 200 mV is lost", worst == 0);
}

static void testLargeSignalsStayExact(void)
{
    expect("a full scale pin reading is exact", settled(3300) == 3300);
    // The biggest number the filter carries: full scale through the current
    // scale, in milliamps.
    expect("a full scale current in mA is exact", settled(305580) == 305580);
}

static void testSilenceReturnsToZero(void)
{
    int32_t accum = 0;

    for (int i = 0; i < SETTLE; i++) {
        senseFilterStep(&accum, 3300);
    }

    int32_t out = 0;

    for (int i = 0; i < SETTLE; i++) {
        out = senseFilterStep(&accum, 0);
    }
    // A residue left here would be frozen in as the sensor's zero offset the
    // next time the operator taps the zeroing button.
    expect("the filter returns all the way to zero", out == 0 && accum == 0);
}

static void testItStillSmooths(void)
{
    int32_t accum = 0;

    // Without this the fix could quietly become a pass-through, and the noise
    // the filter exists to remove would come straight back.
    expect("one sample moves a quarter of the way",
           senseFilterStep(&accum, 1000) == 250);
}

int main(void)
{
    testSmallSignalsSurvive();
    testLargeSignalsStayExact();
    testSilenceReturnsToZero();
    testItStillSmooths();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
