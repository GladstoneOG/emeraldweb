#include "wasm_platform.h"
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define WASM_ALIGN16 __attribute__((aligned(16)))
#else
#define WASM_ALIGN16
#endif

WASM_ALIGN16 uint8_t gWasmEwram[WASM_EWRAM_SIZE];
WASM_ALIGN16 uint8_t gWasmIwram[WASM_IWRAM_SIZE];
WASM_ALIGN16 volatile uint8_t gWasmIoRegs[WASM_IOREG_SIZE];
WASM_ALIGN16 uint8_t gWasmPltt[WASM_PLTT_SIZE];
WASM_ALIGN16 uint8_t gWasmVram[WASM_VRAM_SIZE];
WASM_ALIGN16 uint8_t gWasmOam[WASM_OAM_SIZE];
WASM_ALIGN16 uint8_t gWasmFlash[WASM_FLASH_SIZE];

struct SoundInfo *gWasmSoundInfoPtr = NULL;
volatile uint16_t gWasmIntrCheck = 0;
void *gWasmIntrVector = NULL;

WasmScanlineRegs gWasmScanlineRegs[WASM_DISPLAY_HEIGHT];

static bool sHdma0Active = false;
static uintptr_t sHdma0Src = 0;
static uintptr_t sHdma0Dst = 0;
static uint32_t sHdma0Count = 0;
static bool sHdma0Is32Bit = false;
static bool sHdma0SrcFixed = false;

static inline uint16_t ReadReg16(uint32_t offset)
{
    return (uint16_t)gWasmIoRegs[offset] | ((uint16_t)gWasmIoRegs[offset + 1] << 8);
}

static inline void WriteReg16(uint32_t offset, uint16_t val)
{
    gWasmIoRegs[offset] = (uint8_t)(val & 0xFF);
    gWasmIoRegs[offset + 1] = (uint8_t)(val >> 8);
}

static inline uint32_t ReadReg32(uint32_t offset)
{
    return (uint32_t)gWasmIoRegs[offset]
         | ((uint32_t)gWasmIoRegs[offset + 1] << 8)
         | ((uint32_t)gWasmIoRegs[offset + 2] << 16)
         | ((uint32_t)gWasmIoRegs[offset + 3] << 24);
}

void WasmMmio_Reset(void)
{
    memset(gWasmEwram, 0, sizeof(gWasmEwram));
    memset(gWasmIwram, 0, sizeof(gWasmIwram));
    memset((void *)gWasmIoRegs, 0, sizeof(gWasmIoRegs));
    memset(gWasmPltt, 0, sizeof(gWasmPltt));
    memset(gWasmVram, 0, sizeof(gWasmVram));
    memset(gWasmOam, 0, sizeof(gWasmOam));
    memset(gWasmScanlineRegs, 0, sizeof(gWasmScanlineRegs));
    sHdma0Active = false;

    gWasmIntrCheck = 0;
    /* KEYINPUT (0x130): GBA buttons are active-low, 0x03FF = all released */
    WriteReg16(0x130, 0x03FF);
}

void WasmMmio_CaptureScanline(int y)
{
    if (y < 0 || y >= WASM_DISPLAY_HEIGHT)
        return;

    WasmScanlineRegs *dst = &gWasmScanlineRegs[y];
    dst->dispcnt = ReadReg16(0x00);
    dst->bgcnt[0] = ReadReg16(0x08);
    dst->bgcnt[1] = ReadReg16(0x0A);
    dst->bgcnt[2] = ReadReg16(0x0C);
    dst->bgcnt[3] = ReadReg16(0x0E);
    dst->bghofs[0] = ReadReg16(0x10);
    dst->bgvofs[0] = ReadReg16(0x12);
    dst->bghofs[1] = ReadReg16(0x14);
    dst->bgvofs[1] = ReadReg16(0x16);
    dst->bghofs[2] = ReadReg16(0x18);
    dst->bgvofs[2] = ReadReg16(0x1A);
    dst->bghofs[3] = ReadReg16(0x1C);
    dst->bgvofs[3] = ReadReg16(0x1E);

    dst->bg2pa = (int16_t)ReadReg16(0x20);
    dst->bg2pb = (int16_t)ReadReg16(0x22);
    dst->bg2pc = (int16_t)ReadReg16(0x24);
    dst->bg2pd = (int16_t)ReadReg16(0x26);
    uint32_t raw2x = ReadReg32(0x28);
    uint32_t raw2y = ReadReg32(0x2C);
    dst->bg2x = (int32_t)(raw2x << 4) >> 4;
    dst->bg2y = (int32_t)(raw2y << 4) >> 4;

    dst->bg3pa = (int16_t)ReadReg16(0x30);
    dst->bg3pb = (int16_t)ReadReg16(0x32);
    dst->bg3pc = (int16_t)ReadReg16(0x34);
    dst->bg3pd = (int16_t)ReadReg16(0x36);
    uint32_t raw3x = ReadReg32(0x38);
    uint32_t raw3y = ReadReg32(0x3C);
    dst->bg3x = (int32_t)(raw3x << 4) >> 4;
    dst->bg3y = (int32_t)(raw3y << 4) >> 4;

    dst->win0h = ReadReg16(0x40);
    dst->win1h = ReadReg16(0x42);
    dst->win0v = ReadReg16(0x44);
    dst->win1v = ReadReg16(0x46);
    dst->winin = ReadReg16(0x48);
    dst->winout = ReadReg16(0x4A);
    dst->mosaic = ReadReg16(0x4C);
    dst->bldcnt = ReadReg16(0x50);
    dst->bldalpha = ReadReg16(0x52);
    dst->bldy = ReadReg16(0x54);
}

void WasmMmio_BeginFrame(void)
{
    if ((ReadReg16(0xBA) & 0x8000u) == 0)
        sHdma0Active = false;
}

void WasmMmio_LatchFrameScanlines(void)
{
    for (int y = 0; y < WASM_DISPLAY_HEIGHT; y++) {
        WasmMmio_CaptureScanline(y);
    }

    if (!sHdma0Active || (ReadReg16(0xBA) & 0x8000u) == 0 || sHdma0Src == 0 || sHdma0Dst == 0)
        return;

    uintptr_t ioBase = (uintptr_t)gWasmIoRegs;
    if (sHdma0Dst < ioBase || sHdma0Dst >= ioBase + 0x60)
        return;

    uint32_t regOff = (uint32_t)(sHdma0Dst - ioBase);
    uint32_t step = sHdma0Is32Bit ? 4u : 2u;
    size_t writeBytes = (size_t)sHdma0Count * step;
    if (regOff + writeBytes > 0x60)
        return;

    uint8_t savedIo[0x60];
    memcpy(savedIo, (const void *)gWasmIoRegs, sizeof(savedIo));

    /* HBlank DMA0 transfers occur after each visible scanline (after y=0 -> takes effect on y=1..159). */
    for (int y = 1; y < WASM_DISPLAY_HEIGHT; y++) {
        const uint8_t *lineSrc = (const uint8_t *)sHdma0Src + (sHdma0SrcFixed ? 0 : (size_t)(y - 1) * writeBytes);
        memcpy((void *)&gWasmIoRegs[regOff], lineSrc, writeBytes);
        WasmMmio_CaptureScanline(y);
    }

    memcpy((void *)gWasmIoRegs, savedIo, sizeof(savedIo));
}

void WasmDma_Transfer(int dmaNum, uintptr_t src, uintptr_t dst, uint32_t control)
{
    if ((control & 0x80000000u) == 0) {
        if (dmaNum == 0) {
            sHdma0Active = false;
            WriteReg16(0xBA, (uint16_t)(control >> 16));
        }
        return;
    }

    uint32_t count = control & 0xFFFFu;
    if (count == 0)
        count = (dmaNum == 3) ? 0x10000u : 0x4000u;

    bool is32Bit = (control & (1u << 26)) != 0;
    uint32_t srcCtrl = (control >> 23) & 3u; /* 0=inc, 1=dec, 2=fixed */
    uint32_t dstCtrl = (control >> 21) & 3u; /* 0=inc, 1=dec, 2=fixed, 3=inc/reload */
    uint32_t timing = (control >> 28) & 3u;  /* 0=now, 1=vblank, 2=hblank, 3=special */

    if (dmaNum == 0 && timing == 2u) {
        sHdma0Active = true;
        sHdma0Src = src;
        sHdma0Dst = dst;
        sHdma0Count = count;
        sHdma0Is32Bit = is32Bit;
        sHdma0SrcFixed = (srcCtrl == 2u);
        WriteReg16(0xBA, (uint16_t)(control >> 16));
        return;
    }
    if (dmaNum == 0) {
        sHdma0Active = false;
        WriteReg16(0xBA, 0);
    }

    if (src == 0 || dst == 0)
        return;

    if (is32Bit) {
        const uint32_t *s = (const uint32_t *)src;
        uint32_t *d = (uint32_t *)dst;
        int sStep = (srcCtrl == 2u) ? 0 : (srcCtrl == 1u ? -1 : 1);
        int dStep = (dstCtrl == 2u) ? 0 : (dstCtrl == 1u ? -1 : 1);
        for (uint32_t i = 0; i < count; i++) {
            *d = *s;
            s += sStep;
            d += dStep;
        }
    } else {
        const uint16_t *s = (const uint16_t *)src;
        uint16_t *d = (uint16_t *)dst;
        int sStep = (srcCtrl == 2u) ? 0 : (srcCtrl == 1u ? -1 : 1);
        int dStep = (dstCtrl == 2u) ? 0 : (dstCtrl == 1u ? -1 : 1);
        for (uint32_t i = 0; i < count; i++) {
            *d = *s;
            s += sStep;
            d += dStep;
        }
    }
}
