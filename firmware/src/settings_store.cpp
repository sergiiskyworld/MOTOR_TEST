// Settings storage in the last flash sector.

#include <Arduino.h>
#include <string.h>

#include "settings_store.h"

// Sector 11 of the STM32F405RG: the final 128 KB of flash, at 0x080E0000.
// The firmware is about 42 KB and lives in the first sectors, so it cannot
// grow into this one. Reflashing over DFU writes only as many bytes as the
// image contains, which means the calibration survives a firmware update.
//
// Erasing 128 KB to keep 40 bytes is wasteful in space and generous in time,
// but the alternative - the 16 KB sector 3 - sits right behind the code and
// would be one refactor away from being overwritten. Flash tolerates ten
// thousand erase cycles here and calibration happens a few times a year.
#define STORE_SECTOR  FLASH_SECTOR_11
#define STORE_ADDRESS 0x080E0000UL

// Bumped when the layout of StandSettings changes, so an older blob is
// ignored rather than reinterpreted field by field into nonsense.
#define STORE_MAGIC 0x54535431UL  // "TST1"

struct StoredBlob {
    uint32_t      magic;
    StandSettings values;
    uint32_t      checksum;
};

static const StoredBlob *stored(void)
{
    return (const StoredBlob *)STORE_ADDRESS;
}

static bool storedIsValid(void)
{
    const StoredBlob *blob = stored();
    return blob->magic == STORE_MAGIC && blob->checksum == settingsChecksum(&blob->values);
}

bool settingsLoad(StandSettings *out)
{
    if (!storedIsValid()) {
        return false;
    }
    *out = stored()->values;
    return true;
}

bool settingsAreStored(const StandSettings *s)
{
    return storedIsValid() && memcmp(&stored()->values, s, sizeof(*s)) == 0;
}

bool settingsSave(const StandSettings *s)
{
    if (settingsAreStored(s)) {
        // Nothing to do, and skipping the erase spares both the flash and the
        // second of frozen processor that comes with it.
        return true;
    }

    StoredBlob blob;
    blob.magic    = STORE_MAGIC;
    blob.values   = *s;
    blob.checksum = settingsChecksum(&blob.values);

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    FLASH_EraseInitTypeDef erase = {};
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector    = STORE_SECTOR;
    erase.NbSectors = 1;
    // 2.7-3.6 V. The board runs from 3.3 V, and this range is what allows
    // flash to be written a full word at a time.
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    uint32_t failedSector = 0;
    bool     ok           = HAL_FLASHEx_Erase(&erase, &failedSector) == HAL_OK;

    if (ok) {
        const uint32_t *words = (const uint32_t *)&blob;
        for (unsigned i = 0; ok && i < sizeof(blob) / sizeof(uint32_t); i++) {
            ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, STORE_ADDRESS + i * sizeof(uint32_t),
                                   words[i]) == HAL_OK;
        }
    }

    HAL_FLASH_Lock();

    // Read it back rather than trust the return codes: a half-written blob
    // that still reports HAL_OK would come back as valid settings at the next
    // boot, which is worse than having no stored settings at all.
    return ok && settingsAreStored(s);
}
