#pragma once

// Pure bit-level helpers shared by the firmware and the native tests in
// firmware/test. Nothing here touches hardware, so it can be compiled and
// checked on a PC.

#include <stdbool.h>
#include <stdint.h>

#define DSHOT_MIN_THROTTLE 48
#define DSHOT_MAX_THROTTLE 2047

#define PWM_MIN_US 1000
#define PWM_MAX_US 2000

// DShot frame: 11 throttle bits, one telemetry-request bit, then a 4-bit
// checksum formed by XOR-folding the 12 payload bits in nibbles.
static inline uint16_t dshotEncodeFrame(uint16_t value, bool telemetryRequest)
{
    uint16_t packet   = (uint16_t)((value << 1) | (telemetryRequest ? 1u : 0u));
    uint16_t checksum = 0;
    uint16_t scratch  = packet;
    for (uint8_t i = 0; i < 3; i++) {
        checksum ^= scratch;
        scratch >>= 4;
    }
    return (uint16_t)((packet << 4) | (checksum & 0x0F));
}

// CRC-8 with polynomial 0x07 and zero initial value, as used by KISS/BLHeli_32
// telemetry and by AM32's get_crc8().
static inline uint8_t kissCrc8Update(uint8_t crc, uint8_t data)
{
    crc ^= data;
    for (uint8_t i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static inline uint8_t kissCrc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < length; i++) {
        crc = kissCrc8Update(crc, data[i]);
    }
    return crc;
}

// Per-mille throttle to DShot frame value. Zero maps to the dedicated "stopped"
// value rather than to the bottom of the 48..2047 throttle band.
static inline uint16_t throttleToDshot(uint16_t perMille)
{
    if (perMille == 0) {
        return 0;
    }
    if (perMille > 1000) {
        perMille = 1000;
    }
    const uint32_t span = DSHOT_MAX_THROTTLE - DSHOT_MIN_THROTTLE;
    return (uint16_t)(DSHOT_MIN_THROTTLE + (span * perMille + 500) / 1000);
}

static inline uint16_t throttleToPwmUs(uint16_t perMille)
{
    if (perMille > 1000) {
        perMille = 1000;
    }
    return (uint16_t)(PWM_MIN_US + ((PWM_MAX_US - PWM_MIN_US) * (uint32_t)perMille + 500) / 1000);
}

// DShot bit timings for a given timer clock: a one is 75% of the bit period,
// a zero is 37.5%.
static inline void dshotBitTiming(uint32_t timerClockHz, uint32_t bitRate, uint32_t *period,
                                  uint32_t *zeroHigh, uint32_t *oneHigh)
{
    const uint32_t ticks = timerClockHz / bitRate;
    *period   = ticks;
    *oneHigh  = (ticks * 3) / 4;
    *zeroHigh = (ticks * 3) / 8;
}
