#include "wasm_platform.h"
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static WasmMpPacket sRxQueue[WASM_MP_QUEUE_CAP];
static uint32_t sRxHead = 0;
static uint32_t sRxTail = 0;

static bool sHasPendingWarp = false;
static int sWarpGroup = 0;
static int sWarpNum = 0;
static int sWarpX = 0;
static int sWarpY = 0;

static bool sHasPendingBattle = false;
static uint32_t sPendingBattleSeed = 0;
static int sPendingBattleMode = 0;
static int sPendingBattlePartnerSlot = 0xFF;
static int sPendingBattlePartnerPeerId = 0;

static int sPendingLinkDeaths[32];
static uint32_t sLinkDeathHead = 0;
static uint32_t sLinkDeathTail = 0;

static bool sTradeCommitted = false;
static bool sSnapshotLoaded = false;
static bool sResetNewGameRequested = false;
static int sResetNewGameMode = 0;

/* Dynamic set of function pointers tagged with requests_effects=1 */
#define WASM_MAX_EFFECT_FUNCS 1024
static const void *sEffectFuncs[WASM_MAX_EFFECT_FUNCS];
static size_t sEffectFuncCount = 0;

void WasmScriptEffects_RegisterTable(const void *const *funcs, size_t count)
{
    for (size_t i = 0; i < count && sEffectFuncCount < WASM_MAX_EFFECT_FUNCS; i++) {
        if (funcs[i] != NULL) {
            sEffectFuncs[sEffectFuncCount++] = funcs[i];
        }
    }
}

uint8_t WasmScriptEffects_IsInstrumented(const void *func)
{
    if (func == NULL)
        return 0;
    for (size_t i = 0; i < sEffectFuncCount; i++) {
        if (sEffectFuncs[i] == func)
            return 1;
    }
    return 0;
}

__attribute__((weak)) void MpNet_OnSpeedChangedFromBridge(int speed)
{
    (void)speed;
}

__attribute__((weak)) bool MpOverworld_SaveGameNow(void)
{
    return false;
}

__attribute__((weak)) void MpNet_UpdatePeerMeta(uint8_t slotIndex, uint16_t peerId, bool connected, uint8_t pairTag, const char *name)
{
    (void)slotIndex;
    (void)peerId;
    (void)connected;
    (void)pairTag;
    (void)name;
}

__attribute__((weak)) void MpSoulLink_ImportStateJson(const char *jsonPtr)
{
    (void)jsonPtr;
}

__attribute__((weak)) void MpSoulLink_KillByUid(uint32_t otId, uint32_t personality)
{
    (void)otId;
    (void)personality;
}

void WasmMp_Init(void)
{
    sRxHead = 0;
    sRxTail = 0;
    sHasPendingWarp = false;
    sHasPendingBattle = false;
    sPendingBattlePartnerSlot = 0xFF;
    sPendingBattlePartnerPeerId = 0;
    sLinkDeathHead = 0;
    sLinkDeathTail = 0;
    sTradeCommitted = false;
    sSnapshotLoaded = false;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_mp_notify_ready, (void), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge.onWasmReady === "function" &&
        typeof Module !== "undefined") {
        window.PokemonWasmBridge.onWasmReady(Module);
    }
});

EM_JS(int, js_mp_send_packet, (int channel, int targetPeerId, uintptr_t ptr, int len), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge.sendPacket === "function") {
        return window.PokemonWasmBridge.sendPacket(channel, targetPeerId, ptr, len) ? 1 : 0;
    }
    return 0;
});

EM_JS(void, js_mp_report_local_state, (const char *jsonPtr), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge.reportLocalState === "function") {
        window.PokemonWasmBridge.reportLocalState(UTF8ToString(jsonPtr));
    }
});

EM_JS(int, js_mp_get_run_config_json, (char *dstPtr, int maxLen), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge.getRunConfigJson === "function") {
        const str = String(window.PokemonWasmBridge.getRunConfigJson() || "");
        stringToUTF8(str, dstPtr, maxLen);
        return Math.min(lengthBytesUTF8(str), maxLen - 1);
    }
    return 0;
});

EM_JS(int, js_mp_get_local_slot_index, (void), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge.getLocalSlotIndex === "function") {
        return Number(window.PokemonWasmBridge.getLocalSlotIndex()) || 0;
    }
    return 0;
});

EM_JS(int, js_mp_get_speed_multiplier, (void), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge.getSpeedMultiplier === "function") {
        return Number(window.PokemonWasmBridge.getSpeedMultiplier()) || 1;
    }
    return 1;
});

EM_JS(void, js_mp_notify_teammate_busy, (int partnerSlot, const char *reasonPtr), {
    var reason = (reasonPtr && typeof UTF8ToString === "function") ? UTF8ToString(reasonPtr) : "busy";
    if (typeof window !== "undefined" && window.PokemonNet &&
        typeof window.PokemonNet.onTeammateBusyFallback === "function") {
        window.PokemonNet.onTeammateBusyFallback(partnerSlot, reason || "busy");
    }
});
#endif

void WasmMp_NotifyReady(void)
{
#ifdef __EMSCRIPTEN__
    js_mp_notify_ready();
#endif
}

void WasmMp_NotifyTeammateBusy(int partnerSlot, const char *reason)
{
#ifdef __EMSCRIPTEN__
    js_mp_notify_teammate_busy(partnerSlot, (reason != NULL && reason[0] != '\0') ? reason : "busy");
#else
    (void)partnerSlot;
    (void)reason;
#endif
}

bool WasmMp_SendPacket(uint8_t channel, uint16_t targetPeerId, const uint8_t *data, uint16_t len)
{
#ifdef __EMSCRIPTEN__
    return js_mp_send_packet((int)channel, (int)targetPeerId, (uintptr_t)data, (int)len) != 0;
#else
    (void)channel;
    (void)targetPeerId;
    (void)data;
    (void)len;
    return false;
#endif
}

void WasmMp_ReportLocalState(const char *jsonState)
{
#ifdef __EMSCRIPTEN__
    if (jsonState)
        js_mp_report_local_state(jsonState);
#else
    (void)jsonState;
#endif
}

bool WasmMp_GetRunConfigJson(char *outBuf, size_t maxLen)
{
    if (!outBuf || maxLen == 0)
        return false;
    outBuf[0] = '\0';
#ifdef __EMSCRIPTEN__
    return js_mp_get_run_config_json(outBuf, (int)maxLen) > 0;
#else
    return false;
#endif
}

int WasmMp_GetLocalSlotIndex(void)
{
#ifdef __EMSCRIPTEN__
    return js_mp_get_local_slot_index();
#else
    return 0;
#endif
}

int WasmMp_GetSpeedMultiplierFromBridge(void)
{
#ifdef __EMSCRIPTEN__
    return js_mp_get_speed_multiplier();
#else
    return 1;
#endif
}

bool WasmMp_PopPacket(WasmMpPacket *outPacket)
{
    if (sRxTail == sRxHead)
        return false;
    *outPacket = sRxQueue[sRxTail % WASM_MP_QUEUE_CAP];
    sRxTail++;
    return true;
}

bool WasmMp_ConsumePendingWarp(int *outGroup, int *outNum, int *outX, int *outY)
{
    if (!sHasPendingWarp)
        return false;
    *outGroup = sWarpGroup;
    *outNum = sWarpNum;
    *outX = sWarpX;
    *outY = sWarpY;
    sHasPendingWarp = false;
    return true;
}

bool WasmMp_ConsumePendingBattle(uint32_t *outSeed, int *outMode, int *outPartnerSlot, int *outPartnerPeerId)
{
    if (!sHasPendingBattle)
        return false;
    if (outSeed != NULL)
        *outSeed = sPendingBattleSeed;
    if (outMode != NULL)
        *outMode = sPendingBattleMode;
    if (outPartnerSlot != NULL)
        *outPartnerSlot = sPendingBattlePartnerSlot;
    if (outPartnerPeerId != NULL)
        *outPartnerPeerId = sPendingBattlePartnerPeerId;
    sHasPendingBattle = false;
    return true;
}

int WasmMp_PopPendingLinkDeath(void)
{
    if (sLinkDeathTail == sLinkDeathHead)
        return -1;
    int id = sPendingLinkDeaths[sLinkDeathTail % 32u];
    sLinkDeathTail++;
    return id;
}

bool WasmMp_ConsumeTradeCommitFlag(void)
{
    bool v = sTradeCommitted;
    sTradeCommitted = false;
    return v;
}

bool WasmMp_ConsumeSnapshotLoadedFlag(void)
{
    bool v = sSnapshotLoaded;
    sSnapshotLoaded = false;
    return v;
}

bool WasmMp_ConsumeResetNewGameRequest(int *outMode)
{
    if (!sResetNewGameRequested)
        return false;
    if (outMode != NULL)
        *outMode = sResetNewGameMode;
    sResetNewGameRequested = false;
    sResetNewGameMode = 0;
    return true;
}

/* =========================================================================
 * Exported JS Entry Points (called directly by public/net_bridge.js)
 * ========================================================================= */
WASM_KEEPALIVE void mp_set_speed_multiplier(int speed)
{
    int clamped = speed;
    if (clamped < 1) clamped = 1;
    if (clamped > 3) clamped = 3;
    WasmPlatform_SetSpeedMultiplier(clamped);
    MpNet_OnSpeedChangedFromBridge(clamped);
}

WASM_KEEPALIVE void mp_ingest_packet(int channel, int fromPeerId, const uint8_t *ptr, int len)
{
    if (!ptr || len <= 0 || len > WASM_MAX_MP_PAYLOAD)
        return;
    if ((sRxHead - sRxTail) >= WASM_MP_QUEUE_CAP)
        return; /* drop oldest overflow guard */

    WasmMpPacket *slot = &sRxQueue[sRxHead % WASM_MP_QUEUE_CAP];
    slot->channel = (uint8_t)channel;
    slot->fromPeerId = (uint16_t)fromPeerId;
    slot->length = (uint16_t)len;
    memcpy(slot->data, ptr, (size_t)len);
    sRxHead++;
}

WASM_KEEPALIVE void mp_trigger_warp(int mapGroup, int mapNum, int x, int y)
{
    sWarpGroup = mapGroup;
    sWarpNum = mapNum;
    sWarpX = x;
    sWarpY = y;
    sHasPendingWarp = true;
}

WASM_KEEPALIVE void mp_start_synced_battle(uint32_t battleSeed, int mode, int partnerSlot, int partnerPeerId)
{
    sPendingBattleSeed = battleSeed;
    sPendingBattleMode = mode;
    sPendingBattlePartnerSlot = partnerSlot;
    sPendingBattlePartnerPeerId = partnerPeerId;
    sHasPendingBattle = true;
}

WASM_KEEPALIVE void mp_update_peer_meta(int slotIndex, int peerId, int connected, int pairTag, const char *name)
{
    if (slotIndex < 0 || slotIndex >= 4)
        return;
    MpNet_UpdatePeerMeta((uint8_t)slotIndex,
                         (peerId > 0) ? (uint16_t)peerId : 0u,
                         connected != 0,
                         (pairTag >= 0 && pairTag <= 255) ? (uint8_t)pairTag : 0u,
                         name);
}

WASM_KEEPALIVE void mp_kill_linked_pokemon(int linkIdNum)
{
    if ((sLinkDeathHead - sLinkDeathTail) < 32u) {
        sPendingLinkDeaths[sLinkDeathHead % 32u] = linkIdNum;
        sLinkDeathHead++;
    }
}

WASM_KEEPALIVE void mp_import_soullink_json(const char *jsonPtr)
{
    MpSoulLink_ImportStateJson(jsonPtr);
}

WASM_KEEPALIVE void mp_kill_mon_by_uid(uint32_t otId, uint32_t personality)
{
    MpSoulLink_KillByUid(otId, personality);
}

WASM_KEEPALIVE void mp_on_trade_commit(void)
{
    sTradeCommitted = true;
}

WASM_KEEPALIVE void mp_on_snapshot_loaded(void)
{
    sSnapshotLoaded = true;
}

WASM_KEEPALIVE void mp_reset_new_game(int mode)
{
    sResetNewGameMode = mode;
    sResetNewGameRequested = true;
}

WASM_KEEPALIVE int mp_save_game_now(void)
{
    return MpOverworld_SaveGameNow() ? 1 : 0;
}

#ifdef WASM_SMOKE_TEST
WASM_KEEPALIVE int mp_test_query(int kind, int arg0, int arg1)
{
    (void)kind;
    (void)arg0;
    (void)arg1;
    return 0;
}

WASM_KEEPALIVE int mp_test_action(int kind, int arg0, int arg1, int arg2)
{
    (void)kind;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    return 0;
}
#endif
