// ESC output driver for the two channels the thrust stand uses.
//
// Both outputs happen to be channel 4 of their timer, which keeps the register
// handling uniform: CCR4, CCMR2/OC4M, CCER/CC4E and DIER/CC4DE everywhere.
//
// DMA assignment comes from the STM32F405 request mapping table (RM0090):
//   TIM2_CH4 -> DMA1 stream 6, channel 3
//   TIM3_CH4 -> DMA1 stream 2, channel 5
// Neither collides with USB OTG (no DMA1), SPI1 (DMA2) or UART5_RX (stream 0).
// Both streams sit at index 2 of their status-register half, so the interrupt
// flags live at the same bit offset for both.

#include "motor_out.h"
#include "config.h"

#define DSHOT_FRAME_BITS 16
// 16 data bits plus two zero-width slots, so the line is parked low before the
// timer stops and the ESC sees a clean inter-frame gap.
#define DSHOT_BUF_LEN 18

#define DMA_FLAG_SHIFT 16
#define DMA_FLAG_ALL   (0x3Du << DMA_FLAG_SHIFT)
#define DMA_FLAG_TC    (0x20u << DMA_FLAG_SHIFT)

struct ChannelHw {
    TIM_TypeDef        *tim;
    GPIO_TypeDef       *port;
    uint8_t             bit;
    uint8_t             af;
    DMA_Stream_TypeDef *stream;
    uint8_t             dmaChannel;
    volatile uint32_t  *dmaIfcr;
    IRQn_Type           dmaIrq;
};

static const ChannelHw HW[MOTOR_COUNT] = {
    { TIM2, PIN_M1_PORT, PIN_M1_BIT, PIN_M1_AF, DMA1_Stream6, 3, &DMA1->HIFCR, DMA1_Stream6_IRQn },
    { TIM3, PIN_M3_PORT, PIN_M3_BIT, PIN_M3_AF, DMA1_Stream2, 5, &DMA1->LIFCR, DMA1_Stream2_IRQn },
};

struct ProtocolInfo {
    const char *name;
    bool        dshot;
    uint32_t    rate;  // frame frequency in Hz for PWM, bit rate for DShot
};

static const ProtocolInfo PROTOCOLS[PROTO_COUNT] = {
    { "PWM50",    false,     50 },
    { "PWM100",   false,    100 },
    { "PWM200",   false,    200 },
    { "PWM400",   false,    400 },
    { "DSHOT150", true,  150000 },
    { "DSHOT300", true,  300000 },
    { "DSHOT600", true,  600000 },
};

// DShot is the working configuration of this stand: the ESC is pinned to DShot
// in its own settings, so the board must come up talking DShot on every boot.
// Defaulting to servo PWM meant that any reset left the ESC unable to read the
// signal until the protocol was set again by hand.
static Protocol gProtocol = PROTO_DSHOT300;
static uint32_t gTimerClockHz;
static uint32_t gBitTicks;  // DShot bit period in timer ticks
static uint32_t gTicksT0;   // high time for a zero bit
static uint32_t gTicksT1;   // high time for a one bit

static uint32_t gDshotBuf[MOTOR_COUNT][DSHOT_BUF_LEN];

static volatile uint16_t gThrottle[MOTOR_COUNT];
static volatile uint16_t gRawOutput[MOTOR_COUNT];
static volatile uint16_t gCommand[MOTOR_COUNT];
static volatile uint8_t  gCommandRepeat[MOTOR_COUNT];

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

// TIM2 and TIM3 hang off APB1. When the APB1 prescaler is anything other than
// 1 the timer clock runs at twice PCLK1, which is the case at 168 MHz.
static uint32_t timerClockHz(void)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
    return (ppre1 >= 4) ? pclk1 * 2 : pclk1;
}

static void configurePin(const ChannelHw &hw)
{
    const uint8_t  bit   = hw.bit;
    const uint32_t mask2 = 3u << (bit * 2);

    hw.port->OTYPER  &= ~(1u << bit);                               // push-pull
    hw.port->OSPEEDR  = (hw.port->OSPEEDR & ~mask2) | (3u << (bit * 2));
    // Pull-down keeps the signal line at a defined low while the timer is
    // stopped between DShot frames.
    hw.port->PUPDR    = (hw.port->PUPDR & ~mask2) | (2u << (bit * 2));
    hw.port->AFR[bit >> 3] =
        (hw.port->AFR[bit >> 3] & ~(0xFu << ((bit & 7) * 4))) |
        ((uint32_t)hw.af << ((bit & 7) * 4));
    hw.port->MODER    = (hw.port->MODER & ~mask2) | (2u << (bit * 2));
}

static void timerConfigureCommon(const ChannelHw &hw)
{
    hw.tim->CR1  &= ~TIM_CR1_CEN;
    hw.tim->CCER &= ~TIM_CCER_CC4E;
    // PWM mode 1 with preload, so CCR4 updates are taken on the next period.
    hw.tim->CCMR2 = (hw.tim->CCMR2 & ~(TIM_CCMR2_OC4M | TIM_CCMR2_CC4S)) |
                    (6u << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    hw.tim->CCER |= TIM_CCER_CC4E;
    hw.tim->CR1  |= TIM_CR1_ARPE;
}

static void timerStartPwm(const ChannelHw &hw, uint32_t freqHz)
{
    timerConfigureCommon(hw);
    hw.tim->DIER &= ~TIM_DIER_CC4DE;
    hw.tim->PSC   = (gTimerClockHz / PWM_TICK_HZ) - 1;
    hw.tim->ARR   = (PWM_TICK_HZ / freqHz) - 1;
    hw.tim->CCR4  = PWM_MIN_US * PWM_TICKS_PER_US;
    hw.tim->EGR   = TIM_EGR_UG;
    hw.tim->CR1  |= TIM_CR1_CEN;
}

static void timerPrepareDshot(const ChannelHw &hw)
{
    timerConfigureCommon(hw);
    hw.tim->PSC  = 0;
    hw.tim->ARR  = gBitTicks - 1;
    hw.tim->CCR4 = 0;
    hw.tim->EGR  = TIM_EGR_UG;
    // The counter stays stopped; every frame is kicked off by dshotSendFrame().
}

static void dmaStop(const ChannelHw &hw)
{
    hw.stream->CR &= ~DMA_SxCR_EN;
    while (hw.stream->CR & DMA_SxCR_EN) {
    }
    *hw.dmaIfcr = DMA_FLAG_ALL;
}

static void dmaConfigure(const ChannelHw &hw, uint32_t *buffer)
{
    dmaStop(hw);
    hw.stream->PAR  = (uint32_t)&hw.tim->CCR4;
    hw.stream->M0AR = (uint32_t)buffer;
    hw.stream->NDTR = DSHOT_BUF_LEN;
    hw.stream->FCR  = 0;  // direct mode, no FIFO
    hw.stream->CR   = ((uint32_t)hw.dmaChannel << DMA_SxCR_CHSEL_Pos) |
                    DMA_SxCR_DIR_0 |                // memory to peripheral
                    DMA_SxCR_MINC |
                    (2u << DMA_SxCR_PSIZE_Pos) |    // 32-bit
                    (2u << DMA_SxCR_MSIZE_Pos) |
                    (2u << DMA_SxCR_PL_Pos) |       // high priority
                    DMA_SxCR_TCIE;
    NVIC_SetPriority(hw.dmaIrq, 1);
    NVIC_EnableIRQ(hw.dmaIrq);
}

static void dshotSendFrame(const ChannelHw &hw, uint32_t *buffer, uint16_t frame)
{
    // A frame is ~27 us at DShot600 and the control loop runs at 500 us, so an
    // in-flight transfer here means something is badly wrong. Skip rather than
    // corrupt the buffer mid-DMA.
    if (hw.stream->CR & DMA_SxCR_EN) {
        return;
    }

    for (uint8_t i = 0; i < DSHOT_FRAME_BITS; i++) {
        buffer[i] = (frame & 0x8000) ? gTicksT1 : gTicksT0;
        frame <<= 1;
    }
    buffer[16] = 0;
    buffer[17] = 0;

    *hw.dmaIfcr     = DMA_FLAG_ALL;
    hw.stream->M0AR = (uint32_t)buffer;
    hw.stream->NDTR = DSHOT_BUF_LEN;
    hw.stream->CR  |= DMA_SxCR_EN;

    hw.tim->CNT   = 0;
    hw.tim->DIER |= TIM_DIER_CC4DE;
    hw.tim->CR1  |= TIM_CR1_CEN;
}

static void dshotFrameComplete(const ChannelHw &hw)
{
    *hw.dmaIfcr    = DMA_FLAG_ALL;
    hw.stream->CR &= ~DMA_SxCR_EN;
    hw.tim->DIER  &= ~TIM_DIER_CC4DE;
    hw.tim->CR1   &= ~TIM_CR1_CEN;
    hw.tim->CCR4   = 0;
}

extern "C" void DMA1_Stream6_IRQHandler(void)
{
    dshotFrameComplete(HW[MOTOR_1]);
}

extern "C" void DMA1_Stream2_IRQHandler(void)
{
    dshotFrameComplete(HW[MOTOR_3]);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void motorBegin(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_DMA1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN | RCC_APB1ENR_TIM3EN;

    gTimerClockHz = timerClockHz();

    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        configurePin(HW[m]);
        gThrottle[m]      = 0;
        gRawOutput[m]     = 0;
        gCommandRepeat[m] = 0;
    }

    motorSetProtocol(gProtocol);
}

bool motorSetProtocol(Protocol p)
{
    if (p >= PROTO_COUNT) {
        return false;
    }

    const ProtocolInfo &info = PROTOCOLS[p];

    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        gThrottle[m]      = 0;
        gCommandRepeat[m] = 0;
        dmaStop(HW[m]);
        HW[m].tim->CR1  &= ~TIM_CR1_CEN;
        HW[m].tim->DIER &= ~TIM_DIER_CC4DE;
    }

    if (info.dshot) {
        dshotBitTiming(gTimerClockHz, info.rate, &gBitTicks, &gTicksT0, &gTicksT1);
        for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
            timerPrepareDshot(HW[m]);
            dmaConfigure(HW[m], gDshotBuf[m]);
        }
    } else {
        for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
            NVIC_DisableIRQ(HW[m].dmaIrq);
            timerStartPwm(HW[m], info.rate);
        }
    }

    gProtocol = p;
    return true;
}

Protocol motorGetProtocol(void)
{
    return gProtocol;
}

bool motorProtocolIsDshot(Protocol p)
{
    return (p < PROTO_COUNT) && PROTOCOLS[p].dshot;
}

const char *motorProtocolName(Protocol p)
{
    return (p < PROTO_COUNT) ? PROTOCOLS[p].name : "?";
}

static bool equalsIgnoreCase(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a++) != toupper((unsigned char)*b++)) {
            return false;
        }
    }
    return *a == *b;
}

bool motorProtocolFromName(const char *name, Protocol *out)
{
    for (uint8_t i = 0; i < PROTO_COUNT; i++) {
        if (equalsIgnoreCase(name, PROTOCOLS[i].name)) {
            *out = (Protocol)i;
            return true;
        }
    }
    return false;
}

void motorSetThrottle(MotorId m, uint16_t perMille)
{
    if (m >= MOTOR_COUNT) {
        return;
    }
    gThrottle[m] = (perMille > 1000) ? 1000 : perMille;
}

uint16_t motorGetThrottle(MotorId m)
{
    return (m < MOTOR_COUNT) ? gThrottle[m] : 0;
}

uint16_t motorRawOutput(MotorId m)
{
    return (m < MOTOR_COUNT) ? gRawOutput[m] : 0;
}

bool motorQueueCommand(MotorId m, uint16_t command, uint8_t repeat)
{
    if (m >= MOTOR_COUNT || !motorProtocolIsDshot(gProtocol) || command > 47) {
        return false;
    }
    if (gThrottle[m] != 0) {
        return false;
    }
    gCommand[m]       = command;
    gCommandRepeat[m] = repeat ? repeat : 10;
    return true;
}

bool motorCommandPending(void)
{
    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        if (gCommandRepeat[m] > 0) {
            return true;
        }
    }
    return false;
}

static uint16_t throttleToNative(uint16_t perMille)
{
    return motorProtocolIsDshot(gProtocol) ? throttleToDshot(perMille) : throttleToPwmUs(perMille);
}

void motorTick(void)
{
    const bool dshot = motorProtocolIsDshot(gProtocol);

    // Ask one ESC, and only one, for telemetry. AM32 honours the DShot
    // telemetry-request bit (Src/dshot.c: `if (dpulse[11] == 1) send_telemetry
    // = 1;`), which is what makes this work without the ESC's own timed
    // telemetry. That matters on a 4-in-1: with every regulator transmitting
    // on its own schedule they periodically fall into step and jam each
    // other, and the readings vanish for up to a second at a time. Asking one
    // at a time is how ArduPilot does it, and it removes the collisions
    // rather than coping with them.
    //
    // A request is never sent alongside a special DShot command, which has to
    // arrive unchanged, and never on motor 3, which nothing measures.
    static uint16_t sinceRequest;
    bool            requestTelem = false;

    if (++sinceRequest >= CONTROL_HZ / ESC_TELEM_REQUEST_HZ) {
        sinceRequest = 0;
        requestTelem = true;
    }

    for (uint8_t m = 0; m < MOTOR_COUNT; m++) {
        uint16_t native;
        bool     command = false;

        if (dshot && gCommandRepeat[m] > 0) {
            native = gCommand[m];
            command = true;
            gCommandRepeat[m]--;
        } else {
            native = throttleToNative(gThrottle[m]);
        }

        gRawOutput[m] = native;

        if (dshot) {
            const bool ask = requestTelem && !command && (m == MOTOR_1);
            dshotSendFrame(HW[m], gDshotBuf[m], dshotEncodeFrame(native, ask));
        } else {
            HW[m].tim->CCR4 = native * PWM_TICKS_PER_US;
        }
    }
}
