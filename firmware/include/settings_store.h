// Runtime settings and their storage in flash.
//
// Calibration coefficients are useless if they vanish with the power: the
// operator would have to repeat the whole procedure with a multimeter before
// every session. So the settings live in the last flash sector and are read
// back at boot.
//
// The pure parts - the checksum and the range clamps - sit here as inline
// functions so the native tests in firmware/test can exercise them without a
// board. The flash access itself is in settings_store.cpp.

#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdbool.h>
#include <stdint.h>

// All fields are four bytes wide on purpose: the struct is written to flash
// word by word, and any padding would go into the checksum as uninitialised
// bytes that differ between two otherwise identical saves.
typedef struct {
    uint32_t maxThrottle;      // per-mille cap on the commanded throttle
    uint32_t rampPerS;         // per-mille per second slew limit
    uint32_t motorPoles;       // magnet count, used to convert eRPM to RPM
    uint32_t vbatScaleM;       // milli-units: 8300 means 8.3 V per sensed volt
    uint32_t currentScaleM;    // milli-units: 92600 means 92.6 A per sensed volt
    int32_t  currentOffsetMv;  // sense voltage at zero current
    uint32_t currentLimitMa;   // cutoff, 0 disables
    int32_t  tempLimitC;       // cutoff, 0 disables
} StandSettings;

// FNV-1a. Not a CRC, but the job here is only to tell a complete save from an
// erased sector or an interrupted write, and for that any avalanche hash does.
static inline uint32_t settingsChecksum(const StandSettings *s)
{
    const unsigned char *bytes = (const unsigned char *)s;
    uint32_t             hash  = 2166136261u;

    for (unsigned i = 0; i < sizeof(StandSettings); i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

// Applied after every change and after every load, so a value that arrives
// from the host and one that comes back from flash are held to the same rules.
// A stored blob can outlive the firmware that wrote it, and a scale of zero
// would silently zero out a whole measurement channel.
static inline void settingsClamp(StandSettings *s)
{
    if (s->maxThrottle > 1000u) {
        s->maxThrottle = 1000u;
    }
    if (s->rampPerS < 10u) {
        s->rampPerS = 10u;
    }
    if (s->motorPoles < 2u) {
        s->motorPoles = 2u;
    }
    if (s->motorPoles > 64u) {
        s->motorPoles = 64u;
    }
    if (s->vbatScaleM == 0u) {
        s->vbatScaleM = 1u;
    }
    if (s->currentScaleM == 0u) {
        s->currentScaleM = 1u;
    }
    if (s->tempLimitC < 0) {
        s->tempLimitC = 0;
    }
}

#ifdef __cplusplus
extern "C" {
#endif

// Reads the stored settings. Returns false, leaving *out untouched, when the
// sector has never been written or the contents do not check out.
bool settingsLoad(StandSettings *out);

// Erases and rewrites the storage sector. Takes about a second, during which
// the processor cannot fetch instructions from flash and therefore stops
// dead - interrupts included. Only call it with the motor stopped.
bool settingsSave(const StandSettings *s);

// True when what is in flash is byte for byte what is in memory.
bool settingsAreStored(const StandSettings *s);

#ifdef __cplusplus
}
#endif

#endif  // SETTINGS_STORE_H
