#ifndef GUARD_MP_PROTOCOL_H
#define GUARD_MP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Wire Channels (1:1 with server.js, cloudflare_worker.js, public/net_bridge.js)
 * ========================================================================= */
#define MP_CH_CONTROL    0
#define MP_CH_OVERWORLD  1
#define MP_CH_WORLD_SYNC 2
#define MP_CH_BATTLE     3
#define MP_CH_SOUL_LINK  4
#define MP_CH_TRADE      5
#define MP_CHANNEL_COUNT 6

#define MP_MAX_PLAYERS          4
#define MP_PEER_BROADCAST       0
#define MP_PEER_HOST            1
#define MP_MAX_PARTY            6
#define MP_MAX_NAME_LEN         20
#define MP_MAX_SOUL_LINK_MAP    256
#define MP_SERIALIZED_MON_BYTES 100  /* sizeof(struct Pokemon) in Gen 3 */

/* Binary engine packet prefix to distinguish from JSON control strings ('{') */
#define MP_BINARY_MAGIC 0x4D50u /* 'MP' */

#if defined(__GNUC__) || defined(__clang__)
#define MP_PACKED __attribute__((packed))
#else
#define MP_PACKED
#endif

/* =========================================================================
 * Presets & Configuration Bitmasks (matches RANDOMIZER_PRESETS / NUZLOCKE_PRESETS)
 * ========================================================================= */
enum MpRandomizerPreset {
    MP_RANDO_OFF      = 0,
    MP_RANDO_BALANCED = 1,
    MP_RANDO_CLASSIC  = 2,
    MP_RANDO_CHAOS    = 3,
};

enum MpRandomizerFlags {
    MP_RANDO_F_WILD_ENCOUNTERS     = 1u << 0,
    MP_RANDO_F_STARTERS_STATICS    = 1u << 1,
    MP_RANDO_F_TRAINERS_2V2        = 1u << 2,
    MP_RANDO_F_ITEMS_MARTS         = 1u << 3,
    MP_RANDO_F_ABILITIES           = 1u << 4,
    MP_RANDO_F_BAN_WONDER_GUARD    = 1u << 5,
    MP_RANDO_F_MOVESETS            = 1u << 6,
    MP_RANDO_F_TYPES               = 1u << 7,
    MP_RANDO_F_EVOLUTIONS          = 1u << 8,
    MP_RANDO_F_PRESERVE_GYM_THEMES = 1u << 9,
    MP_RANDO_F_SIMILAR_BST         = 1u << 10,
};

enum MpNuzlockePreset {
    MP_NUZ_STANDARD_SOULLINK = 0,
    MP_NUZ_HARDCORE          = 1,
    MP_NUZ_CASUAL_COOP       = 2,
};

enum MpNuzlockeFlags {
    MP_NUZ_F_ENABLED            = 1u << 0,
    MP_NUZ_F_DUPES_CLAUSE       = 1u << 1,
    MP_NUZ_F_SHINY_CLAUSE       = 1u << 2,
    MP_NUZ_F_LEVEL_CAPS         = 1u << 3,
    MP_NUZ_F_SET_MODE           = 1u << 4,
    MP_NUZ_F_NO_BATTLE_ITEMS    = 1u << 5,
    MP_NUZ_F_WHITEOUT_GAME_OVER = 1u << 6,
};

enum MpSoulLinkMode {
    MP_SOULLINK_PAIRS = 0, /* 2-player partner link groups */
    MP_SOULLINK_GROUP = 1, /* Full 1-4 group link */
};

enum MpBattleMode {
    MP_BATTLE_SOLO_2V2 = 0,
    MP_BATTLE_COOP_2V2 = 1,
    MP_BATTLE_PVP      = 2,
};

enum MpCutscenePhase {
    MP_CUTSCENE_IDLE    = 0,
    MP_CUTSCENE_TRIGGER = 1,
    MP_CUTSCENE_START   = 2,
    MP_CUTSCENE_ACK     = 3,
    MP_CUTSCENE_END     = 4,
};

/* =========================================================================
 * Binary Packet Header & Opcodes across Channels 0..5
 * ========================================================================= */
enum MpOpcode {
    /* Channel 0: Control */
    MP_PKT_CTRL_RUN_CONFIG  = 0x01,
    MP_PKT_CTRL_SPEED_SET   = 0x02,
    MP_PKT_CTRL_PARTY_PAIR  = 0x03,
    MP_PKT_CTRL_WARP_EXEC   = 0x04,

    /* Channel 1: Overworld */
    MP_PKT_OW_PLAYER_POS    = 0x10,
    MP_PKT_OW_PLAYER_LEAVE  = 0x11,

    /* Channel 2: World Sync */
    MP_PKT_WORLD_FLAG_SYNC  = 0x20,
    MP_PKT_WORLD_VAR_SYNC   = 0x21,
    MP_PKT_WORLD_BADGE_HM   = 0x22,
    MP_PKT_WORLD_SNAPSHOT   = 0x23,
    MP_PKT_WORLD_CUTSCENE   = 0x24,

    /* Channel 3: 2v2 Battle & PvP */
    MP_PKT_BAT_START        = 0x30,
    MP_PKT_BAT_TURN_ACTION  = 0x31,
    MP_PKT_BAT_RNG_CHECK    = 0x32,
    MP_PKT_BAT_END          = 0x33,
    MP_PKT_BAT_DECLINE_BUSY = 0x35,
    MP_PKT_BAT_SWITCH_CHOICE = 0x36,
    MP_PKT_BAT_LEARN_MOVE   = 0x37,

    /* Channel 4: Soul-Link & Nuzlocke */
    MP_PKT_SL_ROUTE_CATCH   = 0x40,
    MP_PKT_SL_MON_FAINTED   = 0x41,
    MP_PKT_SL_LINK_KILL     = 0x42,
    MP_PKT_SL_ROUTE_FAIL    = 0x43,

    /* Channel 5: Atomic Trading */
    MP_PKT_TRD_OFFER        = 0x50,
    MP_PKT_TRD_LOCK         = 0x51,
    MP_PKT_TRD_COMMIT       = 0x52,
};

typedef struct MP_PACKED MpPacketHeader {
    uint16_t magic;      /* MP_BINARY_MAGIC (0x4D50) */
    uint8_t channel;     /* 0..5 */
    uint8_t opcode;      /* enum MpOpcode */
    uint16_t seq;        /* monotonic per-sender sequence */
    uint8_t fromSlot;    /* 0..3 */
    uint8_t flags;       /* bit0: reliable/host-authoritative */
} MpPacketHeader;

typedef struct MpRunConfig {
    uint64_t seedHash;
    char seedStr[32];
    uint8_t randomizerPreset;
    uint32_t randomizerFlags;
    uint8_t nuzlockePreset;
    uint32_t nuzlockeFlags;
    uint8_t soulLinkMode;
    uint8_t speedMultiplier; /* 1, 2, or 3 */
    uint8_t localSlot;       /* 0..3 */
    uint8_t isHost;          /* 1 if peerId == 1 */
    uint8_t badgesMask;      /* synced badges 0..7 */
} MpRunConfig;

typedef struct MP_PACKED MpControlSpeedPayload {
    uint8_t speedMultiplier;
} MpControlSpeedPayload;

#define MP_OW_STATUS_IN_BATTLE   (1u << 0)
#define MP_OW_STATUS_BUSY        (1u << 1)
#define MP_OW_STATUS_IN_TUTORIAL (1u << 2)

#define MP_FOLLOWER_F_MOVING   (1u << 0)
#define MP_FOLLOWER_F_FAST     (1u << 1)
#define MP_FOLLOWER_F_JUMPING  (1u << 2)

typedef struct MP_PACKED MpOverworldPosPayload {
    uint8_t slotIndex;
    int8_t mapGroup;
    int8_t mapNum;
    uint8_t elevation;
    int16_t x;
    int16_t y;
    uint8_t facing;
    uint8_t movementState;
    uint16_t graphicsId;
    uint8_t gender;
    uint8_t inBattle;
    uint8_t partyTag; /* 0=none, 1=PAIR_1_2, 2=PAIR_3_4, etc. */
    uint16_t followerGfxId; /* 0 if hidden/none, else OBJ_EVENT_MON | shiny | female | species */
    int16_t followerX;
    int16_t followerY;
    uint8_t followerElevation;
    uint8_t followerFacing;
    uint8_t followerFlags;
} MpOverworldPosPayload;

typedef struct MP_PACKED MpFlagSyncPayload {
    uint16_t flagId;
    uint8_t value;
} MpFlagSyncPayload;

typedef struct MP_PACKED MpVarSyncPayload {
    uint16_t varId;
    uint16_t value;
} MpVarSyncPayload;

typedef struct MP_PACKED MpBadgeHmPayload {
    uint8_t badgeIndex;   /* 0..7 */
    uint8_t badgesMask;   /* bitmask 0..7 */
    uint16_t hmFlagsMask; /* granted HM flag mask */
} MpBadgeHmPayload;

typedef struct MP_PACKED MpCutscenePayload {
    uint16_t cutsceneId;
    uint8_t phase;      /* enum MpCutscenePhase */
    uint8_t deferred;   /* 1 if peer deferred because in battle */
    uint8_t fromSlot;
    int8_t returnMapGroup;
    int8_t returnMapNum;
    int16_t returnX;
    int16_t returnY;
    char title[32];
} MpCutscenePayload;

typedef struct MP_PACKED MpBattleStartPayload {
    uint32_t battleSeed;
    uint8_t mode; /* enum MpBattleMode */
    uint8_t partnerSlot;
    uint16_t partnerPeerId;
    uint16_t trainerId;
    uint16_t species[2];
    uint8_t levels[2];
} MpBattleStartPayload;

typedef struct MP_PACKED MpBattleTurnActionPayload {
    uint16_t turnNumber;
    uint8_t battlerId;
    uint8_t actionChoice;   /* MOVE=0, SWITCH=1, ITEM=2, RUN=3 */
    uint8_t targetBattler;
    uint8_t moveOrItemSlot;
    uint16_t itemOrSpecies;
    uint32_t battleRngState;
    uint8_t aiValid;
    uint8_t aiAction[2];
    uint8_t aiMoveOrSwitch[2];
    uint8_t aiTarget[2];
    uint16_t aiMoveOrItem[2];
    uint8_t gimmickFlags;   /* bit 0: player battler wants gimmick activation */
    uint8_t aiGimmickFlags; /* bits 0..1: oppLeft/oppRight want gimmick activation */
} MpBattleTurnActionPayload;

typedef struct MP_PACKED MpBattleSwitchChoicePayload {
    uint16_t turnNumber;
    uint16_t switchSeq;
    uint8_t battlerPos;
    uint8_t caseId;
    uint8_t chosenMonId;
    uint8_t reserved;
    uint32_t rngA;
    uint32_t rngB;
    uint32_t rngC;
    uint32_t rngCtr;
} MpBattleSwitchChoicePayload;

typedef struct MP_PACKED MpBattleLearnMovePayload {
    uint8_t partyIndex;
    uint8_t moveSlot;
    uint16_t moveId;
    uint8_t pp;
    uint8_t ppBonuses;
} MpBattleLearnMovePayload;

typedef struct MP_PACKED MpSoulLinkKey {
    uint32_t otId;
    uint32_t personality;
} MpSoulLinkKey;

typedef struct MP_PACKED MpSoulLinkCatchPayload {
    uint8_t slotIndex;
    uint8_t linkGroup; /* 0=ALL, 1=PAIR_A, 2=PAIR_B */
    uint16_t mapsec;
    uint16_t linkId;
    uint16_t species;
    uint8_t level;
    struct MpSoulLinkKey key;
} MpSoulLinkCatchPayload;

typedef struct MP_PACKED MpSoulLinkDeathPayload {
    uint8_t slotIndex;
    uint8_t linkGroup;
    uint16_t linkId;
    uint8_t cause;
    struct MpSoulLinkKey key;
} MpSoulLinkDeathPayload;

typedef struct MP_PACKED MpSoulLinkRouteFailPayload {
    uint8_t slotIndex;
    uint8_t linkGroup;
    uint16_t mapsec;
    uint16_t linkId;
} MpSoulLinkRouteFailPayload;

typedef struct MP_PACKED MpTradeOfferPayload {
    uint32_t sessionId;
    uint8_t fromSlot;
    uint8_t hasMon;
    uint8_t locked;
    uint8_t _pad;
    uint16_t linkId;
    uint16_t mapsec;
    uint16_t itemId;
    uint16_t itemQty;
    uint32_t money;
    uint8_t monBytes[MP_SERIALIZED_MON_BYTES];
} MpTradeOfferPayload;

typedef struct MP_PACKED MpTradeCommitPayload {
    uint32_t sessionId;
    uint8_t slotA;
    uint8_t slotB;
    MpTradeOfferPayload offerA;
    MpTradeOfferPayload offerB;
} MpTradeCommitPayload;

/* =========================================================================
 * Player Tracking Snapshot (Shared by Net, Overworld & Gameplay Modules)
 * ========================================================================= */
typedef struct MpPeerState {
    bool connected;
    uint16_t peerId;
    uint8_t slotIndex;
    char name[MP_MAX_NAME_LEN + 1];
    uint8_t partyPairTag; /* 0 = solo, non-zero shared pair ID */
    bool hasPosition;
    MpOverworldPosPayload pos;
} MpPeerState;

typedef void (*MpChannelHandler)(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t payloadLen);

/* =========================================================================
 * Engine Netplay Core API (mp_net.c)
 * ========================================================================= */
void MpNet_Init(void);
void MpNet_StepFrame(void);
bool MpNet_SendBinary(uint8_t channel, uint16_t targetPeerId, uint8_t opcode, const void *payload, uint16_t payloadLen);
bool MpNet_SendJson(uint8_t channel, uint16_t targetPeerId, const char *jsonText);
void MpNet_RegisterHandler(uint8_t channel, MpChannelHandler handler);

uint8_t MpNet_GetLocalSlot(void);
uint16_t MpNet_GetLocalPeerId(void);
bool MpNet_IsHost(void);
const MpRunConfig *MpNet_GetRunConfig(void);
MpRunConfig *MpNet_GetRunConfigMutable(void);
const MpPeerState *MpNet_GetPeerState(uint8_t slotIndex);
void MpNet_RefreshConfigFromBridge(void);
void MpNet_SetLocalInBattle(bool inBattle);
bool MpNet_IsLocalInBattle(void);
uint8_t MpNet_GetCoopPartnerSlot(void); /* 0xFF if solo */
uint16_t MpNet_GetCoopPartnerPeerId(void);
uint8_t MpNet_FindExplicitPairSlot(void);

/* =========================================================================
 * Overworld / WorldSync / CutsceneSync API (owned by engine-porter)
 * ========================================================================= */
void MpOverworld_Init(void);
void MpOverworld_OnFrame(void);
void MpOverworld_OnMapLoad(void);
void MpOverworld_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
bool MpOverworld_ExecuteWarp(int8_t mapGroup, int8_t mapNum, int16_t x, int16_t y);

void MpWorldSync_Init(void);
bool MpWorldSync_IsGlobalFlag(uint16_t flagId);
bool MpWorldSync_IsGlobalVar(uint16_t varId);
void MpWorldSync_OnLocalFlagSet(uint16_t flagId, bool value);
void MpWorldSync_OnLocalVarSet(uint16_t varId, uint16_t value);
void MpWorldSync_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpWorldSync_GrantBadgeHmsLocal(uint8_t badgesMask);

void MpCutscene_Init(void);
void MpCutscene_OnFrame(void);
bool MpCutscene_TriggerGlobal(uint16_t cutsceneId, const char *title);
void MpCutscene_FinishActive(void);
bool MpCutscene_IsActive(void);
void MpCutscene_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);

/* =========================================================================
 * Weak / Optional Gameplay Module Callbacks (implemented by gameplay-coder)
 * ========================================================================= */
void MpBattle2v2_Init(void);
void MpBattle2v2_StepFrame(void);
void MpBattle2v2_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpBattle2v2_StartSyncedFromBridge(uint32_t battleSeed, uint8_t mode, uint8_t partnerSlot, uint16_t partnerPeerId);

void MpSoulLink_Init(void);
void MpSoulLink_StepFrame(void);
void MpSoulLink_OnTutorialCompleted(void);
void MpSoulLink_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpSoulLink_KillByLinkId(uint16_t linkId);
void MpSoulLink_ImportStateJson(const char *jsonPtr);
void MpSoulLink_KillByUid(uint32_t otId, uint32_t personality);

void MpRandomizer_OnRunConfigChanged(const MpRunConfig *cfg);

void MpTrade_Init(void);
void MpTrade_StepFrame(void);
void MpTrade_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpTrade_ApplyCommitFromBridge(void);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_PROTOCOL_H */
