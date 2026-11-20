// Motor thrust test stand - flight controller firmware.
//
// The board is used purely as a programmable ESC driver: no IMU, no attitude
// estimation, no arming dance. A host PC drives it over USB CDC with the line
// protocol described in docs/PROTOCOL.md and gets telemetry back as JSON.
//
// Every number crossing the link is an integer in a fixed unit (millivolts,
// milliamps, per-mille). That keeps float formatting out of the firmware and
// makes the protocol easy to read in a plain serial terminal.

#include <Arduino.h>

#include "config.h"
#include "esc_telem.h"
#include "motor_out.h"
#include "sense_filter.h"
#include "settings_store.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum StandState : uint8_t {
    STATE_SAFE = 0,
    STATE_ARMED,
    STATE_FAULT,
};

// StandSettings and its range rules live in settings_store.h, next to the
// code that writes them to flash.
static StandSettings gSettings = {
    DEF_MAX_THROTTLE,
    DEF_RAMP_PER_S,
    DEF_MOTOR_POLES,
    (uint32_t)(DEF_VBAT_SCALE * 1000.0f),
    (uint32_t)(DEF_CURRENT_SCALE * 1000.0f),
    (int32_t)(DEF_CURRENT_OFFSET * 1000.0f),
    (uint32_t)(DEF_CURRENT_LIMIT * 1000.0f),
    DEF_TEMP_LIMIT,
};

static volatile StandState gState = STATE_SAFE;
static volatile uint32_t   gFaults;

// Throttle pipeline: the host sets a target, the control interrupt slews the
// applied value towards it. Q8 fixed point, because at 2 kHz a 500 per-mille
// per second ramp is only a quarter of a per-mille per tick.
static volatile uint16_t gTargetThrottle[MOTOR_COUNT];
static volatile int32_t  gAppliedQ8[MOTOR_COUNT];
static volatile int32_t  gRampStepQ8 = ((int32_t)DEF_RAMP_PER_S << 8) / CONTROL_HZ;

static volatile uint32_t gLastHostMs;
static bool              gHostSeen;

static uint32_t gBoardVoltageMv;
static uint32_t gBoardCurrentMa;
// The sense pins before any scaling is applied. Reported because calibration
// needs them: the zero offset of the current sensor cannot be recovered from
// the scaled reading, which is clamped at zero and so hides how far below
// zero the raw signal actually sits.
static int32_t  gVSensePinMv;
static int32_t  gISensePinMv;
static uint32_t gReportsSkipped;

// Saving to flash stops the processor for about a second, so it waits for a
// moment when nothing is spinning instead of happening where it is asked for.
static volatile bool gSavePending;

// What the DFU boot flag mechanisms held at boot, published in telemetry so a
// failure to carry the flag across the reset is visible rather than silent.
static uint32_t gBootFlagBkp;
static uint32_t gBootFlagNoinit;
static uint32_t gBootTries;

static HardwareTimer *gControlTimer;

// ---------------------------------------------------------------------------
// Control interrupt
// ---------------------------------------------------------------------------

static void controlTick(void)
{
    const int32_t step  = gRampStepQ8;
    const bool    armed = (gState == STATE_ARMED);

    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        int32_t target = armed ? ((int32_t)gTargetThrottle[m] << 8) : 0;
        int32_t applied = gAppliedQ8[m];

        if (applied < target) {
            applied += step;
            if (applied > target) {
                applied = target;
            }
        } else if (applied > target) {
            applied -= step;
            if (applied < target) {
                applied = target;
            }
        }

        gAppliedQ8[m] = applied;
        motorSetThrottle((MotorId)m, (uint16_t)(applied >> 8));
    }

    motorTick();
}

// Cut power without waiting for the ramp. Used for the emergency stop and for
// every protective cutoff.
static void cutThrottleNow(void)
{
    noInterrupts();
    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        gTargetThrottle[m] = 0;
        gAppliedQ8[m]      = 0;
        motorSetThrottle((MotorId)m, 0);
    }
    interrupts();
}

static void raiseFault(uint32_t bits)
{
    gFaults |= bits;
    gState = STATE_FAULT;
    cutThrottleNow();
}

static bool tryArm(void)
{
    if (gFaults != 0 || gState == STATE_ARMED) {
        return false;
    }
    // Arming with a stale throttle command is the classic way to lose a finger.
    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        if (gTargetThrottle[m] != 0) {
            return false;
        }
    }
    cutThrottleNow();
    gState = STATE_ARMED;
    return true;
}

static void disarm(void)
{
    cutThrottleNow();
    if (gState == STATE_ARMED) {
        gState = STATE_SAFE;
    }
}

// ---------------------------------------------------------------------------
// Analog inputs
// ---------------------------------------------------------------------------

static void sampleAnalog(void)
{
    const uint32_t vRaw = analogRead(PIN_VBAT_SENSE);
    const uint32_t iRaw = analogRead(PIN_CURRENT_SENSE);

    // Rounded, not truncated. Truncation biases every sample down by half a
    // step on average, and half a step of the current sensor is 37 mA.
    const int32_t vPinMv = (int32_t)((vRaw * 3300u + 2047u) / 4095u);
    const int32_t iPinMv = (int32_t)((iRaw * 3300u + 2047u) / 4095u);

    // Light low-pass; the sense lines on a 65 A ESC are noisy.
    // See sense_filter.h for why the state needs a fractional part.
    static int32_t vPinAcc, iPinAcc;

    gVSensePinMv = senseFilterStep(&vPinAcc, vPinMv);
    gISensePinMv = senseFilterStep(&iPinAcc, iPinMv);

    // Filter first, subtract the zero and clamp afterwards - never the other
    // way round. Clamping each raw sample rectifies the noise around zero:
    // the negative half of the swing is flattened while the positive half is
    // kept, so the average comes out above the truth. The bias is a fixed
    // number of millivolts, so it is invisible at 50 A and enormous at 1 A,
    // which is what makes a linear sensor look like it reads exponentially.
    // ArduPilot does not clamp at all, it lets current go negative.
    gBoardVoltageMv = (uint32_t)((int64_t)gVSensePinMv * gSettings.vbatScaleM / 1000);

    int32_t dMv = gISensePinMv - gSettings.currentOffsetMv;

    if (dMv < 0) {
        dMv = 0;
    }
    gBoardCurrentMa = (uint32_t)((int64_t)dMv * gSettings.currentScaleM / 1000);
}

// ---------------------------------------------------------------------------
// Protective cutoffs
// ---------------------------------------------------------------------------

static void checkLimits(void)
{
    if (gState != STATE_ARMED) {
        return;
    }

    const EscTelemetry &esc = motorTelemGet();

    // Only the board's own sensor, never the ESC's. The AM32 targets for this
    // regulator leave MILLIVOLT_PER_AMP at the project-wide default, so the
    // current in its telemetry is an uncalibrated guess - here it reads a
    // steady 143 A with the motor stopped. Feeding that to the cutoff means
    // the stand refuses to spin the moment anyone sets a limit.
    if (gSettings.currentLimitMa > 0 && gBoardCurrentMa > gSettings.currentLimitMa) {
        raiseFault(FAULT_OVERCURRENT);
        return;
    }

    if (gSettings.tempLimitC > 0 && esc.valid && esc.temperatureC >= gSettings.tempLimitC) {
        raiseFault(FAULT_OVERTEMP);
    }
}

static void checkHostLink(void)
{
    if (gState != STATE_ARMED) {
        return;
    }
    if ((millis() - gLastHostMs) > LINK_TIMEOUT_MS) {
        raiseFault(FAULT_LINK_TIMEOUT);
    }
}

// ---------------------------------------------------------------------------
// Indicators
// ---------------------------------------------------------------------------

static void updateIndicators(void)
{
    const uint32_t now   = millis();
    const bool     blink = (now / 100) & 1;

    switch (gState) {
        case STATE_ARMED:
            digitalWrite(PIN_LED_RED, LED_ON);
            digitalWrite(PIN_LED_GREEN, LED_OFF);
            break;
        case STATE_FAULT:
            digitalWrite(PIN_LED_RED, blink ? LED_ON : LED_OFF);
            digitalWrite(PIN_LED_GREEN, LED_OFF);
            break;
        default:
            digitalWrite(PIN_LED_RED, LED_OFF);
            digitalWrite(PIN_LED_GREEN, gHostSeen ? LED_ON : (blink ? LED_ON : LED_OFF));
            break;
    }
}

static void beep(uint16_t ms)
{
    digitalWrite(PIN_BUZZER, HIGH);
    delay(ms);
    digitalWrite(PIN_BUZZER, LOW);
}

// ---------------------------------------------------------------------------
// Host link
// ---------------------------------------------------------------------------

static void sendHello(void)
{
    SerialUSB.printf(
        "{\"hello\":\"%s\",\"version\":\"%s\",\"board\":\"%s\","
        "\"motors\":[1,3],\"protocols\":[",
        FW_NAME, FW_VERSION, FW_BOARD);
    for (uint8_t i = 0; i < PROTO_COUNT; i++) {
        SerialUSB.printf("%s\"%s\"", i ? "," : "", motorProtocolName((Protocol)i));
    }
    SerialUSB.printf("],\"controlHz\":%u,\"linkTimeoutMs\":%u}\n",
                     (unsigned)CONTROL_HZ, (unsigned)LINK_TIMEOUT_MS);
}

static void sendSettings(void)
{
    SerialUSB.printf(
        "{\"settings\":{\"maxThrottle\":%lu,\"rampPerS\":%lu,\"motorPoles\":%lu,"
        "\"vbatScaleM\":%lu,\"currentScaleM\":%lu,\"currentOffsetMv\":%ld,"
        "\"currentLimitMa\":%lu,\"tempLimitC\":%ld,\"saved\":%s}}\n",
        (unsigned long)gSettings.maxThrottle, (unsigned long)gSettings.rampPerS,
        (unsigned long)gSettings.motorPoles, (unsigned long)gSettings.vbatScaleM,
        (unsigned long)gSettings.currentScaleM, (long)gSettings.currentOffsetMv,
        (unsigned long)gSettings.currentLimitMa, (long)gSettings.tempLimitC,
        // Tells the host whether these numbers would survive a power cycle,
        // which is the only thing that makes a calibration worth doing.
        settingsAreStored(&gSettings) ? "true" : "false");
}

static void sendReport(void)
{
    const EscTelemetry &esc = motorTelemGet();

    SerialUSB.printf(
        "{\"t\":%lu,\"state\":%u,\"faults\":%lu,\"proto\":\"%s\","
        "\"thr\":[%u,%u],\"out\":[%u,%u],"
        "\"esc\":{\"ok\":%u,\"ageMs\":%lu,\"tempC\":%d,\"mv\":%lu,\"ma\":%lu,"
        "\"mah\":%u,\"erpm\":%lu,\"rpm\":%lu,\"pkts\":%lu,\"crcErr\":%lu,"
        "\"bytes\":%lu,\"edges\":%lu,\"line\":%u,\"frameErr\":%lu,"
        "\"padUp\":%u,\"padDown\":%u,\"foreign\":%lu},"
        "\"board\":{\"mv\":%lu,\"ma\":%lu,\"vPinMv\":%ld,\"iPinMv\":%ld},\"skipped\":%lu,"
        "\"bootFlag\":{\"bkp\":%lu,\"noinit\":%lu,\"tries\":%lu}}\n",
        (unsigned long)millis(), (unsigned)gState, (unsigned long)gFaults,
        motorProtocolName(motorGetProtocol()),
        (unsigned)(gAppliedQ8[MOTOR_1] >> 8), (unsigned)(gAppliedQ8[MOTOR_3] >> 8),
        (unsigned)motorRawOutput(MOTOR_1), (unsigned)motorRawOutput(MOTOR_3),
        esc.valid ? 1u : 0u,
        (unsigned long)(esc.lastPacketMs ? millis() - esc.lastPacketMs : 0),
        (int)esc.temperatureC, (unsigned long)esc.voltageCv * 10ul,
        (unsigned long)esc.currentCa * 10ul, (unsigned)esc.consumptionMah,
        (unsigned long)esc.eRpm, (unsigned long)motorTelemRpm(gSettings.motorPoles),
        (unsigned long)esc.packetCount, (unsigned long)esc.crcErrors,
        (unsigned long)esc.bytesSeen, (unsigned long)esc.lineEdges,
        (unsigned)esc.lineLevel, (unsigned long)esc.frameErrors,
        (unsigned)esc.probeUp, (unsigned)esc.probeDown,
        (unsigned long)esc.foreignPackets,
        (unsigned long)gBoardVoltageMv, (unsigned long)gBoardCurrentMa,
        (long)gVSensePinMv, (long)gISensePinMv,
        (unsigned long)gReportsSkipped,
        (unsigned long)gBootFlagBkp, (unsigned long)gBootFlagNoinit,
        (unsigned long)gBootTries);
}

static void sendAck(const char *what, bool ok, const char *detail)
{
    if (ok || detail == nullptr) {
        SerialUSB.printf("{\"ack\":\"%s\",\"ok\":%s}\n", what, ok ? "true" : "false");
    } else {
        SerialUSB.printf("{\"ack\":\"%s\",\"ok\":false,\"detail\":\"%s\"}\n", what, detail);
    }
}

// Jump into the STM32 ROM bootloader so the board can be reflashed over DFU
// without reaching for the boot pads.
// Jumping into the ROM bootloader straight from a running application leaves
// the USB core half-configured, so the device drops off the bus without ever
// coming back as DFU. Instead we leave a flag in RAM and reset: the reset
// detaches USB cleanly for the host, and the jump below then happens from a
// pristine machine state, which is what the bootloader expects.
#define BOOT_MAGIC_DFU 0x0DF00DF0u

// Backup SRAM sits in the backup power domain and is explicitly meant to
// survive a reset, unlike the CCM RAM that .noinit lands in on this part.
#define BKPSRAM_FLAG (*(volatile uint32_t *)BKPSRAM_BASE)
// Counts how many times the flag was actually seen after a reset. If this is
// non-zero while the board is running the application, the flag handoff worked
// and it was the jump into the bootloader that fell through.
#define BKPSRAM_TRIES (*(volatile uint32_t *)(BKPSRAM_BASE + 4))

// Kept as a second opinion: written and read alongside the backup SRAM so the
// report can show which of the two actually carried the flag across the reset.
__attribute__((section(".noinit"))) static volatile uint32_t gNoinitFlag;

static void backupSramUnlock(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_BKPSRAM_CLK_ENABLE();
}

static void rebootToDfu(void)
{
    backupSramUnlock();
    BKPSRAM_FLAG = BOOT_MAGIC_DFU;
    gNoinitFlag  = BOOT_MAGIC_DFU;

    SerialUSB.flush();
    delay(50);
    NVIC_SystemReset();
}

// Must run before any peripheral (USB in particular) is brought up.
static void checkDfuRequest(void)
{
    backupSramUnlock();
    gBootFlagBkp    = BKPSRAM_FLAG;
    gBootFlagNoinit = gNoinitFlag;

    // Backup SRAM holds garbage on a cold start, so only trust a plausible
    // count and reset it otherwise.
    if (BKPSRAM_TRIES > 1000u) {
        BKPSRAM_TRIES = 0;
    }
    gBootTries = BKPSRAM_TRIES;

    // Clear both first: one shot, so a failed jump cannot trap the board in a
    // reset loop that needs the BOOT button to escape.
    BKPSRAM_FLAG = 0;
    gNoinitFlag  = 0;

    if (gBootFlagBkp != BOOT_MAGIC_DFU && gBootFlagNoinit != BOOT_MAGIC_DFU) {
        return;
    }
    const uint32_t bootAddr  = 0x1FFF0000u;
    const uint32_t bootStack = *(uint32_t *)bootAddr;
    const uint32_t bootEntry = *(uint32_t *)(bootAddr + 4);

    // Refuse to jump to something that is not a plausible stack pointer and
    // entry point, so a bad read leaves the board booting normally instead of
    // dead on the bus and needing the BOOT button.
    const bool stackOk = (bootStack & 0xFFF00000u) == 0x20000000u ||
                         (bootStack & 0xFFF00000u) == 0x10000000u;
    const bool entryOk = (bootEntry & 0xFFF00000u) == 0x1FF00000u;
    if (!stackOk || !entryOk) {
        return;
    }

    gBootTries = BKPSRAM_TRIES + 1;
    BKPSRAM_TRIES = gBootTries;

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    // Hand the bootloader a quiet machine: nothing enabled, nothing pending.
    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    // On F4 the bootloader is reached by pointing VTOR at it. The SYSCFG
    // memory remap is an F0/F1/F3 mechanism and using it here made an earlier
    // attempt land back in the application.
    SCB->VTOR = bootAddr;
    __DSB();
    __ISB();

    // Interrupts were disabled only to keep a pending one from firing between
    // the VTOR change and the jump. They have to come back on: the ROM
    // bootloader services USB from an interrupt, so arriving with PRIMASK set
    // leaves it never answering enumeration, and the board drops off the bus
    // without reappearing as DFU - indistinguishable from a failed jump.
    __enable_irq();

    __set_MSP(bootStack);
    ((void (*)(void))bootEntry)();

    while (1) { }  // unreachable; keeps the compiler from assuming a return
}

// The ramp is stored as a per-tick step, so it has to be recomputed whenever
// rampPerS changes - including when it arrives from flash at boot.
static void applyRampSetting(void)
{
    gRampStepQ8 = ((int32_t)gSettings.rampPerS << 8) / CONTROL_HZ;
    if (gRampStepQ8 < 1) {
        gRampStepQ8 = 1;
    }
}

static bool applySetting(const char *key, long value)
{
    if (strcmp(key, "maxThrottle") == 0) {
        gSettings.maxThrottle = (value < 0) ? 0 : (uint32_t)value;
    } else if (strcmp(key, "rampPerS") == 0) {
        gSettings.rampPerS = (value < 0) ? 0 : (uint32_t)value;
    } else if (strcmp(key, "motorPoles") == 0) {
        gSettings.motorPoles = (value < 0) ? 0 : (uint32_t)value;
    } else if (strcmp(key, "vbatScaleM") == 0) {
        gSettings.vbatScaleM = (uint32_t)value;
    } else if (strcmp(key, "currentScaleM") == 0) {
        gSettings.currentScaleM = (uint32_t)value;
    } else if (strcmp(key, "currentOffsetMv") == 0) {
        gSettings.currentOffsetMv = (int32_t)value;
    } else if (strcmp(key, "currentLimitMa") == 0) {
        gSettings.currentLimitMa = (value < 0) ? 0 : (uint32_t)value;
    } else if (strcmp(key, "tempLimitC") == 0) {
        gSettings.tempLimitC = (int32_t)value;
    } else {
        return false;
    }
    // One set of range rules for everything that can change a setting: a
    // value from the host and a value read back from flash are held to the
    // same limits, so neither path can leave the board in a state the other
    // would have rejected.
    settingsClamp(&gSettings);
    applyRampSetting();
    return true;
}

static MotorId motorFromLabel(long label, bool *ok)
{
    *ok = true;
    if (label == 1) {
        return MOTOR_1;
    }
    if (label == 3) {
        return MOTOR_3;
    }
    *ok = false;
    return MOTOR_1;
}

static void handleCommand(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    gLastHostMs = millis();
    gHostSeen   = true;

    const char cmd  = *line++;
    char      *args = line;

    switch (toupper((unsigned char)cmd)) {
        case 'K':  // keepalive, already handled by touching gLastHostMs
            break;

        case '?':
            sendHello();
            sendSettings();
            break;

        case 'A':
            sendAck("arm", tryArm(), gFaults ? "faults active" : "throttle must be zero");
            break;

        case 'D':
            disarm();
            sendAck("disarm", true, nullptr);
            break;

        case 'X':
            raiseFault(FAULT_ESTOP);
            sendAck("estop", true, nullptr);
            break;

        case 'R':
            if (gState == STATE_FAULT) {
                cutThrottleNow();
                gFaults = 0;
                gState  = STATE_SAFE;
            }
            sendAck("reset", true, nullptr);
            break;

        case 'T': {
            char *end;
            long  label = strtol(args, &end, 10);
            long  value = strtol(end, nullptr, 10);
            bool  ok;
            MotorId m = motorFromLabel(label, &ok);
            if (!ok) {
                sendAck("throttle", false, "motor must be 1 or 3");
                break;
            }
            if (value < 0) {
                value = 0;
            }
            if ((uint32_t)value > gSettings.maxThrottle) {
                value = (long)gSettings.maxThrottle;
            }
            gTargetThrottle[m] = (uint16_t)value;
            break;
        }

        case 'P': {
            while (*args == ' ') {
                args++;
            }
            Protocol p;
            if (!motorProtocolFromName(args, &p)) {
                sendAck("protocol", false, "unknown protocol");
                break;
            }
            // Never reconfigure the output stage while something can spin.
            disarm();
            gFaults = 0;
            gState  = STATE_SAFE;
            motorSetProtocol(p);
            for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
                gTargetThrottle[m] = 0;
                gAppliedQ8[m]      = 0;
            }
            sendAck("protocol", true, nullptr);
            break;
        }

        case 'S': {
            char key[24];
            long value = 0;
            if (sscanf(args, "%23s %ld", key, &value) == 2 && applySetting(key, value)) {
                sendSettings();
            } else {
                sendAck("setting", false, "bad key or value");
            }
            break;
        }

        case 'W':
            // Never saves on the spot. Erasing flash freezes the processor
            // for about a second, and the current calibration is done with
            // the motor running, so the request waits for a standstill. The
            // host is told which of the two happened.
            if (settingsAreStored(&gSettings)) {
                SerialUSB.printf("{\"save\":\"unchanged\"}\n");
            } else {
                gSavePending = true;
                SerialUSB.printf("{\"save\":\"%s\"}\n",
                                 (gState == STATE_SAFE) ? "queued" : "deferred");
            }
            break;

        case 'C': {
            char *end;
            long  label   = strtol(args, &end, 10);
            long  command = strtol(end, &end, 10);
            long  repeat  = strtol(end, nullptr, 10);
            bool  ok;
            MotorId m = motorFromLabel(label, &ok);
            if (!ok || gState == STATE_ARMED) {
                sendAck("dshot", false, "disarm first, motor must be 1 or 3");
                break;
            }
            ok = motorQueueCommand(m, (uint16_t)command, (uint8_t)repeat);
            sendAck("dshot", ok, ok ? nullptr : "DShot protocol required");
            break;
        }

        case 'L':  // re-probe the telemetry line
            if (gState == STATE_ARMED) {
                sendAck("probe", false, "disarm first");
                break;
            }
            motorTelemProbeAgain();
            sendAck("probe", true, nullptr);
            break;

        case 'Z':
            disarm();
            sendAck("dfu", true, nullptr);
            rebootToDfu();
            break;

        default:
            sendAck("unknown", false, nullptr);
            break;
    }
}

static void pollHost(void)
{
    static char   buffer[96];
    static size_t length;

    while (SerialUSB.available() > 0) {
        const char c = (char)SerialUSB.read();
        if (c == '\n' || c == '\r') {
            if (length > 0) {
                buffer[length] = '\0';
                handleCommand(buffer);
                length = 0;
            }
            continue;
        }
        if (length < sizeof(buffer) - 1) {
            buffer[length++] = c;
        } else {
            length = 0;  // overlong line, drop it rather than split a command
        }
    }
}

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

// 8 MHz crystal -> 168 MHz core with a clean 48 MHz USB clock. Overriding the
// core's weak implementation guarantees the PLL matches this board's HSE.
extern "C" void SystemClock_Config(void)
{
    // Runs before the PLL is raised, so the chip is still in its post-reset
    // state here. That is the closest we can get to how the bootloader is
    // entered by the BOOT0 pin, and it is why the check lives here rather
    // than in setup(): from there, the 168 MHz clock tree is already up and
    // handing the ROM bootloader a clock setup it does not expect leaves it
    // unable to bring USB online.
    checkDfuRequest();

    RCC_OscInitTypeDef osc = {};
    RCC_ClkInitTypeDef clk = {};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 8;
    osc.PLL.PLLN       = 336;
    osc.PLL.PLLP       = RCC_PLLP_DIV2;
    osc.PLL.PLLQ       = 7;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void setup(void)
{
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_LED_RED, LED_OFF);
    digitalWrite(PIN_LED_GREEN, LED_OFF);
    digitalWrite(PIN_BUZZER, LOW);

    analogReadResolution(12);
    pinMode(PIN_VBAT_SENSE, INPUT_ANALOG);
    pinMode(PIN_CURRENT_SENSE, INPUT_ANALOG);

    SerialUSB.begin();

    motorBegin();
    motorTelemBegin();

    gControlTimer = new HardwareTimer(TIM7);
    gControlTimer->setOverflow(CONTROL_HZ, HERTZ_FORMAT);
    gControlTimer->attachInterrupt(controlTick);
    gControlTimer->resume();

    // Calibration coefficients from a previous session, if any. Defaults
    // stay in place when the sector has never been written, which is also
    // what happens after the very first flash of this firmware.
    StandSettings stored;
    if (settingsLoad(&stored)) {
        gSettings = stored;
        settingsClamp(&gSettings);
        applyRampSetting();
    }

    gLastHostMs = millis();
    beep(60);
}

// Writing flash halts instruction fetch, so the whole board stops: the
// control interrupt does not run and the outputs hold their last value. That
// is only acceptable at a standstill, which is what this waits for.
static void serviceSaveRequest(void)
{
    if (!gSavePending || gState != STATE_SAFE) {
        return;
    }
    if (gAppliedQ8[MOTOR_1] != 0 || gAppliedQ8[MOTOR_3] != 0) {
        return;
    }

    gSavePending  = false;
    const bool ok = settingsSave(&gSettings);

    // millis() kept counting through the stall, so without this the link
    // watchdog would see a second of silence that never happened.
    gLastHostMs = millis();

    SerialUSB.printf("{\"save\":\"%s\"}\n", ok ? "done" : "failed");
    sendSettings();
}

void loop(void)
{
    static uint32_t nextAdcMs;
    static uint32_t nextReportMs;
    static StandState lastState = STATE_SAFE;

    const uint32_t now = millis();

    pollHost();
    motorTelemPoll();

    if ((int32_t)(now - nextAdcMs) >= 0) {
        nextAdcMs = now + (1000 / ADC_SAMPLE_HZ);
        sampleAnalog();
    }

    checkHostLink();
    checkLimits();
    serviceSaveRequest();

    if (gState != lastState) {
        if (gState == STATE_ARMED) {
            beep(40);
        }
        lastState = gState;
    }

    if ((int32_t)(now - nextReportMs) >= 0) {
        nextReportMs = now + (1000 / REPORT_HZ);
        // Skip the report rather than block if the host stopped draining the
        // CDC endpoint; blocking here would stall the whole safety loop.
        if (SerialUSB.availableForWrite() >= REPORT_MAX_BYTES) {
            sendReport();
        } else {
            // Surfaced in the next report that does get through, so a
            // threshold that can never be met cannot hide in silence.
            gReportsSkipped++;
        }
    }

    updateIndicators();
}
