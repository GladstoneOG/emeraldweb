#include "global.h"
#include "battle.h"
#include "data.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "item.h"
#include "main.h"
#include "money.h"
#include "overworld.h"
#include "pokedex.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "string_util.h"
#include "constants/abilities.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#ifdef WASM_PLATFORM
#include "wasm_platform.h"
#endif
#include "mp_randomizer.h"
#include "mp_soul_link.h"
#include "mp_trade.h"
#include "mp_world_sync.h"

#define MP_TRADE_HISTORY_CAP 16u
#define MP_TRADE_PENDING_CAP 8u
#define MP_TRADE_WIRE_TAG    0x5452u /* 'TR' */

static struct MpTradeSession sTradeSession;
static struct MpTradeCommitPayload sPendingCommits[MP_TRADE_PENDING_CAP];
static uint8_t sPendingHead = 0;
static uint8_t sPendingTail = 0;
static uint32_t sCommittedSessions[MP_TRADE_HISTORY_CAP];
static uint8_t sCommittedCursor = 0;
static bool sTradeInitialized = false;

static void QueuePendingCommit(const struct MpTradeCommitPayload *commit)
{
    uint8_t nextTail;
    if (commit == NULL)
        return;
    nextTail = (uint8_t)((sPendingTail + 1u) % MP_TRADE_PENDING_CAP);
    if (nextTail == sPendingHead)
        sPendingHead = (uint8_t)((sPendingHead + 1u) % MP_TRADE_PENDING_CAP);
    sPendingCommits[sPendingTail] = *commit;
    sPendingTail = nextTail;
    sTradeSession.pendingCommit = *commit;
    sTradeSession.hasPendingCommit = true;
}

static bool PopPendingCommit(struct MpTradeCommitPayload *outCommit)
{
    if (sPendingHead == sPendingTail)
    {
        sTradeSession.hasPendingCommit = false;
        return false;
    }
    if (outCommit != NULL)
        *outCommit = sPendingCommits[sPendingHead];
    sPendingHead = (uint8_t)((sPendingHead + 1u) % MP_TRADE_PENDING_CAP);
    sTradeSession.hasPendingCommit = (sPendingHead != sPendingTail);
    if (sTradeSession.hasPendingCommit)
        sTradeSession.pendingCommit = sPendingCommits[sPendingHead];
    return true;
}

static uint16_t ComputeBytesCrc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    uint32_t bit;
    for (i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i];
        for (bit = 0; bit < 8; bit++)
        {
            if (crc & 1u)
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else
                crc >>= 1;
        }
    }
    return crc;
}

static bool WasSessionAlreadyCommitted(uint32_t sessionId)
{
    uint32_t i;
    if (sessionId == 0)
        return false;
    for (i = 0; i < MP_TRADE_HISTORY_CAP; i++)
    {
        if (sCommittedSessions[i] == sessionId)
            return true;
    }
    return false;
}

static void MarkSessionCommitted(uint32_t sessionId)
{
    if (sessionId == 0)
        return;
    sCommittedSessions[sCommittedCursor % MP_TRADE_HISTORY_CAP] = sessionId;
    sCommittedCursor++;
}

static bool IsTradableItem(uint16_t itemId)
{
    if (itemId == ITEM_NONE)
        return true;
    return MpRandomizer_IsItemRandomizable(itemId);
}

bool MpTrade_SerializePokemon(const struct Pokemon *mon, uint8_t outBytes[MP_SERIALIZED_MON_BYTES])
{
    uint16_t species;
    uint32_t personality;
    uint32_t otId;
    uint32_t exp;
    uint32_t ivs;
    uint32_t status;
    uint16_t heldItem;
    uint16_t hp;
    uint16_t maxHp;
    uint8_t level;
    uint8_t flagsByte;
    uint16_t metLoc;
    uint16_t tag = MP_TRADE_WIRE_TAG;
    uint16_t crc;
    u8 nick[POKEMON_NAME_LENGTH + 1];
    u8 otName[PLAYER_NAME_LENGTH + 1];
    uint32_t i;

    if (outBytes == NULL)
        return false;
    memset(outBytes, 0, MP_SERIALIZED_MON_BYTES);
    if (mon == NULL)
        return false;

    species = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_SPECIES);
    if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES)
        return false;

    personality = GetMonData((struct Pokemon *)mon, MON_DATA_PERSONALITY);
    otId = GetMonData((struct Pokemon *)mon, MON_DATA_OT_ID);
    exp = GetMonData((struct Pokemon *)mon, MON_DATA_EXP);
    ivs = GetMonData((struct Pokemon *)mon, MON_DATA_IVS);
    status = GetMonData((struct Pokemon *)mon, MON_DATA_STATUS);
    heldItem = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_HELD_ITEM);
    hp = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_HP);
    maxHp = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_MAX_HP);
    level = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_LEVEL);
    metLoc = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_MET_LOCATION);

    flagsByte = 0;
    if (GetMonData((struct Pokemon *)mon, MON_DATA_IS_SHINY))
        flagsByte |= 1u << 0;
    if (GetMonData((struct Pokemon *)mon, MON_DATA_OT_GENDER))
        flagsByte |= 1u << 1;

    GetMonData((struct Pokemon *)mon, MON_DATA_NICKNAME, nick);
    GetMonData((struct Pokemon *)mon, MON_DATA_OT_NAME, otName);

    memcpy(&outBytes[0], &personality, 4);
    memcpy(&outBytes[4], &otId, 4);
    memcpy(&outBytes[8], &species, 2);
    memcpy(&outBytes[10], &heldItem, 2);
    memcpy(&outBytes[12], &exp, 4);
    memcpy(&outBytes[16], &ivs, 4);
    memcpy(&outBytes[20], &status, 4);
    memcpy(&outBytes[24], &hp, 2);
    memcpy(&outBytes[26], &maxHp, 2);
    outBytes[28] = level;
    outBytes[29] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_ABILITY_NUM);
    outBytes[30] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_FRIENDSHIP);
    outBytes[31] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_PP_BONUSES);
    outBytes[32] = flagsByte;
    outBytes[33] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_POKEBALL);
    outBytes[34] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_MET_LEVEL);
    outBytes[35] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_POKERUS);
    memcpy(&outBytes[36], &metLoc, 2);
    outBytes[38] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_TERA_TYPE);
    outBytes[39] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_MARKINGS);

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        uint16_t mv = (uint16_t)GetMonData((struct Pokemon *)mon, MON_DATA_MOVE1 + i);
        uint8_t pp = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_PP1 + i);
        memcpy(&outBytes[40 + i * 2], &mv, 2);
        outBytes[48 + i] = pp;
    }

    for (i = 0; i < NUM_STATS; i++)
        outBytes[52 + i] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_HP_EV + i);

    memcpy(&outBytes[58], nick, min((size_t)(POKEMON_NAME_LENGTH + 1), (size_t)12));
    memcpy(&outBytes[70], otName, min((size_t)(PLAYER_NAME_LENGTH + 1), (size_t)8));
    outBytes[78] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_LANGUAGE);
    outBytes[79] = (uint8_t)GetMonData((struct Pokemon *)mon, MON_DATA_MET_GAME);

    memcpy(&outBytes[96], &tag, 2);
    crc = ComputeBytesCrc16(outBytes, 98);
    memcpy(&outBytes[98], &crc, 2);
    return true;
}

bool MpTrade_DeserializePokemon(const uint8_t inBytes[MP_SERIALIZED_MON_BYTES], struct Pokemon *outMon)
{
    uint16_t tag;
    uint16_t expectedCrc;
    uint16_t actualCrc;
    uint32_t personality;
    uint32_t otId;
    uint16_t species;
    uint16_t heldItem;
    uint32_t exp;
    uint32_t ivs;
    uint32_t status;
    uint16_t hp;
    uint16_t maxHp;
    uint8_t level;
    uint8_t abilityNum;
    uint8_t friendship;
    uint8_t ppBonuses;
    uint8_t flagsByte;
    uint8_t ball;
    uint8_t metLevel;
    uint8_t pokerus;
    uint16_t metLoc;
    uint8_t teraType;
    uint8_t markings;
    uint8_t lang;
    uint8_t metGame;
    u8 nick[12];
    u8 otName[8];
    uint32_t i;

    if (inBytes == NULL || outMon == NULL)
        return false;

    memcpy(&tag, &inBytes[96], 2);
    memcpy(&expectedCrc, &inBytes[98], 2);
    if (tag != MP_TRADE_WIRE_TAG)
        return false;
    actualCrc = ComputeBytesCrc16(inBytes, 98);
    if (actualCrc != expectedCrc)
        return false;

    memcpy(&species, &inBytes[8], 2);
    if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES)
        return false;

    memcpy(&personality, &inBytes[0], 4);
    memcpy(&otId, &inBytes[4], 4);
    memcpy(&heldItem, &inBytes[10], 2);
    memcpy(&exp, &inBytes[12], 4);
    memcpy(&ivs, &inBytes[16], 4);
    memcpy(&status, &inBytes[20], 4);
    memcpy(&hp, &inBytes[24], 2);
    memcpy(&maxHp, &inBytes[26], 2);
    level = (inBytes[28] == 0) ? 1u : inBytes[28];
    abilityNum = inBytes[29];
    friendship = inBytes[30];
    ppBonuses = inBytes[31];
    flagsByte = inBytes[32];
    ball = inBytes[33];
    metLevel = inBytes[34];
    pokerus = inBytes[35];
    memcpy(&metLoc, &inBytes[36], 2);
    teraType = inBytes[38];
    markings = inBytes[39];
    lang = inBytes[78];
    metGame = inBytes[79];

    MpRandomizer_PushSuppressStatic();
    CreateMon(outMon, (enum Species)species, level, personality, OTID_STRUCT_PRESET(otId));
    MpRandomizer_PopSuppressStatic();

    SetMonData(outMon, MON_DATA_IVS, &ivs);
    SetMonData(outMon, MON_DATA_EXP, &exp);
    SetMonData(outMon, MON_DATA_HELD_ITEM, &heldItem);
    SetMonData(outMon, MON_DATA_ABILITY_NUM, &abilityNum);
    SetMonData(outMon, MON_DATA_FRIENDSHIP, &friendship);
    SetMonData(outMon, MON_DATA_PP_BONUSES, &ppBonuses);
    SetMonData(outMon, MON_DATA_POKEBALL, &ball);
    SetMonData(outMon, MON_DATA_MET_LEVEL, &metLevel);
    SetMonData(outMon, MON_DATA_POKERUS, &pokerus);
    SetMonData(outMon, MON_DATA_MET_LOCATION, &metLoc);
    SetMonData(outMon, MON_DATA_TERA_TYPE, &teraType);
    SetMonData(outMon, MON_DATA_MARKINGS, &markings);
    if (lang != 0)
        SetMonData(outMon, MON_DATA_LANGUAGE, &lang);
    if (metGame != 0)
        SetMonData(outMon, MON_DATA_MET_GAME, &metGame);

    {
        u32 shiny = (flagsByte & 1u) ? 1u : 0u;
        u32 otGender = (flagsByte & 2u) ? 1u : 0u;
        SetMonData(outMon, MON_DATA_IS_SHINY, &shiny);
        SetMonData(outMon, MON_DATA_OT_GENDER, &otGender);
    }

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        uint16_t mv;
        uint8_t pp;
        memcpy(&mv, &inBytes[40 + i * 2], 2);
        pp = inBytes[48 + i];
        SetMonData(outMon, MON_DATA_MOVE1 + i, &mv);
        SetMonData(outMon, MON_DATA_PP1 + i, &pp);
    }
    for (i = 0; i < NUM_STATS; i++)
    {
        u32 ev = inBytes[52 + i];
        SetMonData(outMon, MON_DATA_HP_EV + i, &ev);
    }

    memcpy(nick, &inBytes[58], sizeof(nick));
    memcpy(otName, &inBytes[70], sizeof(otName));
    SetMonData(outMon, MON_DATA_NICKNAME, nick);
    SetMonData(outMon, MON_DATA_OT_NAME, otName);

    CalculateMonStats(outMon);
    if (maxHp > 0)
    {
        u32 clampedHp = min((u32)hp, GetMonData(outMon, MON_DATA_MAX_HP));
        SetMonData(outMon, MON_DATA_HP, &clampedHp);
    }
    SetMonData(outMon, MON_DATA_STATUS, &status);
    return true;
}

static int FindPartySlotMatchingOffer(const struct MpTradeOfferPayload *offer)
{
    uint32_t otId;
    uint32_t personality;
    uint32_t i;

    if (offer == NULL || !offer->hasMon)
        return -1;

    memcpy(&personality, &offer->monBytes[0], 4);
    memcpy(&otId, &offer->monBytes[4], 4);

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(mon, MON_DATA_IS_EGG))
            continue;
        if (GetMonData(mon, MON_DATA_OT_ID) == otId && GetMonData(mon, MON_DATA_PERSONALITY) == personality)
            return (int)i;
    }
    return -1;
}

static u32 CountAliveNonEggPartyMonsExcludingSlot(int excludeIdx)
{
    u32 count = 0;
    u32 i;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon;
        enum Species sp;
        if ((int)i == excludeIdx)
            continue;
        mon = &gParties[B_TRAINER_PLAYER][i];
        sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(mon, MON_DATA_HP) > 0)
            count++;
    }
    return count;
}

void MpTrade_Init(void)
{
    if (sTradeInitialized)
        return;
    memset(&sTradeSession, 0, sizeof(sTradeSession));
    memset(sCommittedSessions, 0, sizeof(sCommittedSessions));
    sCommittedCursor = 0;
    sTradeSession.offeredPartyIndex = PARTY_SIZE;
    sTradeInitialized = true;
    MpNet_RegisterHandler(MP_CH_TRADE, MpTrade_HandlePacket);
}

int MpTrade_BuildOffer(uint32_t sessionId, uint8_t partyIndex, uint16_t itemId, uint16_t itemQty, uint32_t money, struct MpTradeOfferPayload *outOffer)
{
    if (outOffer == NULL || !MpWorldSync_IsLocalTutorialComplete())
        return MP_TRADE_ERR_INVALID;

    memset(outOffer, 0, sizeof(*outOffer));
    outOffer->sessionId = sessionId;
    outOffer->fromSlot = MpNet_GetLocalSlot();
    outOffer->mapsec = MP_MAPSEC_INVALID;

    if (partyIndex < PARTY_SIZE)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][partyIndex];
        const MpSoulLinkEntry *sl;
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(mon, MON_DATA_IS_EGG))
            return MP_TRADE_ERR_NO_MON;
        if (!MpTrade_SerializePokemon(mon, outOffer->monBytes))
            return MP_TRADE_ERR_INVALID;
        outOffer->hasMon = 1;
        sl = MpSoulLink_LookupMon(mon);
        if (sl != NULL)
        {
            outOffer->linkId = sl->linkId;
            outOffer->mapsec = sl->originMapsec;
        }
        else
        {
            outOffer->mapsec = (uint16_t)GetMonData(mon, MON_DATA_MET_LOCATION);
        }
    }

    if (itemId != ITEM_NONE && itemQty > 0)
    {
        if (!IsTradableItem(itemId))
            return MP_TRADE_ERR_KEY_OR_HM;
        if (!CheckBagHasItem((enum Item)itemId, itemQty))
            return MP_TRADE_ERR_NO_ITEM;
        outOffer->itemId = itemId;
        outOffer->itemQty = itemQty;
    }

    if (money > 0)
    {
        if (!IsEnoughMoney(&gSaveBlock1Ptr->money, money))
            return MP_TRADE_ERR_NO_MONEY;
        outOffer->money = money;
    }

    return MP_TRADE_OK;
}

bool MpTrade_SendOffer(uint16_t partnerPeerId, uint32_t sessionId, uint8_t partyIndex, uint16_t itemId, uint16_t itemQty, uint32_t money)
{
    struct MpTradeOfferPayload offer;
    int err;

    MpTrade_Init();
    if (!MpWorldSync_IsLocalTutorialComplete())
        return false;
    err = MpTrade_BuildOffer(sessionId, partyIndex, itemId, itemQty, money, &offer);
    if (err != MP_TRADE_OK)
        return false;

    sTradeSession.active = true;
    sTradeSession.sessionId = sessionId;
    sTradeSession.partnerPeerId = partnerPeerId;
    sTradeSession.offeredPartyIndex = partyIndex;
    sTradeSession.hasLocalOffer = true;
    sTradeSession.localLocked = false;
    sTradeSession.remoteLocked = false;
    sTradeSession.localOffer = offer;

    return MpNet_SendBinary(MP_CH_TRADE, partnerPeerId, MP_PKT_TRD_OFFER, &offer, (uint16_t)sizeof(offer));
}

bool MpTrade_SetLockAndSend(uint16_t partnerPeerId, bool locked)
{
    MpTrade_Init();
    if (!MpWorldSync_IsLocalTutorialComplete() || !sTradeSession.hasLocalOffer)
        return false;

    sTradeSession.localLocked = locked;
    sTradeSession.localOffer.locked = locked ? 1u : 0u;
    return MpNet_SendBinary(MP_CH_TRADE, partnerPeerId, MP_PKT_TRD_LOCK,
                            &sTradeSession.localOffer, (uint16_t)sizeof(sTradeSession.localOffer));
}

bool MpTrade_SubmitCommitToPeer(uint16_t partnerPeerId)
{
    struct MpTradeCommitPayload commit;
    int res;

    MpTrade_Init();
    if (!MpWorldSync_IsLocalTutorialComplete())
        return false;
    if (!sTradeSession.hasLocalOffer || !sTradeSession.hasRemoteOffer)
        return false;
    if (!sTradeSession.localLocked || !sTradeSession.remoteLocked)
        return false;

    memset(&commit, 0, sizeof(commit));
    commit.sessionId = sTradeSession.sessionId;
    commit.slotA = sTradeSession.localOffer.fromSlot;
    commit.slotB = sTradeSession.remoteOffer.fromSlot;
    commit.offerA = sTradeSession.localOffer;
    commit.offerB = sTradeSession.remoteOffer;

    res = MpTrade_ApplyCommit(&commit);
    if (res != MP_TRADE_OK)
        return false;

    return MpNet_SendBinary(MP_CH_TRADE, partnerPeerId, MP_PKT_TRD_COMMIT, &commit, (uint16_t)sizeof(commit));
}

uint16_t MpTrade_ApplyTradeEvolution(struct Pokemon *mon, struct Pokemon *partnerMon)
{
    enum Species oldSpecies;
    enum Species evoTarget;
    bool32 canStopEvo = FALSE;
    u8 nick[POKEMON_NAME_LENGTH + 1];

    if (mon == NULL)
        return SPECIES_NONE;

    oldSpecies = GetMonData(mon, MON_DATA_SPECIES);
    if (oldSpecies == SPECIES_NONE || oldSpecies == SPECIES_EGG)
        return SPECIES_NONE;

    /* Run canonical pokeemerald-expansion trade evolution check (which includes held items & randomizer hook) */
    evoTarget = GetEvolutionTargetSpecies(mon, EVO_MODE_TRADE, ITEM_NONE, partnerMon, &canStopEvo, CHECK_EVO);
    if (evoTarget == SPECIES_NONE || evoTarget == oldSpecies)
        return SPECIES_NONE;

    GetEvolutionTargetSpecies(mon, EVO_MODE_TRADE, ITEM_NONE, partnerMon, &canStopEvo, DO_EVO);
    GetMonData(mon, MON_DATA_NICKNAME, nick);

    SetMonData(mon, MON_DATA_SPECIES, &evoTarget);
    if (StringCompare(nick, gSpeciesInfo[oldSpecies].speciesName) == 0)
        SetMonData(mon, MON_DATA_NICKNAME, gSpeciesInfo[evoTarget].speciesName);

    CalculateMonStats(mon);
    MonTryLearningNewMoveEvolution(mon, FALSE);
    HandleSetPokedexFlagFromMon(mon, FLAG_SET_CAUGHT);
    return (uint16_t)evoTarget;
}

int MpTrade_ApplyCommit(const struct MpTradeCommitPayload *commit)
{
    uint8_t localSlot;
    const struct MpTradeOfferPayload *myOffer;
    const struct MpTradeOfferPayload *theirOffer;
    struct Pokemon incomingMon;
    struct Pokemon outgoingMonCopy;
    struct BoxPokemon oldOutgoingBox;
    int outgoingSlot = -1;
    bool hasIncomingMon = false;
    bool hasOutgoingBox = false;
    u32 partyCount;

    if (commit == NULL || !MpWorldSync_IsLocalTutorialComplete())
        return MP_TRADE_ERR_INVALID;
    if (WasSessionAlreadyCommitted(commit->sessionId))
        return MP_TRADE_ERR_ALREADY_DONE;

    localSlot = MpNet_GetLocalSlot();
    if (commit->slotA == localSlot)
    {
        myOffer = &commit->offerA;
        theirOffer = &commit->offerB;
    }
    else if (commit->slotB == localSlot)
    {
        myOffer = &commit->offerB;
        theirOffer = &commit->offerA;
    }
    else
    {
        return MP_TRADE_ERR_INVALID;
    }

    /* Phase 1: Validate all preconditions before mutating save state */
    if (myOffer->hasMon)
    {
        outgoingSlot = FindPartySlotMatchingOffer(myOffer);
        if (outgoingSlot < 0)
            return MP_TRADE_ERR_NO_MON;
        if (!theirOffer->hasMon && CountAliveNonEggPartyMonsExcludingSlot(outgoingSlot) == 0)
            return MP_TRADE_ERR_PARTY_EMPTY;
        outgoingMonCopy = gParties[B_TRAINER_PLAYER][outgoingSlot];
        oldOutgoingBox = outgoingMonCopy.box;
        hasOutgoingBox = true;
    }

    if (theirOffer->hasMon)
    {
        if (!MpTrade_DeserializePokemon(theirOffer->monBytes, &incomingMon))
            return MP_TRADE_ERR_INVALID;
        if (outgoingSlot >= 0
         && CountAliveNonEggPartyMonsExcludingSlot(outgoingSlot) == 0
         && (GetMonData(&incomingMon, MON_DATA_SPECIES_OR_EGG) == SPECIES_EGG
          || GetMonData(&incomingMon, MON_DATA_HP) == 0))
            return MP_TRADE_ERR_PARTY_EMPTY;
        hasIncomingMon = true;
    }

    if (myOffer->itemId != ITEM_NONE && myOffer->itemQty > 0)
    {
        if (!IsTradableItem(myOffer->itemId))
            return MP_TRADE_ERR_KEY_OR_HM;
        if (!CheckBagHasItem((enum Item)myOffer->itemId, myOffer->itemQty))
            return MP_TRADE_ERR_NO_ITEM;
    }

    if (theirOffer->itemId != ITEM_NONE && theirOffer->itemQty > 0)
    {
        if (!IsTradableItem(theirOffer->itemId))
            return MP_TRADE_ERR_KEY_OR_HM;
        if (!CheckBagHasSpace((enum Item)theirOffer->itemId, theirOffer->itemQty))
            return MP_TRADE_ERR_NO_BAG_SPACE;
    }

    if (myOffer->money > 0 && !IsEnoughMoney(&gSaveBlock1Ptr->money, myOffer->money))
        return MP_TRADE_ERR_NO_MONEY;

    /* Phase 2: Atomic commit (all preconditions verified) */
    if (myOffer->itemId != ITEM_NONE && myOffer->itemQty > 0)
        RemoveBagItem((enum Item)myOffer->itemId, myOffer->itemQty);
    if (theirOffer->itemId != ITEM_NONE && theirOffer->itemQty > 0)
        AddBagItem((enum Item)theirOffer->itemId, theirOffer->itemQty);

    if (myOffer->money > 0)
        RemoveMoney(&gSaveBlock1Ptr->money, myOffer->money);
    if (theirOffer->money > 0)
        AddMoney(&gSaveBlock1Ptr->money, theirOffer->money);

    if (outgoingSlot >= 0 && hasIncomingMon)
    {
        struct Pokemon *dst = &gParties[B_TRAINER_PLAYER][outgoingSlot];
        *dst = incomingMon;
        MpTrade_ApplyTradeEvolution(dst, &outgoingMonCopy);
        HandleSetPokedexFlagFromMon(dst, FLAG_SET_CAUGHT);
        MpSoulLink_OnTradePreserveLink(&oldOutgoingBox, dst, theirOffer->linkId, theirOffer->mapsec, localSlot);
    }
    else if (outgoingSlot >= 0 && !hasIncomingMon)
    {
        ZeroMonData(&gParties[B_TRAINER_PLAYER][outgoingSlot]);
        CompactPartySlots();
    }
    else if (hasIncomingMon)
    {
        partyCount = CalculatePlayerPartyCount();
        if (partyCount < PARTY_SIZE)
        {
            struct Pokemon *dst = &gParties[B_TRAINER_PLAYER][partyCount];
            *dst = incomingMon;
            MpTrade_ApplyTradeEvolution(dst, NULL);
            HandleSetPokedexFlagFromMon(dst, FLAG_SET_CAUGHT);
            MpSoulLink_OnTradePreserveLink(hasOutgoingBox ? &oldOutgoingBox : NULL, dst, theirOffer->linkId, theirOffer->mapsec, localSlot);
        }
        else
        {
            MpTrade_ApplyTradeEvolution(&incomingMon, NULL);
            HandleSetPokedexFlagFromMon(&incomingMon, FLAG_SET_CAUGHT);
            MpSoulLink_OnTradePreserveLink(NULL, &incomingMon, theirOffer->linkId, theirOffer->mapsec, localSlot);
            CopyMonToPC(&incomingMon);
        }
    }

    CalculatePlayerPartyCount();
    MarkSessionCommitted(commit->sessionId);
    if (!gMain.inBattle)
        UpdateFollowingPokemon();
    sTradeSession.active = false;
    sTradeSession.hasLocalOffer = false;
    sTradeSession.hasRemoteOffer = false;
    sTradeSession.localLocked = false;
    sTradeSession.remoteLocked = false;
    sTradeSession.offeredPartyIndex = PARTY_SIZE;
    return MP_TRADE_OK;
}

const MpTradeSession *MpTrade_GetSession(void)
{
    return &sTradeSession;
}

void MpTrade_StepFrame(void)
{
#ifdef WASM_PLATFORM
    if (WasmMp_ConsumeTradeCommitFlag())
        MpTrade_ApplyCommitFromBridge();
#endif
    if (!gMain.inBattle)
    {
        struct MpTradeCommitPayload queued;
        while (PopPendingCommit(&queued))
            MpTrade_ApplyCommit(&queued);
    }
}

void MpTrade_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    MpTrade_Init();
    if (payload == NULL || !MpWorldSync_IsLocalTutorialComplete())
        return;

    if (opcode == MP_PKT_TRD_OFFER && len >= sizeof(struct MpTradeOfferPayload))
    {
        const struct MpTradeOfferPayload *pkt = (const struct MpTradeOfferPayload *)payload;
        sTradeSession.active = true;
        sTradeSession.sessionId = pkt->sessionId;
        sTradeSession.partnerPeerId = fromPeerId;
        sTradeSession.partnerSlot = pkt->fromSlot;
        sTradeSession.hasRemoteOffer = true;
        sTradeSession.remoteOffer = *pkt;
        sTradeSession.remoteLocked = false;
        sTradeSession.localLocked = false; /* Unlock local on partner edit */
    }
    else if (opcode == MP_PKT_TRD_LOCK && len >= sizeof(struct MpTradeOfferPayload))
    {
        const struct MpTradeOfferPayload *pkt = (const struct MpTradeOfferPayload *)payload;
        if (pkt->sessionId == sTradeSession.sessionId)
        {
            sTradeSession.remoteOffer = *pkt;
            sTradeSession.hasRemoteOffer = true;
            sTradeSession.remoteLocked = (pkt->locked != 0);
        }
    }
    else if (opcode == MP_PKT_TRD_COMMIT && len >= sizeof(struct MpTradeCommitPayload))
    {
        const struct MpTradeCommitPayload *pkt = (const struct MpTradeCommitPayload *)payload;
        if (!gMain.inBattle)
            MpTrade_ApplyCommit(pkt);
        else
            QueuePendingCommit(pkt);
    }
}

void MpTrade_ApplyCommitFromBridge(void)
{
    MpTrade_Init();
    if (gMain.inBattle || !MpWorldSync_IsLocalTutorialComplete())
        return;

    {
        struct MpTradeCommitPayload queued;
        bool appliedQueued = false;
        while (PopPendingCommit(&queued))
        {
            MpTrade_ApplyCommit(&queued);
            appliedQueued = true;
        }
        if (appliedQueued)
            return;
    }

    if (sTradeSession.hasLocalOffer && sTradeSession.hasRemoteOffer)
    {
        struct MpTradeCommitPayload commit;
        memset(&commit, 0, sizeof(commit));
        commit.sessionId = sTradeSession.sessionId;
        commit.slotA = sTradeSession.localOffer.fromSlot;
        commit.slotB = sTradeSession.remoteOffer.fromSlot;
        commit.offerA = sTradeSession.localOffer;
        commit.offerB = sTradeSession.remoteOffer;
        MpTrade_ApplyCommit(&commit);
    }
}
