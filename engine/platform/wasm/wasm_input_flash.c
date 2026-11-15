#include "wasm_platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static uint8_t sFlashBank = 0;

void WasmFlash_Init(void)
{
    memset(gWasmFlash, 0xFF, sizeof(gWasmFlash));
    sFlashBank = 0;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_load_flash_from_storage, (uintptr_t dstPtr, int size), {
    try {
        const b64 = localStorage.getItem("pokemon_web_flash_save_v1");
        if (!b64) return;
        const bin = atob(b64);
        const len = Math.min(bin.length, size);
        for (let i = 0; i < len; i++) {
            HEAPU8[dstPtr + i] = bin.charCodeAt(i);
        }
    } catch (_) {}
});

EM_JS(void, js_save_flash_to_storage, (uintptr_t srcPtr, int size), {
    try {
        const chunk = 0x8000;
        let parts = [];
        for (let off = 0; off < size; off += chunk) {
            const sub = HEAPU8.subarray(srcPtr + off, srcPtr + Math.min(size, off + chunk));
            parts.push(String.fromCharCode.apply(null, sub));
        }
        localStorage.setItem("pokemon_web_flash_save_v1", btoa(parts.join("")));
    } catch (_) {}
});
EM_JS(void, js_clear_flash_from_storage, (void), {
    try {
        localStorage.removeItem("pokemon_web_flash_save_v1");
        localStorage.removeItem("pokemon_web_flash_save_meta_v1");
    } catch (_) {}
});
#endif

void WasmFlash_LoadFromStorage(void)
{
#ifdef __EMSCRIPTEN__
    js_load_flash_from_storage((uintptr_t)gWasmFlash, WASM_FLASH_SIZE);
#endif
}

void WasmFlash_SyncToStorage(void)
{
#ifdef __EMSCRIPTEN__
    js_save_flash_to_storage((uintptr_t)gWasmFlash, WASM_FLASH_SIZE);
#endif
}

void WasmFlash_ClearStorage(void)
{
    WasmFlash_Init();
#ifdef __EMSCRIPTEN__
    js_clear_flash_from_storage();
#endif
}

WASM_KEEPALIVE uint8_t *wasm_get_flash_ptr(void)
{
    return gWasmFlash;
}

/* Synchronous Flash 1M replacements for Thumb self-modifying code in agb_flash.c */
static uint32_t WasmFlash_SectorBase(uint16_t sectorNum)
{
    uint32_t bank = (sectorNum >= 16) ? 1u : (uint32_t)sFlashBank;
    uint32_t secInBank = sectorNum & 15u;
    return (bank * 0x10000u) + (secInBank * 0x1000u);
}

void WasmFlash_SwitchBank(uint8_t bankNum)
{
    sFlashBank = bankNum & 1u;
}

void WasmFlash_Read(uint16_t sectorNum, uint32_t offset, uint8_t *dest, uint32_t size)
{
    uint32_t base = WasmFlash_SectorBase(sectorNum) + offset;
    if (base <= WASM_FLASH_SIZE && size <= (WASM_FLASH_SIZE - base)) {
        memcpy(dest, &gWasmFlash[base], size);
    }
}

uint32_t WasmFlash_VerifySectorNBytes(uint16_t sectorNum, const uint8_t *src, uint32_t size)
{
    uint32_t base = WasmFlash_SectorBase(sectorNum);
    uint32_t limit = (size > 0x1000u) ? 0x1000u : size;
    for (uint32_t i = 0; i < limit; i++) {
        if (gWasmFlash[base + i] != src[i])
            return 0x8000u + i;
    }
    return 0;
}

uint32_t WasmFlash_VerifySector(uint16_t sectorNum, const uint8_t *src)
{
    return WasmFlash_VerifySectorNBytes(sectorNum, src, 0x1000u);
}

uint16_t WasmFlash_EraseChip(void)
{
    WasmFlash_ClearStorage();
    return 0;
}

uint16_t WasmFlash_EraseSector(uint16_t sectorNum)
{
    uint32_t base = WasmFlash_SectorBase(sectorNum);
    if (base + 0x1000u <= WASM_FLASH_SIZE) {
        memset(&gWasmFlash[base], 0xFF, 0x1000u);
        WasmFlash_SyncToStorage();
        return 0;
    }
    return 0x8000u;
}

uint16_t WasmFlash_ProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data)
{
    uint32_t base = WasmFlash_SectorBase(sectorNum) + offset;
    if (offset < 0x1000u && base < WASM_FLASH_SIZE) {
        gWasmFlash[base] = data;
        WasmFlash_SyncToStorage();
        return 0;
    }
    return 0x8000u;
}

uint16_t WasmFlash_ProgramSector(uint16_t sectorNum, uint8_t *src)
{
    uint32_t base = WasmFlash_SectorBase(sectorNum);
    if (base + 0x1000u <= WASM_FLASH_SIZE) {
        memcpy(&gWasmFlash[base], src, 0x1000u);
        WasmFlash_SyncToStorage();
        return 0;
    }
    return 0x8000u;
}

static uint8_t ToBcd(int val)
{
    int v = val < 0 ? 0 : (val > 99 ? 99 : val);
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void WasmRtc_GetBcdDateTime(uint8_t out7[7])
{
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt) {
        memset(out7, 0, 7);
        return;
    }
    out7[0] = ToBcd((lt->tm_year + 1900) - 2000);
    out7[1] = ToBcd(lt->tm_mon + 1);
    out7[2] = ToBcd(lt->tm_mday);
    out7[3] = (uint8_t)lt->tm_wday;
    out7[4] = ToBcd(lt->tm_hour);
    out7[5] = ToBcd(lt->tm_min);
    out7[6] = ToBcd(lt->tm_sec);
}

void WasmDebugPrintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
