// AM32 serial telemetry receiver.
//
// AM32 emits a 10-byte KISS-compatible packet on its telemetry pad roughly
// every 30 ms, with no start delimiter and no framing. Two things let us lock
// onto it reliably: the packet takes ~0.87 ms to transmit at 115200 baud while
// the gap between packets is ~30 ms, and every packet carries a CRC-8.
//
// Layout (Inc/kiss_telemetry.h in the AM32 tree):
//   [0]   temperature, degrees C, signed
//   [1:2] voltage, centivolts, big endian
//   [3:4] current, centiamps, big endian
//   [5:6] consumption, mAh, big endian
//   [7:8] eRPM / 100, big endian
//   [9]   CRC-8, polynomial 0x07, initial value 0
//
// The Arduino core insists on a TX pin for every Uart object and already owns
// UART5_IRQHandler, so neither its wrapper nor a hand-written interrupt is an
// option here. Instead UART5 feeds a circular DMA buffer (UART5_RX is DMA1
// stream 0 channel 4, clear of the two streams the DShot outputs use) and the
// main loop drains it.

#include "esc_telem.h"
#include "config.h"
#include "esc_stream.h"

#define TELEM_DMA_BUF 64

static EscTelemetry gTelem;
static uint8_t      gDmaBuf[TELEM_DMA_BUF];
static uint16_t     gReadPos;
static uint8_t      gPacket[ESC_TELEM_LEN];
static uint8_t      gIndex;
static uint32_t     gLastByteUs;

// Reads the pad as a plain input under each internal pull in turn. Has to run
// before the UART takes the pin over, and is worth the twenty lines: without
// it, a disconnected wire and a wire shorted to ground both show up as "no
// telemetry" and nothing on screen tells them apart.
void probePad(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;

    const uint32_t mask2 = 3u << (2 * 2);
    const uint32_t saved = GPIOD->PUPDR;

    GPIOD->MODER &= ~mask2;  // plain input

    GPIOD->PUPDR = (saved & ~mask2) | (1u << (2 * 2));  // pull up
    for (volatile int i = 0; i < 20000; i++) {
    }
    gTelem.probeUp = (GPIOD->IDR & (1u << 2)) ? 1u : 0u;

    GPIOD->PUPDR = (saved & ~mask2) | (2u << (2 * 2));  // pull down
    for (volatile int i = 0; i < 20000; i++) {
    }
    gTelem.probeDown = (GPIOD->IDR & (1u << 2)) ? 1u : 0u;
}

static void uart5RxBegin(uint32_t baud)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_DMA1EN;
    RCC->APB1ENR |= RCC_APB1ENR_UART5EN;

    // PD2 -> UART5_RX, alternate function 8.
    const uint32_t mask2 = 3u << (2 * 2);
    GPIOD->OTYPER  &= ~(1u << 2);
    GPIOD->OSPEEDR  = (GPIOD->OSPEEDR & ~mask2) | (2u << (2 * 2));
    GPIOD->PUPDR    = (GPIOD->PUPDR & ~mask2) | (1u << (2 * 2));  // idle high
    GPIOD->AFR[0]   = (GPIOD->AFR[0] & ~(0xFu << (2 * 4))) | (8u << (2 * 4));
    GPIOD->MODER    = (GPIOD->MODER & ~mask2) | (2u << (2 * 2));

    UART5->CR1 = 0;
    UART5->BRR = (HAL_RCC_GetPCLK1Freq() + baud / 2) / baud;
    UART5->CR3 = USART_CR3_DMAR;
    UART5->CR1 = USART_CR1_RE | USART_CR1_UE;

    DMA1_Stream0->CR &= ~DMA_SxCR_EN;
    while (DMA1_Stream0->CR & DMA_SxCR_EN) {
    }
    DMA1->LIFCR        = 0x3Du;  // stream 0 flags sit in bits 0..5
    DMA1_Stream0->PAR  = (uint32_t)&UART5->DR;
    DMA1_Stream0->M0AR = (uint32_t)gDmaBuf;
    DMA1_Stream0->NDTR = TELEM_DMA_BUF;
    DMA1_Stream0->FCR  = 0;
    // Peripheral to memory, byte wide, circular, no interrupt.
    DMA1_Stream0->CR = (4u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_CIRC;
    DMA1_Stream0->CR |= DMA_SxCR_EN;

    gReadPos = 0;
}

static void consumeByte(uint8_t b, uint32_t now)
{
    gTelem.bytesSeen++;

    // Bytes drained in the same pass share a timestamp, so a gap only shows up
    // between batches. That is exactly the inter-packet silence we look for.
    if ((uint32_t)(now - gLastByteUs) > ESC_TELEM_GAP_US) {
        gIndex = 0;
    }
    gLastByteUs = now;

    if (gIndex < ESC_TELEM_LEN) {
        gPacket[gIndex++] = b;
    }
    if (gIndex < ESC_TELEM_LEN) {
        return;
    }
    gIndex = 0;

    if (kissCrc8(gPacket, ESC_TELEM_LEN - 1) != gPacket[ESC_TELEM_LEN - 1]) {
        gTelem.crcErrors++;
        return;
    }

    const uint32_t eRpm = (uint32_t)((gPacket[7] << 8) | gPacket[8]) * 100u;

    // See esc_stream.h: several regulators share one telemetry pad and the
    // packet says nothing about which one sent it.
    if (escPacketIsForeign(eRpm, gTelem.eRpm, millis() - gTelem.lastPacketMs)) {
        gTelem.foreignPackets++;
        return;
    }

    gTelem.temperatureC   = (int8_t)gPacket[0];
    gTelem.voltageCv      = (uint16_t)((gPacket[1] << 8) | gPacket[2]);
    gTelem.currentCa      = (uint16_t)((gPacket[3] << 8) | gPacket[4]);
    gTelem.consumptionMah = (uint16_t)((gPacket[5] << 8) | gPacket[6]);
    gTelem.eRpm           = eRpm;
    gTelem.packetCount++;
    gTelem.lastPacketMs = millis();
    gTelem.valid        = true;
}

void motorTelemBegin(void)
{
    memset(&gTelem, 0, sizeof(gTelem));
    gIndex      = 0;
    gLastByteUs = micros();
    probePad();
    uart5RxBegin(ESC_TELEM_BAUD);
}

void motorTelemPoll(void)
{
    // Watching the pad directly is the only way to tell an unconnected TLM
    // wire from a connected one the UART cannot decode. Both leave bytesSeen
    // at zero; only one of them shows edges.
    const uint8_t level = (GPIOD->IDR & (1u << 2)) ? 1u : 0u;

    if (level != gTelem.lineLevel) {
        gTelem.lineLevel = level;
        gTelem.lineEdges++;
    }

    // Counted on the rising edge only. These flags are cleared by the DMA's
    // own read of DR, so polling faster than bytes arrive would otherwise
    // count one bad byte many times over.
    static bool wasBad;
    const uint32_t status = UART5->SR;
    const bool     bad    = (status & (USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0;

    if (bad && !wasBad) {
        gTelem.frameErrors++;
    }
    wasBad = bad;

    // An overrun would otherwise wedge the receiver; reading SR then DR clears
    // it. Only for an overrun: DR belongs to the DMA, and reading it while a
    // byte is waiting would steal that byte out of the stream.
    if (status & USART_SR_ORE) {
        (void)UART5->DR;
    }

    const uint16_t writePos = (uint16_t)(TELEM_DMA_BUF - DMA1_Stream0->NDTR);
    const uint32_t now      = micros();

    while (gReadPos != writePos) {
        consumeByte(gDmaBuf[gReadPos], now);
        gReadPos = (uint16_t)((gReadPos + 1) % TELEM_DMA_BUF);
    }

    if (gTelem.valid && (millis() - gTelem.lastPacketMs) > ESC_TELEM_STALE_MS) {
        gTelem.valid = false;
    }
}

const EscTelemetry &motorTelemGet(void)
{
    return gTelem;
}

// Same probe as at boot, on demand. The one measurement that splits the fault
// in half is "unplug the ESC harness and look again", and that is not worth a
// reflash every time.
void motorTelemProbeAgain(void)
{
    probePad();
    uart5RxBegin(ESC_TELEM_BAUD);

    // Switching the pull off a floating pad swings it, and a downward swing is
    // indistinguishable from a start bit, so the probe hands the UART one
    // garbage byte of its own making. Throw away whatever landed in the buffer
    // during the swing, or the counters report the measurement instead of the
    // line.
    delay(2);
    gReadPos  = (uint16_t)(TELEM_DMA_BUF - DMA1_Stream0->NDTR);
    gIndex    = 0;
    (void)UART5->SR;
    (void)UART5->DR;

    gTelem.bytesSeen   = 0;
    gTelem.frameErrors = 0;
    gTelem.crcErrors   = 0;
    gTelem.packetCount = 0;
    gTelem.lineLevel   = (GPIOD->IDR & (1u << 2)) ? 1u : 0u;
    gTelem.lineEdges   = 0;
}

void motorTelemResetCounters(void)
{
    gTelem.packetCount = 0;
    gTelem.crcErrors   = 0;
    gTelem.bytesSeen   = 0;
}

uint32_t motorTelemRpm(uint8_t poles)
{
    if (poles < 2) {
        return 0;
    }
    return gTelem.eRpm / (poles / 2);
}
