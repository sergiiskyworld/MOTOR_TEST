// Native checks for the shared-telemetry-line rule in include/esc_stream.h.
//
//   cc -I../include -o /tmp/estest test_esc_stream.c && /tmp/estest
//
// What this guards against was measured on the stand: two regulators of one
// 4-in-1 both had interval telemetry on, and their packets arrived
// interleaved. Every other reading came from an idle ESC, so the rpm tile
// flicked between the real figure and zero and the chart came out dotted.
// Both streams were valid packets with good CRCs, so nothing downstream could
// have noticed.

#include <stdio.h>

#include "esc_stream.h"

static int failures;

static void expect(const char *name, int ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

int main(void)
{
    // The case the rule exists for: a neighbour's packet arriving between two
    // packets of the regulator that is actually turning a motor.
    expect("a zero interrupting a spin is another regulator",
           escPacketIsForeign(0, 22100, 30));

    // Nothing may be rejected before something has been accepted, or the
    // stream could never start.
    expect("the first packet is always accepted",
           !escPacketIsForeign(0, 0, 0));
    expect("a zero while nothing is spinning is accepted",
           !escPacketIsForeign(0, 0, 1000));

    // A reading is only ever dropped for being zero. Two regulators both
    // turning motors cannot be separated this way, and pretending otherwise
    // would silently discard real data.
    expect("a non-zero packet is never rejected",
           !escPacketIsForeign(14800, 22100, 30));
    expect("even a wildly different non-zero packet is accepted",
           !escPacketIsForeign(100, 22100, 30));

    // The claim has to expire, otherwise the last spin would latch forever
    // and the stand would never admit the motor had stopped.
    expect("the claim expires exactly at the hold time",
           !escPacketIsForeign(0, 22100, ESC_TELEM_SPIN_HOLD_MS));
    expect("a zero long after the last packet is a real stop",
           !escPacketIsForeign(0, 22100, ESC_TELEM_SPIN_HOLD_MS + 1));
    expect("a zero just inside the hold time is still rejected",
           escPacketIsForeign(0, 22100, ESC_TELEM_SPIN_HOLD_MS - 1));

    // The hold has to outlast the gap between two packets of the same ESC,
    // which AM32 sends every 30 ms, or the rule would let neighbours through
    // whenever one packet went missing to a collision.
    expect("the hold covers several missed packets in a row",
           ESC_TELEM_SPIN_HOLD_MS > 30 * 3);

    printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
