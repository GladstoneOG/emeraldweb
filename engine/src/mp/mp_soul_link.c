#include "global.h"
#include "battle.h"
#include "caps.h"
#include "data.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "item.h"
#include "main.h"
#include "overworld.h"
#include "pokedex.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "safari_zone.h"
#include "string_util.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/region_map_sections.h"
#include "constants/species.h"
#ifdef WASM_PLATFORM
#include "wasm_platform.h"
#endif
#include "mp_overworld.h"
#include "mp_soul_link.h"
#include "mp_world_sync.h"

#include <ctype.h>
#include <stdlib.h>

#define MP_GRAVEYARD_BOX_IDX ((uint8_t)(TOTAL_BOXES_COUNT - 1))
#define MP_SL_MAX_PENDING_KILLS 64u

typedef struct MpPendingSoulLinkKill {
    struct MpSoulLinkKey key;
    uint16_t linkId;
    uint16_t mapsec;
    bool fromPreTutorial;
} MpPendingSoulLinkKill;

static MpSoulLinkEntry sSoulLinkEntries[MP_SOUL_LINK_TABLE_CAP];
static uint16_t sSoulLinkEntryCount = 0;
static MpSoulLinkRouteState sRouteStates[MP_MAX_SOUL_LINK_MAP];
static uint16_t sRouteStateCount = 0;
static MpPendingSoulLinkKill sPendingKills[MP_SL_MAX_PENDING_KILLS];
static uint16_t sPendingKillCount = 0;
static uint16_t sNextLinkSeq = 2;
static bool sRunGameOver = false;
static bool sGraveyardBoxNamed = false;
static bool sWildEncounterActive = false;
static uint16_t sActiveWildMapsec = MP_MAPSEC_INVALID;
static bool sActiveWildCaughtAny = false;
static bool sInitialized = false;
static bool sSuppressGiftHook = false;
static uint8_t sLastLinkedPartnerSlot = 0xFF;
static bool sStarterPendingBroadcast = false;
static bool sStarterBroadcasted = false;
static struct MpSoulLinkKey sStarterKey = {0, 0};
static bool sRemoteStarterDeadDuringTutorial = false;

static struct MpSoulLinkKey ExtractKeyFromBoxMon(const struct BoxPokemon *boxMon)
{
    struct MpSoulLinkKey key;
    if (boxMon == NULL)
    {
        key.otId = 0;
        key.personality = 0;
        return key;
    }
    key.otId = GetBoxMonData((struct BoxPokemon *)boxMon, MON_DATA_OT_ID);
    key.personality = GetBoxMonData((struct BoxPokemon *)boxMon, MON_DATA_PERSONALITY);
    return key;
}

static struct MpSoulLinkKey ExtractKeyFromMon(const struct Pokemon *mon)
{
    if (mon == NULL)
        return ExtractKeyFromBoxMon(NULL);
    return ExtractKeyFromBoxMon(&mon->box);
}

static bool KeysEqual(struct MpSoulLinkKey a, struct MpSoulLinkKey b)
{
    return a.otId == b.otId && a.personality == b.personality;
}

static uint16_t ComputeDeterministicLinkId(uint8_t linkGroup, uint16_t mapsec)
{
    uint16_t groupNib = (uint16_t)(((uint32_t)(linkGroup + 1u) & 0x0Fu) << 11);
    uint16_t secPart = (uint16_t)((mapsec & 0x03FFu) + 1u);
    return (uint16_t)(groupNib | secPart);
}

static MpSoulLinkEntry *FindMutableEntryByKey(struct MpSoulLinkKey key)
{
    uint16_t i;
    if (key.otId == 0 && key.personality == 0)
        return NULL;
    for (i = 0; i < sSoulLinkEntryCount; i++)
    {
        if (KeysEqual(sSoulLinkEntries[i].key, key))
            return &sSoulLinkEntries[i];
    }
    return NULL;
}

static MpSoulLinkRouteState *FindOrCreateRouteState(uint16_t mapsec, bool create)
{
    uint16_t i;
    if (mapsec == MP_MAPSEC_INVALID)
        return NULL;
    for (i = 0; i < sRouteStateCount; i++)
    {
        if (sRouteStates[i].mapsec == mapsec)
            return &sRouteStates[i];
    }
    if (!create || sRouteStateCount >= MP_MAX_SOUL_LINK_MAP)
        return NULL;

    MpSoulLinkRouteState *rs = &sRouteStates[sRouteStateCount++];
    memset(rs, 0, sizeof(*rs));
    rs->mapsec = mapsec;
    return rs;
}

static void EnsureGraveyardBoxNamed(void)
{
    static const u8 sGraveyardName[] = _("GRAVEYRD");
    if (sGraveyardBoxNamed || gPokemonStoragePtr == NULL)
        return;
    StringCopy(gPokemonStoragePtr->boxNames[MP_GRAVEYARD_BOX_IDX], sGraveyardName);
    sGraveyardBoxNamed = true;
}

static bool MoveBoxMonToGraveyardBox(struct BoxPokemon *srcBoxMon)
{
    uint32_t boxPos;
    uint32_t boxNo;

    if (srcBoxMon == NULL)
        return false;
    if (GetBoxMonData(srcBoxMon, MON_DATA_SPECIES) == SPECIES_NONE)
        return false;

    EnsureGraveyardBoxNamed();

    /* Prefer dedicated Graveyard Box (last box), then fall back if full. */
    for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
    {
        struct BoxPokemon *dst = GetBoxedMonPtr(MP_GRAVEYARD_BOX_IDX, (u8)boxPos);
        if (GetBoxMonData(dst, MON_DATA_SPECIES) == SPECIES_NONE)
        {
            CopyMon(dst, srcBoxMon, sizeof(*dst));
            ZeroBoxMonData(srcBoxMon);
            return true;
        }
    }

    for (boxNo = 0; boxNo < TOTAL_BOXES_COUNT; boxNo++)
    {
        if (boxNo == MP_GRAVEYARD_BOX_IDX)
            continue;
        for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
        {
            struct BoxPokemon *dst = GetBoxedMonPtr((u8)boxNo, (u8)boxPos);
            if (GetBoxMonData(dst, MON_DATA_SPECIES) == SPECIES_NONE)
            {
                CopyMon(dst, srcBoxMon, sizeof(*dst));
                ZeroBoxMonData(srcBoxMon);
                return true;
            }
        }
    }
    return false;
}

static bool HasAliveNonEggPartyMon(void);
static bool WithdrawFirstAlivePcSurvivor(void);
static void FlushPendingKills(void);

static void MarkMonDeadLocalAndApplyEffects(MpSoulLinkEntry *entry, bool movePcToGraveyard)
{
    u32 i;
    u32 boxNo;
    u32 boxPos;

    if (entry == NULL || !MpSoulLink_AreRulesActive())
        return;
    entry->flags |= MP_SL_FLAG_DEAD;

    if (entry->originMapsec != MP_MAPSEC_INVALID && entry->originMapsec != MP_STARTER_MAPSEC)
    {
        MpSoulLinkRouteState *rs = FindOrCreateRouteState(entry->originMapsec, true);
        if (rs != NULL)
        {
            rs->routeDead = 1;
            rs->localConsumed = 1;
        }
    }

    /* Zero HP on matching party mons (graveyard transfer occurs at battle end or immediately out of battle). */
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *partyMon = &gParties[B_TRAINER_PLAYER][i];
        if (GetMonData(partyMon, MON_DATA_SPECIES) == SPECIES_NONE)
            continue;
        if (KeysEqual(ExtractKeyFromMon(partyMon), entry->key))
        {
            u32 zeroHp = 0;
            SetMonData(partyMon, MON_DATA_HP, &zeroHp);
            if (!gMain.inBattle && movePcToGraveyard)
            {
                EnsureGraveyardBoxNamed();
                if (MoveBoxMonToGraveyardBox(&partyMon->box))
                {
                    ZeroMonData(partyMon);
                    CompactPartySlots();
                    CalculatePlayerPartyCount();
                }
            }
        }
    }

    if (movePcToGraveyard)
    {
        for (boxNo = 0; boxNo < MP_GRAVEYARD_BOX_IDX; boxNo++)
        {
            for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
            {
                struct BoxPokemon *boxMon = GetBoxedMonPtr((u8)boxNo, (u8)boxPos);
                if (GetBoxMonData(boxMon, MON_DATA_SPECIES) == SPECIES_NONE)
                    continue;
                if (KeysEqual(ExtractKeyFromBoxMon(boxMon), entry->key))
                    MoveBoxMonToGraveyardBox(boxMon);
            }
        }

        if (!gMain.inBattle && !HasAliveNonEggPartyMon())
        {
            const MpRunConfig *cfg = MpNet_GetRunConfig();
            if ((cfg != NULL && (cfg->nuzlockeFlags & MP_NUZ_F_WHITEOUT_GAME_OVER)) || !WithdrawFirstAlivePcSurvivor())
            {
                sRunGameOver = true;
                MpNet_SendJson(MP_CH_SOUL_LINK, MP_PEER_BROADCAST, "{\"t\":\"nuzlocke_gameover\"}");
            }
        }
    }

    if (!gMain.inBattle)
        UpdateFollowingPokemon();
}

static bool IsDirectSpeciesOrEvoMatch(enum Species ownedSpecies, enum Species targetSpecies)
{
    const struct Evolution *evos;
    u32 i;

    if (ownedSpecies == targetSpecies)
        return true;

    evos = GetSpeciesEvolutions(ownedSpecies);
    if (evos != NULL)
    {
        for (i = 0; evos[i].method != EVOLUTIONS_END; i++)
        {
            if (evos[i].targetSpecies == targetSpecies)
                return true;
        }
    }

    evos = GetSpeciesEvolutions(targetSpecies);
    if (evos != NULL)
    {
        for (i = 0; evos[i].method != EVOLUTIONS_END; i++)
        {
            if (evos[i].targetSpecies == ownedSpecies)
                return true;
        }
    }
    return false;
}

void MpSoulLink_Init(void)
{
    if (sInitialized)
        return;
    sInitialized = true;
    MpSoulLink_ResetRun();
    MpNet_RegisterHandler(MP_CH_SOUL_LINK, MpSoulLink_HandlePacket);
}

void MpSoulLink_ResetRun(void)
{
    memset(sSoulLinkEntries, 0, sizeof(sSoulLinkEntries));
    memset(sRouteStates, 0, sizeof(sRouteStates));
    memset(sPendingKills, 0, sizeof(sPendingKills));
    sSoulLinkEntryCount = 0;
    sRouteStateCount = 0;
    sPendingKillCount = 0;
    sNextLinkSeq = 2;
    sRunGameOver = false;
    sGraveyardBoxNamed = false;
    sWildEncounterActive = false;
    sActiveWildMapsec = MP_MAPSEC_INVALID;
    sActiveWildCaughtAny = false;
    sSuppressGiftHook = false;
    sLastLinkedPartnerSlot = 0xFF;
    sStarterPendingBroadcast = false;
    sStarterBroadcasted = false;
    sStarterKey.otId = 0;
    sStarterKey.personality = 0;
    sRemoteStarterDeadDuringTutorial = false;
}

void MpSoulLink_SetSuppressGiftHook(bool suppress)
{
    sSuppressGiftHook = suppress;
}

bool MpSoulLink_IsEnabled(void)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    return (cfg != NULL) && ((cfg->nuzlockeFlags & MP_NUZ_F_ENABLED) != 0);
}

bool MpSoulLink_AreRulesActive(void)
{
    return MpSoulLink_IsEnabled() && MpWorldSync_IsLocalTutorialComplete();
}

bool MpSoulLink_IsSetModeEnforced(void)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    if (!MpSoulLink_AreRulesActive())
        return false;
    return (cfg->nuzlockeFlags & MP_NUZ_F_SET_MODE) != 0;
}

bool MpSoulLink_IsLevelCapActive(void)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    if (!MpSoulLink_AreRulesActive())
        return false;
    return (cfg->nuzlockeFlags & MP_NUZ_F_LEVEL_CAPS) != 0;
}

bool MpSoulLink_AreBattleItemsBlocked(void)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    if (!MpSoulLink_AreRulesActive())
        return false;
    return (cfg->nuzlockeFlags & MP_NUZ_F_NO_BATTLE_ITEMS) != 0;
}

bool MpSoulLink_IsBattleItemBlocked(uint16_t itemId)
{
    if (!MpSoulLink_AreBattleItemsBlocked())
        return false;
    if (itemId == ITEM_NONE)
        return false;
    /* Poke Balls must remain usable to capture wild encounters. */
    if (GetItemPocket((enum Item)itemId) == POCKET_POKE_BALLS)
        return false;
    return true;
}

bool MpSoulLink_IsRunGameOver(void)
{
    return sRunGameOver;
}

uint8_t MpSoulLink_GetCurrentLevelCap(void)
{
    static const u16 sMilestoneCaps[9][2] = {
        { FLAG_BADGE01_GET, 15 },
        { FLAG_BADGE02_GET, 19 },
        { FLAG_BADGE03_GET, 24 },
        { FLAG_BADGE04_GET, 29 },
        { FLAG_BADGE05_GET, 31 },
        { FLAG_BADGE06_GET, 33 },
        { FLAG_BADGE07_GET, 42 },
        { FLAG_BADGE08_GET, 46 },
        { FLAG_IS_CHAMPION, 58 },
    };
    u32 i;
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    u8 syncedBadges = (cfg != NULL) ? cfg->badgesMask : 0;

    for (i = 0; i < 8; i++)
    {
        bool hasBadge = FlagGet(sMilestoneCaps[i][0]) || ((syncedBadges & (1u << i)) != 0);
        if (!hasBadge)
            return (uint8_t)sMilestoneCaps[i][1];
    }
    if (!FlagGet(FLAG_IS_CHAMPION))
        return (uint8_t)sMilestoneCaps[8][1];
    return MAX_LEVEL;
}

uint32_t MpSoulLink_ClampExpToCap(const struct Pokemon *mon, uint32_t candidateExp)
{
    u8 levelCap;
    enum Species species;
    enum GrowthRate growthRate;
    u32 currentExp;
    u32 capExp;

    if (mon == NULL || !MpSoulLink_IsLevelCapActive())
        return candidateExp;

    levelCap = MpSoulLink_GetCurrentLevelCap();
    if (levelCap >= MAX_LEVEL)
        return candidateExp;

    species = SanitizeSpeciesId(GetMonData((struct Pokemon *)mon, MON_DATA_SPECIES));
    if (species == SPECIES_NONE)
        return candidateExp;

    growthRate = gSpeciesInfo[species].growthRate;
    capExp = gExperienceTables[growthRate][levelCap];
    currentExp = GetMonData((struct Pokemon *)mon, MON_DATA_EXP);

    if (currentExp >= capExp)
        return (candidateExp > currentExp) ? currentExp : candidateExp;
    if (candidateExp > capExp)
        return capExp;
    return candidateExp;
}

uint8_t MpSoulLink_GetEffectiveGroupForSlot(uint8_t slotIndex)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    const MpPeerState *peer;

    if (cfg != NULL && cfg->soulLinkMode == MP_SOULLINK_GROUP)
        return 0; /* Group 0 = all players linked together */

    peer = MpNet_GetPeerState(slotIndex);
    if (peer != NULL && peer->partyPairTag != 0)
        return peer->partyPairTag;

    return (slotIndex < 2) ? 1u : 2u;
}

bool MpSoulLink_IsSlotLinkedWithLocal(uint8_t slotIndex, uint8_t packetLinkGroup)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    const MpPeerState *localPeer;
    const MpPeerState *remotePeer;
    uint8_t localSlot = MpNet_GetLocalSlot();
    uint8_t localGroup;

    if (slotIndex == localSlot)
        return true;
    if (cfg != NULL && cfg->soulLinkMode == MP_SOULLINK_GROUP)
        return true;

    localPeer = MpNet_GetPeerState(localSlot);
    remotePeer = MpNet_GetPeerState(slotIndex);
    if (localPeer != NULL && remotePeer != NULL &&
        localPeer->partyPairTag != 0 &&
        localPeer->partyPairTag == remotePeer->partyPairTag)
    {
        sLastLinkedPartnerSlot = slotIndex;
        return true;
    }

    if ((localPeer == NULL || localPeer->partyPairTag == 0) &&
        (remotePeer == NULL || remotePeer->partyPairTag == 0))
    {
        u8 connectedCount = 0;
        u8 i;
        if (sLastLinkedPartnerSlot == slotIndex)
            return true;
        for (i = 0; i < MP_MAX_PLAYERS; i++)
        {
            const MpPeerState *p = MpNet_GetPeerState(i);
            if (p != NULL && p->connected)
                connectedCount++;
        }
        if (connectedCount <= 2)
            return true;
    }

    if (packetLinkGroup == 0)
        return true;

    localGroup = MpSoulLink_GetEffectiveGroupForSlot(localSlot);
    return (MpSoulLink_GetEffectiveGroupForSlot(slotIndex) == localGroup) || (packetLinkGroup == localGroup);
}

const MpSoulLinkEntry *MpSoulLink_LookupKey(struct MpSoulLinkKey key)
{
    return FindMutableEntryByKey(key);
}

const MpSoulLinkEntry *MpSoulLink_LookupMon(const struct Pokemon *mon)
{
    if (mon == NULL)
        return NULL;
    if (GetMonData((struct Pokemon *)mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA)
        return NULL;
    return FindMutableEntryByKey(ExtractKeyFromMon(mon));
}

const MpSoulLinkEntry *MpSoulLink_LookupBoxMon(const struct BoxPokemon *boxMon)
{
    return FindMutableEntryByKey(ExtractKeyFromBoxMon(boxMon));
}

MpSoulLinkEntry *MpSoulLink_UpsertMon(const struct Pokemon *mon, uint16_t mapsec, uint16_t linkId, uint8_t originSlot, uint8_t extraFlags)
{
    struct MpSoulLinkKey key;
    MpSoulLinkEntry *entry;
    uint16_t species;

    if (mon == NULL)
        return NULL;

    key = ExtractKeyFromMon(mon);
    if (key.otId == 0 && key.personality == 0)
        return NULL;

    species = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_SPECIES);
    entry = FindMutableEntryByKey(key);
    if (entry == NULL)
    {
        if (sSoulLinkEntryCount >= MP_SOUL_LINK_TABLE_CAP)
            return NULL;
        entry = &sSoulLinkEntries[sSoulLinkEntryCount++];
        memset(entry, 0, sizeof(*entry));
        entry->key = key;
        entry->linkSeqNum = sNextLinkSeq++;
    }

    if (linkId != 0)
        entry->linkId = linkId;
    else if (entry->linkId == 0)
        entry->linkId = ComputeDeterministicLinkId(MpSoulLink_GetEffectiveGroupForSlot(originSlot), mapsec);

    if (mapsec != MP_MAPSEC_INVALID)
        entry->originMapsec = mapsec;
    if (species != SPECIES_NONE)
        entry->species = species;
    entry->originSlot = originSlot;
    entry->linkGroup = MpSoulLink_GetEffectiveGroupForSlot(originSlot);
    entry->flags |= (uint8_t)(MP_SL_FLAG_ACTIVE | extraFlags);
    if (IsMonShiny((struct Pokemon *)mon))
        entry->flags |= MP_SL_FLAG_SHINY;
    return entry;
}

bool MpSoulLink_IsMonDeadKey(struct MpSoulLinkKey key)
{
    const MpSoulLinkEntry *entry = FindMutableEntryByKey(key);
    return (entry != NULL) && ((entry->flags & MP_SL_FLAG_DEAD) != 0);
}

bool MpSoulLink_IsMonDead(const struct Pokemon *mon)
{
    if (mon == NULL)
        return false;
    if (GetMonData((struct Pokemon *)mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA)
        return false;
    return MpSoulLink_IsMonDeadKey(ExtractKeyFromMon(mon));
}

bool MpSoulLink_IsSpeciesLineOwned(uint16_t species)
{
    enum Species sp = SanitizeSpeciesId((enum Species)species);
    u32 i;
    u32 boxNo;
    u32 boxPos;

    if (sp == SPECIES_NONE || sp == SPECIES_EGG)
        return false;

    if (GetSetPokedexFlag(SpeciesToNationalPokedexNum(sp), FLAG_GET_CAUGHT))
        return true;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        enum Species pSp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES);
        if (pSp != SPECIES_NONE && IsDirectSpeciesOrEvoMatch(pSp, sp))
            return true;
    }

    for (boxNo = 0; boxNo < TOTAL_BOXES_COUNT; boxNo++)
    {
        for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
        {
            struct BoxPokemon *bMon = GetBoxedMonPtr((u8)boxNo, (u8)boxPos);
            enum Species bSp = GetBoxMonData(bMon, MON_DATA_SPECIES);
            if (bSp != SPECIES_NONE && IsDirectSpeciesOrEvoMatch(bSp, sp))
                return true;
        }
    }
    return false;
}

static bool IsRouteDead(uint16_t mapsec)
{
    MpSoulLinkRouteState *rs;
    if (mapsec == MP_MAPSEC_INVALID)
        return false;
    rs = FindOrCreateRouteState(mapsec, false);
    return (rs != NULL) && (rs->routeDead != 0);
}

bool MpSoulLink_IsRouteConsumed(uint16_t mapsec)
{
    MpSoulLinkRouteState *rs;
    if (!MpSoulLink_AreRulesActive() || mapsec == MP_MAPSEC_INVALID)
        return false;

    rs = FindOrCreateRouteState(mapsec, false);
    if (rs == NULL)
        return false;

    /* Solo catches 1 of 2 wild mons; co-op catches 1 per player on the route; dead/failed routes are consumed. */
    return (rs->localCapturedCount >= 1) || (rs->localConsumed != 0) || (rs->routeDead != 0);
}

bool MpSoulLink_CanThrowBallAtBattler(uint8_t battlerId, uint16_t mapsec)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    struct Pokemon *wildMon;
    enum Species species;
    bool isShiny;

    if (!MpSoulLink_AreRulesActive())
        return true;

    if ((gBattleTypeFlags & (BATTLE_TYPE_CATCH_TUTORIAL | BATTLE_TYPE_FIRST_BATTLE)) != 0)
        return true;

    /* Dead/failed routes are strictly locked even if shiny (holds Soul-Link parity). */
    if (IsRouteDead(mapsec))
        return false;

    if (battlerId >= MAX_BATTLERS_COUNT)
        return false;

    wildMon = GetBattlerMon((enum BattlerId)battlerId);
    species = GetMonData(wildMon, MON_DATA_SPECIES);
    isShiny = IsMonShiny(wildMon);

    /* Shiny Clause overrides per-route catch limit and dupes clause on live routes. */
    if (isShiny && (cfg->nuzlockeFlags & MP_NUZ_F_SHINY_CLAUSE))
        return true;

    if (MpSoulLink_IsRouteConsumed(mapsec))
        return false;

    if ((cfg->nuzlockeFlags & MP_NUZ_F_DUPES_CLAUSE) && MpSoulLink_IsSpeciesLineOwned((uint16_t)species))
        return false;

    return true;
}

void MpSoulLink_OnWildEncounterStarted(uint16_t mapsec)
{
    if (!MpSoulLink_AreRulesActive())
        return;
    if ((GetSafariZoneFlag() != 0) || (gBattleTypeFlags & (BATTLE_TYPE_CATCH_TUTORIAL | BATTLE_TYPE_FIRST_BATTLE | BATTLE_TYPE_SAFARI)) != 0)
        return;
    sWildEncounterActive = true;
    sActiveWildMapsec = mapsec;
    sActiveWildCaughtAny = false;
}

static void BroadcastStarterCatchPacket(const struct Pokemon *mon, uint16_t linkId)
{
    struct MpSoulLinkCatchPayload pkt;
    uint8_t localSlot;

    if (mon == NULL || linkId == 0)
        return;

    localSlot = MpNet_GetLocalSlot();
    memset(&pkt, 0, sizeof(pkt));
    pkt.slotIndex = localSlot;
    pkt.linkGroup = MpSoulLink_GetEffectiveGroupForSlot(localSlot);
    pkt.mapsec = MP_STARTER_MAPSEC;
    pkt.linkId = linkId;
    pkt.species = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_SPECIES);
    pkt.level = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_LEVEL);
    pkt.key = ExtractKeyFromMon(mon);
    MpNet_SendBinary(MP_CH_SOUL_LINK, MP_PEER_BROADCAST, MP_PKT_SL_ROUTE_CATCH, &pkt, (uint16_t)sizeof(pkt));
    MpOverworld_RequestAutoSave(1);
}

static void KillMatchingEntriesImmediate(struct MpSoulLinkKey key, uint16_t linkId, uint16_t mapsec)
{
    uint16_t i;
    bool hasKey = (key.otId != 0 || key.personality != 0);

    for (i = 0; i < sSoulLinkEntryCount; i++)
    {
        MpSoulLinkEntry *entry = &sSoulLinkEntries[i];
        bool matchKey = hasKey && KeysEqual(entry->key, key);
        bool matchLink = (linkId != 0) && (entry->linkId == linkId);
        bool matchMap = (mapsec != MP_MAPSEC_INVALID)
                     && (entry->originMapsec == mapsec)
                     && (entry->linkId != 0)
                     && !(entry->flags & MP_SL_FLAG_STARTER);
        if (matchKey || matchLink || matchMap)
            MarkMonDeadLocalAndApplyEffects(entry, !gMain.inBattle);
    }
}

void MpSoulLink_OnWildEncounterFinished(uint16_t mapsec, bool caughtAny, bool allOpponentsDupes)
{
    MpSoulLinkRouteState *rs;
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    uint16_t effectiveSec = (mapsec != MP_MAPSEC_INVALID) ? mapsec : sActiveWildMapsec;
    bool dupesClause = (cfg != NULL) && ((cfg->nuzlockeFlags & MP_NUZ_F_DUPES_CLAUSE) != 0);
    bool skippedSpecial = (GetSafariZoneFlag() != 0)
                       || ((gBattleTypeFlags & (BATTLE_TYPE_CATCH_TUTORIAL | BATTLE_TYPE_FIRST_BATTLE | BATTLE_TYPE_SAFARI)) != 0);
    bool caughtNow = caughtAny || sActiveWildCaughtAny;

    if (!MpSoulLink_AreRulesActive() || effectiveSec == MP_MAPSEC_INVALID || skippedSpecial)
    {
        sWildEncounterActive = false;
        sActiveWildMapsec = MP_MAPSEC_INVALID;
        sActiveWildCaughtAny = false;
        return;
    }

    rs = FindOrCreateRouteState(effectiveSec, true);
    if (rs != NULL)
    {
        bool wasFreshRoute = (rs->localCapturedCount == 0 && rs->localConsumed == 0 && rs->routeDead == 0);
        if (caughtNow)
        {
            if (rs->localCapturedCount == 0)
                rs->localCapturedCount = 1;
            rs->localConsumed = 1;
        }
        else if (sWildEncounterActive && wasFreshRoute && (!allOpponentsDupes || !dupesClause))
        {
            /* Fresh non-dupe encounter failed/fled -> route failed/dead across Soul-Link group. */
            struct MpSoulLinkRouteFailPayload failPkt;
            uint8_t localSlot = MpNet_GetLocalSlot();

            rs->localConsumed = 1;
            rs->routeDead = 1;

            memset(&failPkt, 0, sizeof(failPkt));
            failPkt.slotIndex = localSlot;
            failPkt.linkGroup = MpSoulLink_GetEffectiveGroupForSlot(localSlot);
            failPkt.mapsec = effectiveSec;
            failPkt.linkId = rs->linkId;
            MpNet_SendBinary(MP_CH_SOUL_LINK, MP_PEER_BROADCAST, MP_PKT_SL_ROUTE_FAIL, &failPkt, (uint16_t)sizeof(failPkt));
            if (rs->linkId != 0)
            {
                struct MpSoulLinkKey emptyKey = {0, 0};
                KillMatchingEntriesImmediate(emptyKey, rs->linkId, effectiveSec);
            }
        }
    }

    if (rs != NULL)
    {
        /* Persist the route outcome and player position once the battle returns to the field. */
        MpOverworld_RequestAutoSave(1);
    }
    sWildEncounterActive = false;
    sActiveWildMapsec = MP_MAPSEC_INVALID;
    sActiveWildCaughtAny = false;
}

void MpSoulLink_RegisterStarter(const struct Pokemon *mon)
{
    uint8_t localSlot;
    MpSoulLinkEntry *entry;

    if (mon == NULL)
        return;

    MpSoulLink_Init();
    localSlot = MpNet_GetLocalSlot();
    entry = MpSoulLink_UpsertMon(mon, MP_STARTER_MAPSEC, MP_STARTER_LINK_ID, localSlot, MP_SL_FLAG_STARTER);
    if (entry != NULL)
        entry->linkSeqNum = MP_STARTER_LINK_ID;
    sStarterKey = ExtractKeyFromMon(mon);

    if (MpWorldSync_IsLocalTutorialComplete())
    {
        BroadcastStarterCatchPacket(mon, (entry != NULL) ? entry->linkId : MP_STARTER_LINK_ID);
        sStarterBroadcasted = true;
        sStarterPendingBroadcast = false;
    }
    else
    {
        sStarterPendingBroadcast = true;
        sStarterBroadcasted = false;
    }
}

void MpSoulLink_OnPokemonCaptured(struct Pokemon *mon, uint16_t mapsec)
{
    uint8_t localSlot;
    uint8_t linkGroup;
    MpSoulLinkRouteState *rs;
    MpSoulLinkEntry *entry;
    uint16_t assignedLinkId;
    uint8_t extraFlags = MP_SL_FLAG_NONE;
    struct MpSoulLinkCatchPayload pkt;

    if (mon == NULL)
        return;

    MpSoulLink_Init();
    if (mapsec == MP_MAPSEC_INVALID)
        mapsec = (uint16_t)GetCurrentRegionMapSectionId();

    localSlot = MpNet_GetLocalSlot();
    linkGroup = MpSoulLink_GetEffectiveGroupForSlot(localSlot);
    rs = FindOrCreateRouteState(mapsec, true);

    if (IsMonShiny(mon))
        extraFlags |= MP_SL_FLAG_SHINY;

    /* Share route linkId with co-op partner if partner already registered this mapsec. */
    if (rs != NULL && rs->linkId != 0 && rs->localCapturedCount == 0)
        assignedLinkId = rs->linkId;
    else if (rs != NULL && rs->localCapturedCount > 0)
        assignedLinkId = (uint16_t)(0x8000u | (mapsec & 0x0FFFu) | ((uint16_t)localSlot << 12));
    else
        assignedLinkId = ComputeDeterministicLinkId(linkGroup, mapsec);

    entry = MpSoulLink_UpsertMon(mon, mapsec, assignedLinkId, localSlot, extraFlags);
    if (rs != NULL)
    {
        if (rs->linkId == 0)
            rs->linkId = assignedLinkId;
        if (entry != NULL && rs->linkSeqNum == 0)
            rs->linkSeqNum = entry->linkSeqNum;
        else if (entry != NULL && rs->linkSeqNum != 0 && rs->localCapturedCount == 0)
            entry->linkSeqNum = rs->linkSeqNum;

        rs->localCapturedCount++;
        rs->localConsumed = 1;
        if (localSlot < MP_MAX_PLAYERS)
        {
            rs->slotMask |= (uint8_t)(1u << localSlot);
            rs->speciesBySlot[localSlot] = (uint16_t)GetMonData(mon, MON_DATA_SPECIES);
        }
    }

    sActiveWildCaughtAny = true;

    memset(&pkt, 0, sizeof(pkt));
    pkt.slotIndex = localSlot;
    pkt.linkGroup = linkGroup;
    pkt.mapsec = mapsec;
    pkt.linkId = (entry != NULL) ? entry->linkId : assignedLinkId;
    pkt.species = (uint16_t)GetMonData(mon, MON_DATA_SPECIES);
    pkt.level = (uint8_t)GetMonData(mon, MON_DATA_LEVEL);
    pkt.key = ExtractKeyFromMon(mon);
    MpNet_SendBinary(MP_CH_SOUL_LINK, MP_PEER_BROADCAST, MP_PKT_SL_ROUTE_CATCH, &pkt, (uint16_t)sizeof(pkt));
    MpOverworld_RequestAutoSave(1);
}

void MpSoulLink_OnGiftOrEggReceived(struct Pokemon *mon, uint16_t mapsec)
{
    uint16_t baseSec;
    uint16_t giftSec;
    bool prevWildCaught;

    if (mon == NULL || sSuppressGiftHook)
        return;

    baseSec = (mapsec == MP_MAPSEC_INVALID) ? (uint16_t)GetCurrentRegionMapSectionId() : mapsec;
    /* Dedicated Kanto mapsec band (100..189) unused in Hoenn avoids colliding with wild route locks. */
    giftSec = (uint16_t)(100u + (baseSec % 90u));
    prevWildCaught = sActiveWildCaughtAny;
    MpSoulLink_OnPokemonCaptured(mon, giftSec);
    sActiveWildCaughtAny = prevWildCaught;
}

void MpSoulLink_OnMonFainted(struct Pokemon *mon, bool isPvp, uint8_t cause)
{
    MpSoulLinkEntry *entry;
    uint16_t mapsec;
    uint8_t localSlot;
    struct MpSoulLinkDeathPayload pkt;

    if (mon == NULL || isPvp)
        return;
    if (!MpSoulLink_AreRulesActive())
        return;
    if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_SHEDINJA)
        return;

    MpSoulLink_Init();
    localSlot = MpNet_GetLocalSlot();
    mapsec = (uint16_t)GetMonData(mon, MON_DATA_MET_LOCATION);

    entry = FindMutableEntryByKey(ExtractKeyFromMon(mon));
    if (entry == NULL)
        entry = MpSoulLink_UpsertMon(mon, mapsec, 0, localSlot, MP_SL_FLAG_NONE);
    if (entry == NULL)
        return;

    /* Avoid double-broadcasting if this faint was already marked dead. */
    if ((entry->flags & MP_SL_FLAG_DEAD) != 0)
        return;

    entry->flags |= MP_SL_FLAG_DEAD;

    memset(&pkt, 0, sizeof(pkt));
    pkt.slotIndex = localSlot;
    pkt.linkGroup = MpSoulLink_GetEffectiveGroupForSlot(localSlot);
    pkt.linkId = entry->linkId;
    pkt.cause = cause;
    pkt.key = entry->key;
    MpNet_SendBinary(MP_CH_SOUL_LINK, MP_PEER_BROADCAST, MP_PKT_SL_MON_FAINTED, &pkt, (uint16_t)sizeof(pkt));
}

void MpSoulLink_OnTradePreserveLink(const struct BoxPokemon *oldKeyMon, const struct Pokemon *newMon, uint16_t newLinkId, uint16_t mapsec, uint8_t newOwnerSlot)
{
    MpSoulLinkEntry *oldEntry = NULL;
    uint16_t preservedLinkId = newLinkId;
    uint16_t preservedMapsec = mapsec;
    uint16_t preservedSeq = 0;

    if (oldKeyMon != NULL)
        oldEntry = FindMutableEntryByKey(ExtractKeyFromBoxMon(oldKeyMon));
    if (oldEntry != NULL)
    {
        if (preservedLinkId == 0)
            preservedLinkId = oldEntry->linkId;
        if (preservedMapsec == MP_MAPSEC_INVALID)
            preservedMapsec = oldEntry->originMapsec;
        preservedSeq = oldEntry->linkSeqNum;
    }

    if (newMon != NULL)
    {
        MpSoulLinkEntry *added = MpSoulLink_UpsertMon(newMon, preservedMapsec, preservedLinkId, newOwnerSlot, MP_SL_FLAG_TRADED);
        if (added != NULL && preservedSeq != 0)
            added->linkSeqNum = preservedSeq;
    }
}

uint16_t MpSoulLink_TransferDeadPartyMonsToGraveyard(void)
{
    u32 i;
    uint16_t movedCount = 0;

    if (!MpSoulLink_AreRulesActive())
        return 0;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        enum Species sp = GetMonData(mon, MON_DATA_SPECIES);
        bool dead;

        if (sp == SPECIES_NONE || sp == SPECIES_EGG)
            continue;

        dead = (GetMonData(mon, MON_DATA_HP) == 0) || MpSoulLink_IsMonDead(mon);
        if (dead)
        {
            MpSoulLink_OnMonFainted(mon, false, MP_SL_CAUSE_BATTLE_FAINT);
            MonRestorePP(mon);
            if (MoveBoxMonToGraveyardBox(&mon->box))
            {
                ZeroMonData(mon);
                movedCount++;
            }
        }
    }

    if (movedCount > 0)
    {
        CompactPartySlots();
        CalculatePlayerPartyCount();
        if (!gMain.inBattle)
            UpdateFollowingPokemon();
    }
    return movedCount;
}

static bool HasAliveNonEggPartyMon(void)
{
    u32 i;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        enum Species sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(mon, MON_DATA_HP) > 0)
            return true;
    }
    return false;
}

static bool WithdrawFirstAlivePcSurvivor(void)
{
    u32 boxNo;
    u32 boxPos;
    u8 partyCount = CalculatePlayerPartyCount();

    if (partyCount >= PARTY_SIZE)
        return false;

    for (boxNo = 0; boxNo < MP_GRAVEYARD_BOX_IDX; boxNo++)
    {
        for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
        {
            struct BoxPokemon *boxMon = GetBoxedMonPtr((u8)boxNo, (u8)boxPos);
            enum Species sp = GetBoxMonData(boxMon, MON_DATA_SPECIES);
            if (sp == SPECIES_NONE || sp == SPECIES_EGG)
                continue;
            if (MpSoulLink_IsMonDeadKey(ExtractKeyFromBoxMon(boxMon)))
                continue;

            BoxMonToMon(boxMon, &gParties[B_TRAINER_PLAYER][partyCount]);
            ZeroBoxMonData(boxMon);
            CalculatePlayerPartyCount();
            return true;
        }
    }
    return false;
}

bool MpSoulLink_ProcessBattleEndWhiteout(bool isPvp)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    bool teamDefeated = (gBattleOutcome == B_OUTCOME_LOST
                      || gBattleOutcome == B_OUTCOME_DREW
                      || gBattleOutcome == B_OUTCOME_FORFEITED);

    if (isPvp || !MpSoulLink_AreRulesActive())
        return false;

    if (!gMain.inBattle && sPendingKillCount > 0)
        FlushPendingKills();

    MpSoulLink_TransferDeadPartyMonsToGraveyard();

    if (HasAliveNonEggPartyMon())
        return false;

    /* All active party Pokemon perished -> only treat Whiteout=GameOver as game over on an actual defeat; on a Co-op win try withdrawing a PC survivor first. */
    if ((teamDefeated && (cfg->nuzlockeFlags & MP_NUZ_F_WHITEOUT_GAME_OVER)) || !WithdrawFirstAlivePcSurvivor())
    {
        sRunGameOver = true;
        MpNet_SendJson(MP_CH_SOUL_LINK, MP_PEER_BROADCAST, "{\"t\":\"nuzlocke_gameover\"}");
        return true;
    }

    return false;
}

static uint16_t CountLocalAliveMons(void)
{
    uint16_t alive = 0;
    u32 i;
    u32 boxNo;
    u32 boxPos;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        enum Species sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(mon, MON_DATA_HP) > 0 && !MpSoulLink_IsMonDead(mon))
            alive++;
    }

    if (gPokemonStoragePtr != NULL)
    {
        for (boxNo = 0; boxNo < MP_GRAVEYARD_BOX_IDX; boxNo++)
        {
            for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
            {
                struct BoxPokemon *boxMon = GetBoxedMonPtr((u8)boxNo, (u8)boxPos);
                enum Species sp = GetBoxMonData(boxMon, MON_DATA_SPECIES);
                if (sp != SPECIES_NONE && sp != SPECIES_EGG && !GetBoxMonData(boxMon, MON_DATA_IS_EGG)
                 && !MpSoulLink_IsMonDeadKey(ExtractKeyFromBoxMon(boxMon)))
                    alive++;
            }
        }
    }
    return alive;
}

static MpSoulLinkEntry *FindLocalStarterEntry(const struct Pokemon **outPartyMon)
{
    MpSoulLinkEntry *entry = NULL;
    u32 i;

    if (outPartyMon != NULL)
        *outPartyMon = NULL;

    if (sStarterKey.otId != 0 || sStarterKey.personality != 0)
        entry = FindMutableEntryByKey(sStarterKey);

    if (entry == NULL)
    {
        uint16_t k;
        uint8_t localSlot = MpNet_GetLocalSlot();
        for (k = 0; k < sSoulLinkEntryCount; k++)
        {
            if ((sSoulLinkEntries[k].flags & MP_SL_FLAG_STARTER) != 0 && sSoulLinkEntries[k].originSlot == localSlot)
            {
                entry = &sSoulLinkEntries[k];
                break;
            }
        }
    }

    if (entry != NULL && outPartyMon != NULL)
    {
        for (i = 0; i < PARTY_SIZE; i++)
        {
            struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
            if (GetMonData(mon, MON_DATA_SPECIES) != SPECIES_NONE && KeysEqual(ExtractKeyFromMon(mon), entry->key))
            {
                *outPartyMon = mon;
                break;
            }
        }
    }
    return entry;
}

static MpSoulLinkEntry *EnsureEntryForKey(struct MpSoulLinkKey key, uint16_t mapsec, uint16_t linkId)
{
    MpSoulLinkEntry *entry;
    u32 i;
    u32 boxNo;
    u32 boxPos;

    if (key.otId == 0 && key.personality == 0)
        return NULL;

    entry = FindMutableEntryByKey(key);
    if (entry != NULL)
    {
        if (linkId != 0 && entry->linkId == 0)
            entry->linkId = linkId;
        if (mapsec != MP_MAPSEC_INVALID && entry->originMapsec == 0)
            entry->originMapsec = mapsec;
        return entry;
    }

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        if (GetMonData(mon, MON_DATA_SPECIES) != SPECIES_NONE && KeysEqual(ExtractKeyFromMon(mon), key))
            return MpSoulLink_UpsertMon(mon, mapsec, linkId, MpNet_GetLocalSlot(), MP_SL_FLAG_NONE);
    }

    if (gPokemonStoragePtr != NULL)
    {
        for (boxNo = 0; boxNo < TOTAL_BOXES_COUNT; boxNo++)
        {
            for (boxPos = 0; boxPos < IN_BOX_COUNT; boxPos++)
            {
                struct BoxPokemon *boxMon = GetBoxedMonPtr((u8)boxNo, (u8)boxPos);
                enum Species sp = GetBoxMonData(boxMon, MON_DATA_SPECIES);
                if (sp != SPECIES_NONE && KeysEqual(ExtractKeyFromBoxMon(boxMon), key))
                {
                    if (sSoulLinkEntryCount >= MP_SOUL_LINK_TABLE_CAP)
                        return NULL;
                    entry = &sSoulLinkEntries[sSoulLinkEntryCount++];
                    memset(entry, 0, sizeof(*entry));
                    entry->key = key;
                    entry->linkSeqNum = sNextLinkSeq++;
                    entry->linkId = linkId;
                    entry->originMapsec = (mapsec != MP_MAPSEC_INVALID)
                                        ? mapsec
                                        : (uint16_t)GetBoxMonData(boxMon, MON_DATA_MET_LOCATION);
                    entry->species = (uint16_t)sp;
                    entry->originSlot = MpNet_GetLocalSlot();
                    entry->linkGroup = MpSoulLink_GetEffectiveGroupForSlot(entry->originSlot);
                    entry->flags = MP_SL_FLAG_ACTIVE;
                    return entry;
                }
            }
        }
    }

    if (sSoulLinkEntryCount >= MP_SOUL_LINK_TABLE_CAP)
        return NULL;
    entry = &sSoulLinkEntries[sSoulLinkEntryCount++];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->linkSeqNum = sNextLinkSeq++;
    entry->linkId = linkId;
    entry->originMapsec = mapsec;
    entry->originSlot = MpNet_GetLocalSlot();
    entry->linkGroup = MpSoulLink_GetEffectiveGroupForSlot(entry->originSlot);
    entry->flags = MP_SL_FLAG_ACTIVE;
    return entry;
}

static void QueuePendingKill(struct MpSoulLinkKey key, uint16_t linkId, uint16_t mapsec, bool fromPreTutorial)
{
    uint16_t i;
    for (i = 0; i < sPendingKillCount; i++)
    {
        bool sameKey = (key.otId != 0 || key.personality != 0)
                    && KeysEqual(sPendingKills[i].key, key);
        bool sameLink = (linkId != 0) && (sPendingKills[i].linkId == linkId);
        bool sameMap = (mapsec != MP_MAPSEC_INVALID) && (sPendingKills[i].mapsec == mapsec);
        if ((sameKey || (key.otId == 0 && key.personality == 0 && sPendingKills[i].key.otId == 0 && sPendingKills[i].key.personality == 0 && (sameLink || sameMap))))
        {
            if (linkId != 0 && sPendingKills[i].linkId == 0)
                sPendingKills[i].linkId = linkId;
            if (mapsec != MP_MAPSEC_INVALID && sPendingKills[i].mapsec == MP_MAPSEC_INVALID)
                sPendingKills[i].mapsec = mapsec;
            return;
        }
    }
    if (sPendingKillCount < MP_SL_MAX_PENDING_KILLS)
    {
        MpPendingSoulLinkKill *q = &sPendingKills[sPendingKillCount++];
        q->key = key;
        q->linkId = linkId;
        q->mapsec = mapsec;
        q->fromPreTutorial = fromPreTutorial;
    }
}

static void FlushPendingKills(void)
{
    uint16_t count;
    uint16_t i;

    if (!MpSoulLink_AreRulesActive() || sPendingKillCount == 0)
        return;

    count = sPendingKillCount;
    sPendingKillCount = 0;
    for (i = 0; i < count; i++)
    {
        if (sPendingKills[i].key.otId != 0 || sPendingKills[i].key.personality != 0)
            EnsureEntryForKey(sPendingKills[i].key, sPendingKills[i].mapsec, sPendingKills[i].linkId);
        KillMatchingEntriesImmediate(sPendingKills[i].key, sPendingKills[i].linkId, sPendingKills[i].mapsec);
    }
}

static void EnqueueOrKillByParams(struct MpSoulLinkKey key, uint16_t linkId, uint16_t mapsec, bool fromPreTutorial)
{
    if (key.otId != 0 || key.personality != 0)
        EnsureEntryForKey(key, mapsec, linkId);

    if (!MpWorldSync_IsLocalTutorialComplete() && (linkId == MP_STARTER_LINK_ID || mapsec == MP_STARTER_MAPSEC))
        sRemoteStarterDeadDuringTutorial = true;

    if (!MpSoulLink_AreRulesActive() || gMain.inBattle)
    {
        QueuePendingKill(key, linkId, mapsec, fromPreTutorial);
        return;
    }

    KillMatchingEntriesImmediate(key, linkId, mapsec);
}

/* -------------------------------------------------------------------------
 * Zero-heap scoped JSON helpers for _mp_import_soullink_json
 * ------------------------------------------------------------------------- */
static const char *JsonSkipWs(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

static const char *JsonSkipString(const char *p, const char *end)
{
    if (p >= end || *p != '"')
        return p;
    p++;
    while (p < end)
    {
        if (*p == '\\' && (p + 1) < end)
        {
            p += 2;
            continue;
        }
        if (*p == '"')
            return p + 1;
        p++;
    }
    return end;
}

static const char *JsonFindMatchingClose(const char *start, const char *end, char openCh, char closeCh)
{
    int depth = 0;
    const char *p = start;

    if (p >= end || *p != openCh)
        return NULL;

    while (p < end)
    {
        if (*p == '"')
        {
            p = JsonSkipString(p, end);
            continue;
        }
        if (*p == openCh)
            depth++;
        else if (*p == closeCh)
        {
            depth--;
            if (depth == 0)
                return p;
        }
        p++;
    }
    return NULL;
}

static const char *JsonSkipValue(const char *p, const char *end)
{
    p = JsonSkipWs(p, end);
    if (p >= end)
        return end;
    if (*p == '"')
        return JsonSkipString(p, end);
    if (*p == '{')
    {
        const char *close = JsonFindMatchingClose(p, end, '{', '}');
        return close ? (close + 1) : end;
    }
    if (*p == '[')
    {
        const char *close = JsonFindMatchingClose(p, end, '[', ']');
        return close ? (close + 1) : end;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *JsonFindKeyInObject(const char *objStart, const char *objEnd, const char *targetKey)
{
    const char *p = JsonSkipWs(objStart, objEnd);
    if (p >= objEnd || *p != '{')
        return NULL;
    p++;

    while (p < objEnd)
    {
        char keyBuf[64];
        size_t klen = 0;
        const char *valPtr;

        p = JsonSkipWs(p, objEnd);
        while (p < objEnd && *p == ',')
            p = JsonSkipWs(p + 1, objEnd);
        if (p >= objEnd || *p == '}')
            break;
        if (*p != '"')
        {
            p = JsonSkipValue(p, objEnd);
            continue;
        }

        p++;
        while (p < objEnd && *p != '"')
        {
            if (*p == '\\' && (p + 1) < objEnd)
                p++;
            if (klen + 1 < sizeof(keyBuf))
                keyBuf[klen++] = *p;
            p++;
        }
        keyBuf[klen] = '\0';
        if (p < objEnd && *p == '"')
            p++;

        p = JsonSkipWs(p, objEnd);
        if (p >= objEnd || *p != ':')
            continue;
        valPtr = JsonSkipWs(p + 1, objEnd);
        if (strcmp(keyBuf, targetKey) == 0)
            return valPtr;

        p = JsonSkipValue(valPtr, objEnd);
    }
    return NULL;
}

static bool JsonReadUint32(const char *objStart, const char *objEnd, const char *key, uint32_t *outVal)
{
    const char *p = JsonFindKeyInObject(objStart, objEnd, key);
    char *endPtr = NULL;
    unsigned long v;

    if (p == NULL || !(*p >= '0' && *p <= '9'))
        return false;
    v = strtoul(p, &endPtr, 10);
    if (endPtr == p)
        return false;
    *outVal = (uint32_t)v;
    return true;
}

static bool JsonReadBool(const char *objStart, const char *objEnd, const char *key, bool *outVal)
{
    const char *p = JsonFindKeyInObject(objStart, objEnd, key);
    if (p == NULL)
        return false;
    if ((p + 4) <= objEnd && strncmp(p, "true", 4) == 0)
    {
        *outVal = true;
        return true;
    }
    if ((p + 5) <= objEnd && strncmp(p, "false", 5) == 0)
    {
        *outVal = false;
        return true;
    }
    if (*p == '1')
    {
        *outVal = true;
        return true;
    }
    if (*p == '0')
    {
        *outVal = false;
        return true;
    }
    return false;
}

static void ImportSoulLinkEntriesArray(const char *arrStart, const char *arrEnd, bool defaultDead)
{
    const char *p = JsonSkipWs(arrStart, arrEnd);
    if (p >= arrEnd || *p != '[')
        return;
    p++;

    while (p < arrEnd)
    {
        const char *objClose;
        uint32_t otId = 0;
        uint32_t personality = 0;
        uint32_t linkId = 0;
        uint32_t mapsec = MP_MAPSEC_INVALID;
        bool isDead = defaultDead;
        struct MpSoulLinkKey key;

        p = JsonSkipWs(p, arrEnd);
        while (p < arrEnd && *p == ',')
            p = JsonSkipWs(p + 1, arrEnd);
        if (p >= arrEnd || *p == ']')
            break;
        if (*p != '{')
        {
            p = JsonSkipValue(p, arrEnd);
            continue;
        }
        objClose = JsonFindMatchingClose(p, arrEnd, '{', '}');
        if (objClose == NULL)
            break;

        JsonReadUint32(p, objClose + 1, "otId", &otId);
        JsonReadUint32(p, objClose + 1, "personality", &personality);
        JsonReadUint32(p, objClose + 1, "linkId", &linkId);
        JsonReadUint32(p, objClose + 1, "mapsec", &mapsec);
        JsonReadBool(p, objClose + 1, "dead", &isDead);

        key.otId = otId;
        key.personality = personality;

        if (otId != 0 || personality != 0)
        {
            MpSoulLinkEntry *entry = EnsureEntryForKey(key, (uint16_t)mapsec, (uint16_t)linkId);
            if (entry != NULL)
            {
                if (linkId != 0)
                    entry->linkId = (uint16_t)linkId;
                if (mapsec < MP_MAPSEC_INVALID)
                    entry->originMapsec = (uint16_t)mapsec;
            }
        }

        if (isDead)
            EnqueueOrKillByParams(key, (uint16_t)linkId, (uint16_t)mapsec, !MpWorldSync_IsLocalTutorialComplete());

        p = objClose + 1;
    }
}

void MpSoulLink_OnTutorialCompleted(void)
{
    const struct Pokemon *starterPartyMon = NULL;
    MpSoulLinkEntry *starterEntry;
    MpSoulLinkRouteState *rsStarter;
    bool hostStarterAlreadyDead;

    MpSoulLink_Init();
    if (!MpWorldSync_IsLocalTutorialComplete())
        return;

    starterEntry = FindLocalStarterEntry(&starterPartyMon);
    if (starterEntry == NULL && !sStarterBroadcasted && CalculatePlayerPartyCount() > 0 && FlagGet(FLAG_SYS_POKEMON_GET))
    {
        struct Pokemon *lead = &gParties[B_TRAINER_PLAYER][0];
        u16 metLoc = (u16)GetMonData(lead, MON_DATA_MET_LOCATION);
        rsStarter = FindOrCreateRouteState(MP_STARTER_MAPSEC, false);
        if ((rsStarter == NULL || rsStarter->localCapturedCount == 0)
         && (metLoc == MAPSEC_ROUTE_101 || metLoc == MAPSEC_LITTLEROOT_TOWN))
        {
            starterEntry = MpSoulLink_UpsertMon(lead, MP_STARTER_MAPSEC, MP_STARTER_LINK_ID, MpNet_GetLocalSlot(), MP_SL_FLAG_STARTER);
            if (starterEntry != NULL)
                starterEntry->linkSeqNum = MP_STARTER_LINK_ID;
            sStarterKey = ExtractKeyFromMon(lead);
            starterPartyMon = lead;
            sStarterPendingBroadcast = true;
        }
    }

    rsStarter = FindOrCreateRouteState(MP_STARTER_MAPSEC, false);
    hostStarterAlreadyDead = sRemoteStarterDeadDuringTutorial
                          || (rsStarter != NULL && rsStarter->routeDead != 0);

    /* Guard against late-joiner soft-lock: if Host's starter died before joiner finished
     * tutorial and joiner only owns 1 mon, make joiner's starter an unlinked solo mon. */
    if (hostStarterAlreadyDead && starterEntry != NULL && !(starterEntry->flags & MP_SL_FLAG_DEAD) && CountLocalAliveMons() <= 1u)
    {
        uint16_t writeIdx = 0;
        uint16_t i;

        starterEntry->linkId = 0;
        sStarterPendingBroadcast = false;
        sStarterBroadcasted = false;
        sRemoteStarterDeadDuringTutorial = false;

        for (i = 0; i < sPendingKillCount; i++)
        {
            bool targetsStarter = sPendingKills[i].fromPreTutorial
                && (KeysEqual(sPendingKills[i].key, starterEntry->key)
                 || sPendingKills[i].linkId == MP_STARTER_LINK_ID
                 || sPendingKills[i].mapsec == MP_STARTER_MAPSEC);
            if (!targetsStarter)
                sPendingKills[writeIdx++] = sPendingKills[i];
        }
        sPendingKillCount = writeIdx;
    }
    else if (sStarterPendingBroadcast && starterEntry != NULL && !(starterEntry->flags & MP_SL_FLAG_DEAD))
    {
        if (starterPartyMon != NULL)
            BroadcastStarterCatchPacket(starterPartyMon, (starterEntry->linkId != 0) ? starterEntry->linkId : MP_STARTER_LINK_ID);
        sStarterBroadcasted = true;
        sStarterPendingBroadcast = false;
    }

    FlushPendingKills();
}

void MpSoulLink_StepFrame(void)
{
#ifdef WASM_PLATFORM
    int pendingLinkId;
    while ((pendingLinkId = WasmMp_PopPendingLinkDeath()) >= 0)
        MpSoulLink_KillByLinkId((uint16_t)pendingLinkId);
#endif
    if (MpSoulLink_AreRulesActive() && sPendingKillCount > 0 && !gMain.inBattle)
        FlushPendingKills();
}

void MpSoulLink_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    (void)fromPeerId;
    MpSoulLink_Init();

    if (opcode == MP_PKT_SL_ROUTE_CATCH && payload != NULL && len >= sizeof(struct MpSoulLinkCatchPayload))
    {
        const struct MpSoulLinkCatchPayload *pkt = (const struct MpSoulLinkCatchPayload *)payload;
        MpSoulLinkRouteState *rs;
        MpSoulLinkEntry *entry;

        if (!MpSoulLink_IsSlotLinkedWithLocal(pkt->slotIndex, pkt->linkGroup))
            return;

        rs = FindOrCreateRouteState(pkt->mapsec, true);
        if (rs != NULL)
        {
            if (rs->linkId == 0 && pkt->linkId != 0)
                rs->linkId = pkt->linkId;
            if (pkt->slotIndex < MP_MAX_PLAYERS)
            {
                rs->slotMask |= (uint8_t)(1u << pkt->slotIndex);
                rs->speciesBySlot[pkt->slotIndex] = pkt->species;
            }
        }

        entry = FindMutableEntryByKey(pkt->key);
        if (entry == NULL && sSoulLinkEntryCount < MP_SOUL_LINK_TABLE_CAP)
        {
            entry = &sSoulLinkEntries[sSoulLinkEntryCount++];
            memset(entry, 0, sizeof(*entry));
            entry->key = pkt->key;
            entry->linkSeqNum = sNextLinkSeq++;
        }
        if (entry != NULL)
        {
            entry->linkId = pkt->linkId;
            entry->originMapsec = pkt->mapsec;
            entry->species = pkt->species;
            entry->originSlot = pkt->slotIndex;
            entry->linkGroup = pkt->linkGroup;
            entry->flags |= MP_SL_FLAG_ACTIVE;
        }
    }
    else if ((opcode == MP_PKT_SL_MON_FAINTED || opcode == MP_PKT_SL_LINK_KILL)
             && payload != NULL && len >= sizeof(struct MpSoulLinkDeathPayload))
    {
        const struct MpSoulLinkDeathPayload *pkt = (const struct MpSoulLinkDeathPayload *)payload;

        if (!MpSoulLink_IsEnabled())
            return;
        if (!MpSoulLink_IsSlotLinkedWithLocal(pkt->slotIndex, pkt->linkGroup))
            return;

        EnqueueOrKillByParams(pkt->key, pkt->linkId, MP_MAPSEC_INVALID, !MpWorldSync_IsLocalTutorialComplete());
    }
    else if (opcode == MP_PKT_SL_ROUTE_FAIL && payload != NULL && len >= 4u)
    {
        const struct MpSoulLinkRouteFailPayload *pkt = (const struct MpSoulLinkRouteFailPayload *)payload;
        uint16_t linkId = (len >= sizeof(struct MpSoulLinkRouteFailPayload)) ? pkt->linkId : 0u;
        MpSoulLinkRouteState *rs;
        struct MpSoulLinkKey emptyKey = {0, 0};

        if (!MpSoulLink_IsEnabled())
            return;
        if (!MpSoulLink_IsSlotLinkedWithLocal(pkt->slotIndex, pkt->linkGroup))
            return;

        rs = FindOrCreateRouteState(pkt->mapsec, true);
        if (rs != NULL)
        {
            rs->routeDead = 1;
            rs->localConsumed = 1;
            if (rs->linkId == 0 && linkId != 0)
                rs->linkId = linkId;
        }

        EnqueueOrKillByParams(emptyKey, linkId, pkt->mapsec, !MpWorldSync_IsLocalTutorialComplete());
    }
}

void MpSoulLink_KillByLinkId(uint16_t linkId)
{
    struct MpSoulLinkKey emptyKey = {0, 0};
    if (linkId == 0 || !MpSoulLink_IsEnabled())
        return;

    EnqueueOrKillByParams(emptyKey, linkId, MP_MAPSEC_INVALID, !MpWorldSync_IsLocalTutorialComplete());
}

void MpSoulLink_KillByUid(uint32_t otId, uint32_t personality)
{
    struct MpSoulLinkKey key;

    MpSoulLink_Init();
    if (!MpSoulLink_IsEnabled())
        return;
    if (otId == 0 && personality == 0)
        return;

    key.otId = otId;
    key.personality = personality;
    EnqueueOrKillByParams(key, 0, MP_MAPSEC_INVALID, !MpWorldSync_IsLocalTutorialComplete());
}

void MpSoulLink_ImportStateJson(const char *jsonPtr)
{
    const char *end;
    const char *objStart;
    const char *objClose;
    const char *valPtr;
    uint32_t nextLinkNum = 0;

    if (jsonPtr == NULL || jsonPtr[0] == '\0')
        return;

    MpSoulLink_Init();
    end = jsonPtr + strlen(jsonPtr);
    objStart = JsonSkipWs(jsonPtr, end);
    if (objStart >= end || *objStart != '{')
        return;
    objClose = JsonFindMatchingClose(objStart, end, '{', '}');
    if (objClose == NULL)
        return;

    if (JsonReadUint32(objStart, objClose + 1, "nextLinkNum", &nextLinkNum))
    {
        if (nextLinkNum > sNextLinkSeq && nextLinkNum < 0xFFFEu)
            sNextLinkSeq = (uint16_t)nextLinkNum;
    }

    valPtr = JsonFindKeyInObject(objStart, objClose + 1, "routes");
    if (valPtr != NULL && *valPtr == '[')
    {
        const char *arrClose = JsonFindMatchingClose(valPtr, objClose + 1, '[', ']');
        if (arrClose != NULL)
        {
            const char *p = JsonSkipWs(valPtr + 1, arrClose);
            while (p < arrClose)
            {
                const char *rClose;
                uint32_t mapsec = MP_MAPSEC_INVALID;
                uint32_t linkId = 0;
                bool dead = false;
                bool localCaught = false;

                p = JsonSkipWs(p, arrClose);
                while (p < arrClose && *p == ',')
                    p = JsonSkipWs(p + 1, arrClose);
                if (p >= arrClose || *p == ']')
                    break;
                if (*p != '{')
                {
                    p = JsonSkipValue(p, arrClose);
                    continue;
                }
                rClose = JsonFindMatchingClose(p, arrClose, '{', '}');
                if (rClose == NULL)
                    break;

                JsonReadUint32(p, rClose + 1, "mapsec", &mapsec);
                JsonReadUint32(p, rClose + 1, "linkId", &linkId);
                JsonReadBool(p, rClose + 1, "dead", &dead);
                JsonReadBool(p, rClose + 1, "localCaught", &localCaught);

                if (mapsec < MP_MAPSEC_INVALID)
                {
                    MpSoulLinkRouteState *rs = FindOrCreateRouteState((uint16_t)mapsec, true);
                    if (rs != NULL)
                    {
                        if (linkId > 0)
                            rs->linkId = (uint16_t)linkId;
                        if (localCaught)
                        {
                            if (rs->localCapturedCount == 0)
                                rs->localCapturedCount = 1;
                            rs->localConsumed = 1;
                        }
                        if (dead)
                        {
                            struct MpSoulLinkKey emptyKey = {0, 0};
                            rs->routeDead = 1;
                            rs->localConsumed = 1;
                            EnqueueOrKillByParams(emptyKey, (uint16_t)linkId, (uint16_t)mapsec, !MpWorldSync_IsLocalTutorialComplete());
                        }
                    }
                }
                p = rClose + 1;
            }
        }
    }

    valPtr = JsonFindKeyInObject(objStart, objClose + 1, "kills");
    if (valPtr != NULL && *valPtr == '[')
    {
        const char *arrClose = JsonFindMatchingClose(valPtr, objClose + 1, '[', ']');
        if (arrClose != NULL)
            ImportSoulLinkEntriesArray(valPtr, arrClose + 1, true);
    }

    valPtr = JsonFindKeyInObject(objStart, objClose + 1, "localLinks");
    if (valPtr != NULL && *valPtr == '[')
    {
        const char *arrClose = JsonFindMatchingClose(valPtr, objClose + 1, '[', ']');
        if (arrClose != NULL)
            ImportSoulLinkEntriesArray(valPtr, arrClose + 1, false);
    }
}
