#include "mp_net.h"
#include "mp_cutscene_sync.h"
#include "mp_overworld.h"
#include "mp_world_sync.h"
#include "wasm_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MpRunConfig sRunConfig;
static MpPeerState sPeers[MP_MAX_PLAYERS];
static MpChannelHandler sHandlers[MP_CHANNEL_COUNT];
static bool sLocalInBattle = false;
static uint16_t sTxSeq = 0;
static uint32_t sFrameTick = 0;
static bool sHasConfigBaseline = false;
static uint64_t sLastSeedHash = 0;
static uint32_t sLastRandoFlags = 0;
static uint32_t sLastNuzFlags = 0;
static uint8_t sLastSoulLinkMode = 0;

/* -------------------------------------------------------------------------
 * Weak fallback hooks overridden by gameplay-coder modules when linked.
 * ------------------------------------------------------------------------- */
__attribute__((weak)) void MpBattle2v2_Init(void) {}
__attribute__((weak)) void MpBattle2v2_StepFrame(void) {}
__attribute__((weak)) void MpBattle2v2_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    (void)fromPeerId;
    (void)opcode;
    (void)payload;
    (void)len;
}

static bool sAuthoritativePairTag[MP_MAX_PLAYERS];

__attribute__((weak)) void MpBattle2v2_StartSyncedFromBridge(uint32_t battleSeed, uint8_t mode, uint8_t partnerSlot, uint16_t partnerPeerId)
{
    (void)battleSeed;
    (void)mode;
    (void)partnerSlot;
    (void)partnerPeerId;
}

__attribute__((weak)) void MpSoulLink_Init(void) {}
__attribute__((weak)) void MpSoulLink_StepFrame(void) {}
__attribute__((weak)) void MpSoulLink_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    (void)fromPeerId;
    (void)opcode;
    (void)payload;
    (void)len;
}

__attribute__((weak)) void MpSoulLink_KillByLinkId(uint16_t linkId)
{
    (void)linkId;
}

__attribute__((weak)) void MpSoulLink_OnTutorialCompleted(void) {}
__attribute__((weak)) void MpSoulLink_ImportStateJson(const char *jsonPtr) { (void)jsonPtr; }
__attribute__((weak)) void MpSoulLink_KillByUid(uint32_t otId, uint32_t personality)
{
    (void)otId;
    (void)personality;
}

__attribute__((weak)) void MpRandomizer_OnRunConfigChanged(const MpRunConfig *cfg)
{
    (void)cfg;
}

__attribute__((weak)) void MpTrade_Init(void) {}
__attribute__((weak)) void MpTrade_StepFrame(void) {}
__attribute__((weak)) void MpTrade_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    (void)fromPeerId;
    (void)opcode;
    (void)payload;
    (void)len;
}

__attribute__((weak)) void MpTrade_ApplyCommitFromBridge(void)
{
}

/* -------------------------------------------------------------------------
 * Minimal Zero-Heap JSON Helpers (tailored to net_bridge.js JSON schemas)
 * ------------------------------------------------------------------------- */
static const char *JsonSkipString(const char *p, const char *end)
{
    if (!p || p >= end || *p != '"')
        return p;
    p++;
    while (p < end && *p != '\0') {
        if (*p == '\\') {
            p++;
            if (p < end && *p != '\0')
                p++;
            continue;
        }
        if (*p == '"')
            return p + 1;
        p++;
    }
    return p;
}

static const char *JsonSkipNested(const char *p, const char *end)
{
    if (!p || p >= end || (*p != '{' && *p != '['))
        return p;
    char openCh = *p;
    char closeCh = (openCh == '{') ? '}' : ']';
    int depth = 0;
    while (p < end && *p != '\0') {
        if (*p == '"') {
            p = JsonSkipString(p, end);
            continue;
        }
        if (*p == openCh) {
            depth++;
        } else if (*p == closeCh) {
            depth--;
            if (depth <= 0)
                return p + 1;
        }
        p++;
    }
    return p;
}

static const char *JsonFindKey(const char *json, const char *key)
{
    if (!json || !key || key[0] == '\0')
        return NULL;

    size_t keyLen = strlen(key);
    if (keyLen >= 64u)
        return NULL;

    const char *p = json;
    while (*p && isspace((unsigned char)*p))
        p++;

    const char *end = json + strlen(json);
    if (*p == '{') {
        end = JsonSkipNested(p, end);
        p++;
    }

    while (p < end && *p != '\0') {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ','))
            p++;
        if (p >= end || *p == '\0' || *p == '}')
            break;

        if (*p == '{' || *p == '[') {
            p = JsonSkipNested(p, end);
            continue;
        }
        if (*p != '"') {
            p++;
            continue;
        }

        const char *strStart = p + 1;
        const char *afterStr = JsonSkipString(p, end);
        const char *strEnd = (afterStr > strStart && afterStr[-1] == '"') ? (afterStr - 1) : afterStr;
        p = afterStr;

        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p >= end || *p != ':')
            continue;
        p++;
        while (p < end && isspace((unsigned char)*p))
            p++;

        if ((size_t)(strEnd - strStart) == keyLen && memcmp(strStart, key, keyLen) == 0)
            return p;

        if (p < end) {
            if (*p == '"')
                p = JsonSkipString(p, end);
            else if (*p == '{' || *p == '[')
                p = JsonSkipNested(p, end);
            else {
                while (p < end && *p != ',' && *p != '}')
                    p++;
            }
        }
    }
    return NULL;
}

static bool JsonExtractString(const char *json, const char *key, char *out, size_t maxLen)
{
    if (!json || !key || !out || maxLen == 0)
        return false;
    out[0] = '\0';
    const char *p = JsonFindKey(json, key);
    if (!p || *p != '"')
        return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < maxLen) {
        if (*p == '\\' && p[1] != '\0')
            p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool JsonExtractInt(const char *json, const char *key, int32_t *outVal)
{
    if (!json || !key || !outVal)
        return false;
    const char *p = JsonFindKey(json, key);
    if (!p)
        return false;
    char *endPtr = NULL;
    long v = strtol(p, &endPtr, 10);
    if (endPtr == p)
        return false;
    *outVal = (int32_t)v;
    return true;
}

static bool JsonExtractBool(const char *json, const char *key, bool defaultVal)
{
    if (!json || !key)
        return defaultVal;
    const char *p = JsonFindKey(json, key);
    if (!p)
        return defaultVal;
    if (strncmp(p, "true", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_')
        return true;
    if (strncmp(p, "false", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_')
        return false;
    return defaultVal;
}

static uint32_t ParseRandomizerFlagsFromJson(const char *json, uint8_t preset)
{
    static const struct {
        const char *key;
        uint32_t bit;
        bool defBalanced;
    } kRandoKeys[] = {
        { "wildEncounters",     MP_RANDO_F_WILD_ENCOUNTERS,     true  },
        { "startersAndStatics", MP_RANDO_F_STARTERS_STATICS,    true  },
        { "trainers2v2",        MP_RANDO_F_TRAINERS_2V2,        true  },
        { "itemsAndMarts",      MP_RANDO_F_ITEMS_MARTS,         false },
        { "abilities",          MP_RANDO_F_ABILITIES,           false },
        { "banWonderGuard",     MP_RANDO_F_BAN_WONDER_GUARD,    true  },
        { "movesets",           MP_RANDO_F_MOVESETS,            false },
        { "types",              MP_RANDO_F_TYPES,               false },
        { "evolutions",         MP_RANDO_F_EVOLUTIONS,          false },
        { "preserveGymThemes",  MP_RANDO_F_PRESERVE_GYM_THEMES, true  },
        { "similarBst",         MP_RANDO_F_SIMILAR_BST,         true  },
    };
    const char *scope = JsonFindKey(json, "randomizerToggles");
    bool hasObjScope = (scope != NULL && *scope == '{');
    if (scope == NULL)
        scope = json;

    uint32_t flags = 0;
    for (size_t i = 0; i < sizeof(kRandoKeys) / sizeof(kRandoKeys[0]); i++) {
        bool def = (preset == MP_RANDO_OFF) ? false : kRandoKeys[i].defBalanced;
        bool enabled = hasObjScope ? JsonExtractBool(scope, kRandoKeys[i].key, def)
                                   : ((scope == json) ? JsonExtractBool(json, kRandoKeys[i].key, def) : def);
        if (enabled)
            flags |= kRandoKeys[i].bit;
    }
    return flags;
}

static uint32_t ParseNuzlockeFlagsFromJson(const char *json)
{
    static const struct {
        const char *key;
        uint32_t bit;
        bool defVal;
    } kNuzKeys[] = {
        { "enabled",          MP_NUZ_F_ENABLED,            true  },
        { "dupesClause",      MP_NUZ_F_DUPES_CLAUSE,       true  },
        { "shinyClause",      MP_NUZ_F_SHINY_CLAUSE,       true  },
        { "levelCaps",        MP_NUZ_F_LEVEL_CAPS,         false },
        { "setMode",          MP_NUZ_F_SET_MODE,           false },
        { "noBattleItems",    MP_NUZ_F_NO_BATTLE_ITEMS,    false },
        { "whiteoutGameOver", MP_NUZ_F_WHITEOUT_GAME_OVER, false },
    };
    const char *scope = JsonFindKey(json, "nuzlockeToggles");
    bool hasObjScope = (scope != NULL && *scope == '{');
    if (scope == NULL)
        scope = json;

    uint32_t flags = 0;
    for (size_t i = 0; i < sizeof(kNuzKeys) / sizeof(kNuzKeys[0]); i++) {
        bool enabled = hasObjScope ? JsonExtractBool(scope, kNuzKeys[i].key, kNuzKeys[i].defVal)
                                   : ((scope == json) ? JsonExtractBool(json, kNuzKeys[i].key, kNuzKeys[i].defVal) : kNuzKeys[i].defVal);
        if (enabled)
            flags |= kNuzKeys[i].bit;
    }
    return flags;
}

static uint8_t ParseBadgesArrayMask(const char *json)
{
    const char *arr = JsonFindKey(json, "badges");
    if (!arr || *arr != '[')
        return 0;
    uint8_t mask = 0;
    int idx = 0;
    const char *p = arr + 1;
    while (*p && *p != ']' && idx < 8) {
        if (strncmp(p, "true", 4) == 0) {
            mask |= (uint8_t)(1u << idx);
            idx++;
            p += 4;
        } else if (strncmp(p, "false", 5) == 0) {
            idx++;
            p += 5;
        } else {
            p++;
        }
    }
    return mask;
}

static void ApplyWorldPairsFromJson(const char *json, const char *key, bool isVars)
{
    const char *p = JsonFindKey(json, key);
    if (!p || *p != '[')
        return;
    p++;
    while (*p != '\0' && *p != ']') {
        while (*p != '\0' && *p != '[' && *p != ']')
            p++;
        if (*p != '[')
            break;
        p++;
        char *endPtr = NULL;
        long id = strtol(p, &endPtr, 10);
        if (endPtr == p)
            break;
        p = endPtr;
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        long val = strtol(p, &endPtr, 10);
        if (endPtr == p)
            break;
        p = endPtr;
        while (*p != '\0' && *p != ']')
            p++;
        if (*p == ']')
            p++;
        if (id > 0 && id <= 0xFFFFL) {
            if (isVars)
                MpWorldSync_ApplyRemoteVarFromBridge((uint16_t)id, (uint16_t)(val < 0 ? 0 : val));
            else
                MpWorldSync_ApplyRemoteFlagFromBridge((uint16_t)id, val != 0);
        }
    }
}

static uint16_t ExtractCutsceneId(const char *json)
{
    static const char *const kKeys[] = { "cutsceneId", "completedId" };
    for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
        int32_t intId = 0;
        if (JsonExtractInt(json, kKeys[k], &intId) && intId > 0)
            return (uint16_t)((uint32_t)intId % 512u);

        char strId[64];
        if (JsonExtractString(json, kKeys[k], strId, sizeof(strId)) && strId[0] != '\0') {
            char *endPtr = NULL;
            long parsed = strtol(strId, &endPtr, 10);
            if (endPtr != strId && *endPtr == '\0' && parsed > 0)
                return (uint16_t)((uint32_t)parsed % 512u);
            uint64_t h = MpNet_HashSeedString(strId);
            return (uint16_t)(1u + (h % 511u));
        }
    }
    return 0;
}

static void HandleControlBinary(uint16_t fromPeerId, uint8_t fromSlot, uint8_t opcode, const void *payload, uint16_t len)
{
    bool isRemoteHostOverride = MpNet_IsHost() && fromPeerId != 0 && fromPeerId != MpNet_GetLocalPeerId();
    switch (opcode) {
    case MP_PKT_CTRL_SPEED_SET:
        if (!isRemoteHostOverride && len >= sizeof(MpControlSpeedPayload)) {
            MpControlSpeedPayload sp;
            memcpy(&sp, payload, sizeof(sp));
            uint8_t mult = sp.speedMultiplier;
            if (mult < 1) mult = 1;
            if (mult > 3) mult = 3;
            sRunConfig.speedMultiplier = mult;
            WasmPlatform_SetSpeedMultiplier(mult);
        }
        break;
    case MP_PKT_CTRL_WARP_EXEC:
        if (len >= sizeof(int8_t) * 2 + sizeof(int16_t) * 2) {
            int8_t grp = ((const int8_t *)payload)[0];
            int8_t num = ((const int8_t *)payload)[1];
            int16_t x = 0, y = 0;
            memcpy(&x, (const uint8_t *)payload + 2, sizeof(int16_t));
            memcpy(&y, (const uint8_t *)payload + 4, sizeof(int16_t));
            MpOverworld_ExecuteWarp(grp, num, x, y);
        }
        break;
    case MP_PKT_CTRL_PARTY_PAIR:
        if (fromSlot < MP_MAX_PLAYERS && len >= 1) {
            sPeers[fromSlot].partyPairTag = ((const uint8_t *)payload)[0];
        }
        break;
    case MP_PKT_CTRL_RUN_CONFIG:
        if (!isRemoteHostOverride && len >= sizeof(MpRunConfig)) {
            MpRunConfig incoming;
            memcpy(&incoming, payload, sizeof(incoming));
            uint8_t savedLocalSlot = sRunConfig.localSlot;
            uint8_t savedIsHost = sRunConfig.isHost;
            uint8_t mult = incoming.speedMultiplier;
            if (mult < 1) mult = 1;
            if (mult > 3) mult = 3;
            sRunConfig = incoming;
            sRunConfig.seedStr[sizeof(sRunConfig.seedStr) - 1] = '\0';
            if (sRunConfig.seedHash == 0 && sRunConfig.seedStr[0] != '\0')
                sRunConfig.seedHash = MpNet_HashSeedString(sRunConfig.seedStr);
            sRunConfig.speedMultiplier = mult;
            sRunConfig.localSlot = savedLocalSlot;
            sRunConfig.isHost = savedIsHost;
            sHasConfigBaseline = true;
            sLastSeedHash = sRunConfig.seedHash;
            sLastRandoFlags = sRunConfig.randomizerFlags;
            sLastNuzFlags = sRunConfig.nuzlockeFlags;
            sLastSoulLinkMode = sRunConfig.soulLinkMode;
            WasmPlatform_SetSpeedMultiplier(sRunConfig.speedMultiplier);
            MpRandomizer_OnRunConfigChanged(&sRunConfig);
        }
        break;
    default:
        break;
    }
}

static void HandleIncomingJson(uint16_t fromPeerId, const char *json, size_t len)
{
    char buf[2048];
    size_t copyLen = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
    memcpy(buf, json, copyLen);
    buf[copyLen] = '\0';

    char typeStr[48];
    if (!JsonExtractString(buf, "t", typeStr, sizeof(typeStr)))
        return;

    if (strcmp(typeStr, "run_config_sync") == 0 || strcmp(typeStr, "snapshot_sync") == 0) {
        MpNet_RefreshConfigFromBridge();
        MpRandomizer_OnRunConfigChanged(&sRunConfig);
    } else if (strcmp(typeStr, "speed_set") == 0) {
        if (MpNet_IsHost() && fromPeerId != 0 && fromPeerId != MpNet_GetLocalPeerId())
            return;
        int32_t spd = 1;
        if (JsonExtractInt(buf, "speed", &spd)) {
            if (spd < 1) spd = 1;
            if (spd > 3) spd = 3;
            sRunConfig.speedMultiplier = (uint8_t)spd;
            WasmPlatform_SetSpeedMultiplier(spd);
        }
    } else if (strcmp(typeStr, "badge_unlock") == 0) {
        int32_t badgeIdx = -1;
        if (JsonExtractInt(buf, "badgeIndex", &badgeIdx) && badgeIdx >= 0 && badgeIdx < 8) {
            MpWorldSync_GrantBadgeHmsLocal((uint8_t)(1u << badgeIdx));
        }
    } else if (strcmp(typeStr, "world_flag") == 0) {
        int32_t flagId = 0;
        int32_t val = 0;
        if (JsonExtractInt(buf, "flagId", &flagId) && flagId > 0 && flagId <= 0xFFFF) {
            JsonExtractInt(buf, "val", &val);
            MpWorldSync_ApplyRemoteFlagFromBridge((uint16_t)flagId, val != 0);
        }
    } else if (strcmp(typeStr, "world_var") == 0) {
        int32_t varId = 0;
        int32_t val = 0;
        if (JsonExtractInt(buf, "varId", &varId) && varId > 0 && varId <= 0xFFFF) {
            JsonExtractInt(buf, "val", &val);
            MpWorldSync_ApplyRemoteVarFromBridge((uint16_t)varId, (uint16_t)(val < 0 ? 0 : val));
        }
    } else if (strcmp(typeStr, "world_sync") == 0) {
        uint8_t badgesMask = ParseBadgesArrayMask(buf);
        if (badgesMask != 0)
            MpWorldSync_GrantBadgeHmsLocal(badgesMask);
        ApplyWorldPairsFromJson(buf, "flags", false);
        ApplyWorldPairsFromJson(buf, "vars", true);
    } else if (strcmp(typeStr, "cutscene_trigger") == 0 ||
               strcmp(typeStr, "cutscene_start") == 0 ||
               strcmp(typeStr, "cutscene_update") == 0 ||
               strcmp(typeStr, "cutscene_end") == 0) {
        MpCutscenePayload cs;
        memset(&cs, 0, sizeof(cs));
        cs.cutsceneId = ExtractCutsceneId(buf);
        JsonExtractString(buf, "title", cs.title, sizeof(cs.title));
        if (strcmp(typeStr, "cutscene_trigger") == 0)
            cs.phase = MP_CUTSCENE_TRIGGER;
        else if (strcmp(typeStr, "cutscene_end") == 0)
            cs.phase = MP_CUTSCENE_END;
        else
            cs.phase = MP_CUTSCENE_START;
        MpCutscene_HandlePacket(fromPeerId, MP_PKT_WORLD_CUTSCENE, &cs, sizeof(cs));
    }
}

void MpNet_OnSpeedChangedFromBridge(int speed)
{
    int clamped = speed;
    if (clamped < 1) clamped = 1;
    if (clamped > 3) clamped = 3;
    sRunConfig.speedMultiplier = (uint8_t)clamped;
}

uint64_t MpNet_HashSeedString(const char *seedStr)
{
    uint64_t hash = 14695981039346656037ULL;
    if (!seedStr)
        return hash;
    for (const unsigned char *p = (const unsigned char *)seedStr; *p != '\0'; p++) {
        hash ^= (uint64_t)(*p);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void MpNet_UpdatePeerMeta(uint8_t slotIndex, uint16_t peerId, bool connected, uint8_t pairTag, const char *name)
{
    if (slotIndex >= MP_MAX_PLAYERS)
        return;
    sPeers[slotIndex].slotIndex = slotIndex;
    sPeers[slotIndex].peerId = (peerId != 0) ? peerId : (uint16_t)(slotIndex + 1u);
    sPeers[slotIndex].connected = connected;
    if (!connected)
        sPeers[slotIndex].hasPosition = false;
    sPeers[slotIndex].partyPairTag = pairTag;
    sAuthoritativePairTag[slotIndex] = true;
    if (name != NULL && name[0] != '\0') {
        snprintf(sPeers[slotIndex].name, sizeof(sPeers[slotIndex].name), "%s", name);
    } else if (!connected) {
        sPeers[slotIndex].name[0] = '\0';
    }
}

bool MpNet_HasAuthoritativePairTag(uint8_t slotIndex)
{
    if (slotIndex >= MP_MAX_PLAYERS)
        return false;
    return sAuthoritativePairTag[slotIndex];
}

void MpNet_Init(void)
{
    memset(&sRunConfig, 0, sizeof(sRunConfig));
    sRunConfig.speedMultiplier = 1;
    sRunConfig.randomizerPreset = MP_RANDO_BALANCED;
    sRunConfig.randomizerFlags = MP_RANDO_F_WILD_ENCOUNTERS | MP_RANDO_F_STARTERS_STATICS
                               | MP_RANDO_F_TRAINERS_2V2
                               | MP_RANDO_F_BAN_WONDER_GUARD | MP_RANDO_F_PRESERVE_GYM_THEMES
                               | MP_RANDO_F_SIMILAR_BST;
    sRunConfig.nuzlockePreset = MP_NUZ_STANDARD_SOULLINK;
    sRunConfig.nuzlockeFlags = MP_NUZ_F_ENABLED | MP_NUZ_F_DUPES_CLAUSE | MP_NUZ_F_SHINY_CLAUSE;
    sRunConfig.soulLinkMode = MP_SOULLINK_PAIRS;
    snprintf(sRunConfig.seedStr, sizeof(sRunConfig.seedStr), "EMERALD-COOP");
    sRunConfig.seedHash = MpNet_HashSeedString(sRunConfig.seedStr);

    memset(sPeers, 0, sizeof(sPeers));
    for (uint8_t i = 0; i < MP_MAX_PLAYERS; i++) {
        sPeers[i].slotIndex = i;
        sPeers[i].peerId = (uint16_t)(i + 1u);
    }

    memset(sHandlers, 0, sizeof(sHandlers));
    memset(sAuthoritativePairTag, 0, sizeof(sAuthoritativePairTag));
    sLocalInBattle = false;
    sTxSeq = 0;
    sFrameTick = 0;
    sHasConfigBaseline = false;

    MpNet_RegisterHandler(MP_CH_OVERWORLD, MpOverworld_HandlePacket);
    MpNet_RegisterHandler(MP_CH_WORLD_SYNC, MpWorldSync_HandlePacket);
    MpNet_RegisterHandler(MP_CH_BATTLE, MpBattle2v2_HandlePacket);
    MpNet_RegisterHandler(MP_CH_SOUL_LINK, MpSoulLink_HandlePacket);
    MpNet_RegisterHandler(MP_CH_TRADE, MpTrade_HandlePacket);

    MpWorldSync_Init();
    MpOverworld_Init();
    MpCutscene_Init();
    MpBattle2v2_Init();
    MpSoulLink_Init();
    MpTrade_Init();
    MpNet_RefreshConfigFromBridge();
}

static void SyncLocalSlotFromBridge(void)
{
    int rawSlot = WasmMp_GetLocalSlotIndex();
    if (rawSlot < 0) rawSlot = 0;
    if (rawSlot >= MP_MAX_PLAYERS) rawSlot = MP_MAX_PLAYERS - 1;

    sRunConfig.localSlot = (uint8_t)rawSlot;
    sRunConfig.isHost = (rawSlot == 0) ? 1 : 0;
    sPeers[sRunConfig.localSlot].slotIndex = sRunConfig.localSlot;
    sPeers[sRunConfig.localSlot].connected = true;
    /* Preserve bridge-assigned peerId so 30-frame refreshes never clobber real routing IDs. */
    if (sPeers[sRunConfig.localSlot].peerId == 0)
        sPeers[sRunConfig.localSlot].peerId = (uint16_t)(sRunConfig.localSlot + 1u);
}

void MpNet_RefreshConfigFromBridge(void)
{
    SyncLocalSlotFromBridge();

    int bridgeSpeed = WasmMp_GetSpeedMultiplierFromBridge();
    if (bridgeSpeed < 1) bridgeSpeed = 1;
    if (bridgeSpeed > 3) bridgeSpeed = 3;
    sRunConfig.speedMultiplier = (uint8_t)bridgeSpeed;
    WasmPlatform_SetSpeedMultiplier(bridgeSpeed);

    char cfgJson[2048];
    if (WasmMp_GetRunConfigJson(cfgJson, sizeof(cfgJson))) {
        char tmp[64];
        if (JsonExtractString(cfgJson, "seed", tmp, sizeof(tmp)) && tmp[0] != '\0') {
            snprintf(sRunConfig.seedStr, sizeof(sRunConfig.seedStr), "%s", tmp);
            sRunConfig.seedHash = MpNet_HashSeedString(sRunConfig.seedStr);
        }
        if (JsonExtractString(cfgJson, "randomizerPreset", tmp, sizeof(tmp))) {
            if (strcmp(tmp, "off") == 0) sRunConfig.randomizerPreset = MP_RANDO_OFF;
            else if (strcmp(tmp, "classic") == 0) sRunConfig.randomizerPreset = MP_RANDO_CLASSIC;
            else if (strcmp(tmp, "chaos") == 0) sRunConfig.randomizerPreset = MP_RANDO_CHAOS;
            else sRunConfig.randomizerPreset = MP_RANDO_BALANCED;
        }
        sRunConfig.randomizerFlags = ParseRandomizerFlagsFromJson(cfgJson, sRunConfig.randomizerPreset);

        if (JsonExtractString(cfgJson, "nuzlockePreset", tmp, sizeof(tmp))) {
            if (strcmp(tmp, "hardcore") == 0) sRunConfig.nuzlockePreset = MP_NUZ_HARDCORE;
            else if (strcmp(tmp, "casual_coop") == 0) sRunConfig.nuzlockePreset = MP_NUZ_CASUAL_COOP;
            else sRunConfig.nuzlockePreset = MP_NUZ_STANDARD_SOULLINK;
        }
        sRunConfig.nuzlockeFlags = ParseNuzlockeFlagsFromJson(cfgJson);

        if (JsonExtractString(cfgJson, "soulLinkMode", tmp, sizeof(tmp))) {
            sRunConfig.soulLinkMode = (strcmp(tmp, "group") == 0) ? MP_SOULLINK_GROUP : MP_SOULLINK_PAIRS;
        }
        int32_t spd = 0;
        if (JsonExtractInt(cfgJson, "speedMultiplier", &spd) && spd >= 1 && spd <= 3) {
            sRunConfig.speedMultiplier = (uint8_t)spd;
            WasmPlatform_SetSpeedMultiplier(spd);
        }
        uint8_t cfgBadges = ParseBadgesArrayMask(cfgJson);
        if (cfgBadges != 0)
            MpWorldSync_GrantBadgeHmsLocal(cfgBadges);
    }

    if (!sHasConfigBaseline ||
        sLastSeedHash != sRunConfig.seedHash ||
        sLastRandoFlags != sRunConfig.randomizerFlags ||
        sLastNuzFlags != sRunConfig.nuzlockeFlags ||
        sLastSoulLinkMode != sRunConfig.soulLinkMode) {
        bool wasBaseline = sHasConfigBaseline;
        sHasConfigBaseline = true;
        sLastSeedHash = sRunConfig.seedHash;
        sLastRandoFlags = sRunConfig.randomizerFlags;
        sLastNuzFlags = sRunConfig.nuzlockeFlags;
        sLastSoulLinkMode = sRunConfig.soulLinkMode;
        MpRandomizer_OnRunConfigChanged(&sRunConfig);
        if (wasBaseline && MpNet_IsHost()) {
            MpNet_SendBinary(MP_CH_CONTROL, MP_PEER_BROADCAST, MP_PKT_CTRL_RUN_CONFIG, &sRunConfig, sizeof(sRunConfig));
        }
    }
}

void MpNet_StepFrame(void)
{
    sFrameTick++;
    SyncLocalSlotFromBridge();
    int resetMode = 0;
    bool resetNewGame = WasmMp_ConsumeResetNewGameRequest(&resetMode);
    bool returnToTitle = WasmMp_ConsumeReturnToTitleRequest();
    bool snapshotLoaded = WasmMp_ConsumeSnapshotLoadedFlag();
    if ((sFrameTick % 30u) == 0u || snapshotLoaded || resetNewGame || returnToTitle) {
        MpNet_RefreshConfigFromBridge();
    }
    if (returnToTitle) {
        MpOverworld_ReturnToTitleScreen();
    } else if (resetNewGame) {
        MpOverworld_ResetToNewGame(resetMode != 0);
    } else if (snapshotLoaded) {
        MpOverworld_ResumeOrBootstrapFromSave();
    }

    WasmMpPacket pkt;
    while (WasmMp_PopPacket(&pkt)) {
        if (pkt.length >= sizeof(MpPacketHeader)) {
            MpPacketHeader hdr;
            memcpy(&hdr, pkt.data, sizeof(hdr));
            if (hdr.magic == MP_BINARY_MAGIC && hdr.channel < MP_CHANNEL_COUNT) {
                const uint8_t *payload = pkt.data + sizeof(MpPacketHeader);
                uint16_t payloadLen = (uint16_t)(pkt.length - sizeof(MpPacketHeader));
                if (hdr.fromSlot < MP_MAX_PLAYERS) {
                    if (hdr.fromSlot == sRunConfig.localSlot) {
                        if (!(hdr.channel == MP_CH_TRADE && hdr.opcode == MP_PKT_TRD_COMMIT))
                            continue;
                    } else {
                        sPeers[hdr.fromSlot].slotIndex = hdr.fromSlot;
                        sPeers[hdr.fromSlot].connected = true;
                        if (pkt.fromPeerId != 0 && (pkt.fromPeerId != 1 || hdr.fromSlot == 0))
                            sPeers[hdr.fromSlot].peerId = pkt.fromPeerId;
                    }
                }
                if (hdr.channel == MP_CH_CONTROL) {
                    HandleControlBinary(pkt.fromPeerId, hdr.fromSlot, hdr.opcode, payload, payloadLen);
                } else if (hdr.channel == MP_CH_WORLD_SYNC && hdr.opcode == MP_PKT_WORLD_CUTSCENE) {
                    MpCutscene_HandlePacket(pkt.fromPeerId, hdr.opcode, payload, payloadLen);
                } else if (sHandlers[hdr.channel] != NULL) {
                    sHandlers[hdr.channel](pkt.fromPeerId, hdr.opcode, payload, payloadLen);
                }
                continue;
            }
        }
        if (pkt.length > 0 && pkt.data[0] == '{') {
            HandleIncomingJson(pkt.fromPeerId, (const char *)pkt.data, pkt.length);
        }
    }

    int wGroup = 0, wNum = 0, wX = 0, wY = 0;
    if (WasmMp_ConsumePendingWarp(&wGroup, &wNum, &wX, &wY)) {
        MpOverworld_ExecuteWarp((int8_t)wGroup, (int8_t)wNum, (int16_t)wX, (int16_t)wY);
    }

    /* Gameplay module StepFrame implementations drain battle/soul-link/trade bridge queues */
    MpBattle2v2_StepFrame();
    MpSoulLink_StepFrame();
    MpTrade_StepFrame();
}

bool MpNet_SendBinary(uint8_t channel, uint16_t targetPeerId, uint8_t opcode, const void *payload, uint16_t payloadLen)
{
    if (channel >= MP_CHANNEL_COUNT)
        return false;
    if ((size_t)payloadLen + sizeof(MpPacketHeader) > WASM_MAX_MP_PAYLOAD)
        return false;

    uint8_t frame[WASM_MAX_MP_PAYLOAD];
    MpPacketHeader hdr;
    hdr.magic = MP_BINARY_MAGIC;
    hdr.channel = channel;
    hdr.opcode = opcode;
    hdr.seq = ++sTxSeq;
    hdr.fromSlot = sRunConfig.localSlot;
    hdr.flags = sRunConfig.isHost ? 1u : 0u;

    memcpy(frame, &hdr, sizeof(hdr));
    if (payloadLen > 0 && payload != NULL) {
        memcpy(frame + sizeof(hdr), payload, payloadLen);
    }
    return WasmMp_SendPacket(channel, targetPeerId, frame, (uint16_t)(sizeof(hdr) + payloadLen));
}

bool MpNet_SendJson(uint8_t channel, uint16_t targetPeerId, const char *jsonText)
{
    if (channel >= MP_CHANNEL_COUNT || !jsonText)
        return false;
    size_t len = strlen(jsonText);
    if (len == 0 || len > WASM_MAX_MP_PAYLOAD)
        return false;
    return WasmMp_SendPacket(channel, targetPeerId, (const uint8_t *)jsonText, (uint16_t)len);
}

void MpNet_RegisterHandler(uint8_t channel, MpChannelHandler handler)
{
    if (channel < MP_CHANNEL_COUNT)
        sHandlers[channel] = handler;
}

uint8_t MpNet_GetLocalSlot(void)
{
    return sRunConfig.localSlot;
}

uint16_t MpNet_GetLocalPeerId(void)
{
    uint8_t slot = sRunConfig.localSlot;
    if (slot < MP_MAX_PLAYERS && sPeers[slot].peerId != 0)
        return sPeers[slot].peerId;
    return (uint16_t)(slot + 1u);
}

bool MpNet_IsHost(void)
{
    return sRunConfig.isHost != 0;
}

const MpRunConfig *MpNet_GetRunConfig(void)
{
    return &sRunConfig;
}

MpRunConfig *MpNet_GetRunConfigMutable(void)
{
    return &sRunConfig;
}

const MpPeerState *MpNet_GetPeerState(uint8_t slotIndex)
{
    if (slotIndex >= MP_MAX_PLAYERS)
        return NULL;
    return &sPeers[slotIndex];
}

MpPeerState *MpNet_GetPeerStateMutable(uint8_t slotIndex)
{
    if (slotIndex >= MP_MAX_PLAYERS)
        return NULL;
    return &sPeers[slotIndex];
}

void MpNet_SetLocalInBattle(bool inBattle)
{
    sLocalInBattle = inBattle;
}

bool MpNet_IsLocalInBattle(void)
{
    return sLocalInBattle;
}

static bool IsPeerInTutorialOrUnready(uint8_t slot)
{
    if (slot >= MP_MAX_PLAYERS || !sPeers[slot].connected || !sPeers[slot].hasPosition)
        return true;
    return (sPeers[slot].pos.inBattle & MP_OW_STATUS_IN_TUTORIAL) != 0;
}

uint8_t MpNet_FindExplicitPairSlot(void)
{
    uint8_t local = sRunConfig.localSlot;
    uint8_t localTag = (local < MP_MAX_PLAYERS) ? sPeers[local].partyPairTag : 0;

    if (localTag != 0) {
        for (uint8_t i = 0; i < MP_MAX_PLAYERS; i++) {
            if (i != local && sPeers[i].connected && sPeers[i].partyPairTag == localTag)
                return i;
        }
    }
    return 0xFFu;
}

uint8_t MpNet_GetCoopPartnerSlot(void)
{
    uint8_t local = sRunConfig.localSlot;
    uint8_t localTag = (local < MP_MAX_PLAYERS) ? sPeers[local].partyPairTag : 0;

    if (!MpWorldSync_IsLocalTutorialComplete())
        return 0xFFu;

    /* Co-op 2v2 partners must be explicitly partied (matching non-zero party tag).
     * Merely sharing a room with another player never makes them a co-op partner. */
    if (localTag == 0)
        return 0xFFu;

    {
        uint8_t paired = MpNet_FindExplicitPairSlot();
        if (paired != 0xFFu && !IsPeerInTutorialOrUnready(paired))
            return paired;
    }
    return 0xFFu;
}

uint16_t MpNet_GetCoopPartnerPeerId(void)
{
    uint8_t slot = MpNet_GetCoopPartnerSlot();
    if (slot >= MP_MAX_PLAYERS)
        return 0;
    return sPeers[slot].peerId;
}
