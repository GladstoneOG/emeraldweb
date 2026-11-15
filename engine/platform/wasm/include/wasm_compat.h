#ifndef GUARD_WASM_COMPAT_H
#define GUARD_WASM_COMPAT_H

/*
 * Force-included compatibility layer when compiling pokeemerald-expansion under
 * Emscripten (`-DWASM=1 -include platform/wasm/include/wasm_compat.h`).
 * Neutralizes GBA-only ARM attributes, keeps standard C/WASM headers valid,
 * and bridges virtualized MMIO bases cleanly.
 */

#if defined(WASM) || defined(WASM_PLATFORM)
#ifndef WASM
#define WASM 1
#endif
#ifndef WASM_PLATFORM
#define WASM_PLATFORM 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* Ensure ARM_FUNC / NAKED do not emit invalid ARM target attributes in wasm32 clang */
#ifndef WASM_STRIP_ARM_ATTR
#define WASM_STRIP_ARM_ATTR 1
#endif

/* Forward virtualized hardware backing arrays */
extern uint8_t gWasmEwram[];
extern uint8_t gWasmIwram[];
extern volatile uint8_t gWasmIoRegs[];
extern uint8_t gWasmPltt[];
extern uint8_t gWasmVram[];
extern uint8_t gWasmOam[];
extern uint8_t gWasmFlash[];

struct SoundInfo;
extern struct SoundInfo *gWasmSoundInfoPtr;
extern volatile uint16_t gWasmIntrCheck;
extern void *gWasmIntrVector;

void WasmDma_Transfer(int dmaNum, uintptr_t src, uintptr_t dst, uint32_t control);

#endif /* WASM */

#endif /* GUARD_WASM_COMPAT_H */
