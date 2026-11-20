#pragma once

#include <Arduino.h>

#include "protocol_bits.h"

#define FW_NAME    "thrust-stand-fc"
#define FW_VERSION "1.0.0"
#define FW_BOARD   "OrqaF405Pro"

// ---------------------------------------------------------------------------
// Pin map, taken from the ArduPilot hwdef for OrqaF405Pro.
// Only M1 and M3 are wired to the stand; they are also the only two outputs on
// this board that the vendor marked BIDIR.
// ---------------------------------------------------------------------------
#define PIN_M1_PORT       GPIOA
#define PIN_M1_BIT        3      // PA3, TIM2_CH4, AF1
#define PIN_M1_AF         1

#define PIN_M3_PORT       GPIOB
#define PIN_M3_BIT        1      // PB1, TIM3_CH4, AF2
#define PIN_M3_AF         2

#define PIN_ESC_TELEM_RX  PD_2   // UART5_RX, the board's dedicated ESC telemetry pad
#define PIN_VBAT_SENSE    PC1    // ADC1_IN11
#define PIN_CURRENT_SENSE PC3    // ADC1_IN13
#define PIN_LED_RED       PA15
#define PIN_LED_GREEN     PB4
#define PIN_BUZZER        PA8

// Both LEDs sink current through the MCU pin, so a low level lights them.
#define LED_ON  LOW
#define LED_OFF HIGH

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define CONTROL_HZ        2000   // ESC frame rate and throttle ramp update rate
#define ADC_SAMPLE_HZ     100
#define REPORT_HZ         50     // telemetry lines pushed to the host

// Worst-case length of one telemetry line. A report is only started when at
// least this much room is free in the USB transmit queue, so the safety loop
// never blocks mid-line waiting for the host. The queue is sized to 1024 bytes
// in platformio.ini; keep this comfortably below that.
#define REPORT_MAX_BYTES  540

// The host must keep talking to us. Losing the link at full throttle is the
// single most dangerous failure on a thrust stand, so the window is short.
#define LINK_TIMEOUT_MS   300

#define ESC_TELEM_BAUD    115200
#define ESC_TELEM_LEN     10     // KISS packet, see docs/ESC_AM32.md
// AM32 sends one packet roughly every 30 ms; anything quieter than 2 ms marks
// a frame boundary, which is how we re-sync without a start delimiter.
#define ESC_TELEM_GAP_US  2000
#define ESC_TELEM_STALE_MS 500
// How often to ask the ESC for a telemetry frame over DShot. Matches the rate
// AM32's own timed telemetry uses, and one frame takes 0.87 ms to come back,
// so the line is busy under 3 % of the time.
#define ESC_TELEM_REQUEST_HZ 30

// Servo PWM timebase. The range itself lives in protocol_bits.h alongside the
// conversion helpers.
#define PWM_TICK_HZ       2000000u   // 0.5 us resolution
#define PWM_TICKS_PER_US  (PWM_TICK_HZ / 1000000u)

// ---------------------------------------------------------------------------
// Defaults for the runtime-configurable limits. All of these can be changed
// from the web UI and are echoed back in the telemetry stream.
// ---------------------------------------------------------------------------
#define DEF_MAX_THROTTLE   1000   // per-mille cap on commanded throttle
#define DEF_RAMP_PER_S     500    // per-mille per second slew limit
#define DEF_MOTOR_POLES    14     // pole count, used to turn eRPM into RPM
#define DEF_VBAT_SCALE     8.3f   // volts per volt at the divider
#define DEF_CURRENT_SCALE  92.6f  // amps per volt at the sense pin
#define DEF_CURRENT_OFFSET 0.0f   // volts at zero current
#define DEF_CURRENT_LIMIT  0.0f   // amps, 0 disables the cutoff
#define DEF_TEMP_LIMIT     0      // degrees C, 0 disables the cutoff

// ---------------------------------------------------------------------------
// Fault bits reported to the host
// ---------------------------------------------------------------------------
#define FAULT_LINK_TIMEOUT (1u << 0)
#define FAULT_ESTOP        (1u << 1)
#define FAULT_OVERCURRENT  (1u << 2)
#define FAULT_OVERTEMP     (1u << 3)
