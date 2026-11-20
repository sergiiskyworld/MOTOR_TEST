#pragma once

#include <Arduino.h>

// Output protocols offered to the host.
//
// Oneshot125 and Multishot are deliberately absent: AM32's auto-detect keys on
// the smallest edge-to-edge interval and only accepts servo PWM when that
// interval is above 200 us, so a 125..250 us Oneshot pulse is never recognised
// and the ESC would stay silent. See docs/ESC_AM32.md.
enum Protocol : uint8_t {
    PROTO_PWM50 = 0,
    PROTO_PWM100,
    PROTO_PWM200,
    PROTO_PWM400,
    PROTO_DSHOT150,
    PROTO_DSHOT300,
    PROTO_DSHOT600,
    PROTO_COUNT
};

enum MotorId : uint8_t {
    MOTOR_1 = 0,  // PA3, ESC channel 1
    MOTOR_3 = 1,  // PB1, ESC channel 3
    MOTOR_COUNT
};

void motorBegin(void);

bool        motorSetProtocol(Protocol p);
Protocol    motorGetProtocol(void);
bool        motorProtocolIsDshot(Protocol p);
const char *motorProtocolName(Protocol p);
bool        motorProtocolFromName(const char *name, Protocol *out);

// Throttle is always expressed in per-mille of the ESC range (0..1000).
// Zero means "stopped": DShot sends frame value 0, PWM sends the 1000 us idle
// pulse that the ESC needs in order to stay armed.
void     motorSetThrottle(MotorId m, uint16_t perMille);
uint16_t motorGetThrottle(MotorId m);

// Last value actually clocked onto the wire, in protocol-native units
// (microseconds for PWM, 0..2047 for DShot). Useful for sanity-checking wiring.
uint16_t motorRawOutput(MotorId m);

// Queue a DShot special command (beep, spin direction, save settings...).
// The frame is repeated `repeat` times, as the DShot spec requires.
bool motorQueueCommand(MotorId m, uint16_t command, uint8_t repeat);
bool motorCommandPending(void);

// Called from the control-loop interrupt at CONTROL_HZ.
void motorTick(void);
