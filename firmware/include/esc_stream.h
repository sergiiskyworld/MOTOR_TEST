#pragma once

#include <stdbool.h>
#include <stdint.h>

// How long a reported spin keeps its claim on the stream. Long enough to
// bridge several missed packets from the same ESC, which AM32 sends every
// 30 ms, short enough that a real stop is reported at once. Kept here rather
// than in config.h so the rule can be compiled and tested on a PC.
#define ESC_TELEM_SPIN_HOLD_MS 300

// Telling apart regulators that share one telemetry pad.
//
// A 4-in-1 wires all four regulators to the same TEL contact, and the KISS
// packet has no field identifying the sender. Their consumption counters do
// differ, but only by a few mAh, which is well inside the spread of the
// counters themselves, so that is no way to separate them either.
//
// The motor is. While one regulator is reporting a turning motor, a packet
// claiming zero eRPM cannot be its own, because a motor does not stop and
// restart between two packets 30 ms apart. Such a packet is a neighbour
// talking over the same wire, and accepting it makes the readings alternate
// between a loaded ESC and an idle one - on screen that looks exactly like a
// failing sensor.
//
// The claim expires: once nothing has been accepted for ESC_TELEM_SPIN_HOLD_MS,
// a zero is taken at face value, so a real stop is reported promptly.
static inline bool escPacketIsForeign(uint32_t packetERpm,
                                      uint32_t acceptedERpm,
                                      uint32_t msSinceAccepted)
{
    return packetERpm == 0 && acceptedERpm != 0 &&
           msSinceAccepted < ESC_TELEM_SPIN_HOLD_MS;
}
