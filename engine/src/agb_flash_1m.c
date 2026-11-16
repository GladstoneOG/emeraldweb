#include "gba/gba.h"
#include "gba/flash_internal.h"
#ifdef WASM_PLATFORM
#include "wasm_platform.h"
#endif

KEEP_SECTION USED static const char AgbLibFlashVersion[] = "FLASH1M_V103";

static const struct FlashSetupInfo *const sSetupInfos[] =
{
    &MX29L010,
    &LE26FV10N1TS,
    &DefaultFlash
};

#ifdef WASM_PLATFORM
static u16 ProgramFlashByte_Wasm(u16 sectorNum, u32 offset, u8 data)
{
    if (gFlash && sectorNum >= gFlash->sector.count)
        return 0x80FF;
    if (gFlash && offset >= gFlash->sector.size)
        return 0x8000;

    SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
    sectorNum %= SECTORS_PER_BANK;
    return WasmFlash_ProgramByte(sectorNum, offset, data);
}

static u16 ProgramFlashSector_Wasm(u16 sectorNum, u8 *src)
{
    if (gFlash && sectorNum >= gFlash->sector.count)
        return 0x80FF;

    SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
    sectorNum %= SECTORS_PER_BANK;
    return WasmFlash_ProgramSector(sectorNum, src);
}

static u16 EraseFlashSector_Wasm(u16 sectorNum)
{
    if (gFlash && sectorNum >= gFlash->sector.count)
        return 0x80FF;

    SwitchFlashBank(sectorNum / SECTORS_PER_BANK);
    sectorNum %= SECTORS_PER_BANK;
    return WasmFlash_EraseSector(sectorNum);
}
#endif

u16 IdentifyFlash(void)
{
    u16 result;
    u16 flashId;
    const struct FlashSetupInfo *const *setupInfo;

    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    flashId = ReadFlashId();

    setupInfo = sSetupInfos;
    result = 1;

    for (;;)
    {
        if ((*setupInfo)->type.ids.separate.makerId == 0)
            break;

        if (flashId == (*setupInfo)->type.ids.joined)
        {
            result = 0;
            break;
        }

        setupInfo++;
    }

#ifdef WASM_PLATFORM
    ProgramFlashByte = ProgramFlashByte_Wasm;
    ProgramFlashSector = ProgramFlashSector_Wasm;
    EraseFlashChip = WasmFlash_EraseChip;
    EraseFlashSector = EraseFlashSector_Wasm;
#else
    ProgramFlashByte = (*setupInfo)->programFlashByte;
    ProgramFlashSector = (*setupInfo)->programFlashSector;
    EraseFlashChip = (*setupInfo)->eraseFlashChip;
    EraseFlashSector = (*setupInfo)->eraseFlashSector;
#endif
    WaitForFlashWrite = (*setupInfo)->WaitForFlashWrite;
    gFlashMaxTime = (*setupInfo)->maxTime;
    gFlash = &(*setupInfo)->type;

    return result;
}

u16 WaitForFlashWrite_Common(u8 phase, u8 *addr, u8 lastData)
{
    u16 result = 0;
    u8 status;

    StartFlashTimer(phase);

    while ((status = PollFlashStatus(addr)) != lastData)
    {
        if (status & 0x20)
        {
            // The write operation exceeded the flash chip's time limit.

            if (PollFlashStatus(addr) == lastData)
                break;

            FLASH_WRITE(0x5555, 0xF0);
            result = phase | 0xA000u;
            break;
        }

        if (gFlashTimeoutFlag)
        {
            if (PollFlashStatus(addr) == lastData)
                break;

            FLASH_WRITE(0x5555, 0xF0);
            result = phase | 0xC000u;
            break;
        }
    }

    StopFlashTimer();

    return result;
}
