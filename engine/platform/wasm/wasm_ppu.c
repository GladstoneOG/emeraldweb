#include "wasm_platform.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static uint32_t sFramebuffer[WASM_DISPLAY_PIXELS];

static inline uint32_t Bgr555ToAbgr(uint16_t bgr)
{
    uint32_t r = (uint32_t)(bgr & 0x1Fu);
    uint32_t g = (uint32_t)((bgr >> 5) & 0x1Fu);
    uint32_t b = (uint32_t)((bgr >> 10) & 0x1Fu);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint16_t ReadPltt16(uint32_t index)
{
    uint32_t off = (index & 0x1FFu) * 2u;
    return (uint16_t)gWasmPltt[off] | ((uint16_t)gWasmPltt[off + 1] << 8);
}

static inline uint16_t ReadVram16(uint32_t byteOffset)
{
    if (byteOffset + 1u >= WASM_VRAM_SIZE)
        return 0;
    return (uint16_t)gWasmVram[byteOffset] | ((uint16_t)gWasmVram[byteOffset + 1] << 8);
}

static inline uint16_t ReadOam16(uint32_t byteOffset)
{
    return (uint16_t)gWasmOam[byteOffset] | ((uint16_t)gWasmOam[byteOffset + 1] << 8);
}

static uint16_t SampleTextBgPixel(const WasmScanlineRegs *regs, int bg, int x, int y, uint8_t *outPrio)
{
    uint16_t cnt = regs->bgcnt[bg];
    uint32_t prio = cnt & 3u;
    uint32_t charBase = ((cnt >> 2) & 3u) * 0x4000u;
    bool is8bpp = (cnt & (1u << 7)) != 0;
    uint32_t screenBase = ((cnt >> 8) & 0x1Fu) * 0x800u;
    uint32_t sizeMode = (cnt >> 14) & 3u;

    int mapW = (sizeMode & 1u) ? 512 : 256;
    int mapH = (sizeMode & 2u) ? 512 : 256;

    int sx = (x + (int)(regs->bghofs[bg] & 0x1FFu)) & (mapW - 1);
    int sy = (y + (int)(regs->bgvofs[bg] & 0x1FFu)) & (mapH - 1);

    int tx = sx >> 3;
    int ty = sy >> 3;
    int screenBlock = 0;
    if (tx >= 32) {
        screenBlock += 1;
        tx -= 32;
    }
    if (ty >= 32) {
        screenBlock += (sizeMode == 3u) ? 2 : 1;
        ty -= 32;
    }

    uint32_t entryOff = screenBase + ((uint32_t)screenBlock * 0x800u) + ((uint32_t)(ty * 32 + tx) * 2u);
    uint16_t entry = ReadVram16(entryOff);
    uint32_t tileId = entry & 0x3FFu;
    int px = sx & 7;
    int py = sy & 7;
    if (entry & (1u << 10)) px = 7 - px;
    if (entry & (1u << 11)) py = 7 - py;

    uint32_t palIdx = 0;
    if (is8bpp) {
        uint32_t addr = charBase + tileId * 64u + (uint32_t)(py * 8 + px);
        if (addr < WASM_VRAM_SIZE)
            palIdx = gWasmVram[addr];
    } else {
        uint32_t addr = charBase + tileId * 32u + (uint32_t)(py * 4 + (px >> 1));
        if (addr < WASM_VRAM_SIZE) {
            uint8_t packed = gWasmVram[addr];
            uint32_t nib = (px & 1) ? (packed >> 4) : (packed & 0x0Fu);
            if (nib != 0)
                palIdx = (((uint32_t)(entry >> 12) & 0xFu) << 4) | nib;
        }
    }

    if (palIdx == 0)
        return 0x8000u; /* transparent marker */

    *outPrio = (uint8_t)prio;
    return ReadPltt16(palIdx) & 0x7FFFu;
}

static uint16_t SampleAffineBgPixel(const WasmScanlineRegs *regs, int bg, int x, int y, uint8_t *outPrio)
{
    uint16_t cnt = regs->bgcnt[bg];
    uint32_t charBase = ((cnt >> 2) & 3u) * 0x4000u;
    uint32_t screenBase = ((cnt >> 8) & 0x1Fu) * 0x800u;
    bool wrap = (cnt & (1u << 13)) != 0;
    uint32_t sizeTiles = 16u << ((cnt >> 14) & 3u);
    int sizePx = (int)(sizeTiles * 8u);

    int32_t refX = (bg == 2) ? regs->bg2x : regs->bg3x;
    int32_t refY = (bg == 2) ? regs->bg2y : regs->bg3y;
    int16_t pa = (bg == 2) ? regs->bg2pa : regs->bg3pa;
    int16_t pb = (bg == 2) ? regs->bg2pb : regs->bg3pb;
    int16_t pc = (bg == 2) ? regs->bg2pc : regs->bg3pc;
    int16_t pd = (bg == 2) ? regs->bg2pd : regs->bg3pd;

    int32_t texX = (refX + (int32_t)pa * x + (int32_t)pb * y) >> 8;
    int32_t texY = (refY + (int32_t)pc * x + (int32_t)pd * y) >> 8;

    if (wrap) {
        texX &= (sizePx - 1);
        texY &= (sizePx - 1);
    } else if (texX < 0 || texX >= sizePx || texY < 0 || texY >= sizePx) {
        return 0x8000u;
    }

    uint32_t tx = (uint32_t)(texX >> 3);
    uint32_t ty = (uint32_t)(texY >> 3);
    /* GBA BG VRAM is 64 KiB (0x0000..0xFFFF); WASM_VRAM_SIZE (0x18000) is not a power of two. */
    uint32_t tileId = gWasmVram[(screenBase + ty * sizeTiles + tx) & 0xFFFFu];
    uint32_t addr = charBase + tileId * 64u + (uint32_t)((texY & 7) * 8 + (texX & 7));
    if (addr >= WASM_VRAM_SIZE)
        return 0x8000u;

    uint8_t palIdx = gWasmVram[addr];
    if (palIdx == 0)
        return 0x8000u;

    *outPrio = (uint8_t)(cnt & 3u);
    return ReadPltt16(palIdx) & 0x7FFFu;
}

static void GetObjDimensions(uint32_t shape, uint32_t size, int *outW, int *outH)
{
    static const int dims[3][4][2] = {
        { { 8,  8}, {16, 16}, {32, 32}, {64, 64} }, /* square */
        { {16,  8}, {32,  8}, {32, 16}, {64, 32} }, /* wide */
        { { 8, 16}, { 8, 32}, {16, 32}, {32, 64} }, /* tall */
    };
    if (shape > 2) {
        *outW = 8;
        *outH = 8;
        return;
    }
    *outW = dims[shape][size & 3u][0];
    *outH = dims[shape][size & 3u][1];
}

static uint16_t BlendColor(uint16_t topBgr, uint16_t botBgr, int eva, int evb)
{
    int r1 = topBgr & 0x1F, g1 = (topBgr >> 5) & 0x1F, b1 = (topBgr >> 10) & 0x1F;
    int r2 = botBgr & 0x1F, g2 = (botBgr >> 5) & 0x1F, b2 = (botBgr >> 10) & 0x1F;
    int r = (r1 * eva + r2 * evb) >> 4;
    int g = (g1 * eva + g2 * evb) >> 4;
    int b = (b1 * eva + b2 * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (uint16_t)((b << 10) | (g << 5) | r);
}

static uint16_t ApplyBrightness(uint16_t bgr, int evy, bool lighten)
{
    int r = bgr & 0x1F, g = (bgr >> 5) & 0x1F, b = (bgr >> 10) & 0x1F;
    if (lighten) {
        r += ((31 - r) * evy) >> 4;
        g += ((31 - g) * evy) >> 4;
        b += ((31 - b) * evy) >> 4;
    } else {
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }
    return (uint16_t)((b << 10) | (g << 5) | r);
}

static uint8_t ComputeWindowMask(const WasmScanlineRegs *regs, int x, int y, bool objWinHit)
{
    uint16_t dispcnt = regs->dispcnt;
    if ((dispcnt & 0xE000u) == 0)
        return 0x3Fu; /* all layers + color math enabled */

    if (dispcnt & (1u << 13)) { /* WIN0 */
        int x1 = regs->win0h >> 8, x2 = regs->win0h & 0xFF;
        int y1 = regs->win0v >> 8, y2 = regs->win0v & 0xFF;
        bool inX = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        bool inY = (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        if (inX && inY)
            return (uint8_t)(regs->winin & 0x3Fu);
    }
    if (dispcnt & (1u << 14)) { /* WIN1 */
        int x1 = regs->win1h >> 8, x2 = regs->win1h & 0xFF;
        int y1 = regs->win1v >> 8, y2 = regs->win1v & 0xFF;
        bool inX = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        bool inY = (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        if (inX && inY)
            return (uint8_t)((regs->winin >> 8) & 0x3Fu);
    }
    if ((dispcnt & (1u << 15)) && objWinHit) {
        return (uint8_t)((regs->winout >> 8) & 0x3Fu);
    }
    return (uint8_t)(regs->winout & 0x3Fu);
}

typedef struct ScanlineObj {
    int sprX, sprY;
    int objW, objH;
    int boxW, boxH;
    uint32_t affMode;
    int16_t pa, pb, pc, pd;
    uint32_t baseTile;
    bool is8bpp;
    uint32_t objMode;
    uint8_t prio;
    uint32_t palBankOffset;
    bool flipH, flipV;
} ScanlineObj;

static void RenderScanline(int y)
{
    const WasmScanlineRegs *regs = &gWasmScanlineRegs[y];
    uint16_t dispcnt = regs->dispcnt;
    uint32_t *lineOut = &sFramebuffer[y * WASM_DISPLAY_WIDTH];

    if (dispcnt & (1u << 7)) { /* Forced blank */
        for (int x = 0; x < WASM_DISPLAY_WIDTH; x++)
            lineOut[x] = 0xFFFFFFFFu;
        return;
    }

    int bgMode = dispcnt & 7;
    bool obj1D = (dispcnt & (1u << 6)) != 0;
    uint16_t backdrop = ReadPltt16(0) & 0x7FFFu;

    ScanlineObj lineObjs[128];
    int lineObjCount = 0;
    if (dispcnt & (1u << 12)) {
        for (int o = 0; o < 128; o++) {
            uint16_t a0 = ReadOam16((uint32_t)o * 8u + 0u);
            uint32_t affMode = (a0 >> 8) & 3u;
            if (affMode == 2u)
                continue;

            uint16_t a1 = ReadOam16((uint32_t)o * 8u + 2u);
            int objW = 8, objH = 8;
            GetObjDimensions((a0 >> 14) & 3u, (a1 >> 14) & 3u, &objW, &objH);
            int boxW = (affMode == 3u) ? objW * 2 : objW;
            int boxH = (affMode == 3u) ? objH * 2 : objH;

            int sprY = a0 & 0xFF;
            if (sprY >= 160) sprY -= 256;
            if (y < sprY || y >= sprY + boxH)
                continue;

            int sprX = a1 & 0x1FF;
            if (sprX >= 256) sprX -= 512;
            if (sprX + boxW <= 0 || sprX >= WASM_DISPLAY_WIDTH)
                continue;

            uint16_t a2 = ReadOam16((uint32_t)o * 8u + 4u);
            ScanlineObj *so = &lineObjs[lineObjCount++];
            so->sprX = sprX;
            so->sprY = sprY;
            so->objW = objW;
            so->objH = objH;
            so->boxW = boxW;
            so->boxH = boxH;
            so->affMode = affMode;
            if (affMode & 1u) {
                uint32_t m = (a1 >> 9) & 0x1Fu;
                so->pa = (int16_t)ReadOam16(m * 32u + 0x06u);
                so->pb = (int16_t)ReadOam16(m * 32u + 0x0Eu);
                so->pc = (int16_t)ReadOam16(m * 32u + 0x16u);
                so->pd = (int16_t)ReadOam16(m * 32u + 0x1Eu);
                so->flipH = false;
                so->flipV = false;
            } else {
                so->pa = so->pb = so->pc = so->pd = 0;
                so->flipH = (a1 & (1u << 12)) != 0;
                so->flipV = (a1 & (1u << 13)) != 0;
            }
            so->baseTile = a2 & 0x3FFu;
            so->is8bpp = (a0 & (1u << 13)) != 0;
            so->objMode = (a0 >> 10) & 3u;
            so->prio = (uint8_t)((a2 >> 10) & 3u);
            so->palBankOffset = 256u + (((uint32_t)(a2 >> 12) & 0xFu) << 4);
        }
    }

    for (int x = 0; x < WASM_DISPLAY_WIDTH; x++) {
        uint16_t objCol = 0x8000u;
        uint8_t objPrio = 4;
        bool objSemiTrans = false;
        bool objWinHit = false;

        for (int i = 0; i < lineObjCount; i++) {
            const ScanlineObj *so = &lineObjs[i];
            if (x < so->sprX || x >= so->sprX + so->boxW)
                continue;

            int tx, ty;
            if (so->affMode & 1u) {
                int dx = x - (so->sprX + so->boxW / 2);
                int dy = y - (so->sprY + so->boxH / 2);
                tx = ((so->pa * dx + so->pb * dy) >> 8) + (so->objW / 2);
                ty = ((so->pc * dx + so->pd * dy) >> 8) + (so->objH / 2);
                if (tx < 0 || tx >= so->objW || ty < 0 || ty >= so->objH)
                    continue;
            } else {
                tx = x - so->sprX;
                ty = y - so->sprY;
                if (so->flipH) tx = (so->objW - 1) - tx;
                if (so->flipV) ty = (so->objH - 1) - ty;
            }

            uint32_t tileX = (uint32_t)(tx >> 3);
            uint32_t tileY = (uint32_t)(ty >> 3);
            uint32_t stride = obj1D ? (uint32_t)(so->objW >> 3) : (so->is8bpp ? 16u : 32u);
            uint32_t tileOff = so->baseTile + (so->is8bpp ? (tileY * stride + tileX) * 2u : (tileY * stride + tileX));

            uint32_t palIdx = 0;
            if (so->is8bpp) {
                uint32_t addr = 0x10000u + (tileOff * 32u) + (uint32_t)((ty & 7) * 8 + (tx & 7));
                if (addr < WASM_VRAM_SIZE)
                    palIdx = gWasmVram[addr];
                if (palIdx != 0) palIdx += 256u;
            } else {
                uint32_t addr = 0x10000u + (tileOff * 32u) + (uint32_t)((ty & 7) * 4 + ((tx & 7) >> 1));
                if (addr < WASM_VRAM_SIZE) {
                    uint8_t packed = gWasmVram[addr];
                    uint32_t nib = (tx & 1) ? (packed >> 4) : (packed & 0x0Fu);
                    if (nib != 0)
                        palIdx = so->palBankOffset + nib;
                }
            }

            if (palIdx == 0)
                continue;

            if (so->objMode == 2u) {
                objWinHit = true;
                continue;
            }

            if (objCol == 0x8000u || so->prio < objPrio) {
                objPrio = so->prio;
                objCol = ReadPltt16(palIdx) & 0x7FFFu;
                objSemiTrans = (so->objMode == 1u);
            }
        }

        uint8_t winMask = ComputeWindowMask(regs, x, y, objWinHit);
        if ((winMask & (1u << 4)) == 0) {
            objCol = 0x8000u;
            objSemiTrans = false;
        }

        /* Collect top-2 visible layers ordered by (priority, layerId) */
        uint16_t topCol = backdrop, botCol = backdrop;
        uint8_t topLayer = 5, botLayer = 5;
        uint8_t topPrio = 4, botPrio = 4;

        if (objCol != 0x8000u) {
            topCol = objCol;
            topLayer = 4;
            topPrio = objPrio;
        }

        if (bgMode == 3 && (dispcnt & (1u << 10)) && (winMask & (1u << 2))) {
            uint16_t c = ReadVram16((uint32_t)(y * 240 + x) * 2u) & 0x7FFFu;
            uint8_t p = (uint8_t)(regs->bgcnt[2] & 3u);
            if (p < topPrio) {
                botCol = topCol; botLayer = topLayer; botPrio = topPrio;
                topCol = c; topLayer = 2; topPrio = p;
            } else if (p < botPrio) {
                botCol = c; botLayer = 2; botPrio = p;
            }
        } else {
            for (int bg = 0; bg < 4; bg++) {
                if ((dispcnt & (1u << (8 + bg))) == 0 || (winMask & (1u << bg)) == 0)
                    continue;
                if (bgMode == 0 || (bgMode == 1 && bg < 2)) {
                    uint8_t p = 4;
                    uint16_t c = SampleTextBgPixel(regs, bg, x, y, &p);
                    if (c == 0x8000u) continue;
                    if (p < topPrio) {
                        botCol = topCol; botLayer = topLayer; botPrio = topPrio;
                        topCol = c; topLayer = (uint8_t)bg; topPrio = p;
                    } else if (p < botPrio) {
                        botCol = c; botLayer = (uint8_t)bg; botPrio = p;
                    }
                } else if ((bgMode == 1 && bg == 2) || (bgMode == 2 && bg >= 2)) {
                    uint8_t p = 4;
                    uint16_t c = SampleAffineBgPixel(regs, bg, x, y, &p);
                    if (c == 0x8000u) continue;
                    if (p < topPrio) {
                        botCol = topCol; botLayer = topLayer; botPrio = topPrio;
                        topCol = c; topLayer = (uint8_t)bg; topPrio = p;
                    } else if (p < botPrio) {
                        botCol = c; botLayer = (uint8_t)bg; botPrio = p;
                    }
                }
            }
        }

        uint16_t finalBgr = topCol;
        if (winMask & (1u << 5)) {
            uint16_t bldcnt = regs->bldcnt;
            uint32_t mode = (bldcnt >> 6) & 3u;
            bool firstHit = (bldcnt & (1u << topLayer)) != 0;
            bool secondHit = (bldcnt & (1u << (8 + botLayer))) != 0;

            if (objSemiTrans && topLayer == 4 && secondHit) {
                int eva = regs->bldalpha & 0x1Fu;
                int evb = (regs->bldalpha >> 8) & 0x1Fu;
                finalBgr = BlendColor(topCol, botCol, eva > 16 ? 16 : eva, evb > 16 ? 16 : evb);
            } else if (mode == 1u && firstHit && secondHit) {
                int eva = regs->bldalpha & 0x1Fu;
                int evb = (regs->bldalpha >> 8) & 0x1Fu;
                finalBgr = BlendColor(topCol, botCol, eva > 16 ? 16 : eva, evb > 16 ? 16 : evb);
            } else if ((mode == 2u || mode == 3u) && firstHit) {
                int evy = regs->bldy & 0x1Fu;
                finalBgr = ApplyBrightness(topCol, evy > 16 ? 16 : evy, mode == 2u);
            }
        }

        lineOut[x] = Bgr555ToAbgr(finalBgr);
    }
}

void WasmPpu_Init(void)
{
    memset(sFramebuffer, 0, sizeof(sFramebuffer));
}

void WasmPpu_RenderFrame(void)
{
    for (int y = 0; y < WASM_DISPLAY_HEIGHT; y++) {
        RenderScanline(y);
    }
}

const uint32_t *WasmPpu_GetFramebuffer(void)
{
    return sFramebuffer;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_flush_framebuffer_to_canvas, (uintptr_t fbPtr, int width, int height), {
    if (typeof document === "undefined") return;
    const canvas = (typeof Module !== "undefined" && Module.canvas)
        ? Module.canvas
        : document.getElementById("canvas");
    if (!canvas) return;
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const pixels = new Uint8ClampedArray(HEAPU8.buffer, fbPtr, width * height * 4);
    const imgData = new ImageData(pixels, width, height);
    ctx.putImageData(imgData, 0, 0);
});
#endif

void WasmPpu_FlushFrameToCanvas(void)
{
#ifdef __EMSCRIPTEN__
    js_flush_framebuffer_to_canvas((uintptr_t)sFramebuffer, WASM_DISPLAY_WIDTH, WASM_DISPLAY_HEIGHT);
#endif
}

WASM_KEEPALIVE uintptr_t wasm_get_framebuffer_ptr(void)
{
    return (uintptr_t)sFramebuffer;
}
