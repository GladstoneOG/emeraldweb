#include "global.h"
#include "battle.h"
#include "data.h"
#include "item.h"
#include "move.h"
#include "pokemon.h"
#include "constants/abilities.h"
#include "constants/battle_move_effects.h"
#include "constants/characters.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "constants/opponents.h"
#include "constants/trainers.h"
#include "mp_randomizer.h"
#include "mp_world_sync.h"

#define MP_LEARNSET_CACHE_SLOTS 8u
#define MP_LEARNSET_MAX_ENTRIES 64u

struct MpLearnsetCacheSlot {
    uint64_t seed;
    uint32_t flags;
    uint16_t species;
    uint32_t lruTick;
    struct LevelUpMove moves[MP_LEARNSET_MAX_ENTRIES];
};

static uint64_t sCachedSeed = 0x504F4B454D4F4E33ULL;
static uint32_t sCachedFlags = 0;
static bool sConfigInitialized = false;
static uint8_t sSuppressStaticDepth = 0;

static uint16_t sValidSpecies[NUM_SPECIES];
static uint16_t sValidSpeciesCount = 0;
static bool sSpeciesTableBuilt = false;

static uint16_t sValidMoves[MOVES_COUNT];
static uint16_t sValidMoveCount = 0;
static bool sMoveTableBuilt = false;

static uint16_t sValidGeneralItems[ITEMS_COUNT];
static uint16_t sValidGeneralItemCount = 0;
static uint16_t sValidMartItems[ITEMS_COUNT];
static uint16_t sValidMartItemCount = 0;
static uint16_t sValidTms[256];
static uint16_t sValidTmCount = 0;
static bool sItemTablesBuilt = false;

static struct MpLearnsetCacheSlot sLearnsetCache[MP_LEARNSET_CACHE_SLOTS];
static uint32_t sLearnsetLruClock = 1;

static uint8_t sTypePool[NUMBER_OF_MON_TYPES];
static uint8_t sTypePoolCount = 0;

uint64_t MpRand_SplitMix64(uint64_t runSeed, uint32_t categorySalt, uint32_t entityId)
{
    uint64_t z = runSeed;
    z ^= ((uint64_t)categorySalt * 0x9E3779B97F4A7C15ULL);
    z ^= ((uint64_t)entityId * 0xBF58476D1CE4E5B9ULL);
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t MpRandomizer_HashSeedString(const char *seedStr)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    if (seedStr == NULL || seedStr[0] == '\0')
        return sCachedSeed;
    while (*seedStr != '\0')
    {
        hash ^= (uint8_t)(*seedStr++);
        hash *= 0x100000001B3ULL;
    }
    return (hash == 0) ? 1ULL : hash;
}

static void SyncConfigIfNeeded(void)
{
    const MpRunConfig *cfg = MpNet_GetRunConfig();
    if (cfg != NULL)
        MpRandomizer_OnRunConfigChanged(cfg);
    else if (!sConfigInitialized)
        sConfigInitialized = true;
}

void MpRandomizer_OnRunConfigChanged(const MpRunConfig *cfg)
{
    uint64_t nextSeed;
    uint32_t nextFlags;

    if (cfg == NULL)
        return;

    nextSeed = (cfg->seedHash != 0) ? cfg->seedHash : MpRandomizer_HashSeedString(cfg->seedStr);
    nextFlags = cfg->randomizerFlags;

    if (!sConfigInitialized || sCachedSeed != nextSeed || sCachedFlags != nextFlags)
    {
        sCachedSeed = nextSeed;
        sCachedFlags = nextFlags;
        sConfigInitialized = true;
        memset(sLearnsetCache, 0, sizeof(sLearnsetCache));
    }
}

uint64_t MpRandomizer_GetRunSeed(void)
{
    SyncConfigIfNeeded();
    return sCachedSeed;
}

uint32_t MpRandomizer_GetFlags(void)
{
    SyncConfigIfNeeded();
    return sCachedFlags;
}

bool MpRandomizer_HasFlag(uint32_t flag)
{
    return (MpRandomizer_GetFlags() & flag) != 0;
}

void MpRandomizer_PushSuppressStatic(void)
{
    if (sSuppressStaticDepth < 255u)
        sSuppressStaticDepth++;
}

void MpRandomizer_PopSuppressStatic(void)
{
    if (sSuppressStaticDepth > 0u)
        sSuppressStaticDepth--;
}

bool MpRandomizer_IsStaticSuppressed(void)
{
    return sSuppressStaticDepth > 0u;
}

static bool IsSpeciesFormRandomizable(enum Species sp)
{
    if (sp <= SPECIES_NONE || sp >= NUM_SPECIES || sp == SPECIES_EGG)
        return false;
    if (gSpeciesInfo[sp].speciesName[0] == '\0' || gSpeciesInfo[sp].speciesName[0] == EOS)
        return false;
    if (gSpeciesInfo[sp].baseHP == 0)
        return false;
    if (gSpeciesInfo[sp].isMegaEvolution
     || gSpeciesInfo[sp].isPrimalReversion
     || gSpeciesInfo[sp].isUltraBurst
     || gSpeciesInfo[sp].isGigantamax
     || gSpeciesInfo[sp].isTeraForm
     || gSpeciesInfo[sp].isTotem
     || gSpeciesInfo[sp].cannotBeTraded)
        return false;
    if (sp >= SPECIES_UNOWN_B && sp <= SPECIES_UNOWN_QUESTION)
        return false;
    /* Exclude cosmetic/battle subforms while preserving base species and Alolan/Galarian/Hisuian/Paldean forms. */
    if (gSpeciesInfo[sp].formSpeciesIdTable != NULL
     && gSpeciesInfo[sp].formSpeciesIdTable[0] != sp
     && !gSpeciesInfo[sp].isAlolanForm
     && !gSpeciesInfo[sp].isGalarianForm
     && !gSpeciesInfo[sp].isHisuianForm
     && !gSpeciesInfo[sp].isPaldeanForm)
        return false;
    return true;
}

static void EnsureSpeciesTable(void)
{
    uint32_t sp;
    if (sSpeciesTableBuilt)
        return;
    sValidSpeciesCount = 0;
    for (sp = 1; sp < NUM_SPECIES; sp++)
    {
        if (IsSpeciesFormRandomizable((enum Species)sp))
            sValidSpecies[sValidSpeciesCount++] = (uint16_t)sp;
    }
    sSpeciesTableBuilt = true;
}

static void EnsureTypePool(void)
{
    uint32_t t;
    if (sTypePoolCount > 0)
        return;
    for (t = 1; t < NUMBER_OF_MON_TYPES; t++)
    {
        if (t == TYPE_MYSTERY || t == TYPE_STELLAR)
            continue;
        sTypePool[sTypePoolCount++] = (uint8_t)t;
    }
}

static bool IsEvasionBoostMove(enum Move move)
{
    u32 i;
    u32 count;
    if (GetMoveEffect(move) == EFFECT_MINIMIZE)
        return true;
    count = GetMoveAdditionalEffectCount(move);
    for (i = 0; i < count; i++)
    {
        const struct AdditionalEffect *eff = GetMoveAdditionalEffectById(move, i);
        if ((eff->moveEffect == STAT_CHANGE_EFFECT_PLUS || eff->moveEffect == MOVE_EFFECT_STAT_PLUS) && eff->evasion > 0u)
            return true;
    }
    return false;
}

static bool IsUsableDamagingMove(enum Move move)
{
    u32 acc;
    if (GetMoveCategory(move) == DAMAGE_CATEGORY_STATUS)
        return false;
    if (GetMovePower(move) < 35u)
        return false;
    if (GetMoveEffect(move) == EFFECT_OHKO)
        return false;
    acc = GetMoveAccuracy(move);
    return (acc == 0u || acc >= 85u);
}

static bool LearnsetHasMove(const struct LevelUpMove *moves, uint32_t count, enum Move move)
{
    uint32_t i;
    for (i = 0; i < count; i++)
    {
        if (moves[i].move == move)
            return true;
    }
    return false;
}

static void EnsureMoveTable(void)
{
    uint32_t m;
    if (sMoveTableBuilt)
        return;
    sValidMoveCount = 0;
    for (m = 1; m < MOVES_COUNT; m++)
    {
        enum Move move = (enum Move)m;
        u32 acc;
        if (m == MOVE_STRUGGLE)
            continue;
        if (GetMoveEffect(move) == EFFECT_PLACEHOLDER || GetMoveEffect(move) == EFFECT_OHKO)
            continue;
        if (GetMovePP(move) == 0)
            continue;
        if (IsEvasionBoostMove(move))
            continue;
        acc = GetMoveAccuracy(move);
        if (acc > 0u && acc < 80u)
            continue;
        sValidMoves[sValidMoveCount++] = (uint16_t)m;
    }
    sMoveTableBuilt = true;
}

static bool IsHmItem(uint16_t itemId)
{
    return (itemId >= ITEM_HM01 && itemId <= ITEM_HM08);
}

bool MpRandomizer_IsItemRandomizable(uint16_t itemId)
{
    enum Pocket pocket;
    const u8 *name;

    if (itemId == ITEM_NONE || itemId >= ITEMS_COUNT)
        return false;
    if (IsHmItem(itemId))
        return false;
    if (GetItemImportance((enum Item)itemId) != 0)
        return false;

    pocket = GetItemPocket((enum Item)itemId);
    if (pocket >= POCKETS_COUNT || pocket == POCKET_KEY_ITEMS)
        return false;

    name = GetItemName((enum Item)itemId);
    if (name == NULL || name[0] == '\0' || name[0] == EOS)
        return false;

    return true;
}

static void EnsureItemTables(void)
{
    uint32_t id;
    if (sItemTablesBuilt)
        return;

    sValidGeneralItemCount = 0;
    sValidMartItemCount = 0;
    sValidTmCount = 0;

    for (id = 1; id < ITEMS_COUNT; id++)
    {
        enum Pocket pocket;
        if (!MpRandomizer_IsItemRandomizable((uint16_t)id))
            continue;

        pocket = GetItemPocket((enum Item)id);
        if (pocket == POCKET_TM_HM)
        {
            if (sValidTmCount < ARRAY_COUNT(sValidTms))
                sValidTms[sValidTmCount++] = (uint16_t)id;
            continue;
        }

        sValidGeneralItems[sValidGeneralItemCount++] = (uint16_t)id;
        if (GetItemPrice((enum Item)id) > 0)
            sValidMartItems[sValidMartItemCount++] = (uint16_t)id;
    }

    sItemTablesBuilt = true;
}

static uint32_t ComputeGcdU32(uint32_t a, uint32_t b)
{
    while (b != 0u)
    {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint32_t PickCoprimeStride(uint32_t count, uint32_t preferred)
{
    uint32_t stride;
    if (count <= 1u)
        return 1u;
    stride = preferred % count;
    if (stride == 0u)
        stride = 1u;
    while (ComputeGcdU32(stride, count) != 1u)
    {
        stride++;
        if (stride >= count)
            stride = 1u;
    }
    return stride;
}

static uint16_t PickSpeciesWithConstraints(uint64_t r, uint16_t origSpecies, bool similarBst, bool allowLegendaries, uint8_t requiredType)
{
    uint32_t origBst;
    uint32_t minBst = 0;
    uint32_t maxBst = 9999u;
    uint32_t startIdx;
    uint32_t stride;
    uint32_t pass;
    uint32_t i;

    EnsureSpeciesTable();
    if (sValidSpeciesCount == 0)
        return origSpecies;

    origBst = (origSpecies > SPECIES_NONE && origSpecies < NUM_SPECIES)
        ? GetSpeciesBaseStatTotal((enum Species)origSpecies)
        : 350u;
    startIdx = (uint32_t)(r % sValidSpeciesCount);
    stride = PickCoprimeStride(sValidSpeciesCount, 37u);

    for (pass = 0; pass < 3; pass++)
    {
        if (similarBst)
        {
            if (pass == 0)
            {
                minBst = (origBst * 4u) / 5u;
                maxBst = (origBst * 5u) / 4u;
            }
            else if (pass == 1)
            {
                minBst = (origBst * 2u) / 3u;
                maxBst = (origBst * 3u) / 2u;
            }
            else
            {
                minBst = 0;
                maxBst = 9999u;
            }
        }

        for (i = 0; i < sValidSpeciesCount; i++)
        {
            uint16_t cand = sValidSpecies[(startIdx + i * stride) % sValidSpeciesCount];
            uint32_t candBst;

            if (!allowLegendaries && (gSpeciesInfo[cand].isRestrictedLegendary || gSpeciesInfo[cand].isMythical))
                continue;
            if (requiredType != TYPE_NONE)
            {
                enum Type t0 = gSpeciesInfo[cand].types[0];
                enum Type t1 = gSpeciesInfo[cand].types[1];
                if (t0 != requiredType && t1 != requiredType)
                    continue;
            }
            if (similarBst && pass < 2)
            {
                candBst = GetSpeciesBaseStatTotal((enum Species)cand);
                if (candBst < minBst || candBst > maxBst)
                    continue;
            }
            return cand;
        }
        if (!similarBst)
            break;
    }

    return sValidSpecies[startIdx];
}

static bool IsBossTrainerClass(uint8_t trainerClass)
{
    switch (trainerClass)
    {
    case TRAINER_CLASS_LEADER:
    case TRAINER_CLASS_ELITE_FOUR:
    case TRAINER_CLASS_CHAMPION:
    case TRAINER_CLASS_LEADER_FRLG:
    case TRAINER_CLASS_ELITE_FOUR_FRLG:
    case TRAINER_CLASS_CHAMPION_FRLG:
        return true;
    default:
        return false;
    }
}

static uint8_t GetDeterministicTrainerThemeType(uint16_t trainerId)
{
    uint64_t r;
    EnsureTypePool();
    if (sTypePoolCount == 0)
        return TYPE_NONE;
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_GYM_TYPE, trainerId);
    return sTypePool[r % sTypePoolCount];
}

uint16_t MpRandomizer_GetWildSpecies(uint8_t mapGroup, uint8_t mapNum, uint8_t area, uint8_t slot, uint16_t origSpecies)
{
    uint32_t entityId;
    uint64_t r;

    (void)slot;
    if (!MpRandomizer_HasFlag(MP_RANDO_F_WILD_ENCOUNTERS))
        return origSpecies;
    if (origSpecies == SPECIES_NONE || origSpecies >= NUM_SPECIES)
        return origSpecies;

    /* 1:1 per-species mapping on each route/area regardless of local gRngValue slot roll. */
    entityId = ((uint32_t)mapGroup << 24) | ((uint32_t)mapNum << 16) | ((uint32_t)area << 8);
    entityId ^= ((uint32_t)origSpecies * 2654435761u);
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_WILD, entityId);
    return PickSpeciesWithConstraints(r, origSpecies, MpRandomizer_HasFlag(MP_RANDO_F_SIMILAR_BST), false, TYPE_NONE);
}

uint16_t MpRandomizer_GetStarterSpecies(uint16_t starterIndex, uint16_t origSpecies)
{
    uint64_t seed;
    uint16_t chosen[3] = {SPECIES_NONE, SPECIES_NONE, SPECIES_NONE};
    uint16_t idx;
    uint16_t attempt;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_STARTERS_STATICS))
        return origSpecies;
    if (starterIndex >= 3u)
        return origSpecies;

    seed = MpRandomizer_GetRunSeed();
    for (idx = 0; idx <= starterIndex; idx++)
    {
        for (attempt = 0; attempt < 32u; attempt++)
        {
            uint32_t ent = ((uint32_t)idx << 8) | attempt;
            uint64_t r = MpRand_SplitMix64(seed, MP_RANDO_SALT_STARTER, ent);
            uint16_t sp = PickSpeciesWithConstraints(r, origSpecies, MpRandomizer_HasFlag(MP_RANDO_F_SIMILAR_BST), false, TYPE_NONE);
            bool dup = false;
            uint16_t k;
            for (k = 0; k < idx; k++)
            {
                if (chosen[k] == sp)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup || attempt == 31u)
            {
                chosen[idx] = sp;
                break;
            }
        }
    }
    return chosen[starterIndex];
}

uint16_t MpRandomizer_GetStaticSpecies(uint32_t entityId, uint16_t origSpecies)
{
    uint64_t r;
    bool allowLegend;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_STARTERS_STATICS))
        return origSpecies;
    if (origSpecies == SPECIES_NONE || origSpecies >= NUM_SPECIES || origSpecies == SPECIES_EGG)
        return origSpecies;

    allowLegend = gSpeciesInfo[origSpecies].isRestrictedLegendary || gSpeciesInfo[origSpecies].isSubLegendary || gSpeciesInfo[origSpecies].isMythical;
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_STATIC, entityId ^ ((uint32_t)origSpecies << 12));
    return PickSpeciesWithConstraints(r, origSpecies, MpRandomizer_HasFlag(MP_RANDO_F_SIMILAR_BST), allowLegend, TYPE_NONE);
}

static uint16_t NormalizeRivalTrainerId(uint16_t trainerId, bool *outIsRival)
{
    bool isRival = true;
    uint16_t norm = trainerId;

    switch (trainerId)
    {
    case TRAINER_MAY_ROUTE_103_MUDKIP:
    case TRAINER_BRENDAN_ROUTE_103_MUDKIP:
        norm = TRAINER_BRENDAN_ROUTE_103_MUDKIP;
        break;
    case TRAINER_MAY_ROUTE_110_MUDKIP:
    case TRAINER_BRENDAN_ROUTE_110_MUDKIP:
        norm = TRAINER_BRENDAN_ROUTE_110_MUDKIP;
        break;
    case TRAINER_MAY_ROUTE_119_MUDKIP:
    case TRAINER_BRENDAN_ROUTE_119_MUDKIP:
        norm = TRAINER_BRENDAN_ROUTE_119_MUDKIP;
        break;
    case TRAINER_MAY_ROUTE_103_TREECKO:
    case TRAINER_BRENDAN_ROUTE_103_TREECKO:
        norm = TRAINER_BRENDAN_ROUTE_103_TREECKO;
        break;
    case TRAINER_MAY_ROUTE_110_TREECKO:
    case TRAINER_BRENDAN_ROUTE_110_TREECKO:
        norm = TRAINER_BRENDAN_ROUTE_110_TREECKO;
        break;
    case TRAINER_MAY_ROUTE_119_TREECKO:
    case TRAINER_BRENDAN_ROUTE_119_TREECKO:
        norm = TRAINER_BRENDAN_ROUTE_119_TREECKO;
        break;
    case TRAINER_MAY_ROUTE_103_TORCHIC:
    case TRAINER_BRENDAN_ROUTE_103_TORCHIC:
        norm = TRAINER_BRENDAN_ROUTE_103_TORCHIC;
        break;
    case TRAINER_MAY_ROUTE_110_TORCHIC:
    case TRAINER_BRENDAN_ROUTE_110_TORCHIC:
        norm = TRAINER_BRENDAN_ROUTE_110_TORCHIC;
        break;
    case TRAINER_MAY_ROUTE_119_TORCHIC:
    case TRAINER_BRENDAN_ROUTE_119_TORCHIC:
        norm = TRAINER_BRENDAN_ROUTE_119_TORCHIC;
        break;
    case TRAINER_MAY_RUSTBORO_TREECKO:
    case TRAINER_BRENDAN_RUSTBORO_TREECKO:
        norm = TRAINER_BRENDAN_RUSTBORO_TREECKO;
        break;
    case TRAINER_MAY_RUSTBORO_MUDKIP:
    case TRAINER_BRENDAN_RUSTBORO_MUDKIP:
        norm = TRAINER_BRENDAN_RUSTBORO_MUDKIP;
        break;
    case TRAINER_MAY_RUSTBORO_TORCHIC:
    case TRAINER_BRENDAN_RUSTBORO_TORCHIC:
        norm = TRAINER_BRENDAN_RUSTBORO_TORCHIC;
        break;
    case TRAINER_MAY_LILYCOVE_MUDKIP:
    case TRAINER_BRENDAN_LILYCOVE_MUDKIP:
        norm = TRAINER_BRENDAN_LILYCOVE_MUDKIP;
        break;
    case TRAINER_MAY_LILYCOVE_TREECKO:
    case TRAINER_BRENDAN_LILYCOVE_TREECKO:
        norm = TRAINER_BRENDAN_LILYCOVE_TREECKO;
        break;
    case TRAINER_MAY_LILYCOVE_TORCHIC:
    case TRAINER_BRENDAN_LILYCOVE_TORCHIC:
        norm = TRAINER_BRENDAN_LILYCOVE_TORCHIC;
        break;
    case TRAINER_MAY_PLACEHOLDER:
    case TRAINER_BRENDAN_PLACEHOLDER:
        norm = TRAINER_BRENDAN_PLACEHOLDER;
        break;
    default:
        isRival = false;
        break;
    }

    if (outIsRival != NULL)
        *outIsRival = isRival;
    return norm;
}

uint16_t MpRandomizer_GetTrainerSpecies(uint16_t trainerId, uint8_t monSlot, uint8_t trainerClass, uint16_t origSpecies)
{
    uint32_t entityId;
    uint64_t r;
    uint8_t themeType = TYPE_NONE;
    bool isRival = false;
    uint16_t normTrainerId;
    uint8_t effectiveSlot;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_TRAINERS_2V2))
        return origSpecies;
    if (origSpecies == SPECIES_NONE || origSpecies >= NUM_SPECIES)
        return origSpecies;

    normTrainerId = NormalizeRivalTrainerId(trainerId, &isRival);
    effectiveSlot = isRival ? 0u : monSlot;

    if (MpRandomizer_HasFlag(MP_RANDO_F_PRESERVE_GYM_THEMES) && IsBossTrainerClass(trainerClass))
        themeType = GetDeterministicTrainerThemeType(normTrainerId);

    entityId = ((uint32_t)normTrainerId << 16) | ((uint32_t)effectiveSlot << 8) | (uint32_t)origSpecies;
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_TRAINER, entityId);
    return PickSpeciesWithConstraints(r, origSpecies, MpRandomizer_HasFlag(MP_RANDO_F_SIMILAR_BST), false, themeType);
}

uint16_t MpRandomizer_GetNpcPartnerSpecies(uint16_t trainerId, uint8_t trainerClass, uint16_t aceSpecies, uint8_t aceLevel)
{
    uint16_t normTrainerId = NormalizeRivalTrainerId(trainerId, NULL);
    uint32_t entityId = ((uint32_t)normTrainerId << 16) | ((uint32_t)aceSpecies << 4) | (uint32_t)aceLevel;
    uint64_t r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_NPC_PARTNER, entityId);
    uint8_t themeType = TYPE_NONE;

    if (MpRandomizer_HasFlag(MP_RANDO_F_PRESERVE_GYM_THEMES) && IsBossTrainerClass(trainerClass))
        themeType = GetDeterministicTrainerThemeType(normTrainerId);

    return PickSpeciesWithConstraints(r, aceSpecies, true, false, themeType);
}

static bool IsValidPartnerSpeciesBase(uint16_t cand, uint16_t excludeSpecies)
{
    if (cand == excludeSpecies)
        return false;
    if (gSpeciesInfo[cand].baseHP <= 1u)
        return false;
    if (gSpeciesInfo[cand].isRestrictedLegendary
     || gSpeciesInfo[cand].isSubLegendary
     || gSpeciesInfo[cand].isMythical
     || gSpeciesInfo[cand].isUltraBeast
     || gSpeciesInfo[cand].isParadox)
        return false;
    return true;
}

static bool SpeciesHasAttackByLevel(enum Species species, u8 level)
{
    const struct LevelUpMove *learnset = GetSpeciesLevelUpLearnset(species);
    u32 i;
    if (learnset == NULL)
        return false;
    for (i = 0; learnset[i].move != LEVEL_UP_MOVE_END; i++)
    {
        enum Move move;
        u32 acc;
        if (learnset[i].level > level)
            break;
        move = learnset[i].move;
        acc = GetMoveAccuracy(move);
        if (GetMoveCategory(move) != DAMAGE_CATEGORY_STATUS
         && GetMovePower(move) >= 30u
         && (acc == 0u || acc >= 80u))
            return true;
    }
    return false;
}

u16 MpRandomizer_GetPartnerSpecies(u32 seed, u8 slotIdx, u16 excludeSpecies, u8 level)
{
    u8 effLevel = (level < 1u) ? 5u : level;
    uint32_t minBst;
    uint32_t maxBst;
    uint64_t combinedSeed;
    uint32_t ent;
    uint64_t r;
    uint32_t startIdx;
    uint32_t stride;
    uint32_t pass;
    uint32_t i;

    EnsureSpeciesTable();
    if (sValidSpeciesCount == 0)
        return (excludeSpecies != SPECIES_TREECKO) ? SPECIES_TREECKO : SPECIES_TORCHIC;

    if (effLevel <= 12u)
    {
        minBst = 250u;
        maxBst = 420u;
    }
    else if (effLevel <= 20u)
    {
        minBst = 300u;
        maxBst = 470u;
    }
    else if (effLevel <= 32u)
    {
        minBst = 360u;
        maxBst = 520u;
    }
    else if (effLevel <= 45u)
    {
        minBst = 420u;
        maxBst = 560u;
    }
    else
    {
        minBst = 460u;
        maxBst = 600u;
    }

    combinedSeed = MpRandomizer_GetRunSeed() ^ (((uint64_t)seed << 32) | (uint64_t)seed);
    ent = ((uint32_t)slotIdx << 24) ^ ((uint32_t)excludeSpecies << 8) ^ (uint32_t)effLevel;
    r = MpRand_SplitMix64(combinedSeed, MP_RANDO_SALT_NPC_PARTNER, ent);

    startIdx = (uint32_t)(r % sValidSpeciesCount);
    stride = PickCoprimeStride(sValidSpeciesCount, 41u + (uint32_t)slotIdx * 6u);

    for (pass = 0; pass < 3u; pass++)
    {
        uint32_t passMinBst = (pass == 0u) ? minBst : ((pass == 1u) ? ((minBst * 3u) / 4u) : 0u);
        uint32_t passMaxBst = (pass == 0u) ? maxBst : ((pass == 1u) ? ((maxBst * 5u) / 4u) : 9999u);
        bool requireMove = (pass < 2u);

        for (i = 0; i < sValidSpeciesCount; i++)
        {
            uint16_t cand = sValidSpecies[(startIdx + i * stride) % sValidSpeciesCount];
            uint32_t candBst;

            if (!IsValidPartnerSpeciesBase(cand, excludeSpecies))
                continue;
            if (pass < 2u)
            {
                candBst = GetSpeciesBaseStatTotal((enum Species)cand);
                if (candBst < passMinBst || candBst > passMaxBst)
                    continue;
            }
            if (requireMove && !SpeciesHasAttackByLevel((enum Species)cand, effLevel))
                continue;
            return cand;
        }
    }

    return sValidSpecies[startIdx];
}

uint16_t MpRandomizer_GetItemReplacement(uint32_t entityId, uint16_t origItem, bool martOnly)
{
    uint64_t r;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_ITEMS_MARTS))
        return origItem;
    if (origItem == ITEM_POKE_BALL)
        return origItem;
    if (!martOnly && gSaveBlock1Ptr != NULL && !MpWorldSync_IsLocalTutorialComplete())
        return origItem;
    if (!MpRandomizer_IsItemRandomizable(origItem))
        return origItem;

    EnsureItemTables();

    if (GetItemPocket((enum Item)origItem) == POCKET_TM_HM)
    {
        if (sValidTmCount == 0)
            return origItem;
        r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_TM, entityId ^ origItem);
        return sValidTms[r % sValidTmCount];
    }

    if (martOnly)
    {
        if (sValidMartItemCount == 0)
            return origItem;
        r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_MART, entityId ^ origItem);
        return sValidMartItems[r % sValidMartItemCount];
    }

    if (sValidGeneralItemCount == 0)
        return origItem;
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_ITEM, entityId ^ origItem);
    return sValidGeneralItems[r % sValidGeneralItemCount];
}

uint16_t MpRandomizer_GetHeldItem(uint32_t entityId, uint16_t origItem)
{
    uint64_t r;
    uint32_t start;
    uint32_t stride;
    uint32_t probe;

    if (origItem == ITEM_NONE || origItem == ITEM_POKE_BALL || !MpRandomizer_HasFlag(MP_RANDO_F_ITEMS_MARTS))
        return origItem;
    if (!MpRandomizer_IsItemRandomizable(origItem))
        return origItem;

    EnsureItemTables();
    if (sValidGeneralItemCount == 0)
        return origItem;
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_HELD_ITEM, entityId ^ origItem);
    start = (uint32_t)(r % sValidGeneralItemCount);
    stride = PickCoprimeStride(sValidGeneralItemCount, 13u);
    for (probe = 0; probe < sValidGeneralItemCount; probe++)
    {
        uint16_t cand = sValidGeneralItems[(start + probe * stride) % sValidGeneralItemCount];
        if (cand != ITEM_BRIGHT_POWDER && cand != ITEM_LAX_INCENSE)
            return cand;
    }
    return ITEM_NONE;
}

void MpRandomizer_TransformShopItems(uint16_t *items, uint16_t count, uint8_t mapGroup, uint8_t mapNum)
{
    uint16_t i;
    if (items == NULL || count == 0)
        return;
    if (!MpRandomizer_HasFlag(MP_RANDO_F_ITEMS_MARTS))
        return;

    for (i = 0; i < count; i++)
    {
        uint32_t entityId = ((uint32_t)mapGroup << 20) | ((uint32_t)mapNum << 12) | i;
        /* Keep Poke Balls buyable in slots 0 or keep Key Items / HMs untouched. */
        if (items[i] == ITEM_POKE_BALL && i == 0)
            continue;
        items[i] = MpRandomizer_GetItemReplacement(entityId, items[i], true);
    }
}

static bool IsRandomizableAbility(uint32_t ability, bool banWonderGuard)
{
    if (ability == ABILITY_NONE || ability >= ABILITIES_COUNT)
        return false;
    if (ability == ABILITY_314 || ability == ABILITY_317 || ability == ABILITY_AURA_GUARD)
        return false;
    if (banWonderGuard && ability == ABILITY_WONDER_GUARD)
        return false;
    return true;
}

uint16_t MpRandomizer_GetSpeciesAbility(uint16_t species, uint8_t slot, uint16_t origAbility)
{
    uint64_t r;
    uint32_t start;
    uint32_t stride;
    uint32_t probe;
    uint32_t poolCount = ABILITIES_COUNT - 1u;
    bool banWonderGuard;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_ABILITIES))
        return origAbility;
    if (origAbility == ABILITY_NONE || species == SPECIES_NONE || species >= NUM_SPECIES)
        return origAbility;

    banWonderGuard = MpRandomizer_HasFlag(MP_RANDO_F_BAN_WONDER_GUARD);
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_ABILITY, ((uint32_t)species << 4) | (slot & 0x0Fu));
    start = (uint32_t)(r % poolCount);
    stride = PickCoprimeStride(poolCount, 7u);

    for (probe = 0; probe < poolCount; probe++)
    {
        uint32_t cand = 1u + ((start + probe * stride) % poolCount);
        if (IsRandomizableAbility(cand, banWonderGuard))
            return (uint16_t)cand;
    }

    return origAbility;
}

uint8_t MpRandomizer_GetSpeciesType(uint16_t species, uint8_t slot, uint8_t origType)
{
    uint64_t r0;
    uint64_t r1;
    uint8_t primary;
    uint8_t secondary;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_TYPES))
        return origType;
    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return origType;

    EnsureTypePool();
    if (sTypePoolCount == 0)
        return origType;

    r0 = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_TYPE, ((uint32_t)species << 2) | 0u);
    primary = sTypePool[r0 % sTypePoolCount];
    if (slot == 0)
        return primary;

    /* Preserve monotype vs dual-type parity deterministically. */
    if (gSpeciesInfo[species].types[0] == gSpeciesInfo[species].types[1])
        return primary;

    r1 = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_TYPE, ((uint32_t)species << 2) | 1u);
    secondary = sTypePool[r1 % sTypePoolCount];
    if (secondary == primary)
        secondary = sTypePool[(r1 + 1u) % sTypePoolCount];
    return secondary;
}

const struct LevelUpMove *MpRandomizer_GetLevelUpLearnset(uint16_t species, const struct LevelUpMove *origLearnset)
{
    uint32_t i;
    uint64_t seed;
    uint32_t flags;
    struct MpLearnsetCacheSlot *chosenSlot = NULL;
    uint32_t oldestTick = 0xFFFFFFFFu;
    uint32_t writeIdx = 0;
    uint32_t earlyUsableCount = 0;
    uint32_t moveStride;
    enum Type stabType0;
    enum Type stabType1;

    if (origLearnset == NULL || !MpRandomizer_HasFlag(MP_RANDO_F_MOVESETS))
        return origLearnset;
    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return origLearnset;

    seed = MpRandomizer_GetRunSeed();
    flags = MpRandomizer_GetFlags();

    for (i = 0; i < MP_LEARNSET_CACHE_SLOTS; i++)
    {
        if (sLearnsetCache[i].species == species && sLearnsetCache[i].seed == seed && sLearnsetCache[i].flags == flags)
        {
            sLearnsetCache[i].lruTick = ++sLearnsetLruClock;
            return sLearnsetCache[i].moves;
        }
        if (sLearnsetCache[i].species == SPECIES_NONE || sLearnsetCache[i].lruTick < oldestTick)
        {
            oldestTick = (sLearnsetCache[i].species == SPECIES_NONE) ? 0u : sLearnsetCache[i].lruTick;
            chosenSlot = &sLearnsetCache[i];
        }
    }

    EnsureMoveTable();
    if (chosenSlot == NULL || sValidMoveCount == 0)
        return origLearnset;

    chosenSlot->seed = seed;
    chosenSlot->flags = flags;
    chosenSlot->species = species;
    chosenSlot->lruTick = ++sLearnsetLruClock;

    stabType0 = (enum Type)MpRandomizer_GetSpeciesType(species, 0, gSpeciesInfo[species].types[0]);
    stabType1 = (enum Type)MpRandomizer_GetSpeciesType(species, 1, gSpeciesInfo[species].types[1]);
    moveStride = PickCoprimeStride(sValidMoveCount, 17u);

    for (i = 0; origLearnset[i].move != LEVEL_UP_MOVE_END && writeIdx + 1u < MP_LEARNSET_MAX_ENTRIES; i++)
    {
        uint32_t ent = ((uint32_t)species << 10) | i;
        uint64_t r = MpRand_SplitMix64(seed, MP_RANDO_SALT_LEARNSET, ent);
        uint32_t start = (uint32_t)(r % sValidMoveCount);
        uint16_t picked = sValidMoves[start];
        uint32_t probe;

        /* Guarantee a reliable damaging STAB attacking move at early learn slot 0. */
        if (writeIdx == 0)
        {
            bool foundStab = false;
            for (probe = 0; probe < sValidMoveCount; probe++)
            {
                enum Move candidate = (enum Move)sValidMoves[(start + probe * moveStride) % sValidMoveCount];
                if (IsUsableDamagingMove(candidate)
                 && GetMovePower(candidate) <= 85u
                 && (GetMoveType(candidate) == stabType0 || GetMoveType(candidate) == stabType1))
                {
                    picked = (uint16_t)candidate;
                    foundStab = true;
                    break;
                }
            }
            if (!foundStab)
            {
                for (probe = 0; probe < sValidMoveCount; probe++)
                {
                    enum Move candidate = (enum Move)sValidMoves[(start + probe * moveStride) % sValidMoveCount];
                    if (IsUsableDamagingMove(candidate) && GetMovePower(candidate) <= 85u)
                    {
                        picked = (uint16_t)candidate;
                        break;
                    }
                }
            }
        }
        else if (origLearnset[i].level <= 15u && earlyUsableCount < 2u)
        {
            /* Guarantee at least 2 distinct usable damaging moves in early (<= 15) learnsets. */
            bool foundEarly = false;
            for (probe = 0; probe < sValidMoveCount; probe++)
            {
                enum Move candidate = (enum Move)sValidMoves[(start + probe * moveStride) % sValidMoveCount];
                if (IsUsableDamagingMove(candidate)
                 && GetMovePower(candidate) <= 90u
                 && !LearnsetHasMove(chosenSlot->moves, writeIdx, candidate))
                {
                    picked = (uint16_t)candidate;
                    foundEarly = true;
                    break;
                }
            }
            if (!foundEarly)
            {
                for (probe = 0; probe < sValidMoveCount; probe++)
                {
                    enum Move candidate = (enum Move)sValidMoves[(start + probe * moveStride) % sValidMoveCount];
                    if (IsUsableDamagingMove(candidate))
                    {
                        picked = (uint16_t)candidate;
                        break;
                    }
                }
            }
        }
        else if (LearnsetHasMove(chosenSlot->moves, writeIdx, (enum Move)picked))
        {
            for (probe = 1; probe < sValidMoveCount; probe++)
            {
                enum Move candidate = (enum Move)sValidMoves[(start + probe * moveStride) % sValidMoveCount];
                if (!LearnsetHasMove(chosenSlot->moves, writeIdx, candidate))
                {
                    picked = (uint16_t)candidate;
                    break;
                }
            }
        }

        if (origLearnset[i].level <= 15u && IsUsableDamagingMove((enum Move)picked))
            earlyUsableCount++;

        chosenSlot->moves[writeIdx].level = origLearnset[i].level;
        chosenSlot->moves[writeIdx].move = (enum Move)picked;
        writeIdx++;
    }

    chosenSlot->moves[writeIdx].level = 0;
    chosenSlot->moves[writeIdx].move = LEVEL_UP_MOVE_END;
    return chosenSlot->moves;
}

uint16_t MpRandomizer_GetEvolutionTarget(uint16_t fromSpecies, uint8_t evoIndex, uint16_t origTarget)
{
    uint32_t fromBst;
    uint64_t r;
    uint32_t startIdx;
    uint32_t i;

    if (!MpRandomizer_HasFlag(MP_RANDO_F_EVOLUTIONS))
        return origTarget;
    if (origTarget == SPECIES_NONE || fromSpecies == SPECIES_NONE || fromSpecies >= NUM_SPECIES)
        return origTarget;

    EnsureSpeciesTable();
    if (sValidSpeciesCount == 0)
        return origTarget;

    fromBst = GetSpeciesBaseStatTotal((enum Species)fromSpecies);
    r = MpRand_SplitMix64(MpRandomizer_GetRunSeed(), MP_RANDO_SALT_EVO, ((uint32_t)fromSpecies << 8) | evoIndex);
    startIdx = (uint32_t)(r % sValidSpeciesCount);
    {
        uint32_t stride = PickCoprimeStride(sValidSpeciesCount, 29u);
        for (i = 0; i < sValidSpeciesCount; i++)
        {
            uint16_t cand = sValidSpecies[(startIdx + i * stride) % sValidSpeciesCount];
            uint32_t candBst = GetSpeciesBaseStatTotal((enum Species)cand);
            if (cand == fromSpecies)
                continue;
            if (gSpeciesInfo[cand].isRestrictedLegendary || gSpeciesInfo[cand].isMythical)
                continue;
            if (candBst >= fromBst && candBst <= (fromBst * 3u) / 2u + 50u)
                return cand;
        }
    }

    return PickSpeciesWithConstraints(r, origTarget, true, false, TYPE_NONE);
}
