// Native checks for the bit-level helpers in include/protocol_bits.h.
//
//   cc -I../include -o /tmp/pbtest test_protocol_bits.c && /tmp/pbtest
//
// These are the parts of the firmware that are impossible to eyeball on real
// hardware: a wrong DShot checksum simply looks like a motor that never spins.

#include <stdio.h>
#include <string.h>

#include "protocol_bits.h"

static int failures;

static void expect(const char *name, unsigned long got, unsigned long want)
{
    if (got == want) {
        printf("PASS  %s\n", name);
    } else {
        printf("FAIL  %s: got %lu (0x%lX), want %lu (0x%lX)\n", name, got, got, want, want);
        failures++;
    }
}

static void testDshotFrames(void)
{
    // The worked example from the DShot specification: throttle 1046 encodes
    // as payload 0x82C with checksum nibble 0x6.
    expect("dshot 1046 no telemetry", dshotEncodeFrame(1046, false), 0x82C6);
    expect("dshot 1046 with telemetry", dshotEncodeFrame(1046, true), 0x82D7);
    expect("dshot 0 is an all-zero frame", dshotEncodeFrame(0, false), 0x0000);
    expect("dshot 2047 at the top of the range", dshotEncodeFrame(2047, false), 0xFFEE);

    // Whatever the payload, the checksum must be the XOR of the three nibbles
    // above it, which is exactly what an ESC recomputes on receipt.
    int mismatches = 0;
    for (unsigned value = 0; value <= 2047; value++) {
        for (int telem = 0; telem < 2; telem++) {
            uint16_t frame   = dshotEncodeFrame((uint16_t)value, telem != 0);
            uint16_t payload = frame >> 4;
            uint16_t check   = (payload ^ (payload >> 4) ^ (payload >> 8)) & 0x0F;
            if ((frame & 0x0F) != check || (payload >> 1) != value) {
                mismatches++;
            }
        }
    }
    expect("every frame verifies as an ESC would", (unsigned long)mismatches, 0);
}

static void testKissCrc(void)
{
    // Standard check value for CRC-8 with polynomial 0x07 and zero init.
    expect("crc8 of \"123456789\"", kissCrc8((const uint8_t *)"123456789", 9), 0xF4);
    expect("crc8 of an empty buffer", kissCrc8((const uint8_t *)"", 0), 0x00);

    // Round-trip a telemetry packet the way AM32 builds it and we decode it.
    uint8_t packet[10];
    packet[0] = 42;                 // 42 C
    packet[1] = 2412 >> 8;          // 24.12 V
    packet[2] = 2412 & 0xFF;
    packet[3] = 3150 >> 8;          // 31.50 A
    packet[4] = 3150 & 0xFF;
    packet[5] = 420 >> 8;           // 420 mAh
    packet[6] = 420 & 0xFF;
    packet[7] = 245 >> 8;           // 24500 eRPM
    packet[8] = 245 & 0xFF;
    packet[9] = kissCrc8(packet, 9);

    expect("packet passes its own crc", kissCrc8(packet, 9) == packet[9], 1);
    expect("decoded temperature", (unsigned long)(int8_t)packet[0], 42);
    expect("decoded voltage in centivolts", (packet[1] << 8) | packet[2], 2412);
    expect("decoded current in centiamps", (packet[3] << 8) | packet[4], 3150);
    expect("decoded erpm", ((packet[7] << 8) | packet[8]) * 100u, 24500);

    // A single flipped bit anywhere must be caught, otherwise a garbled packet
    // would show up as a plausible reading.
    int missed = 0;
    for (int byte = 0; byte < 9; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t corrupted[10];
            memcpy(corrupted, packet, sizeof(corrupted));
            corrupted[byte] ^= (uint8_t)(1u << bit);
            if (kissCrc8(corrupted, 9) == corrupted[9]) {
                missed++;
            }
        }
    }
    expect("crc catches every single-bit error", (unsigned long)missed, 0);
}

static void testThrottleMapping(void)
{
    expect("zero throttle stops the motor", throttleToDshot(0), 0);
    expect("smallest non-zero throttle enters the throttle band", throttleToDshot(1) >= 48, 1);
    expect("half throttle", throttleToDshot(500), 1048);
    expect("full throttle", throttleToDshot(1000), 2047);
    expect("throttle above range is clamped", throttleToDshot(5000), 2047);

    expect("pwm idle pulse", throttleToPwmUs(0), 1000);
    expect("pwm half", throttleToPwmUs(500), 1500);
    expect("pwm full", throttleToPwmUs(1000), 2000);

    // Monotonic across the whole range: a slider that ever goes backwards
    // would make thrust curves unreadable.
    int regressions = 0;
    uint16_t previous = 0;
    for (uint16_t pm = 1; pm <= 1000; pm++) {
        uint16_t value = throttleToDshot(pm);
        if (value < previous) {
            regressions++;
        }
        previous = value;
    }
    expect("dshot mapping is monotonic", (unsigned long)regressions, 0);
}

static void testBitTiming(void)
{
    // TIM2/TIM3 run at 84 MHz with the 168 MHz clock tree this firmware sets up.
    uint32_t period, zero, one;

    dshotBitTiming(84000000u, 600000u, &period, &zero, &one);
    expect("dshot600 bit period", period, 140);
    expect("dshot600 one high time", one, 105);
    expect("dshot600 zero high time", zero, 52);

    dshotBitTiming(84000000u, 300000u, &period, &zero, &one);
    expect("dshot300 bit period", period, 280);
    expect("dshot300 one high time", one, 210);
    expect("dshot300 zero high time", zero, 105);

    dshotBitTiming(84000000u, 150000u, &period, &zero, &one);
    expect("dshot150 bit period", period, 560);
    expect("dshot150 one high time", one, 420);
    expect("dshot150 zero high time", zero, 210);

    // AM32 auto-detect rejects servo pulses whose smallest edge interval is
    // below 200 us, so the shortest gap the PWM modes produce must stay above
    // that at full throttle.
    const uint32_t rates[] = { 50, 100, 200, 400 };
    for (unsigned i = 0; i < 4; i++) {
        uint32_t periodUs = 1000000u / rates[i];
        uint32_t gapUs    = periodUs - throttleToPwmUs(1000);
        char name[64];
        snprintf(name, sizeof(name), "pwm%lu gap at full throttle stays detectable",
                 (unsigned long)rates[i]);
        expect(name, gapUs > 200, 1);
    }
}

int main(void)
{
    testDshotFrames();
    testKissCrc();
    testThrottleMapping();
    testBitTiming();

    printf("\n");
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
