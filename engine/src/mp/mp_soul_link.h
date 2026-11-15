#ifndef GUARD_MP_SOUL_LINK_H
#define GUARD_MP_SOUL_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pokemon;
struct BoxPokemon;

#define MP_SOUL_LINK_TABLE_CAP 512u
#define MP_MAPSEC_INVALID      0xFFFFu
#define MP_STARTER_LINK_ID     1u
#define MP_STARTER_MAPSEC      190u

enum MpSoulLinkEntryFlags {
    MP_SL_FLAG_NONE    = 0u,
    MP_SL_FLAG_ACTIVE  = 1u << 0,
    MP_SL_FLAG_DEAD    = 1u << 1,
    MP_SL_FLAG_STARTER = 1u << 2,
    MP_SL_FLAG_SHINY   = 1u << 3,
    MP_SL_FLAG_TRADED  = 1u << 4,
};

enum MpSoulLinkDeathCause {
    MP_SL_CAUSE_BATTLE_FAINT = 0,
    MP_SL_CAUSE_POISON_OW    = 1,
    MP_SL_CAUSE_PARTNER_LINK = 2,
    MP_SL_CAUSE_WHITEOUT     = 3,
};

typedef struct MpSoulLinkEntry {
    struct MpSoulLinkKey key;
    uint16_t linkId;
    uint16_t linkSeqNum;
    uint16_t originMapsec;
    uint16_t species;
    uint8_t originSlot;
    uint8_t linkGroup;
    uint8_t flags;
} MpSoulLinkEntry;

typedef struct MpSoulLinkRouteState {
    uint16_t mapsec;
    uint16_t linkId;
    uint16_t linkSeqNum;
    uint8_t localCapturedCount;
    uint8_t localConsumed;
    uint8_t routeDead;
    uint8_t slotMask;
    uint16_t speciesBySlot[MP_MAX_PLAYERS];
} MpSoulLinkRouteState;

void MpSoulLink_Init(void);
void MpSoulLink_ResetRun(void);
void MpSoulLink_OnTutorialCompleted(void);

bool MpSoulLink_IsEnabled(void);
bool MpSoulLink_AreRulesActive(void);
bool MpSoulLink_IsSetModeEnforced(void);
bool MpSoulLink_IsLevelCapActive(void);
bool MpSoulLink_AreBattleItemsBlocked(void);
bool MpSoulLink_IsBattleItemBlocked(uint16_t itemId);
bool MpSoulLink_IsRunGameOver(void);
uint8_t MpSoulLink_GetCurrentLevelCap(void);
uint32_t MpSoulLink_ClampExpToCap(const struct Pokemon *mon, uint32_t candidateExp);

uint8_t MpSoulLink_GetEffectiveGroupForSlot(uint8_t slotIndex);
bool MpSoulLink_IsSlotLinkedWithLocal(uint8_t slotIndex, uint8_t packetLinkGroup);

const MpSoulLinkEntry *MpSoulLink_LookupKey(struct MpSoulLinkKey key);
const MpSoulLinkEntry *MpSoulLink_LookupMon(const struct Pokemon *mon);
const MpSoulLinkEntry *MpSoulLink_LookupBoxMon(const struct BoxPokemon *boxMon);
MpSoulLinkEntry *MpSoulLink_UpsertMon(const struct Pokemon *mon, uint16_t mapsec, uint16_t linkId, uint8_t originSlot, uint8_t extraFlags);

bool MpSoulLink_IsMonDeadKey(struct MpSoulLinkKey key);
bool MpSoulLink_IsMonDead(const struct Pokemon *mon);
bool MpSoulLink_IsSpeciesLineOwned(uint16_t species);

bool MpSoulLink_IsRouteConsumed(uint16_t mapsec);
bool MpSoulLink_CanThrowBallAtBattler(uint8_t battlerId, uint16_t mapsec);
void MpSoulLink_OnWildEncounterStarted(uint16_t mapsec);
void MpSoulLink_OnWildEncounterFinished(uint16_t mapsec, bool caughtAny, bool allOpponentsDupes);

void MpSoulLink_SetSuppressGiftHook(bool suppress);
void MpSoulLink_RegisterStarter(const struct Pokemon *mon);
void MpSoulLink_OnPokemonCaptured(struct Pokemon *mon, uint16_t mapsec);
void MpSoulLink_OnGiftOrEggReceived(struct Pokemon *mon, uint16_t mapsec);
void MpSoulLink_OnMonFainted(struct Pokemon *mon, bool isPvp, uint8_t cause);
void MpSoulLink_OnTradePreserveLink(const struct BoxPokemon *oldKeyMon, const struct Pokemon *newMon, uint16_t newLinkId, uint16_t mapsec, uint8_t newOwnerSlot);

uint16_t MpSoulLink_TransferDeadPartyMonsToGraveyard(void);
bool MpSoulLink_ProcessBattleEndWhiteout(bool isPvp);
void MpSoulLink_StepFrame(void);

/* Callbacks required by mp_protocol.h / mp_net.c */
void MpSoulLink_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpSoulLink_KillByLinkId(uint16_t linkId);
void MpSoulLink_KillByUid(uint32_t otId, uint32_t personality);
void MpSoulLink_ImportStateJson(const char *jsonPtr);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_SOUL_LINK_H */
