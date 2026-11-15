#include "wasm_platform.h"
#include <math.h>
#include <string.h>

#ifndef WASM_SMOKE_TEST
#include "gba/gba.h"
#else
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

struct BgAffineSrcData {
    s32 texX;
    s32 texY;
    s16 scrX;
    s16 scrY;
    s16 sx;
    s16 sy;
    u16 alpha;
};

struct BgAffineDstData {
    s16 pa;
    s16 pb;
    s16 pc;
    s16 pd;
    s32 dx;
    s32 dy;
};

struct ObjAffineSrcData {
    s16 xScale;
    s16 yScale;
    u16 rotation;
};

struct MultiBootParam;
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * BIOS Memory Copy / Fill Syscalls (CpuSet, CpuFastSet, DMA3 large copy)
 * Parenthesize names when defining to bypass MODERN _Static_assert macros.
 * ------------------------------------------------------------------------- */
void (CpuSet)(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFFu;
    bool fixed = (control & (1u << 24)) != 0;
    bool is32 = (control & (1u << 26)) != 0;

    if (is32) {
        const u32 *s = (const u32 *)src;
        u32 *d = (u32 *)dest;
        u32 fixedVal = *s;
        for (u32 i = 0; i < count; i++) {
            d[i] = fixed ? fixedVal : s[i];
        }
    } else {
        const u16 *s = (const u16 *)src;
        u16 *d = (u16 *)dest;
        u16 fixedVal = *s;
        for (u32 i = 0; i < count; i++) {
            d[i] = fixed ? fixedVal : s[i];
        }
    }
}

void (CpuFastSet)(const void *src, void *dest, u32 control)
{
    /* CpuFastSet rounds count up to a multiple of 8 words (32 bytes) */
    u32 count = (control & 0x1FFFFFu);
    count = (count + 7u) & ~7u;
    bool fixed = (control & (1u << 24)) != 0;

    const u32 *s = (const u32 *)src;
    u32 *d = (u32 *)dest;
    u32 fixedVal = *s;
    for (u32 i = 0; i < count; i++) {
        d[i] = fixed ? fixedVal : s[i];
    }
}

void FastUnsafeCopy32(void *dest, const void *src, u32 size)
{
    memcpy(dest, src, size);
}

void Dma3CopyLarge32_(const void *src, void *dest, u32 size)
{
    memcpy(dest, src, size);
}

void Dma3CopyLarge16_(const void *src, void *dest, u32 size)
{
    memcpy(dest, src, size);
}

void Dma3FillLarge32_(u32 value, void *dest, u32 size)
{
    u32 *d = (u32 *)dest;
    u32 words = size / 4u;
    for (u32 i = 0; i < words; i++)
        d[i] = value;
}

void Dma3FillLarge16_(u16 value, void *dest, u32 size)
{
    u16 *d = (u16 *)dest;
    u32 halfs = size / 2u;
    for (u32 i = 0; i < halfs; i++)
        d[i] = value;
}

/* -------------------------------------------------------------------------
 * BIOS Decompression Syscalls (LZ77, RL, Diff8/16, BitUnPack, Huff)
 * VRAM variants write 16-bit halfwords in real GBA hardware; here memcpy/u16
 * stores behave identically in WASM flat memory.
 * ------------------------------------------------------------------------- */
void LZ77UnCompWram(const u32 *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = (u8 *)dest;
    u32 header = (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
    u32 size = header >> 8;
    size_t srcPos = 4;
    size_t dstPos = 0;

    while (dstPos < size) {
        u8 flags = in[srcPos++];
        for (int bit = 0; bit < 8 && dstPos < size; bit++) {
            if (flags & (0x80 >> bit)) {
                u8 b1 = in[srcPos++];
                u8 b2 = in[srcPos++];
                u32 len = (u32)(b1 >> 4) + 3u;
                u32 disp = (((u32)(b1 & 0x0F) << 8) | (u32)b2) + 1u;
                for (u32 i = 0; i < len && dstPos < size; i++) {
                    out[dstPos] = out[dstPos - disp];
                    dstPos++;
                }
            } else {
                out[dstPos++] = in[srcPos++];
            }
        }
    }
}

void LZ77UnCompVram(const u32 *src, void *dest)
{
    LZ77UnCompWram(src, dest);
}

void LZ77UnCompWRAMOptimized(const u32 *src, void *dest)
{
    LZ77UnCompWram(src, dest);
}

void LZ77UnCompVRAMOptimized(const u32 *src, void *dest)
{
    LZ77UnCompWram(src, dest);
}

void RLUnCompWram(const u32 *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = (u8 *)dest;
    u32 header = (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
    u32 size = header >> 8;
    size_t srcPos = 4;
    size_t dstPos = 0;

    while (dstPos < size) {
        u8 flag = in[srcPos++];
        if (flag & 0x80) {
            u32 runLen = (u32)(flag & 0x7F) + 3u;
            u8 val = in[srcPos++];
            for (u32 i = 0; i < runLen && dstPos < size; i++)
                out[dstPos++] = val;
        } else {
            u32 rawLen = (u32)(flag & 0x7F) + 1u;
            for (u32 i = 0; i < rawLen && dstPos < size; i++)
                out[dstPos++] = in[srcPos++];
        }
    }
}

void RLUnCompVram(const u32 *src, void *dest)
{
    RLUnCompWram(src, dest);
}

void HuffUnComp(const void *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u32 header = (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
    u8 bitDepth = header & 0x0Fu;
    u32 decompSize = header >> 8;
    u8 treeSize = (u8)((in[4] + 1) * 2);
    const u8 *tree = &in[5];
    const u8 *stream = &in[4 + treeSize];

    u32 *outWords = (u32 *)dest;
    u32 wordBuf = 0;
    int wordBits = 0;
    size_t outPos = 0;

    size_t streamWordIdx = 0;
    u32 bits = 0;
    int bitsLeft = 0;

    u32 nodeIdx = 0;
    u8 nodeVal = tree[0];
    while (outPos < decompSize) {
        if (bitsLeft == 0) {
            size_t b = streamWordIdx * 4u;
            bits = (u32)stream[b] | ((u32)stream[b + 1] << 8)
                 | ((u32)stream[b + 2] << 16) | ((u32)stream[b + 3] << 24);
            streamWordIdx++;
            bitsLeft = 32;
        }
        int dir = (bits >> 31) & 1;
        bits <<= 1;
        bitsLeft--;

        u32 childOffset = ((nodeIdx & ~1u) + (((u32)(nodeVal & 0x3Fu) + 1u) * 2u)) + (u32)dir;
        int isLeaf = dir ? (nodeVal & 0x40) : (nodeVal & 0x80);
        u8 childVal = tree[childOffset];

        if (isLeaf) {
            wordBuf |= ((u32)(childVal & ((1u << bitDepth) - 1u))) << wordBits;
            wordBits += bitDepth;
            outPos += (bitDepth == 4) ? ((wordBits & 7) == 0 ? 1u : 0u) : 1u;
            if (wordBits == 32) {
                *outWords++ = wordBuf;
                wordBuf = 0;
                wordBits = 0;
            }
            nodeIdx = 0;
            nodeVal = tree[0];
        } else {
            nodeIdx = childOffset;
            nodeVal = childVal;
        }
    }
    if (wordBits > 0)
        *outWords = wordBuf;
}

struct BitUnPackContext {
    u16 srcNumBytes;
    u8 srcBitNum;
    u8 dstBitNum;
    u32 dataOffset : 31;
    u32 zeroDataFlag : 1;
};

void BitUnPack(const void *src, void *dest, const struct BitUnPackContext *info)
{
    const u8 *in = (const u8 *)src;
    u32 *out = (u32 *)dest;
    u32 wordBuf = 0;
    int wordBits = 0;
    u32 srcMask = (1u << info->srcBitNum) - 1u;

    for (u32 b = 0; b < info->srcNumBytes; b++) {
        u8 byteVal = in[b];
        for (int shift = 0; shift < 8; shift += info->srcBitNum) {
            u32 unit = (byteVal >> shift) & srcMask;
            if (unit != 0 || info->zeroDataFlag)
                unit += info->dataOffset;
            wordBuf |= (unit << wordBits);
            wordBits += info->dstBitNum;
            if (wordBits == 32) {
                *out++ = wordBuf;
                wordBuf = 0;
                wordBits = 0;
            }
        }
    }
    if (wordBits > 0)
        *out = wordBuf;
}

void Diff8bitUnFilterWram(const void *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = (u8 *)dest;
    u32 header = (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
    u32 size = header >> 8;
    u8 acc = 0;
    for (u32 i = 0; i < size; i++) {
        acc = (u8)(acc + in[4 + i]);
        out[i] = acc;
    }
}

void Diff8bitUnFilterVram(const void *src, void *dest)
{
    Diff8bitUnFilterWram(src, dest);
}

void Diff16bitUnFilter(const void *src, void *dest)
{
    const u16 *in = (const u16 *)src;
    u16 *out = (u16 *)dest;
    u32 header = (u32)in[0] | ((u32)in[1] << 16);
    u32 count = (header >> 8) / 2u;
    u16 acc = 0;
    for (u32 i = 0; i < count; i++) {
        acc = (u16)(acc + in[2 + i]);
        out[i] = acc;
    }
}

/* -------------------------------------------------------------------------
 * BIOS Math, Affine & Control Syscalls
 * ------------------------------------------------------------------------- */
u16 Sqrt(u32 num)
{
    return (u16)sqrt((double)num);
}

u16 ArcTan(s16 tanVal)
{
    double rad = atan((double)tanVal / 16384.0);
    return (u16)(s16)llround(rad * (32768.0 / M_PI));
}

u16 ArcTan2(s16 x, s16 y)
{
    double rad = atan2((double)y, (double)x);
    if (rad < 0.0)
        rad += 2.0 * M_PI;
    return (u16)((s32)llround(rad * (65536.0 / (2.0 * M_PI))) & 0xFFFF);
}

s32 Div(s32 num, s32 denom)
{
    return denom == 0 ? 0 : (num / denom);
}

s32 DivArm(s32 denom, s32 num)
{
    return denom == 0 ? 0 : (num / denom);
}

s32 Mod(s32 num, s32 denom)
{
    return denom == 0 ? 0 : (num % denom);
}

s32 ModArm(s32 denom, s32 num)
{
    return denom == 0 ? 0 : (num % denom);
}

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    for (s32 i = 0; i < count; i++) {
        double angle = ((double)src[i].alpha / 65536.0) * (2.0 * M_PI);
        double sinA = sin(angle);
        double cosA = cos(angle);
        double sx = (double)src[i].sx / 256.0;
        double sy = (double)src[i].sy / 256.0;

        s16 pa = (s16)llround(cosA * sx * 256.0);
        s16 pb = (s16)llround(-sinA * sx * 256.0);
        s16 pc = (s16)llround(sinA * sy * 256.0);
        s16 pd = (s16)llround(cosA * sy * 256.0);

        dest[i].pa = pa;
        dest[i].pb = pb;
        dest[i].pc = pc;
        dest[i].pd = pd;
        dest[i].dx = src[i].texX - ((s32)pa * src[i].scrX + (s32)pb * src[i].scrY);
        dest[i].dy = src[i].texY - ((s32)pc * src[i].scrX + (s32)pd * src[i].scrY);
    }
}

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    u8 *ptr = (u8 *)dest;
    for (s32 i = 0; i < count; i++) {
        double angle = ((double)src[i].rotation / 65536.0) * (2.0 * M_PI);
        double sinA = sin(angle);
        double cosA = cos(angle);
        double sx = (double)src[i].xScale / 256.0;
        double sy = (double)src[i].yScale / 256.0;

        s16 pa = (s16)llround(cosA * sx * 256.0);
        s16 pb = (s16)llround(-sinA * sx * 256.0);
        s16 pc = (s16)llround(sinA * sy * 256.0);
        s16 pd = (s16)llround(cosA * sy * 256.0);

        *(s16 *)(ptr + 0 * offset) = pa;
        *(s16 *)(ptr + 1 * offset) = pb;
        *(s16 *)(ptr + 2 * offset) = pc;
        *(s16 *)(ptr + 3 * offset) = pd;
        ptr += 4 * offset;
    }
}

u32 MidiKey2Freq(const void *wa, u8 mk, u8 fp)
{
    (void)wa;
    /* GBA hardware rate helper: MIDI note 60 (C4) base approximation for sound driver */
    double semitones = (double)mk + ((double)fp / 256.0);
    double freq = 440.0 * pow(2.0, (semitones - 69.0) / 12.0);
    return (u32)llround(freq * 65536.0);
}

void RegisterRamReset(u32 resetFlags)
{
    if (resetFlags & 0x01) memset(gWasmEwram, 0, sizeof(gWasmEwram));
    if (resetFlags & 0x02) memset(gWasmIwram, 0, sizeof(gWasmIwram));
    if (resetFlags & 0x04) memset(gWasmPltt, 0, sizeof(gWasmPltt));
    if (resetFlags & 0x08) memset(gWasmVram, 0, sizeof(gWasmVram));
    if (resetFlags & 0x10) memset(gWasmOam, 0, sizeof(gWasmOam));
    if (resetFlags & 0x80) WasmMmio_Reset();
}

void SoftReset(u32 resetFlags)
{
    RegisterRamReset(resetFlags);
}

void SoftResetRom(u32 resetFlags)
{
    SoftReset(resetFlags);
}

void SoftResetExram(u32 resetFlags)
{
    SoftReset(resetFlags);
}

int MultiBoot(struct MultiBootParam *mp)
{
    (void)mp;
    return 1;
}

#ifndef WASM_SMOKE_TEST
#include "libgcnmultiboot.h"

char __iwram_end[4];
uint8_t __rom_end[4];

const u8 RomHeaderGameCode[4] = {'B', 'P', 'E', 'E'};
const u8 RomHeaderSoftwareVersion = 0;

void ReInitializeEWRAM(void)
{
    /* WASM keeps static/EWRAM globals initialized cleanly by Emscripten runtime. */
}

void GameCubeMultiBoot_Init(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_Main(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *pStruct) { (void)pStruct; }
void GameCubeMultiBoot_Quit(void) {}
#endif
