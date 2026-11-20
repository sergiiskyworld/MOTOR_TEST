#pragma once

// Single-pole low-pass for the analog sense lines, with the filter state kept
// at sub-unit resolution.
//
// The fraction is not a refinement, it is the whole point. Written the obvious
// way, with the state in whole millivolts:
//
//     state = (state * 3 + sample) / 4;
//
// integer division rounds down on every step, and the filter stops climbing as
// soon as the remaining gap is under four units: the step it wants to take is
// smaller than one, so it takes none. Starting from zero the state settles
// about three units short of the signal and stays there forever.
//
// On the battery divider that is 0.2 % and nobody would notice. On the current
// sense line at 92.6 A per volt those three millivolts are 278 mA of dead
// zone, and any current below 0.37 A reads as exactly zero — which made
// calibrating the current sensor from a small reference current impossible,
// because the stand was dividing by a number the filter had eaten.
//
// Keeping 1/256 of a unit in the state shrinks the stall to under 4/256 of a
// unit, which is far below the 0.8 mV that one step of the 12-bit ADC is
// worth.

#include <stdint.h>

#define SENSE_FILTER_FRAC  256
#define SENSE_FILTER_SHIFT 2  // smoothing factor 1/4

// Advances the filter by one sample and returns the rounded value.
// `accum` holds the state in 1/SENSE_FILTER_FRAC units and must start at zero.
static inline int32_t senseFilterStep(int32_t *accum, int32_t sample)
{
    if (sample < 0) {
        sample = 0;
    }
    *accum += (sample * SENSE_FILTER_FRAC - *accum) >> SENSE_FILTER_SHIFT;
    return (*accum + SENSE_FILTER_FRAC / 2) / SENSE_FILTER_FRAC;
}
