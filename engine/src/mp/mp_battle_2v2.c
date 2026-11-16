#include "global.h"
#include "battle.h"
#include "battle_controllers.h"
#include "battle_dynamax.h"
#include "battle_gimmick.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "battle_util.h"
#include "battle_z_move.h"
#include "data.h"
#include "event_data.h"
#include "item.h"
#include "main.h"
#include "overworld.h"
#include "pokemon.h"
#include "random.h"
#include "safari_zone.h"
#include "string_util.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/battle_partner.h"
#include "constants/characters.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/opponents.h"
#include "constants/party_menu.h"
#include "constants/species.h"
#ifdef WASM_PLATFORM
#include "wasm_platform.h"
#endif
#include "mp_battle_2v2.h"
#include "mp_overworld.h"
#include "mp_randomizer.h"
#include "mp_soul_link.h"
#include "mp_trade.h"
#include "mp_world_sync.h"

extern void UpdateBattlerPartyOrdersOnSwitch(enum BattlerId battler);
extern u8 GetFirstFaintedPartyIndex(enum BattlerId battler);

#define MP_BAT_MAX_ACTION_QUEUE 16u

#define MP_BUSY_PROMPT_UNDECIDED  0u
#define MP_BUSY_PROMPT_WAIT       1u
#define MP_BUSY_PROMPT_PROCEED_AI 2u

struct MpQueuedTurnAction {
    bool valid;
    struct MpBattleTurnActionPayload pkt;
};

struct MpQueuedSwitchChoice {
    bool valid;
    struct MpBattleSwitchChoicePayload pkt;
};

struct MpBattleSessionState {
    bool active;
    bool initialized;
    bool inBattleCallbackActive;
    bool isInitiator;
    uint8_t mode; /* enum MpBattleMode */
    uint32_t battleSeed;
    uint16_t turnNumber;
    uint16_t trainerId;
    uint16_t lastSyncedTrainerId;
    uint8_t partnerSlot;
    uint16_t partnerPeerId;
    uint8_t localCommitMask;
    uint8_t remoteAppliedMask;
    bool remoteAiAppliedThisTurn;
    bool remotePartyReady;
    uint8_t remotePartyCount;
    uint8_t remotePartyMask;
    struct Pokemon remoteParty[PARTY_SIZE];
    struct MpQueuedTurnAction remoteActions[MP_BAT_MAX_ACTION_QUEUE];
    struct MpQueuedSwitchChoice remoteSwitches[MP_BAT_MAX_ACTION_QUEUE];
    uint16_t switchSeqByBattler[MAX_BATTLERS_COUNT];
    bool pendingLaunch;
    uint32_t pendingSeed;
    uint8_t pendingMode;
    uint16_t pendingTrainerId;
    uint16_t pendingOpponentB;
    bool pendingTwoOpponents;
    uint16_t opponentB;
    bool twoOpponents;
    uint8_t pendingPartnerSlot;
    uint16_t pendingPartnerPeerId;
    uint16_t pendingOverworldWaitFrames;
    uint16_t pendingPartyWaitFrames;
    uint16_t battleStartWaitFrames;
    bool pendingWaitSent;
    bool pendingPartySent;
    bool pendingFromRemote;
    bool remoteDeclinedOrEnded;
    bool remoteBattleEnded;
    uint8_t remoteDeclineSlot;
    uint8_t remoteDeclineCode;
    uint32_t lastCanceledSeed;
    uint16_t lastCanceledSeedTtl;
    uint8_t pvpSavedPlayerPartyCount;
    struct Pokemon pvpSavedPlayerParty[PARTY_SIZE];
    bool pvpPartySaved;
    bool bridgeLaunched;
    bool lastBattleWasPvp;
    bool lastBattleWasBridgeSynced;
    bool pendingCoopFallback;
    bool activeFallbackAiPartner;
    uint64_t seedHash;
    uint32_t randomizerFlags;
    bool hasSeedSync;
    uint64_t pendingSeedHash;
    uint32_t pendingRandomizerFlags;
    bool pendingHasSeedSync;
    bool busyPromptPending;
    uint8_t busyPromptChoice;
    uint8_t busyPromptPartnerSlot;
    uint16_t busyPromptTrainerId;
    uint16_t busyPromptOpponentB;
    bool busyPromptTwoOpponents;
    uint32_t busyPromptBattleSeed;
};

static struct MpBattleSessionState sBattleSession;

static void ResetRemotePartyCache(void)
{
    sBattleSession.remotePartyReady = false;
    sBattleSession.remotePartyCount = 0;
    sBattleSession.remotePartyMask = 0;
    memset(sBattleSession.remoteParty, 0, sizeof(sBattleSession.remoteParty));
}

static void ResetBattleSessionFields(bool preservePostBattleMeta)
{
    bool prevInitialized = sBattleSession.initialized;
    bool prevLastPvp = sBattleSession.lastBattleWasPvp;
    bool prevLastBridge = sBattleSession.lastBattleWasBridgeSynced;
    uint16_t prevLastTrainer = sBattleSession.lastSyncedTrainerId;
    uint32_t prevCanceledSeed = sBattleSession.lastCanceledSeed;
    uint16_t prevCanceledTtl = sBattleSession.lastCanceledSeedTtl;
    bool prevPvpSaved = sBattleSession.pvpPartySaved;
    u8 prevPvpCount = sBattleSession.pvpSavedPlayerPartyCount;

    sBattleSession.active = false;
    sBattleSession.initialized = prevInitialized;
    sBattleSession.inBattleCallbackActive = false;
    sBattleSession.isInitiator = false;
    sBattleSession.mode = MP_BATTLE_SOLO_2V2;
    sBattleSession.battleSeed = 0;
    sBattleSession.turnNumber = 0;
    sBattleSession.trainerId = 0;
    sBattleSession.partnerSlot = 0xFF;
    sBattleSession.partnerPeerId = 0;
    sBattleSession.localCommitMask = 0;
    sBattleSession.remoteAppliedMask = 0;
    sBattleSession.remoteAiAppliedThisTurn = false;
    sBattleSession.pendingLaunch = false;
    sBattleSession.pendingSeed = 0;
    sBattleSession.pendingMode = MP_BATTLE_SOLO_2V2;
    sBattleSession.pendingTrainerId = 0;
    sBattleSession.pendingOpponentB = 0xFFFF;
    sBattleSession.pendingTwoOpponents = false;
    sBattleSession.opponentB = 0xFFFF;
    sBattleSession.twoOpponents = false;
    sBattleSession.pendingPartnerSlot = 0xFF;
    sBattleSession.pendingPartnerPeerId = 0;
    sBattleSession.pendingOverworldWaitFrames = 0;
    sBattleSession.pendingPartyWaitFrames = 0;
    sBattleSession.battleStartWaitFrames = 0;
    sBattleSession.pendingWaitSent = false;
    sBattleSession.pendingPartySent = false;
    sBattleSession.pendingFromRemote = false;
    sBattleSession.remoteDeclinedOrEnded = false;
    sBattleSession.remoteBattleEnded = false;
    sBattleSession.remoteDeclineSlot = 0xFF;
    sBattleSession.remoteDeclineCode = 0;
    sBattleSession.bridgeLaunched = false;
    sBattleSession.pendingCoopFallback = false;
    sBattleSession.activeFallbackAiPartner = false;
    sBattleSession.seedHash = 0;
    sBattleSession.randomizerFlags = 0;
    sBattleSession.hasSeedSync = false;
    sBattleSession.pendingSeedHash = 0;
    sBattleSession.pendingRandomizerFlags = 0;
    sBattleSession.pendingHasSeedSync = false;
    sBattleSession.busyPromptPending = false;
    sBattleSession.busyPromptChoice = MP_BUSY_PROMPT_UNDECIDED;
    sBattleSession.busyPromptPartnerSlot = 0xFF;
    sBattleSession.busyPromptTrainerId = 0;
    sBattleSession.busyPromptOpponentB = 0xFFFF;
    sBattleSession.busyPromptTwoOpponents = false;
    sBattleSession.busyPromptBattleSeed = 0;
    ResetRemotePartyCache();
    memset(sBattleSession.remoteActions, 0, sizeof(sBattleSession.remoteActions));
    memset(sBattleSession.remoteSwitches, 0, sizeof(sBattleSession.remoteSwitches));
    memset(sBattleSession.switchSeqByBattler, 0, sizeof(sBattleSession.switchSeqByBattler));

    if (preservePostBattleMeta)
    {
        sBattleSession.lastBattleWasPvp = prevLastPvp;
        sBattleSession.lastBattleWasBridgeSynced = prevLastBridge;
        sBattleSession.lastSyncedTrainerId = prevLastTrainer;
        sBattleSession.lastCanceledSeed = prevCanceledSeed;
        sBattleSession.lastCanceledSeedTtl = prevCanceledTtl;
    }
    else
    {
        sBattleSession.lastBattleWasPvp = false;
        sBattleSession.lastBattleWasBridgeSynced = false;
        sBattleSession.lastSyncedTrainerId = 0;
        sBattleSession.lastCanceledSeed = 0;
        sBattleSession.lastCanceledSeedTtl = 0;
    }
    sBattleSession.pvpPartySaved = prevPvpSaved;
    sBattleSession.pvpSavedPlayerPartyCount = prevPvpCount;
    gPartnerTrainerId = 0;
}

static void SeedDeterministicBattleRngWithContext(uint32_t seed, uint32_t salt, uint32_t extra)
{
    uint32_t base = (seed != 0) ? seed : 0x5A17B202u;
    uint64_t h1 = MpRand_SplitMix64((uint64_t)base, salt, extra);
    uint64_t h2 = MpRand_SplitMix64((uint64_t)base ^ 0xA5A55A5Au, salt ^ 0x517CC1B7u, extra + 1u);
    uint32_t s1 = (uint32_t)((h1 >> 32) ^ (h1 & 0xFFFFFFFFu));
    uint32_t s2 = (uint32_t)((h2 >> 32) ^ (h2 & 0xFFFFFFFFu));

    if (s1 == 0)
        s1 = 0x5A17B202u;
    if (s2 == 0)
        s2 = 0xA5A55A5Au;
    SeedRng(s1);
    SeedRng2(s2);
}

static void SeedDeterministicBattleRng(uint32_t seed)
{
    SeedDeterministicBattleRngWithContext(seed, 0x42415430u, 0u);
}

void MpBattle2v2_SaveRngState(struct Sfc32State *outRng1, struct Sfc32State *outRng2)
{
    if (outRng1 != NULL)
        *outRng1 = gRngValue;
    if (outRng2 != NULL)
        *outRng2 = gRng2Value;
}

void MpBattle2v2_RestoreRngState(const struct Sfc32State *rng1, const struct Sfc32State *rng2)
{
    if (rng1 != NULL)
        gRngValue = *rng1;
    if (rng2 != NULL)
        gRng2Value = *rng2;
}

void MpBattle2v2_ReseedSyncedBattleRng(uint32_t contextTag)
{
    if (!MpBattle2v2_IsSyncedSession())
        return;
    SeedDeterministicBattleRngWithContext(sBattleSession.battleSeed, contextTag, sBattleSession.turnNumber);
}

static uint16_t FindPartnerPeerId(uint8_t *outSlot)
{
    uint8_t partnerSlot = MpNet_GetCoopPartnerSlot();
    uint16_t partnerPeer = MpNet_GetCoopPartnerPeerId();

    if (partnerSlot != 0xFF && partnerPeer != 0)
    {
        if (outSlot != NULL)
            *outSlot = partnerSlot;
        return partnerPeer;
    }

    /* For direct Friendly PvP without pre-linked pair, pick first connected remote slot */
    for (uint8_t s = 0; s < MP_MAX_PLAYERS; s++)
    {
        const MpPeerState *ps = MpNet_GetPeerState(s);
        if (ps != NULL && ps->connected && s != MpNet_GetLocalSlot())
        {
            if (outSlot != NULL)
                *outSlot = s;
            return ps->peerId;
        }
    }

    if (outSlot != NULL)
        *outSlot = 0xFF;
    return 0;
}

static bool CanLocalPlayerDynamax(void)
{
    if (!CheckBagHasItem(ITEM_DYNAMAX_BAND, 1))
        return false;
    if (B_FLAG_DYNAMAX_BATTLE == 0 || !FlagGet(B_FLAG_DYNAMAX_BATTLE))
        return false;
    return true;
}

static void ApplyDefaultAiGimmickBlock(struct Pokemon *mon)
{
    if (mon == NULL)
        return;
    if (!CanLocalPlayerDynamax())
    {
        u8 dynamaxBlock = BLOCK_AI_DYNAMAX;
        SetMonData(mon, MON_DATA_DYNAMAX_LEVEL, &dynamaxBlock);
    }
}

static void CreateDeterministicRentalMon(struct Pokemon *dst, uint32_t seed, uint32_t slotIdx, u8 level)
{
    static const enum Species sRentalSpecies[4] = {
        SPECIES_TREECKO,
        SPECIES_TORCHIC,
        SPECIES_MUDKIP,
        SPECIES_PIKACHU,
    };
    enum Species sp = sRentalSpecies[(seed + slotIdx) % ARRAY_COUNT(sRentalSpecies)];
    u32 pers = (uint32_t)(MpRand_SplitMix64(seed, 0x52454E54u, slotIdx) & 0xFFFFFFFFu);

    if (level < 5)
        level = 5;

    MpRandomizer_PushSuppressStatic();
    CreateMonWithIVs(dst, sp, level, pers, OTID_STRUCT_PRESET(0x2026u + slotIdx), 16);
    GiveMonInitialMoveset(dst);
    CalculateMonStats(dst);
    ApplyDefaultAiGimmickBlock(dst);
    MpRandomizer_PopSuppressStatic();
}

static u8 GetHighestLocalPartyLevel(void)
{
    u8 maxLvl = 5;
    u32 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        enum Species sp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG)
        {
            u8 lvl = (u8)GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_LEVEL);
            if (lvl > maxLvl)
                maxLvl = lvl;
        }
    }
    return maxLvl;
}

static const MpPeerState *GetActivePvpOpponentPeer(void)
{
    uint8_t s;
    const MpPeerState *fallback = NULL;

    if (sBattleSession.partnerSlot < MP_MAX_PLAYERS)
    {
        const MpPeerState *ps = MpNet_GetPeerState(sBattleSession.partnerSlot);
        if (ps != NULL && ps->connected && sBattleSession.partnerSlot != MpNet_GetLocalSlot())
            return ps;
    }

    for (s = 0; s < MP_MAX_PLAYERS; s++)
    {
        const MpPeerState *ps = MpNet_GetPeerState(s);
        if (ps != NULL && ps->connected && s != MpNet_GetLocalSlot())
        {
            if (sBattleSession.partnerPeerId != 0 && ps->peerId == sBattleSession.partnerPeerId)
                return ps;
            if (fallback == NULL)
                fallback = ps;
        }
    }
    return fallback;
}

static void SendLocalPartyChunks(uint8_t mode, uint32_t battleSeed, uint16_t targetPeerId)
{
    u8 total = CalculatePlayerPartyCount();
    u8 sendTotal = (mode == MP_BATTLE_PVP && total < 2) ? 2 : total;
    u8 baseLevel = GetHighestLocalPartyLevel();
    uint32_t localRentalSeed = battleSeed ^ ((uint32_t)MpNet_GetLocalSlot() * 0x9E3779B9u);
    u8 i;

    if (sendTotal == 0)
        return;

    for (i = 0; i < sendTotal && i < PARTY_SIZE; i++)
    {
        struct MpBattlePartyMonPayload pkt;
        struct Pokemon tempRentalMon;
        struct Pokemon *srcMon = &gParties[B_TRAINER_PLAYER][i];

        if (i >= total)
        {
            CreateDeterministicRentalMon(&tempRentalMon, localRentalSeed, i, baseLevel);
            srcMon = &tempRentalMon;
        }

        memset(&pkt, 0, sizeof(pkt));
        pkt.battleSeed = battleSeed;
        pkt.slotIndex = MpNet_GetLocalSlot();
        pkt.monIndex = i;
        pkt.totalMons = sendTotal;
        pkt.mode = mode;
        if (MpTrade_SerializePokemon(srcMon, pkt.monBytes))
            MpNet_SendBinary(MP_CH_BATTLE, targetPeerId, MP_PKT_BAT_PARTY_MON, &pkt, (uint16_t)sizeof(pkt));
    }
}

static void CopyRemotePartyToTrainer(enum BattleTrainer dstTrainer)
{
    struct Pokemon *dstParty = &gParties[dstTrainer][0];
    u32 count = 0;
    u32 i;
    u8 baseLevel = GetHighestLocalPartyLevel();
    uint32_t oppSeed = sBattleSession.battleSeed ^ ((uint32_t)((sBattleSession.partnerSlot != 0xFF) ? sBattleSession.partnerSlot : 1u) * 0x9E3779B9u);

    ZeroPartyMons(dstParty);
    for (i = 0; i < sBattleSession.remotePartyCount && i < PARTY_SIZE; i++)
    {
        enum Species sp = GetMonData(&sBattleSession.remoteParty[i], MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG)
            dstParty[count++] = sBattleSession.remoteParty[i];
    }

    if (sBattleSession.mode == MP_BATTLE_PVP && dstTrainer == B_TRAINER_OPPONENT_A)
    {
        while (count < 2)
        {
            CreateDeterministicRentalMon(&dstParty[count], oppSeed, count, baseLevel);
            count++;
        }
    }

    gPartiesCount[dstTrainer] = CalculatePartyCount(dstTrainer);
}

static bool IsReliableDamagingMove(enum Move move)
{
    u32 acc;

    if (move == MOVE_NONE || move == MOVE_UNAVAILABLE)
        return false;
    if (GetMoveCategory(move) == DAMAGE_CATEGORY_STATUS)
        return false;
    if (GetMovePower(move) == 0)
        return false;
    acc = GetMoveAccuracy(move);
    return (acc == 0 || acc >= 85u);
}

static enum Move FallbackReliableMoveForType(enum Type type)
{
    switch (type)
    {
    case TYPE_FIGHTING: return MOVE_KARATE_CHOP;
    case TYPE_FLYING:   return MOVE_GUST;
    case TYPE_POISON:   return MOVE_POISON_STING;
    case TYPE_GROUND:   return MOVE_MUD_SLAP;
    case TYPE_ROCK:     return MOVE_ROCK_THROW;
    case TYPE_BUG:      return MOVE_FURY_CUTTER;
    case TYPE_GHOST:    return MOVE_LICK;
    case TYPE_STEEL:    return MOVE_METAL_CLAW;
    case TYPE_FIRE:     return MOVE_EMBER;
    case TYPE_WATER:    return MOVE_WATER_GUN;
    case TYPE_GRASS:    return MOVE_VINE_WHIP;
    case TYPE_ELECTRIC: return MOVE_THUNDER_SHOCK;
    case TYPE_PSYCHIC:  return MOVE_CONFUSION;
    case TYPE_ICE:      return MOVE_POWDER_SNOW;
    case TYPE_DRAGON:   return MOVE_DRAGON_BREATH;
    case TYPE_DARK:     return MOVE_BITE;
    case TYPE_FAIRY:    return MOVE_FAIRY_WIND;
    default:            return MOVE_TACKLE;
    }
}

void MpBattle2v2_EnsureReliableMove(struct Pokemon *mon)
{
    enum Species species;
    u8 level;
    enum Type type0;
    enum Type type1;
    u8 emptySlot = MAX_MON_MOVES;
    enum Move bestLvlStab = MOVE_NONE;
    enum Move bestLvlOther = MOVE_NONE;
    enum Move bestTmStab = MOVE_NONE;
    enum Move bestTmOther = MOVE_NONE;
    enum Move chosenMove = MOVE_NONE;
    u32 bestLvlStabPower = 0;
    u32 bestLvlOtherPower = 0;
    u32 bestTmStabPower = 0;
    const struct LevelUpMove *learnset;
    const u16 *teachable;
    u32 i;

    if (mon == NULL)
        return;

    species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
    if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES)
        return;

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        enum Move m = (enum Move)GetMonData(mon, MON_DATA_MOVE1 + i);
        if (m == MOVE_NONE && emptySlot == MAX_MON_MOVES)
            emptySlot = (u8)i;
        if (IsReliableDamagingMove(m))
            return;
    }

    level = (u8)GetMonData(mon, MON_DATA_LEVEL);
    type0 = GetSpeciesType(species, 0);
    type1 = GetSpeciesType(species, 1);

    learnset = GetSpeciesLevelUpLearnset(species);
    if (learnset != NULL)
    {
        for (i = 0; learnset[i].move != LEVEL_UP_MOVE_END; i++)
        {
            enum Move m = learnset[i].move;
            u32 pwr;
            bool isStab;

            if (learnset[i].level > level)
                break;
            if (learnset[i].level == 0 || !IsReliableDamagingMove(m))
                continue;
            pwr = GetMovePower(m);
            isStab = (GetMoveType(m) == type0 || GetMoveType(m) == type1);
            if (isStab && pwr >= bestLvlStabPower)
            {
                bestLvlStab = m;
                bestLvlStabPower = pwr;
            }
            else if (!isStab && pwr >= bestLvlOtherPower)
            {
                bestLvlOther = m;
                bestLvlOtherPower = pwr;
            }
        }
    }

    teachable = GetSpeciesTeachableLearnset(species);
    if (teachable != NULL)
    {
        for (i = 0; teachable[i] != MOVE_UNAVAILABLE; i++)
        {
            enum Move m = (enum Move)teachable[i];
            u32 pwr;
            bool isStab;

            if (!IsReliableDamagingMove(m))
                continue;
            pwr = GetMovePower(m);
            if (pwr > 85u)
                continue;
            isStab = (GetMoveType(m) == type0 || GetMoveType(m) == type1);
            if (isStab && pwr >= bestTmStabPower)
            {
                bestTmStab = m;
                bestTmStabPower = pwr;
            }
            else if (!isStab && bestTmOther == MOVE_NONE)
            {
                bestTmOther = m;
            }
        }
    }

    if (bestLvlStab != MOVE_NONE)
        chosenMove = bestLvlStab;
    else if (bestTmStab != MOVE_NONE)
        chosenMove = bestTmStab;
    else if (bestLvlOther != MOVE_NONE)
        chosenMove = bestLvlOther;
    else if (bestTmOther != MOVE_NONE)
        chosenMove = bestTmOther;
    else
        chosenMove = FallbackReliableMoveForType(type0);

    {
        u8 targetSlot = (emptySlot < MAX_MON_MOVES) ? emptySlot : (MAX_MON_MOVES - 1);
        u32 pp = GetMovePP(chosenMove);
        SetMonData(mon, MON_DATA_MOVE1 + targetSlot, &chosenMove);
        SetMonData(mon, MON_DATA_PP1 + targetSlot, &pp);
    }
}

static void PopulateFallbackPartnerOrPvpParty(enum BattleTrainer dstTrainer, uint32_t seed)
{
    struct Pokemon *dstParty = &gParties[dstTrainer][0];
    u32 count = 0;
    u32 i;
    u8 baseLevel = GetHighestLocalPartyLevel();

    if (sBattleSession.active && sBattleSession.mode != MP_BATTLE_SOLO_2V2 && sBattleSession.remotePartyReady && sBattleSession.remotePartyCount > 0)
    {
        CopyRemotePartyToTrainer(dstTrainer);
        return;
    }

    if (dstTrainer == B_TRAINER_PARTNER)
    {
        static const enum Species sPartnerBackupSpecies[4] = {
            SPECIES_TREECKO,
            SPECIES_TORCHIC,
            SPECIES_MUDKIP,
            SPECIES_PIKACHU,
        };
        enum Species playerLeadSpecies = SPECIES_NONE;
        enum Species firstAnySpecies = SPECIES_NONE;
        u8 highestAliveLvl = 5;
        u8 highestAnyLvl = 5;
        u32 sumAliveLvl = 0;
        u32 aliveCount = 0;
        u32 locSalt;
        uint32_t allySeed;
        u16 excludeSp;
        u16 chosen[2] = { SPECIES_NONE, SPECIES_NONE };
        u8 allyLevel;

        for (i = 0; i < PARTY_SIZE; i++)
        {
            enum Species sp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES_OR_EGG);
            u8 lvl;
            if (sp == SPECIES_NONE || sp == SPECIES_EGG)
                continue;
            lvl = (u8)GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_LEVEL);
            if (firstAnySpecies == SPECIES_NONE)
                firstAnySpecies = sp;
            if (lvl > highestAnyLvl)
                highestAnyLvl = lvl;
            if (GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_HP) > 0)
            {
                if (playerLeadSpecies == SPECIES_NONE)
                    playerLeadSpecies = sp;
                if (lvl > highestAliveLvl)
                    highestAliveLvl = lvl;
                sumAliveLvl += lvl;
                aliveCount++;
            }
        }

        if (playerLeadSpecies == SPECIES_NONE)
            playerLeadSpecies = firstAnySpecies;
        if (aliveCount > 0)
        {
            u8 avgLvl = (u8)(sumAliveLvl / aliveCount);
            allyLevel = (u8)((((u32)highestAliveLvl + (u32)avgLvl) + 1u) / 2u);
        }
        else
        {
            allyLevel = highestAnyLvl;
        }
        if (allyLevel < 5)
            allyLevel = 5;
        if (allyLevel > MAX_LEVEL)
            allyLevel = MAX_LEVEL;

        locSalt = (gSaveBlock1Ptr != NULL)
            ? (((uint32_t)gSaveBlock1Ptr->location.mapGroup << 24)
             | ((uint32_t)gSaveBlock1Ptr->location.mapNum << 16)
             | (uint32_t)GetCurrentRegionMapSectionId())
            : 0u;
        allySeed = seed ^ locSalt ^ ((uint32_t)TRAINER_BATTLE_PARAM.opponentA << 8) ^ (uint32_t)gMain.vblankCounter1;
        if (allySeed == 0)
            allySeed = 0x414C4C59u;

        ZeroPartyMons(dstParty);
        excludeSp = (playerLeadSpecies != SPECIES_NONE && playerLeadSpecies != SPECIES_EGG)
                  ? (u16)playerLeadSpecies
                  : (u16)SPECIES_NONE;

        for (i = 0; i < 2; i++)
        {
            u16 sp = MpRandomizer_GetPartnerSpecies(allySeed, (u8)i, excludeSp, allyLevel);
            u32 pers;

            if (sp == SPECIES_NONE || sp >= NUM_SPECIES || sp == excludeSp)
            {
                u32 k;
                sp = (u16)sPartnerBackupSpecies[(allySeed + i) % ARRAY_COUNT(sPartnerBackupSpecies)];
                for (k = 0; k < ARRAY_COUNT(sPartnerBackupSpecies); k++)
                {
                    if (sp != excludeSp && !(i == 1 && sp == chosen[0]))
                        break;
                    sp = (u16)sPartnerBackupSpecies[(allySeed + i + k + 1u) % ARRAY_COUNT(sPartnerBackupSpecies)];
                }
            }

            chosen[i] = sp;
            excludeSp = sp;
            pers = (uint32_t)(MpRand_SplitMix64(allySeed, 0x414C4C59u, i) & 0xFFFFFFFFu);

            MpRandomizer_PushSuppressStatic();
            CreateMonWithIVs(&dstParty[i], (enum Species)sp, allyLevel, pers, OTID_STRUCT_PRESET(0x1337u + i), 16);
            GiveMonInitialMoveset(&dstParty[i]);
            MpBattle2v2_EnsureReliableMove(&dstParty[i]);
            CalculateMonStats(&dstParty[i]);
            ApplyDefaultAiGimmickBlock(&dstParty[i]);
            MpRandomizer_PopSuppressStatic();
        }
        gPartiesCount[dstTrainer] = CalculatePartyCount(dstTrainer);
        return;
    }

    struct Pokemon *firstAlive = &gParties[B_TRAINER_PLAYER][0];
    for (i = 0; i < PARTY_SIZE; i++)
    {
        enum Species sp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_HP) > 0)
        {
            firstAlive = &gParties[B_TRAINER_PLAYER][i];
            break;
        }
    }

    ZeroPartyMons(dstParty);
    for (i = 0; i < 2 && i < PARTY_SIZE; i++)
    {
        struct Pokemon *src = &gParties[B_TRAINER_PLAYER][i];
        enum Species sp = GetMonData(src, MON_DATA_SPECIES_OR_EGG);
        if (sp == SPECIES_NONE || sp == SPECIES_EGG)
            src = firstAlive;
        sp = GetMonData(src, MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG)
        {
            u8 lvl = (u8)GetMonData(src, MON_DATA_LEVEL);
            u32 pers = (u32)(MpRand_SplitMix64(seed, 0x50565032u, i) & 0xFFFFFFFFu);
            MpRandomizer_PushSuppressStatic();
            CreateMonWithIVs(&dstParty[count], sp, lvl, pers, OTID_STRUCT_PRESET(0x1337u + i), 16);
            GiveMonInitialMoveset(&dstParty[count]);
            CalculateMonStats(&dstParty[count]);
            ApplyDefaultAiGimmickBlock(&dstParty[count]);
            MpRandomizer_PopSuppressStatic();
            count++;
        }
    }

    while (count < 2)
    {
        CreateDeterministicRentalMon(&dstParty[count], seed ^ 0x4F505032u, count, baseLevel);
        count++;
    }
    gPartiesCount[dstTrainer] = CalculatePartyCount(dstTrainer);
}

static void PopulateDeterministicEnemyParty(struct Pokemon *dstParty, uint32_t seed)
{
    u8 maxLvl = 5;
    u32 i;

    ZeroPartyMons(dstParty);
    for (i = 0; i < PARTY_SIZE; i++)
    {
        enum Species pSp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES_OR_EGG);
        if (pSp != SPECIES_NONE && pSp != SPECIES_EGG)
        {
            u8 lvl = (u8)GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_LEVEL);
            if (lvl > maxLvl)
                maxLvl = lvl;
        }
    }

    for (i = 0; i < 2; i++)
    {
        enum Species fallbackSp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES);
        uint16_t sp;
        u32 pers;
        if (fallbackSp == SPECIES_NONE || fallbackSp == SPECIES_EGG)
            fallbackSp = GetMonData(&gParties[B_TRAINER_PLAYER][0], MON_DATA_SPECIES);
        if (fallbackSp == SPECIES_NONE || fallbackSp == SPECIES_EGG)
            fallbackSp = (enum Species)1;
        sp = MpRandomizer_GetTrainerSpecies((uint16_t)(0xFF00u + i), (uint8_t)i, 0, (uint16_t)fallbackSp);
        if (sp == SPECIES_NONE || sp >= NUM_SPECIES)
            sp = (uint16_t)fallbackSp;
        pers = (uint32_t)(MpRand_SplitMix64(seed, 0x4E504332u, i) & 0xFFFFFFFFu);
        MpRandomizer_PushSuppressStatic();
        CreateMonWithIVs(&dstParty[i], (enum Species)sp, maxLvl, pers, OTID_STRUCT_PRESET(0x4E50u + i), 14);
        GiveMonInitialMoveset(&dstParty[i]);
        CalculateMonStats(&dstParty[i]);
        ApplyDefaultAiGimmickBlock(&dstParty[i]);
        MpRandomizer_PopSuppressStatic();
    }
}

static void SendBattleStartPacket(uint8_t mode, uint32_t seed, uint16_t trainerId,
                                  uint16_t opponentB, bool twoOpponents, uint16_t targetPeer)
{
    struct MpBattleStartPayload startPkt;
    memset(&startPkt, 0, sizeof(startPkt));
    startPkt.battleSeed = seed;
    startPkt.mode = mode;
    startPkt.partnerSlot = MpNet_GetLocalSlot();
    startPkt.partnerPeerId = MpNet_GetLocalPeerId();
    startPkt.trainerId = trainerId;
    startPkt.species[0] = opponentB;
    startPkt.levels[0] = twoOpponents ? 1u : 0u;
    startPkt.seedHash = MpRandomizer_GetRunSeed();
    startPkt.randomizerFlags = MpRandomizer_GetFlags();
    MpNet_SendBinary(MP_CH_BATTLE, targetPeer, MP_PKT_BAT_START, &startPkt, (uint16_t)sizeof(startPkt));
    SendLocalPartyChunks(mode, seed, targetPeer);
}

static uint8_t BusyReasonToCode(const char *reason)
{
    if (reason == NULL)
        return 0;
    if (strcmp(reason, "in_battle") == 0)
        return 1;
    if (strcmp(reason, "in_tutorial") == 0)
        return 2;
    if (strcmp(reason, "not_ready") == 0)
        return 3;
    if (strcmp(reason, "different_map") == 0)
        return 4;
    return 0;
}

static const char *BusyCodeToReason(uint8_t code)
{
    switch (code)
    {
    case 1: return "in_battle";
    case 2: return "in_tutorial";
    case 3: return "not_ready";
    case 4: return "different_map";
    default: return "busy";
    }
}

static void SendDeclineBusy(uint16_t targetPeer, const char *reason)
{
    uint8_t pkt[2];
    pkt[0] = MpNet_GetLocalSlot();
    pkt[1] = BusyReasonToCode(reason);
    MpNet_SendBinary(MP_CH_BATTLE, targetPeer, MP_PKT_BAT_DECLINE_BUSY, pkt, 2);
}

static void AbortPendingRemoteLaunch(uint16_t targetPeer, uint32_t launchSeed, const char *reason)
{
    uint16_t declinePeer = (targetPeer != 0) ? targetPeer : MP_PEER_BROADCAST;
    SendDeclineBusy(declinePeer, reason);
    ResetBattleSessionFields(false);
    sBattleSession.lastCanceledSeed = launchSeed;
    sBattleSession.lastCanceledSeedTtl = 180u;
}

void MpBattle2v2_VerifySyncedSeedBeforeTrainerBuild(void)
{
    uint64_t syncHash = 0;
    uint32_t syncFlags = 0;
    MpRunConfig *cfg;

    if (sBattleSession.active && sBattleSession.hasSeedSync && sBattleSession.seedHash != 0)
    {
        syncHash = sBattleSession.seedHash;
        syncFlags = sBattleSession.randomizerFlags;
    }
    else if (sBattleSession.pendingLaunch && sBattleSession.pendingHasSeedSync && sBattleSession.pendingSeedHash != 0)
    {
        syncHash = sBattleSession.pendingSeedHash;
        syncFlags = sBattleSession.pendingRandomizerFlags;
    }
    else
    {
        return;
    }

    cfg = MpNet_GetRunConfigMutable();
    if (cfg != NULL)
    {
        cfg->seedHash = syncHash;
        cfg->randomizerFlags = syncFlags;
        MpRandomizer_OnRunConfigChanged(cfg);
    }
}

static void AttachFallbackAiPartnerFlags(uint32_t *battleTypeFlags, uint32_t seed);

static void LaunchCoop2v2SessionFromTrainer(uint32_t seed, uint16_t trainerId,
                                           uint16_t opponentB, bool twoOpponents,
                                           uint8_t coopPartner, uint16_t partnerPeer,
                                           uint32_t *battleTypeFlags)
{
    ResetBattleSessionFields(false);
    sBattleSession.active = true;
    sBattleSession.isInitiator = true;
    sBattleSession.mode = MP_BATTLE_COOP_2V2;
    sBattleSession.battleSeed = seed;
    sBattleSession.trainerId = trainerId;
    sBattleSession.twoOpponents = twoOpponents;
    sBattleSession.opponentB = opponentB;
    sBattleSession.partnerSlot = coopPartner;
    sBattleSession.partnerPeerId = partnerPeer;
    sBattleSession.seedHash = MpRandomizer_GetRunSeed();
    sBattleSession.randomizerFlags = MpRandomizer_GetFlags();
    sBattleSession.hasSeedSync = true;

    SeedDeterministicBattleRng(seed);
    PopulateFallbackPartnerOrPvpParty(B_TRAINER_PARTNER, seed);
    if (!twoOpponents)
        TRAINER_BATTLE_PARAM.opponentB = 0xFFFF;
    if (gPartnerTrainerId == 0 || gPartnerTrainerId == TRAINER_PARTNER(PARTNER_NONE))
        gPartnerTrainerId = TRAINER_PARTNER(PARTNER_STEVEN);
    if (battleTypeFlags != NULL)
        *battleTypeFlags |= BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER;

    SendBattleStartPacket(MP_BATTLE_COOP_2V2, seed, trainerId, opponentB, twoOpponents, partnerPeer);
}

static uint8_t FindCoopTargetSlot(void)
{
    uint8_t local = MpNet_GetLocalSlot();
    const MpPeerState *localPeer = (local < MP_MAX_PLAYERS) ? MpNet_GetPeerState(local) : NULL;
    uint8_t localTag = (localPeer != NULL) ? localPeer->partyPairTag : 0u;
    uint8_t explicitPair = MpNet_FindExplicitPairSlot();
    const MpRunConfig *cfg;
    uint8_t coopPartner;

    if (explicitPair != 0xFFu)
        return explicitPair;
    if (localTag != 0u)
        return 0xFFu;

    coopPartner = MpNet_GetCoopPartnerSlot();
    if (coopPartner != 0xFFu)
        return coopPartner;

    /* When partyPairTag == 0 (default room state), include default unpaired co-op
     * partner even while in_tutorial or not_ready so Wait/AI prompt still triggers. */
    cfg = MpNet_GetRunConfig();
    if (cfg != NULL && cfg->soulLinkMode == MP_SOULLINK_PAIRS)
    {
        uint8_t paired = local ^ 1u;
        if (paired < MP_MAX_PLAYERS)
        {
            const MpPeerState *ps = MpNet_GetPeerState(paired);
            if (ps != NULL && ps->connected && ps->partyPairTag == 0u)
                return paired;
        }
    }

    for (uint8_t i = 0; i < MP_MAX_PLAYERS; i++)
    {
        const MpPeerState *ps = MpNet_GetPeerState(i);
        if (i != local && ps != NULL && ps->connected && ps->partyPairTag == 0u)
            return i;
    }

    return 0xFFu;
}

static void PollBusyTrainerPrompt(void)
{
    uint8_t targetPartner;
    uint8_t coopPartner;
    const MpPeerState *ps;
    const char *busyReason;
    uint16_t partnerPeer;

    if (!sBattleSession.busyPromptPending)
        return;

    targetPartner = FindCoopTargetSlot();
    if (targetPartner == 0xFF)
    {
        uint32_t seed = (sBattleSession.busyPromptBattleSeed != 0) ? sBattleSession.busyPromptBattleSeed : 0x20260401u;
        uint8_t prevSlot = sBattleSession.busyPromptPartnerSlot;

        sBattleSession.busyPromptPending = false;
        sBattleSession.busyPromptChoice = MP_BUSY_PROMPT_UNDECIDED;
        SeedDeterministicBattleRng(seed);
        AttachFallbackAiPartnerFlags(&gBattleTypeFlags, seed);
#ifdef WASM_PLATFORM
        if (prevSlot < MP_MAX_PLAYERS)
            WasmMp_NotifyTeammateBusy((int)prevSlot, "not_ready");
        WasmMp_NotifyBusyPromptResolved();
#endif
        return;
    }

    coopPartner = MpNet_GetCoopPartnerSlot();
    ps = MpNet_GetPeerState(targetPartner);
    busyReason = MpOverworld_GetPeerBusyReason(targetPartner);
    partnerPeer = (ps != NULL && ps->connected) ? ps->peerId : 0;

    if (busyReason == NULL && coopPartner == targetPartner && partnerPeer != 0)
    {
        uint32_t seed = (sBattleSession.busyPromptBattleSeed != 0) ? sBattleSession.busyPromptBattleSeed : 0x20260401u;
        uint16_t trainerId = sBattleSession.busyPromptTrainerId;
        uint16_t opponentB = sBattleSession.busyPromptOpponentB;
        bool twoOpponents = sBattleSession.busyPromptTwoOpponents;

        LaunchCoop2v2SessionFromTrainer(seed, trainerId, opponentB, twoOpponents,
                                        coopPartner, partnerPeer, &gBattleTypeFlags);
#ifdef WASM_PLATFORM
        WasmMp_NotifyBusyPromptResolved();
#endif
    }
}

bool MpBattle2v2_WaitForBusyTrainerPrompt(void)
{
    PollBusyTrainerPrompt();
    return !sBattleSession.busyPromptPending;
}

void MpBattle2v2_ResolveBusyPrompt(int choice)
{
    uint32_t seed;

    if (!sBattleSession.busyPromptPending)
        return;
    if (choice == 0)
    {
        sBattleSession.busyPromptChoice = MP_BUSY_PROMPT_WAIT;
        return;
    }

    seed = (sBattleSession.busyPromptBattleSeed != 0) ? sBattleSession.busyPromptBattleSeed : 0x20260401u;
    sBattleSession.busyPromptPending = false;
    sBattleSession.busyPromptChoice = MP_BUSY_PROMPT_UNDECIDED;
    SeedDeterministicBattleRng(seed);
    AttachFallbackAiPartnerFlags(&gBattleTypeFlags, seed);
#ifdef WASM_PLATFORM
    WasmMp_NotifyBusyPromptResolved();
#endif
}

void MpBattle2v2_Init(void)
{
    if (sBattleSession.initialized)
        return;
    memset(&sBattleSession, 0, sizeof(sBattleSession));
    sBattleSession.initialized = true;
    ResetBattleSessionFields(false);
    MpNet_RegisterHandler(MP_CH_BATTLE, MpBattle2v2_HandlePacket);
}

void MpBattle2v2_StepFrame(void)
{
#ifdef WASM_PLATFORM
    uint32_t seed = 0;
    int mode = 0;
    int partnerSlot = 0xFF;
    int partnerPeerId = 0;
    MpBattle2v2_Init();
    if (WasmMp_ConsumePendingBattle(&seed, &mode, &partnerSlot, &partnerPeerId))
        MpBattle2v2_StartSyncedFromBridge(seed, (uint8_t)mode, (uint8_t)partnerSlot, (uint16_t)partnerPeerId);
#endif
    if (sBattleSession.lastCanceledSeedTtl > 0)
    {
        sBattleSession.lastCanceledSeedTtl--;
        if (sBattleSession.lastCanceledSeedTtl == 0)
            sBattleSession.lastCanceledSeed = 0;
    }

    PollBusyTrainerPrompt();
    if (sBattleSession.active && !sBattleSession.isInitiator && sBattleSession.hasSeedSync && sBattleSession.seedHash != 0)
        MpBattle2v2_VerifySyncedSeedBeforeTrainerBuild();

    if (sBattleSession.pendingLaunch)
    {
        uint32_t launchSeed = sBattleSession.pendingSeed;
        uint8_t launchMode = sBattleSession.pendingMode;
        uint16_t launchTrainerId = sBattleSession.pendingTrainerId;
        uint16_t targetPeer = sBattleSession.pendingPartnerPeerId;
        if (targetPeer == 0 && launchMode != MP_BATTLE_SOLO_2V2)
        {
            if (sBattleSession.pendingPartnerSlot < MP_MAX_PLAYERS)
            {
                const MpPeerState *ps = MpNet_GetPeerState(sBattleSession.pendingPartnerSlot);
                if (ps != NULL && ps->connected)
                    targetPeer = (ps->peerId != 0) ? ps->peerId : (uint16_t)(sBattleSession.pendingPartnerSlot + 1u);
            }
            if (targetPeer == 0)
            {
                uint8_t resolvedSlot = 0xFF;
                targetPeer = FindPartnerPeerId(&resolvedSlot);
                if (sBattleSession.pendingPartnerSlot >= MP_MAX_PLAYERS && resolvedSlot < MP_MAX_PLAYERS)
                    sBattleSession.pendingPartnerSlot = resolvedSlot;
            }
            if (targetPeer != 0)
                sBattleSession.pendingPartnerPeerId = targetPeer;
        }
        bool needsRemoteParty = (launchMode == MP_BATTLE_COOP_2V2 || launchMode == MP_BATTLE_PVP) && (targetPeer != 0);

        if (gMain.inBattle || sBattleSession.active || sBattleSession.activeFallbackAiPartner || sBattleSession.busyPromptPending)
        {
            if (sBattleSession.pendingFromRemote && (launchMode == MP_BATTLE_COOP_2V2 || launchMode == MP_BATTLE_PVP))
                SendDeclineBusy((targetPeer != 0) ? targetPeer : MP_PEER_BROADCAST, "in_battle");
            if (!sBattleSession.active && !sBattleSession.busyPromptPending)
                ResetBattleSessionFields(false);
            else
            {
                sBattleSession.pendingLaunch = false;
                sBattleSession.pendingFromRemote = false;
                sBattleSession.pendingWaitSent = false;
                sBattleSession.pendingPartySent = false;
                sBattleSession.pendingOverworldWaitFrames = 0;
                sBattleSession.pendingPartyWaitFrames = 0;
                sBattleSession.pendingPartnerSlot = 0xFF;
                sBattleSession.pendingPartnerPeerId = 0;
                sBattleSession.pendingSeed = 0;
            }
            sBattleSession.lastCanceledSeed = launchSeed;
            sBattleSession.lastCanceledSeedTtl = 180u;
            return;
        }

        if (sBattleSession.pendingFromRemote && !MpWorldSync_IsLocalTutorialComplete())
        {
            AbortPendingRemoteLaunch(targetPeer, launchSeed, "in_tutorial");
            return;
        }

        if (sBattleSession.pendingFromRemote && !MpOverworld_CanJoinSyncedBattleNow())
        {
            MpOverworld_ForceClearFieldForSyncedBattle();
            if (!MpOverworld_CanJoinSyncedBattleNow())
            {
                if (gMain.inBattle || sBattleSession.pendingOverworldWaitFrames >= 420u)
                {
                    const char *reason = gMain.inBattle ? "in_battle" : (!MpWorldSync_IsLocalTutorialComplete() ? "in_tutorial" : "busy");
                    AbortPendingRemoteLaunch(targetPeer, launchSeed, reason);
                    return;
                }
                sBattleSession.pendingOverworldWaitFrames++;
                return;
            }
        }

        if (sBattleSession.pendingFromRemote && needsRemoteParty && !sBattleSession.pendingPartySent)
        {
            sBattleSession.pendingPartySent = true;
            SendLocalPartyChunks(launchMode, launchSeed, targetPeer);
        }
        else if (!sBattleSession.pendingFromRemote && needsRemoteParty && !sBattleSession.pendingWaitSent)
        {
            sBattleSession.pendingWaitSent = true;
            SendBattleStartPacket(launchMode, launchSeed, launchTrainerId,
                                  sBattleSession.pendingOpponentB, sBattleSession.pendingTwoOpponents, targetPeer);
        }

        if (!needsRemoteParty || sBattleSession.remotePartyReady)
        {
            sBattleSession.pendingLaunch = false;
            MpBattle2v2_StartSyncedBattle(launchSeed, launchMode, launchTrainerId);
        }
        else if (sBattleSession.pendingPartyWaitFrames >= 420u)
        {
            if (sBattleSession.pendingFromRemote)
            {
                AbortPendingRemoteLaunch(targetPeer, launchSeed, "not_ready");
                return;
            }
            {
                uint8_t notifySlot = sBattleSession.pendingPartnerSlot;
                if (targetPeer != 0)
                {
                    uint8_t cancelOutcome = 0;
                    MpNet_SendBinary(MP_CH_BATTLE, targetPeer, MP_PKT_BAT_END, &cancelOutcome, 1);
                }
#ifdef WASM_PLATFORM
                if (notifySlot < MP_MAX_PLAYERS)
                    WasmMp_NotifyTeammateBusy((int)notifySlot, "busy");
#endif
                sBattleSession.lastCanceledSeed = launchSeed;
                sBattleSession.lastCanceledSeedTtl = 180u;
                if (launchMode == MP_BATTLE_COOP_2V2)
                {
                    sBattleSession.pendingLaunch = false;
                    sBattleSession.pendingCoopFallback = true;
                    sBattleSession.pendingPartnerSlot = 0xFF;
                    sBattleSession.pendingPartnerPeerId = 0;
                    MpBattle2v2_StartSyncedBattle(launchSeed, MP_BATTLE_SOLO_2V2, launchTrainerId);
                }
                else
                {
                    ResetBattleSessionFields(false);
                }
            }
        }
        else
        {
            if (sBattleSession.pendingPartyWaitFrames > 0 && (sBattleSession.pendingPartyWaitFrames % 20u) == 0u)
            {
                if (!sBattleSession.pendingFromRemote)
                    SendBattleStartPacket(launchMode, launchSeed, launchTrainerId,
                                          sBattleSession.pendingOpponentB, sBattleSession.pendingTwoOpponents, targetPeer);
                else
                    SendLocalPartyChunks(launchMode, launchSeed, targetPeer);
            }
            sBattleSession.pendingPartyWaitFrames++;
        }
    }
}

static bool IsFirstRivalTutorialBattle(uint16_t trainerId)
{
    if (TRAINER_BATTLE_PARAM.earlyRival)
        return true;
    switch (trainerId)
    {
    case TRAINER_BRENDAN_ROUTE_103_MUDKIP:
    case TRAINER_BRENDAN_ROUTE_103_TREECKO:
    case TRAINER_BRENDAN_ROUTE_103_TORCHIC:
    case TRAINER_MAY_ROUTE_103_MUDKIP:
    case TRAINER_MAY_ROUTE_103_TREECKO:
    case TRAINER_MAY_ROUTE_103_TORCHIC:
        return true;
    default:
        return false;
    }
}

uint8_t MpBattle2v2_CountAlivePlayerMons(void)
{
    uint8_t alive = 0;
    u32 i;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        enum Species sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(mon, MON_DATA_HP) > 0)
            alive++;
    }
    return alive;
}

static void AttachFallbackAiPartnerFlags(uint32_t *battleTypeFlags, uint32_t seed)
{
    if (battleTypeFlags == NULL)
        return;
    if (!(*battleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS))
        TRAINER_BATTLE_PARAM.opponentB = 0xFFFF;
    PopulateFallbackPartnerOrPvpParty(B_TRAINER_PARTNER, seed);
    gPartnerTrainerId = TRAINER_PARTNER(PARTNER_STEVEN);
    *battleTypeFlags |= BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER;
    sBattleSession.activeFallbackAiPartner = true;
}

void MpBattle2v2_AttachWildAiPartner(uint32_t *battleTypeFlags)
{
    /* Wild encounters remain strictly 1v1; kept as a no-op for caller ABI compatibility. */
    (void)battleTypeFlags;
}

bool MpBattle2v2_IsFallbackAiPartner(void)
{
    return sBattleSession.activeFallbackAiPartner;
}

bool MpBattle2v2_ShouldForceWildDouble(void)
{
    /* Wild encounters never force 2v2 doubles. */
    return false;
}

void MpBattle2v2_ConfigureTrainerBattleFlags(uint16_t trainerId, uint32_t *battleTypeFlags)
{
    uint8_t targetPartner;
    uint8_t coopPartner;
    uint16_t partnerPeer = 0;

    if (battleTypeFlags == NULL)
        return;

    MpBattle2v2_Init();
    sBattleSession.activeFallbackAiPartner = false;
    if ((*battleTypeFlags & (BATTLE_TYPE_FIRST_BATTLE | BATTLE_TYPE_PYRAMID | BATTLE_TYPE_TRAINER_HILL | BATTLE_TYPE_FRONTIER)) != 0)
        return;

    /* Route 103 rival battle is strictly 1v1 */
    if (IsFirstRivalTutorialBattle(trainerId))
    {
        *battleTypeFlags &= ~(uint32_t)(BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER);
        return;
    }

    if (!MpWorldSync_IsLocalTutorialComplete())
        return;

    /* Force Always-2v2 Double Battles on every trainer encounter */
    *battleTypeFlags |= BATTLE_TYPE_DOUBLE;

    if (*battleTypeFlags & BATTLE_TYPE_INGAME_PARTNER)
        return;

    targetPartner = FindCoopTargetSlot();
    coopPartner = MpNet_GetCoopPartnerSlot();

    if (targetPartner != 0xFF)
    {
        const MpPeerState *ps = MpNet_GetPeerState(targetPartner);
        const char *busyReason = MpOverworld_GetPeerBusyReason(targetPartner);
        uint32_t seed = (uint32_t)(MpRand_SplitMix64(MpRandomizer_GetRunSeed(), 0x32563243u, trainerId) & 0xFFFFFFFFu);

        if (ps != NULL)
            partnerPeer = ps->peerId;

        if (busyReason != NULL || coopPartner != targetPartner || partnerPeer == 0)
        {
#ifdef WASM_PLATFORM
            const char *promptReason = (busyReason != NULL) ? busyReason : ((partnerPeer == 0) ? "not_ready" : "busy");
            bool twoOpponents = (*battleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS) != 0;
            uint16_t opponentB = twoOpponents ? TRAINER_BATTLE_PARAM.opponentB : 0xFFFFu;

            ResetBattleSessionFields(false);
            sBattleSession.battleSeed = seed;
            sBattleSession.busyPromptPending = true;
            sBattleSession.busyPromptChoice = MP_BUSY_PROMPT_UNDECIDED;
            sBattleSession.busyPromptPartnerSlot = targetPartner;
            sBattleSession.busyPromptTrainerId = trainerId;
            sBattleSession.busyPromptTwoOpponents = twoOpponents;
            sBattleSession.busyPromptOpponentB = opponentB;
            if (!twoOpponents)
                TRAINER_BATTLE_PARAM.opponentB = 0xFFFF;
            sBattleSession.busyPromptBattleSeed = seed;
            SeedDeterministicBattleRng(seed);
            WasmMp_PromptTeammateBusy((int)targetPartner, promptReason);
            return;
#else
            SeedDeterministicBattleRng(seed);
            AttachFallbackAiPartnerFlags(battleTypeFlags, seed);
            return;
#endif
        }

        {
            bool twoOpponents = (*battleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS) != 0;
            uint16_t opponentB = twoOpponents ? TRAINER_BATTLE_PARAM.opponentB : 0xFFFFu;

            LaunchCoop2v2SessionFromTrainer(seed, trainerId, opponentB, twoOpponents,
                                            coopPartner, partnerPeer, battleTypeFlags);
        }
    }
    else if (MpBattle2v2_CountAlivePlayerMons() <= 1)
    {
        uint32_t seed = (uint32_t)(MpRand_SplitMix64(MpRandomizer_GetRunSeed(), 0x534F4C4Fu, trainerId) & 0xFFFFFFFFu);
        SeedDeterministicBattleRng(seed);
        AttachFallbackAiPartnerFlags(battleTypeFlags, seed);
    }
}

void MpBattle2v2_EnsureNpcDoubleParty(uint16_t trainerId, struct Pokemon *enemyParty)
{
    u32 count = 0;
    u32 i;
    enum Species aceSpecies;
    u8 aceLevel;
    uint16_t partnerSpecies;
    uint32_t pers;
    uint8_t trainerClass = 0;

    if (enemyParty == NULL)
        return;
    if (!MpWorldSync_IsLocalTutorialComplete() || IsFirstRivalTutorialBattle(trainerId))
        return;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&enemyParty[i], MON_DATA_SPECIES) != SPECIES_NONE)
            count++;
    }

    if (count != 1)
        return;

    aceSpecies = GetMonData(&enemyParty[0], MON_DATA_SPECIES);
    aceLevel = (uint8_t)GetMonData(&enemyParty[0], MON_DATA_LEVEL);
    if (aceSpecies == SPECIES_NONE)
        return;

    if (trainerId < TRAINERS_COUNT)
        trainerClass = GetTrainerStructFromId(trainerId)->trainerClass;

    partnerSpecies = MpRandomizer_GetNpcPartnerSpecies(trainerId, trainerClass, (uint16_t)aceSpecies, aceLevel);
    if (partnerSpecies == SPECIES_NONE || partnerSpecies >= NUM_SPECIES)
        partnerSpecies = (uint16_t)aceSpecies;

    pers = (uint32_t)(MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_NPC_PARTNER, ((uint32_t)trainerId << 16) | aceSpecies) & 0xFFFFFFFFu);

    MpRandomizer_PushSuppressStatic();
    CreateMonWithIVs(&enemyParty[1], (enum Species)partnerSpecies, aceLevel, pers, OTID_STRUCT_PRESET(trainerId ^ 0x4E50u), 12);
    GiveMonInitialMoveset(&enemyParty[1]);
    if (!MpRandomizer_HasFlag(MP_RANDO_F_MOVESETS))
        MpBattle2v2_EnsureReliableMove(&enemyParty[1]);
    CalculateMonStats(&enemyParty[1]);
    ApplyDefaultAiGimmickBlock(&enemyParty[1]);
    MpRandomizer_PopSuppressStatic();
}

bool MpBattle2v2_IsFriendlyPvP(void)
{
    return sBattleSession.active && sBattleSession.mode == MP_BATTLE_PVP;
}

bool MpBattle2v2_IsCoopMulti(void)
{
    return MpBattle2v2_IsSyncedSession() && sBattleSession.mode == MP_BATTLE_COOP_2V2;
}

bool MpBattle2v2_IsSyncedSession(void)
{
    return sBattleSession.active
        && sBattleSession.mode != MP_BATTLE_SOLO_2V2
        && sBattleSession.remotePartyReady;
}

static bool IsPartnerConnectedForSync(void)
{
    const MpPeerState *ps;

    if (!MpBattle2v2_IsSyncedSession())
        return false;
    if (sBattleSession.partnerSlot >= MP_MAX_PLAYERS)
        return false;
    ps = MpNet_GetPeerState(sBattleSession.partnerSlot);
    return (ps != NULL && ps->connected);
}

static u8 AsciiCharToGba(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (u8)(CHAR_A + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z')
        return (u8)(CHAR_a + (ch - 'a'));
    if (ch >= '0' && ch <= '9')
        return (u8)(CHAR_0 + (ch - '0'));
    switch (ch)
    {
    case ' ':
    case '_':
        return CHAR_SPACE;
    case '-':
        return CHAR_HYPHEN;
    case '.':
        return CHAR_PERIOD;
    case '!':
        return CHAR_EXCL_MARK;
    case '?':
        return CHAR_QUESTION_MARK;
    default:
        return CHAR_SPACE;
    }
}

const uint8_t *MpBattle2v2_GetPvpOpponentName(void)
{
    static u8 sPvpOpponentNameGba[TRAINER_NAME_LENGTH + 1];
    const MpPeerState *opp;
    uint32_t i;

    if (!MpBattle2v2_IsFriendlyPvP())
        return NULL;

    opp = GetActivePvpOpponentPeer();
    if (opp == NULL || opp->name[0] == '\0')
        return NULL;

    for (i = 0; i < TRAINER_NAME_LENGTH && opp->name[i] != '\0'; i++)
        sPvpOpponentNameGba[i] = AsciiCharToGba(opp->name[i]);
    sPvpOpponentNameGba[i] = EOS;
    return sPvpOpponentNameGba;
}

bool MpBattle2v2_IsRemoteControlledBattler(uint8_t battlerId)
{
    enum BattlerPosition pos;
    if (!IsPartnerConnectedForSync() || sBattleSession.remoteBattleEnded)
        return false;

    pos = GetBattlerPosition((enum BattlerId)battlerId);
    if (sBattleSession.mode == MP_BATTLE_COOP_2V2)
        return (pos == B_POSITION_PLAYER_RIGHT);
    if (sBattleSession.mode == MP_BATTLE_PVP)
        return (pos == B_POSITION_OPPONENT_LEFT || pos == B_POSITION_OPPONENT_RIGHT);
    return false;
}

void MpBattle2v2_StartSyncedBattle(uint32_t battleSeed, uint8_t mode, uint16_t trainerId)
{
    uint8_t targetSlot = 0xFF;
    uint16_t targetPeer = 0;
    uint32_t i;
    bool fromRemote = sBattleSession.pendingFromRemote;
    bool waitSent = sBattleSession.pendingWaitSent;
    bool twoOpponents = sBattleSession.pendingTwoOpponents;
    uint16_t opponentB = sBattleSession.pendingOpponentB;
    uint8_t savedPartnerSlot = sBattleSession.pendingPartnerSlot;
    uint16_t savedPartnerPeer = sBattleSession.pendingPartnerPeerId;
    bool coopFallback = sBattleSession.pendingCoopFallback;
    uint64_t savedSeedHash = sBattleSession.pendingSeedHash;
    uint32_t savedRandomizerFlags = sBattleSession.pendingRandomizerFlags;
    bool savedHasSeedSync = sBattleSession.pendingHasSeedSync;

    MpBattle2v2_Init();
    sBattleSession.pendingLaunch = false;
    sBattleSession.pendingFromRemote = false;
    sBattleSession.pendingWaitSent = false;
    sBattleSession.pendingPartySent = false;
    sBattleSession.pendingOverworldWaitFrames = 0;
    sBattleSession.pendingPartyWaitFrames = 0;
    sBattleSession.battleStartWaitFrames = 0;
    sBattleSession.pendingPartnerSlot = 0xFF;
    sBattleSession.pendingPartnerPeerId = 0;
    sBattleSession.pendingCoopFallback = false;
    sBattleSession.activeFallbackAiPartner = false;
    sBattleSession.remoteDeclinedOrEnded = false;
    sBattleSession.remoteBattleEnded = false;
    sBattleSession.remoteDeclineSlot = 0xFF;
    sBattleSession.remoteDeclineCode = 0;

    if (mode != MP_BATTLE_SOLO_2V2)
    {
        if (savedPartnerPeer != 0)
        {
            targetSlot = savedPartnerSlot;
            targetPeer = savedPartnerPeer;
        }
        else
        {
            targetPeer = FindPartnerPeerId(&targetSlot);
        }
    }

    if (!MpWorldSync_IsLocalTutorialComplete())
    {
        if (fromRemote && (mode == MP_BATTLE_COOP_2V2 || mode == MP_BATTLE_PVP))
            SendDeclineBusy((targetPeer != 0) ? targetPeer : MP_PEER_BROADCAST, "in_tutorial");
        ResetBattleSessionFields(false);
        if (battleSeed != 0)
        {
            sBattleSession.lastCanceledSeed = battleSeed;
            sBattleSession.lastCanceledSeedTtl = 180u;
        }
        return;
    }

    if (!fromRemote && !waitSent)
        ResetRemotePartyCache();

    if (mode != MP_BATTLE_SOLO_2V2)
    {
        if (!fromRemote && mode == MP_BATTLE_COOP_2V2)
        {
            uint8_t checkSlot = (targetSlot < MP_MAX_PLAYERS) ? targetSlot : MpNet_FindExplicitPairSlot();
            if (checkSlot < MP_MAX_PLAYERS)
            {
                const char *busyReason = MpOverworld_GetPeerBusyReason(checkSlot);
                if (busyReason != NULL)
                {
#ifdef WASM_PLATFORM
                    WasmMp_NotifyTeammateBusy((int)checkSlot, busyReason);
#endif
                    coopFallback = true;
                    mode = MP_BATTLE_SOLO_2V2;
                    targetSlot = 0xFF;
                    targetPeer = 0;
                }
            }
            else if (targetSlot >= MP_MAX_PLAYERS || targetPeer == 0)
            {
                mode = MP_BATTLE_SOLO_2V2;
                targetSlot = 0xFF;
                targetPeer = 0;
            }
        }
    }

    sBattleSession.active = true;
    sBattleSession.isInitiator = !fromRemote;
    sBattleSession.mode = mode;
    sBattleSession.bridgeLaunched = true;
    sBattleSession.battleSeed = (battleSeed != 0) ? battleSeed : 0x20260401u;
    sBattleSession.turnNumber = 0;
    sBattleSession.trainerId = trainerId;
    sBattleSession.twoOpponents = twoOpponents;
    sBattleSession.opponentB = opponentB;
    sBattleSession.partnerSlot = targetSlot;
    sBattleSession.partnerPeerId = targetPeer;
    sBattleSession.localCommitMask = 0;
    memset(sBattleSession.remoteActions, 0, sizeof(sBattleSession.remoteActions));
    if (savedHasSeedSync && savedSeedHash != 0)
    {
        sBattleSession.seedHash = savedSeedHash;
        sBattleSession.randomizerFlags = savedRandomizerFlags;
        sBattleSession.hasSeedSync = true;
    }
    else
    {
        sBattleSession.seedHash = MpRandomizer_GetRunSeed();
        sBattleSession.randomizerFlags = MpRandomizer_GetFlags();
        sBattleSession.hasSeedSync = (sBattleSession.seedHash != 0);
    }
    if (fromRemote && sBattleSession.hasSeedSync && sBattleSession.seedHash != 0)
        MpBattle2v2_VerifySyncedSeedBeforeTrainerBuild();

    SeedDeterministicBattleRng(sBattleSession.battleSeed);
    if (mode == MP_BATTLE_PVP)
    {
        const MpPeerState *opp = GetActivePvpOpponentPeer();
        uint8_t oppGender = (opp != NULL && opp->hasPosition)
                          ? opp->pos.gender
                          : ((gSaveBlock2Ptr->playerGender == MALE) ? FEMALE : MALE);
        uint16_t resolvedPvpTrainer = (trainerId != 0 && trainerId < TRAINERS_COUNT)
                                    ? trainerId
                                    : ((oppGender == FEMALE) ? TRAINER_MAY_ROUTE_103_TREECKO : TRAINER_BRENDAN_ROUTE_103_TREECKO);
        u8 localCount = 0;
        u8 baseLevel = GetHighestLocalPartyLevel();
        uint32_t localRentalSeed = sBattleSession.battleSeed ^ ((uint32_t)MpNet_GetLocalSlot() * 0x9E3779B9u);

        sBattleSession.trainerId = resolvedPvpTrainer;
        TRAINER_BATTLE_PARAM.opponentA = resolvedPvpTrainer;
        TRAINER_BATTLE_PARAM.opponentB = 0xFFFF;

        /* Snapshot player party before Friendly PvP so HP/status/PP and rental slots restore after battle */
        sBattleSession.pvpSavedPlayerPartyCount = gPartiesCount[B_TRAINER_PLAYER];
        for (i = 0; i < PARTY_SIZE; i++)
            sBattleSession.pvpSavedPlayerParty[i] = gParties[B_TRAINER_PLAYER][i];
        sBattleSession.pvpPartySaved = true;

        for (i = 0; i < PARTY_SIZE; i++)
        {
            enum Species sp = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES_OR_EGG);
            if (sp != SPECIES_NONE && sp != SPECIES_EGG)
                localCount++;
        }
        for (i = localCount; i < 2 && i < PARTY_SIZE; i++)
            CreateDeterministicRentalMon(&gParties[B_TRAINER_PLAYER][i], localRentalSeed, i, baseLevel);
        gPartiesCount[B_TRAINER_PLAYER] = CalculatePartyCount(B_TRAINER_PLAYER);

        gBattleTypeFlags = BATTLE_TYPE_DOUBLE | BATTLE_TYPE_TRAINER;
        PopulateFallbackPartnerOrPvpParty(B_TRAINER_OPPONENT_A, sBattleSession.battleSeed);
    }
    else if (mode == MP_BATTLE_COOP_2V2)
    {
        u8 localCount = CalculatePlayerPartyCount();
        if (localCount == 0)
        {
            sBattleSession.pvpSavedPlayerPartyCount = gPartiesCount[B_TRAINER_PLAYER];
            for (i = 0; i < PARTY_SIZE; i++)
                sBattleSession.pvpSavedPlayerParty[i] = gParties[B_TRAINER_PLAYER][i];
            sBattleSession.pvpPartySaved = true;
            for (i = 0; i < 2; i++)
                CreateDeterministicRentalMon(&gParties[B_TRAINER_PLAYER][i], sBattleSession.battleSeed ^ 0x434F3250u, i, 5);
            gPartiesCount[B_TRAINER_PLAYER] = CalculatePartyCount(B_TRAINER_PLAYER);
        }
        else
        {
            sBattleSession.pvpPartySaved = false;
        }
        TRAINER_BATTLE_PARAM.opponentA = (trainerId != 0 && trainerId < TRAINERS_COUNT) ? trainerId : 1;
        TRAINER_BATTLE_PARAM.opponentB = (twoOpponents && opponentB < TRAINERS_COUNT) ? opponentB : 0xFFFF;
        gPartnerTrainerId = TRAINER_PARTNER(PARTNER_STEVEN);
        gBattleTypeFlags = BATTLE_TYPE_DOUBLE | BATTLE_TYPE_MULTI | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_TRAINER;
        if (twoOpponents && opponentB < TRAINERS_COUNT && trainerId != 0)
            gBattleTypeFlags |= BATTLE_TYPE_TWO_OPPONENTS;
        PopulateFallbackPartnerOrPvpParty(B_TRAINER_PARTNER, sBattleSession.battleSeed);
        if (trainerId == 0)
        {
            PopulateDeterministicEnemyParty(gParties[B_TRAINER_OPPONENT_A], sBattleSession.battleSeed);
            gPartiesCount[B_TRAINER_OPPONENT_A] = CalculatePartyCount(B_TRAINER_OPPONENT_A);
        }
    }
    else
    {
        u8 localCount = CalculatePlayerPartyCount();
        if (localCount == 0)
        {
            sBattleSession.pvpSavedPlayerPartyCount = gPartiesCount[B_TRAINER_PLAYER];
            for (i = 0; i < PARTY_SIZE; i++)
                sBattleSession.pvpSavedPlayerParty[i] = gParties[B_TRAINER_PLAYER][i];
            sBattleSession.pvpPartySaved = true;
            for (i = 0; i < 2; i++)
                CreateDeterministicRentalMon(&gParties[B_TRAINER_PLAYER][i], sBattleSession.battleSeed ^ 0x534F3250u, i, 5);
            gPartiesCount[B_TRAINER_PLAYER] = CalculatePartyCount(B_TRAINER_PLAYER);
        }
        else
        {
            sBattleSession.pvpPartySaved = false;
        }
        TRAINER_BATTLE_PARAM.opponentA = (trainerId != 0 && trainerId < TRAINERS_COUNT) ? trainerId : 1;
        TRAINER_BATTLE_PARAM.opponentB = (twoOpponents && opponentB < TRAINERS_COUNT) ? opponentB : 0xFFFF;
        gBattleTypeFlags = BATTLE_TYPE_DOUBLE | BATTLE_TYPE_TRAINER;
        if (twoOpponents && opponentB < TRAINERS_COUNT && trainerId != 0)
            gBattleTypeFlags |= BATTLE_TYPE_TWO_OPPONENTS;
        if (coopFallback || MpBattle2v2_CountAlivePlayerMons() <= 1)
            AttachFallbackAiPartnerFlags(&gBattleTypeFlags, sBattleSession.battleSeed);
        if (trainerId == 0)
        {
            PopulateDeterministicEnemyParty(gParties[B_TRAINER_OPPONENT_A], sBattleSession.battleSeed);
            gPartiesCount[B_TRAINER_OPPONENT_A] = CalculatePartyCount(B_TRAINER_OPPONENT_A);
        }
    }

    if (targetPeer != 0 && (mode == MP_BATTLE_COOP_2V2 || mode == MP_BATTLE_PVP) && !fromRemote && !waitSent)
        SendBattleStartPacket(mode, sBattleSession.battleSeed, trainerId, opponentB, twoOpponents, targetPeer);

    if (!gMain.inBattle)
        BattleSetup_LaunchMpSyncedBattle();
}

bool MpBattle2v2_WasLastBattleFriendlyPvP(void)
{
    return sBattleSession.lastBattleWasPvp;
}

bool MpBattle2v2_WasLastBattleBridgeSynced(void)
{
    return sBattleSession.lastBattleWasBridgeSynced;
}

uint16_t MpBattle2v2_GetSyncedTrainerId(void)
{
    return (sBattleSession.trainerId != 0) ? sBattleSession.trainerId : sBattleSession.lastSyncedTrainerId;
}

uint32_t MpBattle2v2_GetBattleSeed(void)
{
    return sBattleSession.battleSeed;
}

bool MpBattle2v2_IsInitiator(void)
{
    if (!MpBattle2v2_IsSyncedSession())
        return true;
    if (sBattleSession.partnerSlot < MP_MAX_PLAYERS && sBattleSession.partnerSlot != MpNet_GetLocalSlot())
        return MpNet_GetLocalSlot() < sBattleSession.partnerSlot;
    return sBattleSession.isInitiator;
}

uint8_t MpBattle2v2_GetRemotePartnerGender(void)
{
    if (sBattleSession.partnerSlot < MP_MAX_PLAYERS)
    {
        const MpPeerState *ps = MpNet_GetPeerState(sBattleSession.partnerSlot);
        if (ps != NULL && ps->hasPosition)
            return (ps->pos.gender == FEMALE) ? FEMALE : MALE;
    }
    return MALE;
}

bool MpBattle2v2_ReadyToCommitLocalAction(uint8_t battlerId)
{
    enum BattlerId oppLeft;
    enum BattlerId oppRight;

    if (!MpBattle2v2_IsCoopMulti() || !MpBattle2v2_IsInitiator())
        return true;
    if (battlerId >= gBattlersCount || !IsOnPlayerSide((enum BattlerId)battlerId))
        return true;

    oppLeft = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
    if (oppLeft < gBattlersCount && !(gAbsentBattlerFlags & (1u << oppLeft))
     && gBattleCommunication[oppLeft] != 4 && gBattleCommunication[oppLeft] != 5)
        return false;

    if (IsDoubleBattle())
    {
        oppRight = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
        if (oppRight < gBattlersCount && !(gAbsentBattlerFlags & (1u << oppRight))
         && gBattleCommunication[oppRight] != 4 && gBattleCommunication[oppRight] != 5)
            return false;
    }

    return true;
}

static const char *ResolveDeclineReason(uint8_t code, uint8_t slot)
{
    if (code != 0 && code != 0xFFu)
        return BusyCodeToReason(code);
    {
        const char *reason = MpOverworld_GetPeerBusyReason(slot);
        return (reason != NULL) ? reason : "busy";
    }
}

bool MpBattle2v2_ReadyForBattleStart(void)
{
    const MpPeerState *ps = NULL;
    bool shouldFallback = false;
    const char *fallbackReason = "busy";
    uint8_t notifySlot;

    if (!sBattleSession.active)
        return true;
    if (sBattleSession.mode != MP_BATTLE_COOP_2V2 && sBattleSession.mode != MP_BATTLE_PVP)
        return true;
    if (sBattleSession.remotePartyReady)
        return true;

    notifySlot = sBattleSession.partnerSlot;
    if (sBattleSession.partnerSlot < MP_MAX_PLAYERS)
        ps = MpNet_GetPeerState(sBattleSession.partnerSlot);

    if (sBattleSession.remoteDeclinedOrEnded)
    {
        shouldFallback = true;
        if (sBattleSession.remoteDeclineSlot < MP_MAX_PLAYERS)
            notifySlot = sBattleSession.remoteDeclineSlot;
        fallbackReason = ResolveDeclineReason(sBattleSession.remoteDeclineCode, notifySlot);
    }
    else if (sBattleSession.partnerPeerId == 0 || sBattleSession.partnerSlot >= MP_MAX_PLAYERS || ps == NULL || !ps->connected)
    {
        shouldFallback = true;
        fallbackReason = "not_ready";
    }
    else if (sBattleSession.battleStartWaitFrames >= 240u)
    {
        shouldFallback = true;
        fallbackReason = "busy";
    }
    else if (sBattleSession.battleStartWaitFrames >= 120u)
    {
        const char *lateBusyReason = MpOverworld_GetPeerBusyReason(sBattleSession.partnerSlot);
        if (lateBusyReason != NULL)
        {
            shouldFallback = true;
            fallbackReason = lateBusyReason;
        }
    }

    if (shouldFallback)
    {
        uint32_t fallbackSeed = (sBattleSession.battleSeed != 0) ? sBattleSession.battleSeed : 0x20260401u;
        uint16_t cancelPeer = sBattleSession.partnerPeerId;

        if (cancelPeer != 0)
        {
            uint8_t cancelOutcome = 0;
            MpNet_SendBinary(MP_CH_BATTLE, cancelPeer, MP_PKT_BAT_END, &cancelOutcome, 1);
        }
#ifdef WASM_PLATFORM
        if (notifySlot < MP_MAX_PLAYERS)
            WasmMp_NotifyTeammateBusy((int)notifySlot, fallbackReason);
#endif
        ResetBattleSessionFields(false);
        sBattleSession.lastCanceledSeed = fallbackSeed;
        sBattleSession.lastCanceledSeedTtl = 180u;
        AttachFallbackAiPartnerFlags(&gBattleTypeFlags, fallbackSeed);
        return true;
    }

    if ((sBattleSession.battleStartWaitFrames % 20u) == 0u && sBattleSession.partnerPeerId != 0)
    {
        if (sBattleSession.isInitiator)
        {
            SendBattleStartPacket(sBattleSession.mode, sBattleSession.battleSeed,
                                  sBattleSession.trainerId, sBattleSession.opponentB,
                                  sBattleSession.twoOpponents, sBattleSession.partnerPeerId);
        }
        else
        {
            SendLocalPartyChunks(sBattleSession.mode, sBattleSession.battleSeed,
                                 sBattleSession.partnerPeerId);
        }
    }

    sBattleSession.battleStartWaitFrames++;
    return false;
}

void MpBattle2v2_OnBattleInit(void)
{
    MpBattle2v2_Init();
    sBattleSession.inBattleCallbackActive = true;
    sBattleSession.lastBattleWasPvp = false;
    sBattleSession.lastBattleWasBridgeSynced = false;
    sBattleSession.turnNumber = 0;
    sBattleSession.localCommitMask = 0;
    sBattleSession.remoteAppliedMask = 0;
    sBattleSession.remoteAiAppliedThisTurn = false;
    sBattleSession.battleStartWaitFrames = 0;
    memset(sBattleSession.remoteActions, 0, sizeof(sBattleSession.remoteActions));
    memset(sBattleSession.remoteSwitches, 0, sizeof(sBattleSession.remoteSwitches));
    memset(sBattleSession.switchSeqByBattler, 0, sizeof(sBattleSession.switchSeqByBattler));
    MpNet_SetLocalInBattle(true);

    if (sBattleSession.active && sBattleSession.remotePartyReady && sBattleSession.remotePartyCount > 0)
    {
        if (sBattleSession.mode == MP_BATTLE_COOP_2V2)
            CopyRemotePartyToTrainer(B_TRAINER_PARTNER);
        else if (sBattleSession.mode == MP_BATTLE_PVP)
            CopyRemotePartyToTrainer(B_TRAINER_OPPONENT_A);
    }

    if (MpBattle2v2_IsSyncedSession())
    {
        SeedDeterministicBattleRngWithContext(sBattleSession.battleSeed, 0x494E4954u, 0u);
        if (gBattleStruct != NULL)
            gBattleStruct->speedTieBreaks = RandomUniform(RNG_SPEED_TIE, 0, Factorial(MAX_BATTLERS_COUNT) - 1);
    }
}

void MpBattle2v2_OnTurnStart(void)
{
    if (!MpBattle2v2_IsSyncedSession())
        return;

    sBattleSession.turnNumber++;
    sBattleSession.localCommitMask = 0;
    sBattleSession.remoteAppliedMask = 0;
    sBattleSession.remoteAiAppliedThisTurn = false;
    SeedDeterministicBattleRngWithContext(sBattleSession.battleSeed, 0x5455524Eu, sBattleSession.turnNumber);
}

bool MpBattle2v2_ShouldUseLocalOpponentAi(void)
{
    if (MpBattle2v2_IsCoopMulti() && !MpBattle2v2_IsInitiator() && IsPartnerConnectedForSync() && !sBattleSession.remoteBattleEnded)
        return false;
    return true;
}

bool MpBattle2v2_AreRemoteAiActionsReady(void)
{
    if (MpBattle2v2_ShouldUseLocalOpponentAi())
        return true;
    return sBattleSession.remoteAiAppliedThisTurn;
}

bool MpBattle2v2_IsWaitingForRemotePartner(void)
{
    uint8_t i;

    if (!IsPartnerConnectedForSync() || sBattleSession.remoteBattleEnded)
        return false;
    if (!MpBattle2v2_AreRemoteAiActionsReady())
        return true;
    for (i = 0; i < gBattlersCount && i < MAX_BATTLERS_COUNT; i++)
    {
        if (MpBattle2v2_IsRemoteControlledBattler(i) && !(sBattleSession.remoteAppliedMask & (1u << i)))
            return true;
    }
    return false;
}

void MpBattle2v2_CommitLocalAction(uint8_t battlerId)
{
    struct MpBattleTurnActionPayload pkt;
    uint16_t targetPeer;

    if (!MpBattle2v2_IsSyncedSession() || battlerId >= MAX_BATTLERS_COUNT)
        return;
    if (MpBattle2v2_IsRemoteControlledBattler(battlerId))
        return;
    if (!IsOnPlayerSide((enum BattlerId)battlerId))
        return;
    if ((sBattleSession.localCommitMask & (1u << battlerId)) != 0)
        return;

    sBattleSession.localCommitMask |= (uint8_t)(1u << battlerId);
    targetPeer = sBattleSession.partnerPeerId;
    if (targetPeer == 0)
        targetPeer = FindPartnerPeerId(NULL);

    memset(&pkt, 0, sizeof(pkt));
    pkt.turnNumber = sBattleSession.turnNumber;
    pkt.battlerId = battlerId;
    pkt.actionChoice = gChosenActionByBattler[battlerId];
    pkt.targetBattler = (gBattleStruct->moveTarget[battlerId] < gBattlersCount)
                      ? GetBattlerPosition(gBattleStruct->moveTarget[battlerId])
                      : gBattleStruct->moveTarget[battlerId];

    if (pkt.actionChoice == B_ACTION_USE_MOVE)
    {
        pkt.moveOrItemSlot = gBattleStruct->chosenMovePositions[battlerId];
        pkt.itemOrSpecies = gChosenMoveByBattler[battlerId];
        if ((gBattleStruct->gimmick.toActivate & (1u << battlerId)) != 0)
            pkt.gimmickFlags |= 1u;
    }
    else if (pkt.actionChoice == B_ACTION_SWITCH)
    {
        pkt.moveOrItemSlot = gBattleStruct->monToSwitchIntoId[battlerId];
    }
    else if (pkt.actionChoice == B_ACTION_USE_ITEM)
    {
        uint16_t bufItem = 0;
        if (gBattleResources != NULL)
            bufItem = (uint16_t)(gBattleResources->bufferB[battlerId][1] | (gBattleResources->bufferB[battlerId][2] << 8));
        if (bufItem == ITEM_NONE)
            bufItem = (uint16_t)gBattleStruct->chosenItem[battlerId];
        pkt.itemOrSpecies = (bufItem != ITEM_NONE) ? bufItem : (uint16_t)gLastUsedItem;
        pkt.moveOrItemSlot = gBattleStruct->itemPartyIndex[battlerId];
        pkt.targetBattler = gBattleStruct->itemMoveIndex[battlerId];
    }
    else if (pkt.actionChoice == B_ACTION_THROW_BALL)
    {
        pkt.itemOrSpecies = (uint16_t)gBallToDisplay;
    }

    pkt.battleRngState = gRngValue.a ^ gRngValue.b;

    if (MpBattle2v2_IsCoopMulti() && MpBattle2v2_IsInitiator())
    {
        static const enum BattlerPosition sOppPositions[2] = {
            B_POSITION_OPPONENT_LEFT,
            B_POSITION_OPPONENT_RIGHT,
        };
        uint32_t oppIdx;
        pkt.aiValid = 1;
        for (oppIdx = 0; oppIdx < 2; oppIdx++)
        {
            enum BattlerId opp = GetBattlerAtPosition(sOppPositions[oppIdx]);
            if (opp >= gBattlersCount || (gAbsentBattlerFlags & (1u << opp)))
            {
                pkt.aiAction[oppIdx] = B_ACTION_NOTHING_FAINTED;
                continue;
            }
            pkt.aiAction[oppIdx] = gChosenActionByBattler[opp];
            if (gBattleStruct->moveTarget[opp] < gBattlersCount)
                pkt.aiTarget[oppIdx] = GetBattlerPosition(gBattleStruct->moveTarget[opp]);
            else
                pkt.aiTarget[oppIdx] = B_POSITION_PLAYER_LEFT;

            if (gChosenActionByBattler[opp] == B_ACTION_USE_MOVE)
            {
                pkt.aiMoveOrSwitch[oppIdx] = gBattleStruct->chosenMovePositions[opp];
                pkt.aiMoveOrItem[oppIdx] = gChosenMoveByBattler[opp];
                if ((gBattleStruct->gimmick.toActivate & (1u << opp)) != 0)
                    pkt.aiGimmickFlags |= (uint8_t)(1u << oppIdx);
            }
            else if (gChosenActionByBattler[opp] == B_ACTION_SWITCH)
            {
                pkt.aiMoveOrSwitch[oppIdx] = gBattleStruct->monToSwitchIntoId[opp];
            }
            else if (gChosenActionByBattler[opp] == B_ACTION_USE_ITEM)
            {
                uint16_t oppItem = (uint16_t)gBattleStruct->chosenItem[opp];
                if (oppItem == ITEM_NONE && gBattleResources != NULL)
                    oppItem = (uint16_t)(gBattleResources->bufferB[opp][1] | (gBattleResources->bufferB[opp][2] << 8));
                pkt.aiMoveOrSwitch[oppIdx] = gBattleStruct->itemPartyIndex[opp];
                pkt.aiMoveOrItem[oppIdx] = oppItem;
            }
        }
    }

    MpNet_SendBinary(MP_CH_BATTLE, targetPeer != 0 ? targetPeer : MP_PEER_BROADCAST,
                     MP_PKT_BAT_TURN_ACTION, &pkt, (uint16_t)sizeof(pkt));
}

static uint8_t RemapRemoteTargetPosition(uint8_t remoteTarget, uint8_t mode)
{
    if (remoteTarget >= MAX_BATTLERS_COUNT)
        return B_POSITION_OPPONENT_LEFT;

    if (mode == MP_BATTLE_COOP_2V2)
    {
        /* Partner treats self as PLAYER_LEFT(0) and us as PLAYER_RIGHT(2); swap 0<->2 */
        if (remoteTarget == B_POSITION_PLAYER_LEFT)
            return B_POSITION_PLAYER_RIGHT;
        if (remoteTarget == B_POSITION_PLAYER_RIGHT)
            return B_POSITION_PLAYER_LEFT;
        return remoteTarget;
    }

    if (mode == MP_BATTLE_PVP)
    {
        /* Opponent side views player<->opponent mirrored across bit 0 */
        return remoteTarget ^ 1u;
    }
    return remoteTarget;
}

bool MpBattle2v2_ShouldWaitForRemoteAction(uint8_t battlerId)
{
    if (!IsPartnerConnectedForSync() || sBattleSession.remoteBattleEnded)
        return false;
    if (!MpBattle2v2_IsRemoteControlledBattler(battlerId))
        return false;
    return (sBattleSession.partnerPeerId != 0 || sBattleSession.partnerSlot != 0xFF);
}

static bool IsValidRemoteSwitchSlot(struct Pokemon *party, uint8_t battlerId, u8 slotIdx,
                                      bool sharePartnerParty, u8 partnerActiveIdx, u8 partnerSwitchIdx)
{
    enum Species sp;
    if (slotIdx >= PARTY_SIZE)
        return false;
    sp = GetMonData(&party[slotIdx], MON_DATA_SPECIES_OR_EGG);
    if (sp == SPECIES_NONE || sp == SPECIES_EGG)
        return false;
    if (GetMonData(&party[slotIdx], MON_DATA_HP) == 0)
        return false;
    if (slotIdx == gBattlerPartyIndexes[battlerId])
        return false;
    if (sharePartnerParty && (slotIdx == partnerActiveIdx || slotIdx == partnerSwitchIdx))
        return false;
    return true;
}

static void ApplySyncedMoveChoice(enum BattlerId battler, u8 movePos, uint16_t moveId,
                                  enum BattlerPosition targetPos, bool wantGimmick)
{
    enum BattlerId targetBattler = GetBattlerAtPosition(targetPos);

    if (movePos >= MAX_MON_MOVES)
        movePos = 0;

    gBattleStruct->gimmick.toActivate &= ~(1u << battler);
    if (!gBattleMons[battler].volatiles.transformed
     && moveId != MOVE_NONE && moveId != MOVE_STRUGGLE && moveId < MOVES_COUNT
     && !IsZMove((enum Move)moveId) && !IsMaxMove((enum Move)moveId)
     && gBattleMons[battler].moves[movePos] != (enum Move)moveId)
    {
        gBattleMons[battler].moves[movePos] = (enum Move)moveId;
    }

    gChosenActionByBattler[battler] = B_ACTION_USE_MOVE;
    gBattleStruct->chosenMovePositions[battler] = movePos;
    gChosenMoveByBattler[battler] = (moveId != MOVE_NONE && moveId < MOVES_COUNT)
        ? (enum Move)moveId
        : gBattleMons[battler].moves[movePos];
    gBattleStruct->moveTarget[battler] = targetBattler;

    if (gBattleResources != NULL)
    {
        gBattleResources->bufferB[battler][0] = B_ACTION_USE_MOVE;
        gBattleResources->bufferB[battler][1] = B_ACTION_EXEC_SCRIPT;
        gBattleResources->bufferB[battler][2] = (u8)(movePos | (wantGimmick ? RET_GIMMICK : 0));
        gBattleResources->bufferB[battler][3] = (u8)targetBattler;
    }

    if (wantGimmick)
        gBattleStruct->gimmick.toActivate |= (1u << battler);
    if (IsBattleMoveStatus(gChosenMoveByBattler[battler]) && GetBattlerAbility(battler) == ABILITY_MYCELIUM_MIGHT)
        gProtectStructs[battler].myceliumMight = TRUE;
    if (GetBattlerHoldEffect(battler) == HOLD_EFFECT_LAGGING_TAIL)
        gProtectStructs[battler].laggingTail = TRUE;
    if (GetActiveGimmick(battler) == GIMMICK_DYNAMAX || IsGimmickSelected(battler, GIMMICK_DYNAMAX))
        gBattleStruct->dynamax.baseMoves[battler] = GetBattlerChosenMove(battler);
}

static void ApplySyncedItemChoice(enum BattlerId battler, uint16_t itemId, u8 partyIdx, u8 moveIdx)
{
    gChosenActionByBattler[battler] = B_ACTION_USE_ITEM;
    if (gBattleResources != NULL)
    {
        gBattleResources->bufferB[battler][0] = B_ACTION_USE_ITEM;
        gBattleResources->bufferB[battler][1] = (uint8_t)(itemId & 0xFFu);
        gBattleResources->bufferB[battler][2] = (uint8_t)((itemId >> 8) & 0xFFu);
    }
    gBattleStruct->chosenItem[battler] = (enum Item)itemId;
    gLastUsedItem = (enum Item)itemId;
    gBattleStruct->itemPartyIndex[battler] = (partyIdx < PARTY_SIZE) ? partyIdx : gBattlerPartyIndexes[battler];
    gBattleStruct->itemMoveIndex[battler] = (moveIdx < MAX_MON_MOVES) ? moveIdx : 0u;
    if (GetItemPocket((enum Item)itemId) == POCKET_POKE_BALLS)
        gBattleStruct->throwingPokeBall = TRUE;
}

static bool ApplySyncedSwitchChoice(enum BattlerId battler, u8 requestedSlot)
{
    struct Pokemon *party = GetBattlerParty(battler);
    enum BattlerId partnerBattler = GetPartnerBattler(battler);
    bool sharePartnerParty = (IsDoubleBattle() && partnerBattler < gBattlersCount
                           && GetBattlerParty(partnerBattler) == party);
    u8 partnerActiveIdx = (sharePartnerParty && !(gAbsentBattlerFlags & (1u << partnerBattler)))
                        ? gBattlerPartyIndexes[partnerBattler]
                        : PARTY_SIZE;
    u8 partnerSwitchIdx = (sharePartnerParty && gChosenActionByBattler[partnerBattler] == B_ACTION_SWITCH)
                        ? gBattleStruct->monToSwitchIntoId[partnerBattler]
                        : PARTY_SIZE;
    u8 switchSlot = requestedSlot;

    if (!IsValidRemoteSwitchSlot(party, battler, switchSlot, sharePartnerParty, partnerActiveIdx, partnerSwitchIdx))
    {
        u8 alt;
        switchSlot = PARTY_SIZE;
        for (alt = 0; alt < PARTY_SIZE; alt++)
        {
            if (IsValidRemoteSwitchSlot(party, battler, alt, sharePartnerParty, partnerActiveIdx, partnerSwitchIdx))
            {
                switchSlot = alt;
                break;
            }
        }
    }
    if (switchSlot >= PARTY_SIZE)
        return false;

    gChosenActionByBattler[battler] = B_ACTION_SWITCH;
    gBattleStruct->monToSwitchIntoId[battler] = switchSlot;
    if (gBattleResources != NULL)
    {
        gBattleResources->bufferB[battler][0] = B_ACTION_SWITCH;
        gBattleResources->bufferB[battler][1] = switchSlot;
    }
    UpdateBattlerPartyOrdersOnSwitch(battler);
    return true;
}

static void ApplyRemoteAiChoices(const struct MpBattleTurnActionPayload *pkt)
{
    static const enum BattlerPosition sOppPositions[2] = {
        B_POSITION_OPPONENT_LEFT,
        B_POSITION_OPPONENT_RIGHT,
    };
    uint32_t oppIdx;

    if (pkt == NULL || !pkt->aiValid || sBattleSession.remoteAiAppliedThisTurn)
        return;

    for (oppIdx = 0; oppIdx < 2; oppIdx++)
    {
        enum BattlerId opp = GetBattlerAtPosition(sOppPositions[oppIdx]);
        uint8_t act = pkt->aiAction[oppIdx];
        if (opp >= gBattlersCount || (gAbsentBattlerFlags & (1u << opp)))
            continue;
        gBattleStruct->gimmick.toActivate &= ~(1u << opp);
        gChosenActionByBattler[opp] = act;
        if (act == B_ACTION_USE_MOVE)
        {
            uint8_t mappedAiTarget = RemapRemoteTargetPosition(pkt->aiTarget[oppIdx], MP_BATTLE_COOP_2V2);
            bool wantGimmick = (pkt->aiGimmickFlags & (1u << oppIdx)) != 0;
            ApplySyncedMoveChoice(opp, pkt->aiMoveOrSwitch[oppIdx], pkt->aiMoveOrItem[oppIdx],
                                  (enum BattlerPosition)mappedAiTarget, wantGimmick);
        }
        else if (act == B_ACTION_SWITCH)
        {
            ApplySyncedSwitchChoice(opp, pkt->aiMoveOrSwitch[oppIdx]);
        }
        else if (act == B_ACTION_USE_ITEM)
        {
            uint16_t item = pkt->aiMoveOrItem[oppIdx];
            if (item != ITEM_NONE && item < ITEMS_COUNT)
                ApplySyncedItemChoice(opp, item, pkt->aiMoveOrSwitch[oppIdx], 0u);
        }
    }
    sBattleSession.remoteAiAppliedThisTurn = true;
}

bool MpBattle2v2_TryApplyRemoteAction(uint8_t battlerId)
{
    uint32_t i;
    enum BattlerPosition localPos;

    if (!MpBattle2v2_IsRemoteControlledBattler(battlerId))
        return false;

    /* If peer disconnected mid-battle, fall back gracefully to local AI so battle never hangs */
    if (sBattleSession.partnerSlot != 0xFF)
    {
        const MpPeerState *ps = MpNet_GetPeerState(sBattleSession.partnerSlot);
        if (ps != NULL && !ps->connected)
            return false;
    }

    localPos = GetBattlerPosition((enum BattlerId)battlerId);

    for (i = 0; i < MP_BAT_MAX_ACTION_QUEUE; i++)
    {
        struct MpQueuedTurnAction *slot = &sBattleSession.remoteActions[i];
        uint8_t mappedTarget;

        if (!slot->valid)
            continue;
        if (slot->pkt.turnNumber < sBattleSession.turnNumber)
        {
            slot->valid = false;
            continue;
        }
        if (slot->pkt.turnNumber != sBattleSession.turnNumber)
            continue;

        /* Match PvP opponent flank 0->OPPONENT_LEFT, 2->OPPONENT_RIGHT or Co-op partner 0->PLAYER_RIGHT */
        if (sBattleSession.mode == MP_BATTLE_COOP_2V2)
        {
            if (localPos != B_POSITION_PLAYER_RIGHT || slot->pkt.battlerId != 0u)
                continue;
        }
        if (sBattleSession.mode == MP_BATTLE_PVP)
        {
            uint8_t expectedRemoteBattler = (localPos == B_POSITION_OPPONENT_LEFT) ? 0u : 2u;
            if (slot->pkt.battlerId != expectedRemoteBattler)
                continue;
        }

        mappedTarget = RemapRemoteTargetPosition(slot->pkt.targetBattler, sBattleSession.mode);
        gBattleStruct->gimmick.toActivate &= ~(1u << battlerId);

        if ((gAbsentBattlerFlags & (1u << battlerId)) || gBattleStruct->battlerState[battlerId].commandingDondozo)
        {
            gChosenActionByBattler[battlerId] = B_ACTION_NOTHING_FAINTED;
        }
        else if (slot->pkt.actionChoice == B_ACTION_USE_MOVE)
        {
            ApplySyncedMoveChoice((enum BattlerId)battlerId, slot->pkt.moveOrItemSlot,
                                  slot->pkt.itemOrSpecies, (enum BattlerPosition)mappedTarget,
                                  (slot->pkt.gimmickFlags & 1u) != 0);
        }
        else if (slot->pkt.actionChoice == B_ACTION_USE_ITEM)
        {
            uint16_t itemId = slot->pkt.itemOrSpecies;
            if (itemId == ITEM_NONE || itemId >= ITEMS_COUNT)
            {
                slot->valid = false;
                continue;
            }
            ApplySyncedItemChoice((enum BattlerId)battlerId, itemId, slot->pkt.moveOrItemSlot, slot->pkt.targetBattler);
        }
        else if (slot->pkt.actionChoice == B_ACTION_THROW_BALL)
        {
            gChosenActionByBattler[battlerId] = B_ACTION_THROW_BALL;
            if (slot->pkt.itemOrSpecies != ITEM_NONE && slot->pkt.itemOrSpecies < ITEMS_COUNT)
                gBallToDisplay = (enum Item)slot->pkt.itemOrSpecies;
            gBattleStruct->throwingPokeBall = TRUE;
        }
        else if (slot->pkt.actionChoice == B_ACTION_SWITCH)
        {
            if (!ApplySyncedSwitchChoice((enum BattlerId)battlerId, slot->pkt.moveOrItemSlot))
            {
                slot->valid = false;
                return false;
            }
        }
        else if (slot->pkt.actionChoice == B_ACTION_RUN)
        {
            gChosenActionByBattler[battlerId] = B_ACTION_RUN;
            gHitMarker |= HITMARKER_RUN;
        }
        else
        {
            gChosenActionByBattler[battlerId] = slot->pkt.actionChoice;
        }

        if (MpBattle2v2_IsCoopMulti() && !MpBattle2v2_IsInitiator() && slot->pkt.aiValid)
            ApplyRemoteAiChoices(&slot->pkt);

        sBattleSession.remoteAppliedMask |= (uint8_t)(1u << battlerId);
        slot->valid = false;
        return true;
    }

    return false;
}

void MpBattle2v2_OnAllActionsLocked(void)
{
    uint8_t i;
    if (!MpBattle2v2_IsSyncedSession())
        return;

    /* Commit all local player battler actions if any weren't already flushed */
    for (i = 0; i < gBattlersCount; i++)
    {
        if (!MpBattle2v2_IsRemoteControlledBattler(i) && IsOnPlayerSide((enum BattlerId)i))
            MpBattle2v2_CommitLocalAction(i);
    }

    /* Apply any unconsumed remote AI payload for this turn (e.g., if partner disconnected fallback missed it) */
    if (MpBattle2v2_IsCoopMulti() && !MpBattle2v2_IsInitiator() && !sBattleSession.remoteAiAppliedThisTurn)
    {
        uint32_t q;
        for (q = 0; q < MP_BAT_MAX_ACTION_QUEUE; q++)
        {
            struct MpQueuedTurnAction *slot = &sBattleSession.remoteActions[q];
            if (slot->valid && slot->pkt.turnNumber == sBattleSession.turnNumber && slot->pkt.aiValid)
            {
                ApplyRemoteAiChoices(&slot->pkt);
                slot->valid = false;
                break;
            }
        }
    }

    SeedDeterministicBattleRngWithContext(sBattleSession.battleSeed, 0x4C4F434Bu, sBattleSession.turnNumber);
    gBattleStruct->speedTieBreaks = RandomUniform(RNG_SPEED_TIE, 0, Factorial(MAX_BATTLERS_COUNT) - 1);
}

bool MpBattle2v2_ShouldWaitForRemoteSwitch(uint8_t battlerId)
{
    if (!IsPartnerConnectedForSync() || sBattleSession.remoteBattleEnded)
        return false;
    if (battlerId >= gBattlersCount || battlerId >= MAX_BATTLERS_COUNT)
        return false;
    if (MpBattle2v2_IsRemoteControlledBattler(battlerId))
        return true;
    if (MpBattle2v2_IsCoopMulti() && !MpBattle2v2_IsInitiator() && !IsOnPlayerSide((enum BattlerId)battlerId))
        return true;
    return false;
}

void MpBattle2v2_CommitLocalSwitchChoice(uint8_t battlerId, uint8_t caseId, uint8_t chosenMonId)
{
    struct MpBattleSwitchChoicePayload pkt;
    uint16_t targetPeer;

    if (!MpBattle2v2_IsSyncedSession() || sBattleSession.remoteBattleEnded)
        return;
    if (battlerId >= gBattlersCount || battlerId >= MAX_BATTLERS_COUNT)
        return;
    if (MpBattle2v2_ShouldWaitForRemoteSwitch(battlerId))
        return;

    targetPeer = sBattleSession.partnerPeerId;
    if (targetPeer == 0)
        targetPeer = FindPartnerPeerId(NULL);

    memset(&pkt, 0, sizeof(pkt));
    pkt.turnNumber = sBattleSession.turnNumber;
    pkt.switchSeq = sBattleSession.switchSeqByBattler[battlerId]++;
    pkt.battlerPos = GetBattlerPosition((enum BattlerId)battlerId);
    pkt.caseId = caseId;
    pkt.chosenMonId = chosenMonId;
    pkt.rngA = gRngValue.a;
    pkt.rngB = gRngValue.b;
    pkt.rngC = gRngValue.c;
    pkt.rngCtr = gRngValue.ctr;

    MpNet_SendBinary(MP_CH_BATTLE, targetPeer != 0 ? targetPeer : MP_PEER_BROADCAST,
                     MP_PKT_BAT_SWITCH_CHOICE, &pkt, (uint16_t)sizeof(pkt));
}

bool MpBattle2v2_TryConsumeRemoteSwitchChoice(uint8_t battlerId, uint8_t caseId, uint8_t *outChosenMonId)
{
    uint32_t i;
    enum BattlerPosition localPos;
    uint16_t expectedSeq;

    if (!MpBattle2v2_ShouldWaitForRemoteSwitch(battlerId))
        return false;

    localPos = GetBattlerPosition((enum BattlerId)battlerId);
    expectedSeq = sBattleSession.switchSeqByBattler[battlerId];

    for (i = 0; i < MP_BAT_MAX_ACTION_QUEUE; i++)
    {
        struct MpQueuedSwitchChoice *swSlot = &sBattleSession.remoteSwitches[i];
        uint8_t mappedPos;
        u8 chosenMonId;

        if (!swSlot->valid)
            continue;
        mappedPos = RemapRemoteTargetPosition(swSlot->pkt.battlerPos, sBattleSession.mode);
        if (swSlot->pkt.turnNumber < sBattleSession.turnNumber
         || (mappedPos == localPos && swSlot->pkt.switchSeq < expectedSeq))
        {
            swSlot->valid = false;
            continue;
        }
        if (mappedPos != localPos || swSlot->pkt.switchSeq != expectedSeq)
            continue;

        chosenMonId = swSlot->pkt.chosenMonId;
        if (chosenMonId != PARTY_SIZE)
        {
            struct Pokemon *party = GetBattlerParty((enum BattlerId)battlerId);
            if (caseId == PARTY_ACTION_CHOOSE_FAINTED_MON)
            {
                enum Species sp = (chosenMonId < PARTY_SIZE) ? GetMonData(&party[chosenMonId], MON_DATA_SPECIES_OR_EGG) : SPECIES_NONE;
                if (chosenMonId >= PARTY_SIZE || sp == SPECIES_NONE || sp == SPECIES_EGG || GetMonData(&party[chosenMonId], MON_DATA_HP) != 0)
                    chosenMonId = GetFirstFaintedPartyIndex((enum BattlerId)battlerId);
            }
            else
            {
                enum BattlerId partnerBattler = GetPartnerBattler((enum BattlerId)battlerId);
                bool sharePartnerParty = (IsDoubleBattle() && partnerBattler < gBattlersCount
                                       && GetBattlerParty(partnerBattler) == party);
                u8 partnerActiveIdx = (sharePartnerParty && !(gAbsentBattlerFlags & (1u << partnerBattler)))
                                    ? gBattlerPartyIndexes[partnerBattler]
                                    : PARTY_SIZE;
                u8 partnerSwitchIdx = (sharePartnerParty && gSpecialStatuses[partnerBattler].faintedHasReplacement)
                                    ? gBattleStruct->monToSwitchIntoId[partnerBattler]
                                    : PARTY_SIZE;
                if (!IsValidRemoteSwitchSlot(party, battlerId, chosenMonId, sharePartnerParty, partnerActiveIdx, partnerSwitchIdx))
                {
                    u8 alt;
                    chosenMonId = PARTY_SIZE;
                    for (alt = 0; alt < PARTY_SIZE; alt++)
                    {
                        if (IsValidRemoteSwitchSlot(party, battlerId, alt, sharePartnerParty, partnerActiveIdx, partnerSwitchIdx))
                        {
                            chosenMonId = alt;
                            break;
                        }
                    }
                }
            }
        }

        if (MpBattle2v2_IsCoopMulti() && !MpBattle2v2_IsInitiator() && !IsOnPlayerSide((enum BattlerId)battlerId))
        {
            gRngValue.a = swSlot->pkt.rngA;
            gRngValue.b = swSlot->pkt.rngB;
            gRngValue.c = swSlot->pkt.rngC;
            gRngValue.ctr = swSlot->pkt.rngCtr;
        }

        sBattleSession.switchSeqByBattler[battlerId]++;
        swSlot->valid = false;
        if (outChosenMonId != NULL)
            *outChosenMonId = chosenMonId;
        return true;
    }

    return false;
}

void MpBattle2v2_CommitLearnedMove(uint8_t partyIndex, uint8_t moveSlot, uint16_t moveId)
{
    struct MpBattleLearnMovePayload pkt;
    uint16_t targetPeer;

    if (!MpBattle2v2_IsSyncedSession() || sBattleSession.mode == MP_BATTLE_SOLO_2V2 || sBattleSession.remoteBattleEnded)
        return;
    if (partyIndex >= PARTY_SIZE || moveSlot >= MAX_MON_MOVES || moveId == MOVE_NONE || moveId >= MOVES_COUNT)
        return;

    targetPeer = sBattleSession.partnerPeerId;
    if (targetPeer == 0)
        targetPeer = FindPartnerPeerId(NULL);

    memset(&pkt, 0, sizeof(pkt));
    pkt.partyIndex = partyIndex;
    pkt.moveSlot = moveSlot;
    pkt.moveId = moveId;
    pkt.pp = (uint8_t)GetMonData(&gParties[B_TRAINER_PLAYER][partyIndex], MON_DATA_PP1 + moveSlot);
    pkt.ppBonuses = (uint8_t)GetMonData(&gParties[B_TRAINER_PLAYER][partyIndex], MON_DATA_PP_BONUSES);

    MpNet_SendBinary(MP_CH_BATTLE, targetPeer != 0 ? targetPeer : MP_PEER_BROADCAST,
                     MP_PKT_BAT_LEARN_MOVE, &pkt, (uint16_t)sizeof(pkt));
}

void MpBattle2v2_OnBattleEnd(uint8_t battleOutcome)
{
    uint32_t i;
    bool wasCoop = sBattleSession.active && (sBattleSession.mode == MP_BATTLE_COOP_2V2);
    bool wasPvp = sBattleSession.active && (sBattleSession.mode == MP_BATTLE_PVP);
    bool wasBridge = sBattleSession.active && sBattleSession.bridgeLaunched;
    uint16_t syncedTrainer = sBattleSession.active ? sBattleSession.trainerId : 0;

    if (sBattleSession.active && sBattleSession.mode != MP_BATTLE_SOLO_2V2)
    {
        uint16_t targetPeer = sBattleSession.partnerPeerId;
        MpNet_SendBinary(MP_CH_BATTLE, targetPeer != 0 ? targetPeer : MP_PEER_BROADCAST,
                         MP_PKT_BAT_END, &battleOutcome, 1);
    }

    if (sBattleSession.pvpPartySaved)
    {
        /* Restore full pre-PvP party state so Friendly PvP is strictly Nuzlocke death-immune */
        for (i = 0; i < PARTY_SIZE; i++)
            gParties[B_TRAINER_PLAYER][i] = sBattleSession.pvpSavedPlayerParty[i];
        gPartiesCount[B_TRAINER_PLAYER] = sBattleSession.pvpSavedPlayerPartyCount;
        sBattleSession.pvpPartySaved = false;
    }
    else if (wasCoop && battleOutcome == B_OUTCOME_WON && !MpSoulLink_AreRulesActive()
          && MpBattle2v2_CountAlivePlayerMons() == 0)
    {
        /* Partner carried a Co-op victory with all local mons fainted: preserve non-Nuzlocke 1 HP survival. */
        for (i = 0; i < PARTY_SIZE; i++)
        {
            struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
            enum Species sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
            if (sp != SPECIES_NONE && sp != SPECIES_EGG)
            {
                u32 oneHp = 1;
                u32 noStatus = 0;
                SetMonData(mon, MON_DATA_HP, &oneHp);
                SetMonData(mon, MON_DATA_STATUS, &noStatus);
                CalculatePlayerPartyCount();
                break;
            }
        }
    }

    sBattleSession.lastBattleWasPvp = wasPvp;
    sBattleSession.lastBattleWasBridgeSynced = wasBridge;
    sBattleSession.lastSyncedTrainerId = syncedTrainer;
    ResetBattleSessionFields(true);
    MpNet_SetLocalInBattle(false);
}

static bool IsExpectedPartnerSource(uint16_t fromPeerId, uint8_t msgSlot)
{
    uint16_t expectedPeer = sBattleSession.active ? sBattleSession.partnerPeerId : sBattleSession.pendingPartnerPeerId;
    uint8_t expectedSlot = sBattleSession.active ? sBattleSession.partnerSlot : sBattleSession.pendingPartnerSlot;

    if (expectedPeer != 0 && fromPeerId != 0 && fromPeerId != expectedPeer)
        return false;
    if (expectedSlot < MP_MAX_PLAYERS && msgSlot < MP_MAX_PLAYERS && msgSlot != expectedSlot)
        return false;
    return true;
}

void MpBattle2v2_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    MpBattle2v2_Init();
    if (payload == NULL)
        return;

    if (opcode == MP_PKT_BAT_START && len >= sizeof(struct MpBattleStartPayload))
    {
        const struct MpBattleStartPayload *pkt = (const struct MpBattleStartPayload *)payload;
        uint16_t replyPeer = fromPeerId;

        if (replyPeer == 0 && pkt->partnerPeerId != 0)
            replyPeer = pkt->partnerPeerId;
        if (replyPeer == 0 && pkt->partnerSlot < MP_MAX_PLAYERS)
        {
            const MpPeerState *ps = MpNet_GetPeerState(pkt->partnerSlot);
            if (ps != NULL && ps->connected)
                replyPeer = (ps->peerId != 0) ? ps->peerId : (uint16_t)(pkt->partnerSlot + 1u);
        }
        if (replyPeer == 0)
            replyPeer = MP_PEER_BROADCAST;

        if (!MpWorldSync_IsLocalTutorialComplete())
        {
            if (pkt->mode == MP_BATTLE_COOP_2V2 || pkt->mode == MP_BATTLE_PVP)
                SendDeclineBusy(replyPeer, "in_tutorial");
            return;
        }
        if (sBattleSession.active
         && pkt->battleSeed == sBattleSession.battleSeed
         && pkt->mode == sBattleSession.mode
         && IsExpectedPartnerSource(replyPeer, pkt->partnerSlot))
        {
            sBattleSession.partnerSlot = pkt->partnerSlot;
            sBattleSession.partnerPeerId = replyPeer;
            SendLocalPartyChunks(pkt->mode, pkt->battleSeed, replyPeer);
            return;
        }
        if (sBattleSession.busyPromptPending)
        {
            if (pkt->mode == MP_BATTLE_COOP_2V2 || pkt->mode == MP_BATTLE_PVP)
                SendDeclineBusy(replyPeer, "busy");
            return;
        }
        if (sBattleSession.active || gMain.inBattle || sBattleSession.activeFallbackAiPartner)
        {
            if (pkt->mode == MP_BATTLE_COOP_2V2 || pkt->mode == MP_BATTLE_PVP)
                SendDeclineBusy(replyPeer, "in_battle");
            return;
        }
        if (pkt->battleSeed != 0 && pkt->battleSeed == sBattleSession.lastCanceledSeed)
        {
            if (pkt->mode == MP_BATTLE_COOP_2V2 || pkt->mode == MP_BATTLE_PVP)
                SendDeclineBusy(replyPeer, "busy");
            return;
        }
        if (pkt->mode != MP_BATTLE_PVP && pkt->mode != MP_BATTLE_COOP_2V2)
            return;

        if (sBattleSession.pendingLaunch)
        {
            if (sBattleSession.pendingSeed != pkt->battleSeed || !IsExpectedPartnerSource(replyPeer, pkt->partnerSlot))
            {
                SendDeclineBusy(replyPeer, "busy");
                return;
            }
            sBattleSession.pendingPartnerSlot = pkt->partnerSlot;
            sBattleSession.pendingPartnerPeerId = replyPeer;
            if (pkt->seedHash != 0)
            {
                sBattleSession.pendingSeedHash = pkt->seedHash;
                sBattleSession.pendingRandomizerFlags = pkt->randomizerFlags;
                sBattleSession.pendingHasSeedSync = true;
                MpBattle2v2_VerifySyncedSeedBeforeTrainerBuild();
            }
            if (MpOverworld_CanJoinSyncedBattleNow())
            {
                sBattleSession.pendingPartySent = true;
                SendLocalPartyChunks(pkt->mode, pkt->battleSeed, replyPeer);
            }
            return;
        }

        MpOverworld_ForceClearFieldForSyncedBattle();
        ResetRemotePartyCache();
        memset(sBattleSession.remoteActions, 0, sizeof(sBattleSession.remoteActions));
        sBattleSession.remoteDeclinedOrEnded = false;
        sBattleSession.remoteBattleEnded = false;
        sBattleSession.remoteDeclineSlot = 0xFF;
        sBattleSession.remoteDeclineCode = 0;
        sBattleSession.pendingLaunch = true;
        sBattleSession.pendingSeed = pkt->battleSeed;
        sBattleSession.pendingMode = pkt->mode;
        sBattleSession.pendingTrainerId = pkt->trainerId;
        sBattleSession.pendingOpponentB = pkt->species[0];
        sBattleSession.pendingTwoOpponents = (pkt->levels[0] != 0);
        sBattleSession.pendingPartnerSlot = pkt->partnerSlot;
        sBattleSession.pendingPartnerPeerId = replyPeer;
        sBattleSession.pendingOverworldWaitFrames = 0;
        sBattleSession.pendingPartyWaitFrames = 0;
        sBattleSession.pendingFromRemote = true;
        sBattleSession.pendingWaitSent = true;
        if (pkt->seedHash != 0)
        {
            sBattleSession.pendingSeedHash = pkt->seedHash;
            sBattleSession.pendingRandomizerFlags = pkt->randomizerFlags;
            sBattleSession.pendingHasSeedSync = true;
            MpBattle2v2_VerifySyncedSeedBeforeTrainerBuild();
        }
        else
        {
            sBattleSession.pendingSeedHash = 0;
            sBattleSession.pendingRandomizerFlags = 0;
            sBattleSession.pendingHasSeedSync = false;
        }
        if (MpOverworld_CanJoinSyncedBattleNow())
        {
            sBattleSession.pendingPartySent = true;
            SendLocalPartyChunks(pkt->mode, pkt->battleSeed, replyPeer);
        }
        else
        {
            sBattleSession.pendingPartySent = false;
        }
    }
    else if (opcode == MP_PKT_BAT_PARTY_MON && len >= sizeof(struct MpBattlePartyMonPayload))
    {
        const struct MpBattlePartyMonPayload *pkt = (const struct MpBattlePartyMonPayload *)payload;
        if (!sBattleSession.active && !sBattleSession.pendingLaunch)
            return;
        if (sBattleSession.active && sBattleSession.mode == MP_BATTLE_SOLO_2V2)
            return;
        if (sBattleSession.active && pkt->battleSeed != sBattleSession.battleSeed)
            return;
        if (sBattleSession.pendingLaunch && !sBattleSession.active && pkt->battleSeed != sBattleSession.pendingSeed)
            return;
        if (!IsExpectedPartnerSource(fromPeerId, pkt->slotIndex))
            return;
        if (pkt->monIndex < PARTY_SIZE)
        {
            if (MpTrade_DeserializePokemon(pkt->monBytes, &sBattleSession.remoteParty[pkt->monIndex]))
            {
                uint8_t expectedCount = (pkt->totalMons > PARTY_SIZE) ? PARTY_SIZE : pkt->totalMons;
                uint8_t expectedMask = (expectedCount == 0u) ? 0u : (uint8_t)((1u << expectedCount) - 1u);

                sBattleSession.remotePartyMask |= (uint8_t)(1u << pkt->monIndex);
                if (pkt->monIndex + 1 > sBattleSession.remotePartyCount)
                    sBattleSession.remotePartyCount = pkt->monIndex + 1;
                if (expectedCount == 0u || (sBattleSession.remotePartyMask & expectedMask) == expectedMask)
                    sBattleSession.remotePartyReady = true;

                if (sBattleSession.active && sBattleSession.mode != MP_BATTLE_SOLO_2V2)
                {
                    if (sBattleSession.mode == MP_BATTLE_COOP_2V2)
                    {
                        gParties[B_TRAINER_PARTNER][pkt->monIndex] = sBattleSession.remoteParty[pkt->monIndex];
                        gPartiesCount[B_TRAINER_PARTNER] = CalculatePartyCount(B_TRAINER_PARTNER);
                    }
                    else if (sBattleSession.mode == MP_BATTLE_PVP)
                    {
                        gParties[B_TRAINER_OPPONENT_A][pkt->monIndex] = sBattleSession.remoteParty[pkt->monIndex];
                        gPartiesCount[B_TRAINER_OPPONENT_A] = CalculatePartyCount(B_TRAINER_OPPONENT_A);
                    }
                }
            }
        }
    }
    else if (opcode == MP_PKT_BAT_TURN_ACTION && len >= sizeof(struct MpBattleTurnActionPayload))
    {
        const struct MpBattleTurnActionPayload *pkt = (const struct MpBattleTurnActionPayload *)payload;
        uint32_t i;
        if (!sBattleSession.active || sBattleSession.mode == MP_BATTLE_SOLO_2V2)
            return;
        if (!IsExpectedPartnerSource(fromPeerId, 0xFF))
            return;
        for (i = 0; i < MP_BAT_MAX_ACTION_QUEUE; i++)
        {
            if (sBattleSession.remoteActions[i].valid
             && sBattleSession.remoteActions[i].pkt.turnNumber == pkt->turnNumber
             && sBattleSession.remoteActions[i].pkt.battlerId == pkt->battlerId)
            {
                sBattleSession.remoteActions[i].pkt = *pkt;
                return;
            }
        }
        for (i = 0; i < MP_BAT_MAX_ACTION_QUEUE; i++)
        {
            if (!sBattleSession.remoteActions[i].valid)
            {
                sBattleSession.remoteActions[i].valid = true;
                sBattleSession.remoteActions[i].pkt = *pkt;
                break;
            }
        }
    }
    else if (opcode == MP_PKT_BAT_SWITCH_CHOICE && len >= sizeof(struct MpBattleSwitchChoicePayload))
    {
        const struct MpBattleSwitchChoicePayload *pkt = (const struct MpBattleSwitchChoicePayload *)payload;
        uint32_t i;
        if (!sBattleSession.active || sBattleSession.mode == MP_BATTLE_SOLO_2V2)
            return;
        if (!IsExpectedPartnerSource(fromPeerId, 0xFF))
            return;
        for (i = 0; i < MP_BAT_MAX_ACTION_QUEUE; i++)
        {
            if (sBattleSession.remoteSwitches[i].valid
             && sBattleSession.remoteSwitches[i].pkt.turnNumber == pkt->turnNumber
             && sBattleSession.remoteSwitches[i].pkt.switchSeq == pkt->switchSeq
             && sBattleSession.remoteSwitches[i].pkt.battlerPos == pkt->battlerPos)
            {
                sBattleSession.remoteSwitches[i].pkt = *pkt;
                return;
            }
        }
        for (i = 0; i < MP_BAT_MAX_ACTION_QUEUE; i++)
        {
            if (!sBattleSession.remoteSwitches[i].valid)
            {
                sBattleSession.remoteSwitches[i].valid = true;
                sBattleSession.remoteSwitches[i].pkt = *pkt;
                break;
            }
        }
    }
    else if (opcode == MP_PKT_BAT_LEARN_MOVE && len >= sizeof(struct MpBattleLearnMovePayload))
    {
        const struct MpBattleLearnMovePayload *pkt = (const struct MpBattleLearnMovePayload *)payload;
        enum BattleTrainer remoteTrainer;
        u16 moveVal;
        u8 ppVal;
        u8 bonusesVal;
        u32 b;

        if (!sBattleSession.active || sBattleSession.mode == MP_BATTLE_SOLO_2V2)
            return;
        if (!IsExpectedPartnerSource(fromPeerId, 0xFF))
            return;
        if (pkt->partyIndex >= PARTY_SIZE || pkt->moveSlot >= MAX_MON_MOVES
         || pkt->moveId == MOVE_NONE || pkt->moveId >= MOVES_COUNT)
            return;

        remoteTrainer = (sBattleSession.mode == MP_BATTLE_COOP_2V2) ? B_TRAINER_PARTNER : B_TRAINER_OPPONENT_A;
        moveVal = pkt->moveId;
        ppVal = pkt->pp;
        bonusesVal = pkt->ppBonuses;

        SetMonData(&gParties[remoteTrainer][pkt->partyIndex], MON_DATA_MOVE1 + pkt->moveSlot, &moveVal);
        SetMonData(&gParties[remoteTrainer][pkt->partyIndex], MON_DATA_PP_BONUSES, &bonusesVal);
        SetMonData(&gParties[remoteTrainer][pkt->partyIndex], MON_DATA_PP1 + pkt->moveSlot, &ppVal);
        sBattleSession.remoteParty[pkt->partyIndex] = gParties[remoteTrainer][pkt->partyIndex];

        for (b = 0; b < gBattlersCount && b < MAX_BATTLERS_COUNT; b++)
        {
            if (MpBattle2v2_IsRemoteControlledBattler((uint8_t)b)
             && gBattlerPartyIndexes[b] == pkt->partyIndex
             && !gBattleMons[b].volatiles.transformed
             && MOVE_IS_PERMANENT(b, pkt->moveSlot))
            {
                gBattleMons[b].moves[pkt->moveSlot] = (enum Move)moveVal;
                gBattleMons[b].pp[pkt->moveSlot] = ppVal;
                gBattleMons[b].ppBonuses = bonusesVal;
            }
        }
    }
    else if (opcode == MP_PKT_BAT_DECLINE_BUSY)
    {
        const uint8_t *bytes = (const uint8_t *)payload;
        uint8_t busySlot = (len >= 1) ? bytes[0]
                         : (sBattleSession.active ? sBattleSession.partnerSlot : sBattleSession.pendingPartnerSlot);
        uint8_t busyCode = (len >= 2) ? bytes[1] : 0u;
        const char *reason;

        if (!sBattleSession.active && !sBattleSession.pendingLaunch)
            return;
        if (!IsExpectedPartnerSource(fromPeerId, (len >= 1) ? busySlot : 0xFF))
            return;

        if (sBattleSession.active)
        {
            if (sBattleSession.mode == MP_BATTLE_SOLO_2V2)
                return;
            if (!sBattleSession.remotePartyReady)
            {
                sBattleSession.remoteDeclinedOrEnded = true;
                sBattleSession.remoteDeclineSlot = busySlot;
                sBattleSession.remoteDeclineCode = busyCode;
                return;
            }
            if (sBattleSession.mode == MP_BATTLE_COOP_2V2)
            {
                reason = ResolveDeclineReason(busyCode, busySlot);
#ifdef WASM_PLATFORM
                if (busySlot < MP_MAX_PLAYERS)
                    WasmMp_NotifyTeammateBusy((int)busySlot, reason);
#endif
                sBattleSession.mode = MP_BATTLE_SOLO_2V2;
                sBattleSession.activeFallbackAiPartner = true;
                sBattleSession.remoteBattleEnded = true;
                sBattleSession.partnerPeerId = 0;
                if (busySlot < MP_MAX_PLAYERS)
                    sBattleSession.partnerSlot = busySlot;
            }
            return;
        }

        /* sBattleSession.pendingLaunch && !sBattleSession.active */
        reason = ResolveDeclineReason(busyCode, busySlot);
#ifdef WASM_PLATFORM
        if (busySlot < MP_MAX_PLAYERS)
            WasmMp_NotifyTeammateBusy((int)busySlot, reason);
#endif
        if (sBattleSession.pendingMode == MP_BATTLE_COOP_2V2 && !sBattleSession.pendingFromRemote)
        {
            ResetRemotePartyCache();
            sBattleSession.pendingMode = MP_BATTLE_SOLO_2V2;
            sBattleSession.pendingCoopFallback = true;
            sBattleSession.pendingPartnerSlot = 0xFF;
            sBattleSession.pendingPartnerPeerId = 0;
            sBattleSession.pendingWaitSent = false;
            sBattleSession.pendingPartySent = false;
        }
        else
        {
            uint32_t canceledSeed = sBattleSession.pendingSeed;
            ResetBattleSessionFields(false);
            sBattleSession.lastCanceledSeed = canceledSeed;
            sBattleSession.lastCanceledSeedTtl = 180u;
        }
    }
    else if (opcode == MP_PKT_BAT_END)
    {
        if (!IsExpectedPartnerSource(fromPeerId, 0xFF))
            return;
        if (sBattleSession.active)
        {
            if (sBattleSession.mode == MP_BATTLE_SOLO_2V2)
                return;
            if (!sBattleSession.remotePartyReady)
            {
                sBattleSession.remoteDeclinedOrEnded = true;
                sBattleSession.remoteDeclineSlot = sBattleSession.partnerSlot;
                sBattleSession.remoteDeclineCode = 0xFFu;
            }
            else
            {
                sBattleSession.remoteBattleEnded = true;
            }
            return;
        }
        if (sBattleSession.pendingLaunch)
        {
            if (sBattleSession.pendingMode == MP_BATTLE_COOP_2V2 && !sBattleSession.pendingFromRemote)
            {
                uint8_t notifySlot = sBattleSession.pendingPartnerSlot;
                const char *reason = ResolveDeclineReason(0u, notifySlot);
#ifdef WASM_PLATFORM
                if (notifySlot < MP_MAX_PLAYERS)
                    WasmMp_NotifyTeammateBusy((int)notifySlot, reason);
#endif
                ResetRemotePartyCache();
                sBattleSession.pendingMode = MP_BATTLE_SOLO_2V2;
                sBattleSession.pendingCoopFallback = true;
                sBattleSession.pendingPartnerSlot = 0xFF;
                sBattleSession.pendingPartnerPeerId = 0;
                sBattleSession.pendingWaitSent = false;
                sBattleSession.pendingPartySent = false;
            }
            else
            {
                uint32_t canceledSeed = sBattleSession.pendingSeed;
                ResetBattleSessionFields(false);
                sBattleSession.lastCanceledSeed = canceledSeed;
                sBattleSession.lastCanceledSeedTtl = 180u;
            }
        }
    }
}

void MpBattle2v2_StartSyncedFromBridge(uint32_t battleSeed, uint8_t mode, uint8_t partnerSlot, uint16_t partnerPeerId)
{
    /* net_bridge.js passes canonical MpBattleMode: 0=SOLO_2V2, 1=COOP_2V2, 2=PVP */
    uint8_t resolvedMode = (mode == MP_BATTLE_SOLO_2V2 || mode == MP_BATTLE_COOP_2V2 || mode == MP_BATTLE_PVP)
                         ? mode
                         : (uint8_t)MP_BATTLE_COOP_2V2;
    uint8_t resolvedSlot = (partnerSlot < MP_MAX_PLAYERS) ? partnerSlot : 0xFFu;
    uint16_t resolvedPeer = partnerPeerId;
    bool coopFallback = false;

    MpBattle2v2_Init();
    if (!MpWorldSync_IsLocalTutorialComplete())
        return;
    if (gMain.inBattle || sBattleSession.active || sBattleSession.activeFallbackAiPartner || sBattleSession.busyPromptPending)
        return;

    if (resolvedMode == MP_BATTLE_COOP_2V2)
    {
        uint8_t coopPartner = (resolvedSlot < MP_MAX_PLAYERS) ? resolvedSlot : MpNet_GetCoopPartnerSlot();
        if (coopPartner >= MP_MAX_PLAYERS)
            coopPartner = MpNet_FindExplicitPairSlot();
        resolvedSlot = coopPartner;
        if (coopPartner == 0xFF)
        {
            resolvedMode = MP_BATTLE_SOLO_2V2;
            resolvedPeer = 0;
        }
        else
        {
            const char *busyReason = MpOverworld_GetPeerBusyReason(coopPartner);
            if (busyReason != NULL)
            {
#ifdef WASM_PLATFORM
                WasmMp_NotifyTeammateBusy((int)coopPartner, busyReason);
#endif
                coopFallback = true;
                resolvedMode = MP_BATTLE_SOLO_2V2;
                resolvedSlot = 0xFF;
                resolvedPeer = 0;
            }
        }
    }
    if (resolvedMode == MP_BATTLE_PVP)
    {
        uint8_t pvpSlot = (resolvedSlot < MP_MAX_PLAYERS) ? resolvedSlot : 0xFFu;
        if (pvpSlot >= MP_MAX_PLAYERS)
            FindPartnerPeerId(&pvpSlot);
        if (pvpSlot < MP_MAX_PLAYERS)
        {
            const char *busyReason = MpOverworld_GetPeerBusyReason(pvpSlot);
            if (busyReason != NULL
             && (strcmp(busyReason, "in_tutorial") == 0
              || strcmp(busyReason, "in_battle") == 0
              || strcmp(busyReason, "not_ready") == 0))
            {
#ifdef WASM_PLATFORM
                WasmMp_NotifyTeammateBusy((int)pvpSlot, busyReason);
#endif
                return;
            }
        }
    }
    if (resolvedMode != MP_BATTLE_SOLO_2V2 && resolvedSlot < MP_MAX_PLAYERS && resolvedPeer == 0)
    {
        const MpPeerState *ps = MpNet_GetPeerState(resolvedSlot);
        if (ps != NULL && ps->connected)
            resolvedPeer = (ps->peerId != 0) ? ps->peerId : (uint16_t)(resolvedSlot + 1u);
    }
    if (resolvedMode != MP_BATTLE_SOLO_2V2 && resolvedSlot >= MP_MAX_PLAYERS && resolvedPeer == 0)
        resolvedPeer = FindPartnerPeerId(&resolvedSlot);

    uint32_t resolvedSeed = (battleSeed != 0) ? battleSeed : 0x20260401u;
    if ((sBattleSession.pendingLaunch && sBattleSession.pendingSeed == resolvedSeed)
     || (sBattleSession.active && sBattleSession.battleSeed == resolvedSeed))
    {
        return;
    }
    ResetRemotePartyCache();
    sBattleSession.pendingSeed = resolvedSeed;
    sBattleSession.pendingMode = resolvedMode;
    sBattleSession.pendingTrainerId = 0;
    sBattleSession.pendingOpponentB = 0xFFFF;
    sBattleSession.pendingTwoOpponents = false;
    sBattleSession.pendingPartnerSlot = (resolvedMode == MP_BATTLE_SOLO_2V2) ? 0xFFu : resolvedSlot;
    sBattleSession.pendingPartnerPeerId = (resolvedMode == MP_BATTLE_SOLO_2V2) ? 0u : resolvedPeer;
    sBattleSession.pendingOverworldWaitFrames = 0;
    sBattleSession.pendingPartyWaitFrames = 0;
    sBattleSession.pendingWaitSent = false;
    sBattleSession.pendingPartySent = false;
    sBattleSession.pendingFromRemote = false;
    sBattleSession.pendingCoopFallback = coopFallback;
    sBattleSession.remoteDeclinedOrEnded = false;
    sBattleSession.remoteBattleEnded = false;
    sBattleSession.remoteDeclineSlot = 0xFF;
    sBattleSession.remoteDeclineCode = 0;
    sBattleSession.pendingLaunch = true;
}
