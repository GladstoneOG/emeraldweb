#ifndef GUARD_WASM_PLATFORM_H
#define GUARD_WASM_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define WASM_KEEPALIVE
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * GBA Hardware Constants & Virtualized MMIO Regions
 * ========================================================================= */
#define WASM_DISPLAY_WIDTH  240
#define WASM_DISPLAY_HEIGHT 160
#define WASM_DISPLAY_PIXELS (WASM_DISPLAY_WIDTH * WASM_DISPLAY_HEIGHT)

#define WASM_EWRAM_SIZE 0x40000  /* 256 KiB */
#define WASM_IWRAM_SIZE 0x8000   /*  32 KiB */
#define WASM_IOREG_SIZE 0x400    /*   1 KiB */
#define WASM_PLTT_SIZE  0x400    /*   1 KiB */
#define WASM_VRAM_SIZE  0x18000  /*  96 KiB */
#define WASM_OAM_SIZE   0x400    /*   1 KiB */
#define WASM_FLASH_SIZE 0x20000  /* 128 KiB (1Mbit Flash) */

#define WASM_MAX_MP_PAYLOAD 2048
#define WASM_MP_QUEUE_CAP   64

struct SoundInfo;

extern uint8_t gWasmEwram[WASM_EWRAM_SIZE];
extern uint8_t gWasmIwram[WASM_IWRAM_SIZE];
extern volatile uint8_t gWasmIoRegs[WASM_IOREG_SIZE];
extern uint8_t gWasmPltt[WASM_PLTT_SIZE];
extern uint8_t gWasmVram[WASM_VRAM_SIZE];
extern uint8_t gWasmOam[WASM_OAM_SIZE];
extern uint8_t gWasmFlash[WASM_FLASH_SIZE];

extern struct SoundInfo *gWasmSoundInfoPtr;
extern volatile uint16_t gWasmIntrCheck;
extern void *gWasmIntrVector;

/* Per-scanline HDMA/register snapshot for authentic GBA raster effects */
typedef struct WasmScanlineRegs {
    uint16_t dispcnt;
    uint16_t bgcnt[4];
    uint16_t bghofs[4];
    uint16_t bgvofs[4];
    int16_t bg2pa, bg2pb, bg2pc, bg2pd;
    int32_t bg2x, bg2y;
    int16_t bg3pa, bg3pb, bg3pc, bg3pd;
    int32_t bg3x, bg3y;
    uint16_t win0h, win1h, win0v, win1v;
    uint16_t winin, winout;
    uint16_t mosaic;
    uint16_t bldcnt, bldalpha, bldy;
} WasmScanlineRegs;

extern WasmScanlineRegs gWasmScanlineRegs[WASM_DISPLAY_HEIGHT];

/* =========================================================================
 * Platform Lifecycle, Timing & Input
 * ========================================================================= */
void WasmPlatform_Init(void);
void WasmPlatform_StepFrame(void);
void WasmPlatform_VBlankYield(void);
void WasmPlatform_SetKeys(uint16_t pressedMask);
uint16_t WasmPlatform_GetKeys(void);
bool WasmPlatform_IsAuthSignedIn(void);
void WasmPlatform_SetSpeedMultiplier(int speed);
int WasmPlatform_GetSpeedMultiplier(void);

/* Game entry point when linking full pokeemerald-expansion */
void AgbMain(void);

/* =========================================================================
 * MMIO & DMA Shims
 * ========================================================================= */
void WasmMmio_Reset(void);
void WasmMmio_BeginFrame(void);
void WasmMmio_LatchFrameScanlines(void);
void WasmMmio_CaptureScanline(int y);
void WasmDma_Transfer(int dmaNum, uintptr_t src, uintptr_t dst, uint32_t control);

/* =========================================================================
 * Software GBA PPU Renderer (240x160 ABGR8888 / Canvas RGBA8888)
 * ========================================================================= */
void WasmPpu_Init(void);
void WasmPpu_RenderFrame(void);
const uint32_t *WasmPpu_GetFramebuffer(void);
void WasmPpu_FlushFrameToCanvas(void);

/* =========================================================================
 * Audio & M4A / CGB Mixer Hooks
 * ========================================================================= */
void WasmAudio_Init(void);
void WasmAudio_SetMuted(bool muted);
void WasmAudio_MixFrame(void);
uint32_t WasmAudio_ReadSamples(int16_t *outInterleaved, uint32_t maxFrames);

/* =========================================================================
 * Flash Save & Wall-Clock RTC
 * ========================================================================= */
void WasmFlash_Init(void);
void WasmFlash_LoadFromStorage(void);
void WasmFlash_SyncToStorage(void);
void WasmFlash_ClearStorage(void);
void WasmFlash_SwitchBank(uint8_t bankNum);
void WasmFlash_Read(uint16_t sectorNum, uint32_t offset, uint8_t *dest, uint32_t size);
uint32_t WasmFlash_VerifySector(uint16_t sectorNum, const uint8_t *src);
uint32_t WasmFlash_VerifySectorNBytes(uint16_t sectorNum, const uint8_t *src, uint32_t size);
uint16_t WasmFlash_EraseChip(void);
uint16_t WasmFlash_EraseSector(uint16_t sectorNum);
uint16_t WasmFlash_ProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data);
uint16_t WasmFlash_ProgramSector(uint16_t sectorNum, uint8_t *src);
void WasmRtc_GetBcdDateTime(uint8_t out7[7]);
void WasmDebugPrintf(const char *fmt, ...);

/* =========================================================================
 * Script Effect Metadata Lookup (Replaces ROM Mirror 1 +0x02000000 trick)
 * ========================================================================= */
void WasmScriptEffects_RegisterTable(const void *const *funcs, size_t count);
uint8_t WasmScriptEffects_IsInstrumented(const void *func);
void MpNet_OnSpeedChangedFromBridge(int speed);

/* =========================================================================
 * Multiplayer C <-> JS Bridge (Aligned with public/net_bridge.js)
 * ========================================================================= */
typedef struct WasmMpPacket {
    uint16_t fromPeerId;
    uint8_t channel;
    uint16_t length;
    uint8_t data[WASM_MAX_MP_PAYLOAD];
} WasmMpPacket;

void WasmMp_Init(void);
void WasmMp_NotifyReady(void);
void WasmMp_NotifyTeammateBusy(int partnerSlot, const char *reason);
bool WasmMp_SendPacket(uint8_t channel, uint16_t targetPeerId, const uint8_t *data, uint16_t len);
void WasmMp_ReportLocalState(const char *jsonState);
bool WasmMp_GetRunConfigJson(char *outBuf, size_t maxLen);
int WasmMp_GetLocalSlotIndex(void);
int WasmMp_GetSpeedMultiplierFromBridge(void);
bool WasmMp_PopPacket(WasmMpPacket *outPacket);
bool WasmMp_ConsumePendingWarp(int *outGroup, int *outNum, int *outX, int *outY);
bool WasmMp_ConsumePendingBattle(uint32_t *outSeed, int *outMode, int *outPartnerSlot, int *outPartnerPeerId);
int WasmMp_PopPendingLinkDeath(void);
bool WasmMp_ConsumeTradeCommitFlag(void);
bool WasmMp_ConsumeSnapshotLoadedFlag(void);
bool WasmMp_ConsumeResetNewGameRequest(int *outMode);
bool WasmMp_ConsumeReturnToTitleRequest(void);
int WasmPlatform_InvokeFnPtr(uintptr_t fnPtr, int arg0, int arg1);

/* Exported JS-callable symbols matching public/net_bridge.js */
WASM_KEEPALIVE void mp_set_speed_multiplier(int speed);
WASM_KEEPALIVE void mp_ingest_packet(int channel, int fromPeerId, const uint8_t *ptr, int len);
WASM_KEEPALIVE void mp_trigger_warp(int mapGroup, int mapNum, int x, int y);
WASM_KEEPALIVE void mp_start_synced_battle(uint32_t battleSeed, int mode, int partnerSlot, int partnerPeerId);
WASM_KEEPALIVE void mp_update_peer_meta(int slotIndex, int peerId, int connected, int pairTag, const char *name);
WASM_KEEPALIVE void mp_kill_linked_pokemon(int linkIdNum);
WASM_KEEPALIVE void mp_on_trade_commit(void);
WASM_KEEPALIVE void mp_on_snapshot_loaded(void);
WASM_KEEPALIVE void mp_reset_new_game(int mode);
WASM_KEEPALIVE int mp_return_to_title_screen(int autosave);
WASM_KEEPALIVE int mp_save_game_now(void);
WASM_KEEPALIVE uintptr_t wasm_get_framebuffer_ptr(void);
WASM_KEEPALIVE void wasm_set_input_mask(int keyMask);
WASM_KEEPALIVE uint8_t *wasm_get_flash_ptr(void);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_WASM_PLATFORM_H */
