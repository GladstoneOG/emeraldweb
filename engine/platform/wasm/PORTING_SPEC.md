# `pokeemerald-expansion` -> Emscripten WASM Porting Specification

Target codebase: `engine/` (`rh-hideout/pokeemerald-expansion`, commit `dc122d46b6bcee0b4dcd17490af67d57dd967dab`)
Reference architectures audited:
- `tripplyons/pokeemerald-wasm` (software GBA PPU, HBlank HDMA table, flash sector loop, SWI BIOS shims)
- `gradenGnostic/pokeemerald-multiplatform` (virtualized `REG_BASE`/`PLTT`/`VRAM`/`OAM`/`FLASH_BASE` C arrays, real C M4A/CGB mixer)

## 1. Execution Model: Frame Stepper (`WasmRunFrame`) vs Emscripten Fibers vs Asyncify

Audit of all `VBlankIntrWait` / `WaitForVBlank` callers in `engine/src/` and `engine/include/`:
- `src/main.c:172,435` (`AgbMainLoop()` -> `WaitForVBlank()` -> `VBlankIntrWait()`) — **the ONLY normal gameplay caller**.
- `src/assertf.c:398` (assertion crash handler), `src/pokemon_sprite_visualizer.c:1475,1753,1784` (debug sprite viewer), `include/test/test.h:210` (test suite) — diagnostic/non-gameplay paths only.
- Note: Under Emscripten, `emscripten_fiber_swap()` (`<emscripten/fiber.h>`) is implemented **on top of `-sASYNCIFY`**. Linking fiber support therefore triggers full-program Asyncify stack instrumentation unless pruned with `-sASYNCIFY_ONLY`.

| Option | Behavior at `VBlankIntrWait()` / `AgbMainLoop()` | Binary Size / Speed | Recommendation |
|---|---|---|---|
| **Refactored `WasmRunFrame()` (`WASM_USE_FIBER=0`)** | `AgbMain()` initializes systems and returns; `emscripten_set_main_loop` calls `AgbMainLoopOnce()` (`speed` times per vsync). Assert/visualizer wait loops break early on WASM. | Zero `-sASYNCIFY` bloat, fastest execution, tiny `.wasm`, deterministic `1x`/`2x`/`3x`. | **Primary (`WASM_USE_FIBER=0`)** for production 400+ C file `index.wasm` |
| **Emscripten Fibers (`WASM_USE_FIBER=1` + `-sASYNCIFY`)** | `AgbMain()` runs unmodified on a 256 KiB fiber stack; `VBlankIntrWait()` swaps to `sMainFiber`. Useful for standalone smoke harness or unpatched nested wait loops. | Requires `-sASYNCIFY`; use `-sASYNCIFY_ONLY` if enabled in full build to avoid 1.5x-2.0x code bloat. | **Smoke harness default (`wasm-smoke`) / fallback** |

## 2. Virtualized Hardware Memory Map (`emcc` + Libc)

Instead of requiring `clang --target=wasm32-unknown-unknown` with fixed 256 MiB linear memory (`0x04000000..0x0E000000`), `engine/platform/wasm/` virtualizes hardware regions as aligned C arrays so normal `emcc` (`malloc`, `vsnprintf`, `setjmp`, `emscripten_fiber`, `EM_JS`, WebAudio, dynamic memory growth) works cleanly:

- `REG_BASE` -> `(uintptr_t)gWasmIoRegs` (`0x400` bytes)
- `PLTT` -> `(uintptr_t)gWasmPltt` (`0x400` bytes, 512 BGR555 entries)
- `VRAM` -> `(uintptr_t)gWasmVram` (`0x18000` bytes)
- `OAM` -> `(uintptr_t)gWasmOam` (`0x400` bytes)
- `FLASH_BASE` -> `gWasmFlash` (`0x20000` bytes = 128 KiB 1M Flash)
- `SOUND_INFO_PTR`, `INTR_CHECK`, `INTR_VECTOR` -> C globals (`gWasmSoundInfoPtr`, `gWasmIntrCheck`, `gWasmIntrVector`)

## 3. Identified `pokeemerald-expansion` Compile & Runtime Blockers

1. **ARM / Thumb Assembly Functions (`src/` + `libagbsyscall/`)**:
   - `ARM_FUNC` (`__attribute__((target("arm")))`) in `include/gba/defines.h`, `src/battle_ai_util.c`, `src/battle_util.c`, `src/config_changes.c`, `src/decompress.c`, `src/pokemon.c` -> `#define ARM_FUNC` empty when `#ifdef WASM`.
   - `src/random.c:45` (`u32 NAKED Random32(void)` Thumb asm) -> `#ifdef WASM` calls `_SFC32_Next_Stream(&gRngValue, STREAM1);`.
   - `src/m4a.c:316` (`asm("swi 0x2A")` in `MusicPlayerJumpTableCopy`), `src/librfu_intr.c:398-418` (`NAKED asm("bx ...")`), `src/multiboot.c:439` (`MultiBootWaitCycles`), `src/libisagbprn.c` (hardcoded emulator print addresses), `src/siirtc.c` (GPIO `0x80000C4`) -> `#ifdef WASM` C equivalents/stubs.
   - `src/crt0.s`, `src/rom_header.s`, `src/decompress_asm.s`, `src/libgcnmultiboot.s`, `src/m4a_1.s`, `libagbsyscall/libagbsyscall.s` -> excluded in `wasm_build.mk` and implemented in `engine/platform/wasm/`.
2. **Smol Decompression Executing Code on the Stack (`src/decompress.c`)**:
   - `pokeemerald-expansion` decompresses `.smol` sprites/tilemaps using `CopyFuncToIwram` (`FastUnsafeCopy32` from `src/decompress_asm.s`) to copy ARM machine code into a stack `u32 funcBuffer[]` and branch to `(void *)funcBuffer` for `DecodeLOtANSLoop`, `DecodeSymtANSLoop`, `DecodeSymDeltatANSLoop`, `DecodeInstructions`, `DeltaDecodeTileNumbers`, and `LZ77UnCompWRAMOptimized`.
   - WASM enforces Harvard memory separation (`W^X`): data pointers cannot be executed via `call_indirect`. Under `#ifdef WASM`, `CopyFuncToIwram` and `SwitchToArmCall*` must invoke the C decoder functions directly.
3. **Script Effect Instrumentation Pointer Tagging (`src/script.c`, `include/script.h`, `asm/macros/event.inc`, `data/*.inc`)**:
   - `callnative`, `gotonative`, `data/script_cmd_table.inc`, and `data/specials.inc` add `ROM_SIZE` (`0x02000000`) to function pointers (`func + ROM_SIZE`) so runtime code tests `(((uintptr_t)func) & 0xE000000) == 0xA000000` to check `requests_effects=1`.
   - In WASM (`call_indirect` table indices `1..N`), adding `0x02000000` corrupts function pointers and crashes indirect calls.
   - Fix in `engine/tools/wasm_elf_to_c.py` + `#ifdef WASM` in `script.c`/`script.h`: keep `addend = 0` on function relocations while emitting a `WasmIsEffectInstrumentedTarget(const void *fn)` lookup table for every relocation whose ELF addend was `ROM_SIZE` (`0x02000000`).
   - Also replace `__builtin_setjmp` / `__builtin_longjmp` in `src/script.c:623,662,699` with `<setjmp.h>` (`setjmp` / `longjmp` + `-sSUPPORT_LONGJMP=emscripten`).
4. **Flash Self-Modifying Code (`src/agb_flash.c`)**:
   - `ReadFlash` / `VerifyFlashSector` / `ReadFlashId` copy Thumb instructions into `readFlash1Buffer` on the stack (`SetReadFlash1`). Under `#ifdef WASM`, read/verify directly from `gWasmFlash`.
5. **Multiplayer Pointer-Cast Assumptions**:
   - Check 32-bit vs `uintptr_t` pointer casts in `CpuSmartCopy16`/`32` (`include/gba/macro.h`) — `wasm32` uses 32-bit pointers so pointer sizes match GBA, making `u32 <-> uintptr_t` binary-compatible except where tagged with GBA mirror bits (`0x0A000000`).

## 4. `INCBIN` / `INCGFX` / `.s` Data & Asset Strategy

1. **`INCBIN_*` and `INCGFX_*` inside C files**: Already resolved at compile time by `engine/tools/preproc/preproc -i -g` (`tools/preproc/c_file.cpp`) after `gbagfx`, `compresSmol`, `mapjson`, `jsonproc`, `trainerproc`, and `wav2agb` build asset intermediates. No compiler `.incbin` hack is needed inside C translation units.
2. **Assembly Data Files (`data/*.s` + `sound/songs/**/*.s`)**: Notice `pokeemerald-expansion` already moved battle AI out of assembly into C (`data/battle_ai_scripts.s` does not exist). The remaining 14 `data/*.s` files (scripts, maps, `sound_data.s`) and MIDI `.s` song tables are processed by `preproc | cpp | preproc | arm-none-eabi-as` into standard ARM ELF `.o` files, then converted to portable C arrays + symbol relocations by `engine/tools/wasm_elf_to_c.py`.
3. **Cloudflare 25 MiB Chunking Strategy**: Keep code + essential boot data inside `index.wasm` (check final `-Os` + `-flto` size first), and if `.wasm` or `.data` exceeds 20 MiB, `tools/chunk_assets.py` splits outputs in `public/chunks/` (`<19 MiB` `.partXX` files + SHA-256 manifest loaded via IndexedDB in `public/net_bridge.js`).

## 5. Landed `#ifdef WASM_PLATFORM` Engine Patches & Multiplayer Engine Modules

All hardware virtualization headers (`include/gba/defines.h`, `io_reg.h`, `flash_internal.h`, `macro.h`, `include/global.h`, `include/script.h`) and Phase 1 C blocker guards are landed while preserving `#else` GBA ROM builds untouched:

- `src/main.c`: `WasmRunFrame()` stepper (`!WASM_USE_FIBER`) + `AgbMainLoop()` (`WASM_USE_FIBER=1`), `WasmIntrMain()`, fiber-safe `WaitForVBlank()`, `MpNet_Init()`, `MpNet_StepFrame()`, `MpOverworld_OnFrame()`, `MpCutscene_OnFrame()`.
- `src/random.c`: pure C `_SFC32_Next_Stream(&gRngValue, STREAM1)` replacing Thumb asm `Random32()`.
- `src/decompress.c`: direct C calls to `DecodeLOtANSLoop`, `DecodeSymDeltatANSLoop`, `DecodeInstructions`, `DeltaDecodeTileNumbers`, and `LZ77UnCompWram`, removing stack execution (`CopyFuncToIwram` / `SwitchToArmCall*`).
- `src/script.c`: `<setjmp.h>` (`setjmp`/`longjmp`) + `WasmScriptEffects_IsInstrumented()` + clean pointer equality without `0xA000000` mirror tags.
- `src/agb_flash.c` + `src/agb_flash_1m.c`: routes `SwitchFlashBank`, `ReadFlashId` (`0x09C2`), `SetReadFlash1`, `ReadFlash`, `VerifyFlashSector*`, `ProgramFlash*`, `EraseFlash*` to `gWasmFlash` via `platform/wasm/wasm_input_flash.c`.
- `src/siirtc.c`: routes wall-clock BCD reads through `WasmRtc_GetBcdDateTime()` without `0x80000C4` GPIO writes.
- `src/m4a.c`, `src/libisagbprn.c`, `src/librfu_intr.c`, `src/multiboot.c`: eliminates inline ARM/Thumb `swi 0x2A`, `0xEFFF`, `bx r*`, and `asm_unified`.
- `src/mp/{mp_protocol.h,mp_net.*,mp_world_sync.*,mp_overworld.*,mp_cutscene_sync.*}` + `src/event_data.c` & `src/overworld.c` hooks: implements Channels 0..5 routing, global story/boss/HM/badge and defeated-trainer (`TRAINER_FLAGS_START..TRAINER_FLAGS_END`) replication via Host across peers (keeping item balls `0x3E8..0x4BF`, hidden items `0x1F4..0x273`, berry trees, and NPC gifts per-player), HM auto-grant on badge unlock, remote ObjectEvent sprites on matching `(mapGroup, mapNum)`, Teleport-to-Friend fade warp, and Stardew-style Global Cutscene sync with pre-cutscene return warp.
