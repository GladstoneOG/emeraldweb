#include "wasm_platform.h"
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/fiber.h>
#include <emscripten/html5.h>
#endif

#ifndef WASM_USE_FIBER
#define WASM_USE_FIBER 1
#endif

#define WASM_FIBER_STACK_SIZE (256 * 1024)
#define WASM_ASYNCIFY_STACK_SIZE (16 * 1024)

static int sSpeedMultiplier = 1;
static uint16_t sPressedKeysMask = 0; /* keyboard / input mask, active-high matching GBA KEYINPUT */
static uint16_t sLatchedKeysMask = 0; /* single-frame latch so short key taps are never dropped */
static uint16_t sGamepadMask = 0;     /* Web Gamepad mask, active-high matching GBA KEYINPUT */

static inline void WriteReg16(uint32_t offset, uint16_t val)
{
    gWasmIoRegs[offset] = (uint8_t)(val & 0xFF);
    gWasmIoRegs[offset + 1] = (uint8_t)(val >> 8);
}

#ifdef __EMSCRIPTEN__
#if WASM_USE_FIBER
static emscripten_fiber_t sMainFiber;
static emscripten_fiber_t sGameFiber;
static char sMainAsyncifyStack[WASM_ASYNCIFY_STACK_SIZE];
static char sGameAsyncifyStack[WASM_ASYNCIFY_STACK_SIZE];
static char sGameStack[WASM_FIBER_STACK_SIZE] __attribute__((aligned(16)));
static bool sFiberStarted = false;
#endif
#endif

#ifdef WASM_SMOKE_TEST
static void SmokeTestDrawTile(uint32_t tileIdx, uint8_t c0, uint8_t c1)
{
    uint8_t *tile = &gWasmVram[tileIdx * 32u];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 4; x++) {
            bool border = (y == 0 || y == 7 || x == 0 || x == 3);
            uint8_t px = border ? c1 : c0;
            tile[y * 4 + x] = (uint8_t)((px << 4) | px);
        }
    }
}

static void SmokeTestInitScene(void)
{
    /* Write classic Emerald-themed test tiles + animated floating badge */
    uint16_t *pal = (uint16_t *)gWasmPltt;
    pal[0] = 0x1944; /* Deep teal backdrop */
    pal[1] = 0x2AE8; /* Emerald jade */
    pal[2] = 0x57B2; /* Crisp highlight */
    pal[257] = 0x03DF; /* Gold badge */
    pal[258] = 0x7FFF; /* White */

    SmokeTestDrawTile(1, 1, 2);
    uint16_t *map = (uint16_t *)&gWasmVram[0xF800];
    for (int i = 0; i < 32 * 20; i++)
        map[i] = 1;

    uint8_t *spr = &gWasmVram[0x10020];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 4; x++)
            spr[y * 4 + x] = (y == 0 || y == 7) ? 0x22 : 0x11;

    WriteReg16(0x00, 0x1140); /* DISPCNT: BG0 | OBJ | OBJ_1D */
    WriteReg16(0x08, (31u << 8));
}

static void SmokeTestStepScene(void)
{
    static uint32_t sFrameCounter = 0;
    sFrameCounter++;
    int posX = 112 + ((int)(sFrameCounter % 64u) - 32);
    WriteReg16(0x10, (uint16_t)(sFrameCounter & 7u));
    WriteReg16(0x12, (uint16_t)((sFrameCounter >> 1) & 7u));

    uint16_t *oam = (uint16_t *)gWasmOam;
    oam[0] = 76;
    oam[1] = (uint16_t)(posX & 0x1FF);
    oam[2] = 1;
}

void WasmRunFrame(void)
{
    SmokeTestStepScene();
}

void AgbMain(void)
{
    SmokeTestInitScene();
#if WASM_USE_FIBER
    for (;;) {
        SmokeTestStepScene();
        WasmPlatform_VBlankYield();
    }
#endif
}
#else
/* Provided by engine/src/main.c when WASM_USE_FIBER=0 */
__attribute__((weak)) void WasmRunFrame(void) {}
#endif

#if defined(__EMSCRIPTEN__) && WASM_USE_FIBER
static void GameFiberEntry(void *arg)
{
    (void)arg;
    AgbMain();
    for (;;) {
        emscripten_fiber_swap(&sGameFiber, &sMainFiber);
    }
}
#endif

void WasmPlatform_Init(void)
{
    WasmMmio_Reset();
    WasmPpu_Init();
    WasmAudio_Init();
    WasmFlash_Init();
    WasmFlash_LoadFromStorage();
    WasmMp_Init();

#if defined(__EMSCRIPTEN__) && WASM_USE_FIBER
    if (!sFiberStarted) {
        emscripten_fiber_init_from_current_context(&sMainFiber, sMainAsyncifyStack, sizeof(sMainAsyncifyStack));
        emscripten_fiber_init(&sGameFiber, GameFiberEntry, NULL,
                              sGameStack, sizeof(sGameStack),
                              sGameAsyncifyStack, sizeof(sGameAsyncifyStack));
        sFiberStarted = true;
    }
#else
    AgbMain();
#endif
}

static void WasmPlatform_ApplyKeys(void)
{
    uint16_t combined = (sPressedKeysMask | sLatchedKeysMask | sGamepadMask) & 0x03FFu;
    /* GBA REG_KEYINPUT (0x130): 0 = pressed, 1 = released */
    WriteReg16(0x130, (uint16_t)((~combined) & 0x03FFu));
}

void WasmPlatform_SetKeys(uint16_t pressedMask)
{
    sPressedKeysMask = pressedMask & 0x03FFu;
    WasmPlatform_ApplyKeys();
}

uint16_t WasmPlatform_GetKeys(void)
{
    return (sPressedKeysMask | sLatchedKeysMask | sGamepadMask) & 0x03FFu;
}

void WasmPlatform_SetSpeedMultiplier(int speed)
{
    if (speed < 1) speed = 1;
    if (speed > 3) speed = 3;
    sSpeedMultiplier = speed;
}

int WasmPlatform_GetSpeedMultiplier(void)
{
    return sSpeedMultiplier;
}

#ifdef __EMSCRIPTEN__
EM_JS(int, js_wasm_invoke_fn_ptr, (int fnPtr, int arg0, int arg1), {
    if (!fnPtr || typeof wasmExports === "undefined") return 0;
    const tbl = wasmExports["__indirect_function_table"];
    if (!tbl || typeof tbl.get !== "function") return 0;
    const fn = tbl.get(fnPtr);
    return typeof fn === "function" ? (fn(arg0, arg1) | 0) : 0;
});
#endif

int WasmPlatform_InvokeFnPtr(uintptr_t fnPtr, int arg0, int arg1)
{
#ifdef __EMSCRIPTEN__
    return js_wasm_invoke_fn_ptr((int)fnPtr, arg0, arg1);
#else
    typedef int (*GenericFn)(int, int);
    return fnPtr ? ((GenericFn)fnPtr)(arg0, arg1) : 0;
#endif
}

void WasmPlatform_VBlankYield(void)
{
    WasmMmio_BeginFrame();
    gWasmIntrCheck |= 0x0001u; /* INTR_FLAG_VBLANK */
    if (gWasmIntrVector) {
        typedef void (*IntrFn)(void);
        ((IntrFn)gWasmIntrVector)();
    }
    WasmMmio_LatchFrameScanlines();

#if defined(__EMSCRIPTEN__) && WASM_USE_FIBER
    emscripten_fiber_swap(&sGameFiber, &sMainFiber);
#endif
}

void (VBlankIntrWait)(void)
{
    WasmPlatform_VBlankYield();
}

void IntrWait(uint32_t discard, uint32_t intrFlags)
{
    (void)discard;
    if (intrFlags & 0x0001u) {
        WasmPlatform_VBlankYield();
    } else {
        gWasmIntrCheck |= (uint16_t)intrFlags;
    }
}

void WasmPlatform_StepFrame(void)
{
    WasmPlatform_ApplyKeys();

#if defined(__EMSCRIPTEN__) && WASM_USE_FIBER
    emscripten_fiber_swap(&sMainFiber, &sGameFiber);
#else
    WasmRunFrame();
    WasmPlatform_VBlankYield();
#endif

    /* Clear single-frame tap latch once the frame's ReadKeys() has consumed it. */
    if (sLatchedKeysMask != 0) {
        sLatchedKeysMask = 0;
        WasmPlatform_ApplyKeys();
    }

    WasmPpu_RenderFrame();
    WasmAudio_MixFrame();
}

WASM_KEEPALIVE void wasm_step_frames(int count, int renderLast)
{
    static bool sInsideStepFrame = false;
    int i;
#if defined(__EMSCRIPTEN__) && WASM_USE_FIBER
    if (!sFiberStarted)
        return;
#endif
    if (sInsideStepFrame)
        return;
    if (count < 1)
        count = 1;
    sInsideStepFrame = true;
    for (i = 0; i < count; i++)
    {
        WasmPlatform_ApplyKeys();
#if defined(__EMSCRIPTEN__) && WASM_USE_FIBER
        emscripten_fiber_swap(&sMainFiber, &sGameFiber);
#else
        WasmRunFrame();
        WasmPlatform_VBlankYield();
#endif
        if (sLatchedKeysMask != 0)
        {
            sLatchedKeysMask = 0;
            WasmPlatform_ApplyKeys();
        }
        if (renderLast && i == count - 1)
        {
            WasmPpu_RenderFrame();
            WasmAudio_MixFrame();
        }
    }
    sInsideStepFrame = false;
}

WASM_KEEPALIVE void wasm_set_input_mask(int keyMask)
{
    WasmPlatform_SetKeys((uint16_t)keyMask);
}

WASM_KEEPALIVE void wasm_set_gamepad_mask(int keyMask)
{
    sGamepadMask = (uint16_t)keyMask & 0x03FFu;
    WasmPlatform_ApplyKeys();
}

#ifdef __EMSCRIPTEN__
static double sAccumulator = 0.0;
static double sLastTimeMs = 0.0;

static void MainLoopTick(void)
{
    double now = emscripten_get_now();
    if (sLastTimeMs <= 0.0)
        sLastTimeMs = now;

    double dt = now - sLastTimeMs;
    sLastTimeMs = now;
    if (dt > 100.0) dt = 100.0;

    int speed = WasmPlatform_GetSpeedMultiplier();
    double stepMs = 1000.0 / 59.7275;
    sAccumulator += dt * (double)speed;

    int maxSteps = speed * 2;
    int ran = 0;
    while (sAccumulator >= stepMs && ran < maxSteps) {
        WasmPlatform_StepFrame();
        sAccumulator -= stepMs;
        ran++;
    }
    if (ran > 0) {
        WasmPpu_FlushFrameToCanvas();
    }
}

static uint16_t MapDomCodeToGbaBit(const char *code)
{
    if (!code) return 0;
    if (strcmp(code, "KeyZ") == 0 || strcmp(code, "KeyK") == 0) return (1u << 0); /* A */
    if (strcmp(code, "KeyX") == 0 || strcmp(code, "KeyJ") == 0) return (1u << 1); /* B */
    if (strcmp(code, "ShiftLeft") == 0 || strcmp(code, "ShiftRight") == 0 || strcmp(code, "Backspace") == 0)
        return (1u << 2); /* SELECT */
    if (strcmp(code, "Enter") == 0 || strcmp(code, "NumpadEnter") == 0)
        return (1u << 3); /* START */
    if (strcmp(code, "ArrowRight") == 0 || strcmp(code, "KeyD") == 0) return (1u << 4);
    if (strcmp(code, "ArrowLeft") == 0 || strcmp(code, "KeyA") == 0) return (1u << 5);
    if (strcmp(code, "ArrowUp") == 0 || strcmp(code, "KeyW") == 0) return (1u << 6);
    if (strcmp(code, "ArrowDown") == 0 || strcmp(code, "KeyS") == 0) return (1u << 7);
    if (strcmp(code, "KeyE") == 0 || strcmp(code, "KeyO") == 0) return (1u << 8); /* R */
    if (strcmp(code, "KeyQ") == 0 || strcmp(code, "KeyU") == 0) return (1u << 9); /* L */
    return 0;
}

EM_JS(int, js_is_dom_input_focused, (void), {
    const ae = document.activeElement;
    return (ae && (ae.tagName === "INPUT" || ae.tagName === "TEXTAREA" || ae.tagName === "SELECT")) ? 1 : 0;
});

static EM_BOOL OnKeyEvent(int eventType, const EmscriptenKeyboardEvent *e, void *userData)
{
    (void)userData;
    uint16_t bit = MapDomCodeToGbaBit(e->code);
    if (bit == 0)
        return EM_FALSE;

    /* Ignore when typing into HUD modal inputs */
    if (js_is_dom_input_focused()) {
        return EM_FALSE;
    }

    if (eventType == EMSCRIPTEN_EVENT_KEYDOWN) {
        if (!e->repeat)
            sLatchedKeysMask |= bit;
        WasmPlatform_SetKeys(sPressedKeysMask | bit);
    } else if (eventType == EMSCRIPTEN_EVENT_KEYUP) {
        WasmPlatform_SetKeys(sPressedKeysMask & ~bit);
    }

    return EM_TRUE;
}

static EM_BOOL OnBlurEvent(int eventType, const EmscriptenFocusEvent *e, void *userData)
{
    (void)eventType;
    (void)e;
    (void)userData;
    sLatchedKeysMask = 0;
    WasmPlatform_SetKeys(0);
    return EM_FALSE;
}
#endif

int main(void)
{
    WasmPlatform_Init();
    WasmMp_NotifyReady();

#ifdef __EMSCRIPTEN__
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, OnKeyEvent);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, OnKeyEvent);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, OnBlurEvent);
    emscripten_set_main_loop(MainLoopTick, 0, 1);
#else
    WasmPlatform_StepFrame();
#endif
    return 0;
}
