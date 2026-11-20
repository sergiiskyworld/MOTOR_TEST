#pragma once

#include <Arduino.h>

// Decoded AM32 / KISS serial telemetry packet.
struct EscTelemetry {
    bool     valid;           // a CRC-good packet arrived recently
    uint32_t lastPacketMs;
    int8_t   temperatureC;
    uint16_t voltageCv;       // centivolts
    uint16_t currentCa;       // centiamps
    uint16_t consumptionMah;
    uint32_t eRpm;            // electrical RPM
    uint32_t packetCount;
    uint32_t crcErrors;
    // Raw bytes taken off the wire. Distinguishes a silent line from one that
    // is talking but never forms a valid frame - with only packet and CRC
    // counters, both look identical (zero and zero).
    uint32_t bytesSeen;
    // Diagnostics for the case where bytesSeen stays at zero, which on its own
    // cannot tell "nothing is connected to the pad" from "something is talking
    // and the UART cannot decode it". Reading the pin straight out of the GPIO
    // input register works even while the pin is handed to the UART.
    uint32_t lineEdges;   // transitions seen on the RX pad
    uint8_t  lineLevel;   // its level right now; idle for this UART is high
    uint32_t frameErrors; // framing/noise/parity - the signature of a baud mismatch
    // Taken once at boot, before the UART claims the pad: the level the pin
    // settles at under the internal pull-up, then under the pull-down. Weak
    // internal pulls (~40 k) lose to anything actually driving the line, so
    // the pair says what is on the other end:
    //   1 then 0  - nothing connected, the pin just follows the pull
    //   0 then 0  - something holds it at ground
    //   1 then 1  - something holds it high, which is an idle UART
    uint8_t probeUp;
    uint8_t probeDown;
    // Packets thrown away as belonging to a different regulator. A 4-in-1 has
    // one telemetry pad shared by all four of them and a KISS packet carries
    // nothing to tell them apart, so a non-zero count here means the readings
    // would otherwise be a mixture of several motors.
    uint32_t foreignPackets;
};

void                motorTelemBegin(void);
void                motorTelemPoll(void);
const EscTelemetry &motorTelemGet(void);
void                motorTelemResetCounters(void);
void                motorTelemProbeAgain(void);

// Mechanical RPM for a motor with the given pole count.
uint32_t motorTelemRpm(uint8_t poles);
