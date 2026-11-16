#include "global.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "global.fieldmap.h"
#include "item.h"
#include "main.h"
#include "overworld.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/vars.h"
#include "mp_cutscene_sync.h"
#include "mp_world_sync.h"

#include <stdio.h>
#include <string.h>

#define MP_WS_MAX_PENDING_FLAGS 128
#define MP_WS_MAX_PENDING_VARS  32

typedef struct PendingFlagOp {
    uint16_t flagId;
    bool value;
} PendingFlagOp;

typedef struct PendingVarOp {
    uint16_t varId;
    uint16_t value;
} PendingVarOp;

static bool sApplyingRemote = false;
static bool sTutorialCompletedNotified = false;
static PendingFlagOp sPendingFlags[MP_WS_MAX_PENDING_FLAGS];
static uint16_t sPendingFlagCount = 0;
static PendingVarOp sPendingVars[MP_WS_MAX_PENDING_VARS];
static uint16_t sPendingVarCount = 0;
static uint8_t sPendingBadgesMask = 0;

typedef struct BadgeHmMapping {
    uint16_t badgeFlag;
    uint16_t gymDefeatedFlag;
    uint16_t hmFlag;
    enum Item hmItem;
    uint16_t tmFlag;
    enum Item tmItem;
} BadgeHmMapping;

static const BadgeHmMapping kBadgeHmTable[8] = {
    { FLAG_BADGE01_GET, FLAG_DEFEATED_RUSTBORO_GYM,   FLAG_RECEIVED_HM_CUT,        ITEM_HM01, FLAG_RECEIVED_TM_ROCK_TOMB,   ITEM_TM39 },
    { FLAG_BADGE02_GET, FLAG_DEFEATED_DEWFORD_GYM,    FLAG_RECEIVED_HM_FLASH,      ITEM_HM05, FLAG_RECEIVED_TM_BULK_UP,     ITEM_TM08 },
    { FLAG_BADGE03_GET, FLAG_DEFEATED_MAUVILLE_GYM,   FLAG_RECEIVED_HM_ROCK_SMASH, ITEM_HM06, FLAG_RECEIVED_TM_SHOCK_WAVE,  ITEM_TM34 },
    { FLAG_BADGE04_GET, FLAG_DEFEATED_LAVARIDGE_GYM,  FLAG_RECEIVED_HM_STRENGTH,   ITEM_HM04, FLAG_RECEIVED_TM_OVERHEAT,    ITEM_TM50 },
    { FLAG_BADGE05_GET, FLAG_DEFEATED_PETALBURG_GYM,  FLAG_RECEIVED_HM_SURF,       ITEM_HM03, FLAG_RECEIVED_TM_FACADE,      ITEM_TM42 },
    { FLAG_BADGE06_GET, FLAG_DEFEATED_FORTREE_GYM,    FLAG_RECEIVED_HM_FLY,        ITEM_HM02, FLAG_RECEIVED_TM_AERIAL_ACE,  ITEM_TM40 },
    { FLAG_BADGE07_GET, FLAG_DEFEATED_MOSSDEEP_GYM,   FLAG_RECEIVED_HM_DIVE,       ITEM_HM08, FLAG_RECEIVED_TM_CALM_MIND,   ITEM_TM04 },
    { FLAG_BADGE08_GET, FLAG_DEFEATED_SOOTOPOLIS_GYM, FLAG_RECEIVED_HM_WATERFALL,  ITEM_HM07, FLAG_RECEIVED_TM_WATER_PULSE, ITEM_TM03 },
};

/* Explicit Hoenn story, boss, obstacle, badge, HM/TM, and milestone flags synced via Host.
 * Tutorial (Truck/Birch/Starter/Route 103/Pokedex), regular route trainers, per-player item balls,
 * hidden items, berry trees, and general NPC gifts remain strictly per-player. */
static const uint16_t kGlobalStoryFlags[] = {
    /* Story Progression & Unlocks */
    FLAG_LEGENDARIES_IN_SOOTOPOLIS,
    FLAG_RECOVERED_DEVON_GOODS,
    FLAG_RETURNED_DEVON_GOODS,
    FLAG_DELIVERED_DEVON_GOODS,
    FLAG_DELIVERED_STEVEN_LETTER,
    FLAG_EVIL_TEAM_ESCAPED_STERN_SPOKE,
    FLAG_RECEIVED_POKENAV,
    FLAG_HAS_MATCH_CALL,
    FLAG_ADDED_MATCH_CALL_TO_POKENAV,
    FLAG_SYS_POKENAV_GET,
    FLAG_RECEIVED_METEORITE,
    FLAG_MET_PROF_COZMO,
    FLAG_RECEIVED_WAILMER_PAIL,
    FLAG_RECEIVED_POKEBLOCK_CASE,
    FLAG_RECEIVED_GO_GOGGLES,
    FLAG_RECEIVED_DEVON_SCOPE,
    FLAG_RECEIVED_RED_OR_BLUE_ORB,
    FLAG_HIDE_JAGGED_PASS_MAGMA_GUARD,
    FLAG_HIDE_MT_PYRE_SUMMIT_ARCHIE,
    FLAG_HIDE_MT_PYRE_SUMMIT_TEAM_AQUA,
    FLAG_CONTEST_SKETCH_CREATED,
    FLAG_SYS_BRAILLE_DIG,
    FLAG_SYS_REGIROCK_PUZZLE_COMPLETED,
    FLAG_SYS_BRAILLE_REGICE_COMPLETED,
    FLAG_SYS_REGISTEEL_PUZZLE_COMPLETED,
    FLAG_SYS_WEATHER_CTRL,
    FLAG_SYS_TV_LATIAS_LATIOS,
    FLAG_SYS_GAME_CLEAR,

    /* Gym Leaders, Elite 4 & Major Story Bosses */
    FLAG_DEFEATED_RUSTBORO_GYM,
    FLAG_DEFEATED_DEWFORD_GYM,
    FLAG_DEFEATED_MAUVILLE_GYM,
    FLAG_DEFEATED_LAVARIDGE_GYM,
    FLAG_DEFEATED_PETALBURG_GYM,
    FLAG_DEFEATED_FORTREE_GYM,
    FLAG_DEFEATED_MOSSDEEP_GYM,
    FLAG_DEFEATED_SOOTOPOLIS_GYM,
    FLAG_DEFEATED_METEOR_FALLS_STEVEN,
    FLAG_DEFEATED_ELITE_4_SIDNEY,
    FLAG_DEFEATED_ELITE_4_PHOEBE,
    FLAG_DEFEATED_ELITE_4_GLACIA,
    FLAG_DEFEATED_ELITE_4_DRAKE,
    FLAG_DEFEATED_RIVAL_ROUTE_104,
    FLAG_DEFEATED_RIVAL_RUSTBORO,
    FLAG_DEFEATED_EVIL_TEAM_MT_CHIMNEY,
    FLAG_DEFEATED_MAGMA_SPACE_CENTER,
    FLAG_DEFEATED_GRUNT_SPACE_CENTER_1F,
    FLAG_DEFEATED_WALLY_MAUVILLE,
    FLAG_DEFEATED_WALLY_VICTORY_ROAD,
    FLAG_DEFEATED_SEASHORE_HOUSE,
    FLAG_DEFEATED_SS_TIDAL_TRAINERS,
    FLAG_DEFEATED_REGIROCK,
    FLAG_DEFEATED_REGICE,
    FLAG_DEFEATED_REGISTEEL,
    FLAG_DEFEATED_KYOGRE,
    FLAG_DEFEATED_GROUDON,
    FLAG_DEFEATED_RAYQUAZA,
    FLAG_DEFEATED_DEOXYS,
    FLAG_DEFEATED_MEW,
    FLAG_DEFEATED_LATIAS_OR_LATIOS,
    FLAG_DEFEATED_HO_OH,
    FLAG_DEFEATED_LUGIA,
    FLAG_DEFEATED_SUDOWOODO,

    /* Shared Story Gatekeepers / Legendaries / Hideouts / Contest Museum */
    FLAG_HIDE_PETALBURG_WOODS_DEVON_EMPLOYEE,
    FLAG_HIDE_PETALBURG_WOODS_AQUA_GRUNT,
    FLAG_HIDE_RUSTBORO_CITY_DEVON_EMPLOYEE_1,
    FLAG_HIDE_RUSTBORO_CITY_SCIENTIST,
    FLAG_HIDE_RUSTBORO_CITY_POKEMON_SCHOOL_SCOTT,
    FLAG_HIDE_ROUTE_116_WANDAS_BOYFRIEND,
    FLAG_HIDE_RUSTURF_TUNNEL_WANDAS_BOYFRIEND,
    FLAG_HIDE_RUSTURF_TUNNEL_WANDA,
    FLAG_HIDE_RUSTURF_TUNNEL_ROCK_1,
    FLAG_HIDE_RUSTURF_TUNNEL_ROCK_2,
    FLAG_HIDE_RUSTURF_TUNNEL_AQUA_GRUNT,
    FLAG_HIDE_RUSTURF_TUNNEL_PEEKO,
    FLAG_HIDE_BRINEYS_HOUSE_MR_BRINEY,
    FLAG_HIDE_BRINEYS_HOUSE_PEEKO,
    FLAG_HIDE_GRANITE_CAVE_STEVEN,
    FLAG_HIDE_SLATEPORT_CITY_TEAM_AQUA,
    FLAG_HIDE_SLATEPORT_CITY_OCEANIC_MUSEUM_AQUA_GRUNTS,
    FLAG_HIDE_ROUTE_119_TEAM_AQUA,
    FLAG_HIDE_WEATHER_INSTITUTE_1F_WORKERS,
    FLAG_HIDE_WEATHER_INSTITUTE_2F_WORKERS,
    FLAG_HIDE_WEATHER_INSTITUTE_2F_AQUA_GRUNT_M,
    FLAG_HIDE_ROUTE_120_STEVEN,
    FLAG_HIDE_FORTREE_CITY_KECLEON,
    FLAG_HIDE_LILYCOVE_MUSEUM_CURATOR,
    FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_1,
    FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_2,
    FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_3,
    FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_4,
    FLAG_HIDE_AQUA_HIDEOUT_GRUNTS,
    FLAG_HIDE_MAGMA_HIDEOUT_GRUNTS,
    FLAG_HIDE_SEAFLOOR_CAVERN_AQUA_GRUNTS,
    FLAG_HIDE_MOSSDEEP_CITY_TEAM_MAGMA,
    FLAG_HIDE_MOSSDEEP_CITY_SPACE_CENTER_1F_TEAM_MAGMA,
    FLAG_HIDE_MOSSDEEP_CITY_SPACE_CENTER_2F_TEAM_MAGMA,
    FLAG_HIDE_SOOTOPOLIS_CITY_WALLACE,
    FLAG_HIDE_SOOTOPOLIS_CITY_ARCHIE,
    FLAG_HIDE_SOOTOPOLIS_CITY_MAXIE,
    FLAG_HIDE_SOOTOPOLIS_CITY_STEVEN,
    FLAG_HIDE_SOOTOPOLIS_CITY_RAYQUAZA,
    FLAG_HIDE_SOOTOPOLIS_CITY_KYOGRE,
    FLAG_HIDE_SOOTOPOLIS_CITY_GROUDON,
    FLAG_HIDE_SKY_PILLAR_TOP_RAYQUAZA_STILL,
};

static const uint16_t kGlobalStoryVars[] = {
    VAR_PETALBURG_WOODS_STATE,
    VAR_RUSTBORO_CITY_STATE,
    VAR_DEVON_CORP_3F_STATE,
    VAR_ROUTE104_STATE,
    VAR_SLATEPORT_CITY_STATE,
    VAR_SLATEPORT_HARBOR_STATE,
    VAR_PETALBURG_GYM_STATE,
    VAR_MAUVILLE_GYM_STATE,
    VAR_MOSSDEEP_CITY_STATE,
    VAR_MOSSDEEP_SPACE_CENTER_STAIR_GUARD_STATE,
    VAR_MOSSDEEP_SPACE_CENTER_STATE,
    VAR_SOOTOPOLIS_CITY_STATE,
    VAR_SOOTOPOLIS_WALLACE_STATE,
    VAR_SKY_PILLAR_STATE,
    VAR_SKY_PILLAR_RAYQUAZA_CRY_DONE,
    VAR_ELITE_4_STATE,
    VAR_MT_PYRE_STATE,
    VAR_SCOTT_STATE,
};

static bool IsPerPlayerLocalPickupFlag(uint16_t flagId)
{
    /* Overworld item balls (0x3E8..0x4BF) & hidden items (0x1F4..0x273) */
    if (flagId >= FLAG_HIDDEN_ITEMS_START && flagId < (FLAG_HIDDEN_ITEMS_START + 0x80u))
        return true;
    if (flagId >= FLAG_ITEM_ROUTE_102_POTION && flagId <= 0x4BFu)
        return true;
    return false;
}

bool MpWorldSync_IsLocalTutorialComplete(void)
{
    return FlagGet(FLAG_SYS_POKEMON_GET)
        && FlagGet(FLAG_DEFEATED_RIVAL_ROUTE103)
        && FlagGet(FLAG_SYS_POKEDEX_GET)
        && FlagGet(FLAG_ADVENTURE_STARTED)
        && FlagGet(FLAG_RECEIVED_RUNNING_SHOES)
        && FlagGet(FLAG_SYS_B_DASH);
}

static int FindBadgeIndexForFlag(uint16_t flagId)
{
    if (flagId >= FLAG_BADGE01_GET && flagId <= FLAG_BADGE08_GET)
        return (int)(flagId - FLAG_BADGE01_GET);
    return -1;
}

static void GrantItemOnceIfMissing(enum Item item)
{
    if (item != ITEM_NONE && !CheckBagHasItem(item, 1))
        AddBagItem(item, 1);
}

static void RefreshMapObjectForFlag(uint16_t flagId)
{
    u32 i;
    if (flagId == 0 || gMain.inBattle || gMain.callback2 != CB2_Overworld)
        return;
    if (!gSaveBlock1Ptr || !gMapHeader.events)
        return;

    bool isHidden = FlagGet(flagId);
    u32 count = gMapHeader.events->objectEventCount;
    if (count > OBJECT_EVENT_TEMPLATES_COUNT)
        count = OBJECT_EVENT_TEMPLATES_COUNT;

    for (i = 0; i < count; i++) {
        const struct ObjectEventTemplate *tmpl = &gSaveBlock1Ptr->objectEventTemplates[i];
        if (tmpl->flagId != flagId)
            continue;
        if (isHidden) {
            RemoveObjectEventByLocalIdAndMap(tmpl->localId,
                                             gSaveBlock1Ptr->location.mapNum,
                                             gSaveBlock1Ptr->location.mapGroup);
        } else {
            TrySpawnObjectEvents(0, 0);
            break;
        }
    }
}

static void SetFlagAndRefreshIfChanged(uint16_t flagId, bool value)
{
    bool wasSet = FlagGet(flagId);
    if (value)
        FlagSet(flagId);
    else
        FlagClear(flagId);
    if (wasSet != value)
        RefreshMapObjectForFlag(flagId);
}

static void ApplyStoryCheckpointSideEffects(uint16_t triggerFlagId, uint16_t triggerVarId, uint16_t varVal)
{
    bool prev = sApplyingRemote;
    sApplyingRemote = true;

    if ((triggerVarId == VAR_PETALBURG_WOODS_STATE && varVal >= 1) ||
        VarGet(VAR_PETALBURG_WOODS_STATE) >= 1) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_PETALBURG_WOODS_DEVON_EMPLOYEE, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_PETALBURG_WOODS_AQUA_GRUNT, true);
    }

    if (triggerFlagId == FLAG_RECOVERED_DEVON_GOODS && FlagGet(FLAG_RECOVERED_DEVON_GOODS)) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTURF_TUNNEL_AQUA_GRUNT, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTURF_TUNNEL_PEEKO, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_BRINEYS_HOUSE_MR_BRINEY, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_BRINEYS_HOUSE_PEEKO, true);
        if (!FlagGet(FLAG_RETURNED_DEVON_GOODS) && !FlagGet(FLAG_DELIVERED_DEVON_GOODS))
            GrantItemOnceIfMissing(ITEM_DEVON_GOODS);
    }

    if ((triggerFlagId == FLAG_RETURNED_DEVON_GOODS && FlagGet(FLAG_RETURNED_DEVON_GOODS)) ||
        (triggerFlagId == FLAG_RECEIVED_POKENAV && FlagGet(FLAG_RECEIVED_POKENAV)) ||
        (triggerFlagId == FLAG_SYS_POKENAV_GET && FlagGet(FLAG_SYS_POKENAV_GET)) ||
        (triggerVarId == VAR_RUSTBORO_CITY_STATE && varVal >= 5)) {
        SetFlagAndRefreshIfChanged(FLAG_RETURNED_DEVON_GOODS, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTBORO_CITY_DEVON_EMPLOYEE_1, true);
        SetFlagAndRefreshIfChanged(FLAG_RECEIVED_POKENAV, true);
        SetFlagAndRefreshIfChanged(FLAG_SYS_POKENAV_GET, true);
        SetFlagAndRefreshIfChanged(FLAG_HAS_MATCH_CALL, true);
        SetFlagAndRefreshIfChanged(FLAG_ADDED_MATCH_CALL_TO_POKENAV, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTBORO_CITY_SCIENTIST, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTBORO_CITY_POKEMON_SCHOOL_SCOTT, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_ROUTE_116_WANDAS_BOYFRIEND, true);
        VarSet(VAR_DEVON_CORP_3F_STATE, 1);
        VarSet(VAR_ROUTE104_STATE, 1);
        /* Advance remote peers past state 6 to avoid triggering the Scientist door ambush at arbitrary coords. */
        if (VarGet(VAR_RUSTBORO_CITY_STATE) < 7)
            VarSet(VAR_RUSTBORO_CITY_STATE, 7);
        if (VarGet(VAR_BRINEY_LOCATION) == 0)
            VarSet(VAR_BRINEY_LOCATION, 1);
        if (!FlagGet(FLAG_DELIVERED_DEVON_GOODS))
            GrantItemOnceIfMissing(ITEM_DEVON_GOODS);
        if (!FlagGet(FLAG_DELIVERED_STEVEN_LETTER))
            GrantItemOnceIfMissing(ITEM_LETTER);
    }

    if (triggerFlagId == FLAG_DELIVERED_STEVEN_LETTER && FlagGet(FLAG_DELIVERED_STEVEN_LETTER)) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_GRANITE_CAVE_STEVEN, true);
    }

    if ((triggerFlagId == FLAG_DELIVERED_DEVON_GOODS ||
         triggerFlagId == FLAG_EVIL_TEAM_ESCAPED_STERN_SPOKE) &&
        FlagGet(triggerFlagId)) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_SLATEPORT_CITY_OCEANIC_MUSEUM_AQUA_GRUNTS, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_SLATEPORT_CITY_TEAM_AQUA, true);
    }

    if ((triggerFlagId == FLAG_RECEIVED_HM_STRENGTH ||
         triggerFlagId == FLAG_HIDE_RUSTURF_TUNNEL_ROCK_1) &&
        FlagGet(triggerFlagId)) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTURF_TUNNEL_ROCK_1, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTURF_TUNNEL_ROCK_2, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTURF_TUNNEL_WANDAS_BOYFRIEND, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_RUSTURF_TUNNEL_WANDA, true);
    }

    if ((triggerFlagId == FLAG_RECEIVED_METEORITE ||
         triggerFlagId == FLAG_DEFEATED_EVIL_TEAM_MT_CHIMNEY) &&
        FlagGet(triggerFlagId)) {
        SetFlagAndRefreshIfChanged(FLAG_RECEIVED_METEORITE, true);
        if (!FlagGet(FLAG_MET_PROF_COZMO))
            GrantItemOnceIfMissing(ITEM_METEORITE);
    }

    if ((triggerFlagId == FLAG_RECEIVED_GO_GOGGLES ||
         triggerFlagId == FLAG_BADGE04_GET) &&
        FlagGet(triggerFlagId)) {
        SetFlagAndRefreshIfChanged(FLAG_RECEIVED_GO_GOGGLES, true);
        GrantItemOnceIfMissing(ITEM_GO_GOGGLES);
    }

    if (triggerFlagId == FLAG_HIDE_WEATHER_INSTITUTE_2F_AQUA_GRUNT_M &&
        FlagGet(FLAG_HIDE_WEATHER_INSTITUTE_2F_AQUA_GRUNT_M)) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_ROUTE_119_TEAM_AQUA, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_WEATHER_INSTITUTE_1F_WORKERS, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_WEATHER_INSTITUTE_2F_WORKERS, true);
    }

    if (triggerFlagId == FLAG_RECEIVED_DEVON_SCOPE && FlagGet(FLAG_RECEIVED_DEVON_SCOPE)) {
        GrantItemOnceIfMissing(ITEM_DEVON_SCOPE);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_ROUTE_120_STEVEN, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_FORTREE_CITY_KECLEON, true);
    }

    if ((triggerFlagId == FLAG_RECEIVED_RED_OR_BLUE_ORB ||
         triggerFlagId == FLAG_HIDE_JAGGED_PASS_MAGMA_GUARD ||
         triggerFlagId == FLAG_HIDE_SKY_PILLAR_TOP_RAYQUAZA_STILL) &&
        FlagGet(triggerFlagId)) {
        SetFlagAndRefreshIfChanged(FLAG_RECEIVED_RED_OR_BLUE_ORB, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_JAGGED_PASS_MAGMA_GUARD, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_MT_PYRE_SUMMIT_ARCHIE, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_MT_PYRE_SUMMIT_TEAM_AQUA, true);
        if (VarGet(VAR_MT_PYRE_STATE) == 0)
            VarSet(VAR_MT_PYRE_STATE, 1);
        GrantItemOnceIfMissing(ITEM_MAGMA_EMBLEM);
    }

    if (triggerFlagId == FLAG_RECEIVED_WAILMER_PAIL && FlagGet(FLAG_RECEIVED_WAILMER_PAIL))
        GrantItemOnceIfMissing(ITEM_WAILMER_PAIL);

    if (triggerFlagId == FLAG_RECEIVED_POKEBLOCK_CASE && FlagGet(FLAG_RECEIVED_POKEBLOCK_CASE))
        GrantItemOnceIfMissing(ITEM_POKEBLOCK_CASE);

    if (triggerFlagId == FLAG_CONTEST_SKETCH_CREATED && FlagGet(FLAG_CONTEST_SKETCH_CREATED)) {
        SetFlagAndRefreshIfChanged(FLAG_HIDE_LILYCOVE_MUSEUM_CURATOR, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_1, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_2, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_3, true);
        SetFlagAndRefreshIfChanged(FLAG_HIDE_LILYCOVE_MUSEUM_PATRON_4, true);
    }

    sApplyingRemote = prev;
}

static void GrantSingleBadgeAndHm(int badgeIdx)
{
    if (badgeIdx < 0 || badgeIdx >= 8)
        return;

    bool prev = sApplyingRemote;
    sApplyingRemote = true;
    SetFlagAndRefreshIfChanged(kBadgeHmTable[badgeIdx].badgeFlag, true);
    SetFlagAndRefreshIfChanged(kBadgeHmTable[badgeIdx].gymDefeatedFlag, true);
    SetFlagAndRefreshIfChanged(kBadgeHmTable[badgeIdx].hmFlag, true);
    SetFlagAndRefreshIfChanged(kBadgeHmTable[badgeIdx].tmFlag, true);
    GrantItemOnceIfMissing(kBadgeHmTable[badgeIdx].hmItem);
    GrantItemOnceIfMissing(kBadgeHmTable[badgeIdx].tmItem);
    sApplyingRemote = prev;

    ApplyStoryCheckpointSideEffects(kBadgeHmTable[badgeIdx].badgeFlag, 0, 0);

    MpRunConfig *cfg = MpNet_GetRunConfigMutable();
    if (cfg)
        cfg->badgesMask |= (uint8_t)(1u << badgeIdx);
}

static void ApplyRemoteFlagSync(uint16_t flagId, bool value)
{
    bool prev = sApplyingRemote;
    sApplyingRemote = true;
    SetFlagAndRefreshIfChanged(flagId, value);
    sApplyingRemote = prev;

    int badgeIdx = FindBadgeIndexForFlag(flagId);
    if (badgeIdx >= 0 && value)
        GrantSingleBadgeAndHm(badgeIdx);

    ApplyStoryCheckpointSideEffects(flagId, 0, 0);
}

static void ApplyRemoteVarSync(uint16_t varId, uint16_t value)
{
    bool prev = sApplyingRemote;
    sApplyingRemote = true;
    VarSet(varId, value);
    sApplyingRemote = prev;

    ApplyStoryCheckpointSideEffects(0, varId, value);
}

static void QueuePendingFlag(uint16_t flagId, bool value)
{
    for (uint16_t i = 0; i < sPendingFlagCount; i++) {
        if (sPendingFlags[i].flagId == flagId) {
            sPendingFlags[i].value = value;
            return;
        }
    }
    if (sPendingFlagCount < MP_WS_MAX_PENDING_FLAGS) {
        sPendingFlags[sPendingFlagCount].flagId = flagId;
        sPendingFlags[sPendingFlagCount].value = value;
        sPendingFlagCount++;
    }
}

static void QueuePendingVar(uint16_t varId, uint16_t value)
{
    for (uint16_t i = 0; i < sPendingVarCount; i++) {
        if (sPendingVars[i].varId == varId) {
            sPendingVars[i].value = value;
            return;
        }
    }
    if (sPendingVarCount < MP_WS_MAX_PENDING_VARS) {
        sPendingVars[sPendingVarCount].varId = varId;
        sPendingVars[sPendingVarCount].value = value;
        sPendingVarCount++;
    }
}

void MpWorldSync_Init(void)
{
    sApplyingRemote = false;
    sTutorialCompletedNotified = false;
    sPendingFlagCount = 0;
    sPendingVarCount = 0;
    sPendingBadgesMask = 0;
}

bool MpWorldSync_IsApplyingRemote(void)
{
    return sApplyingRemote;
}

void MpWorldSync_CheckFlushDeferred(void)
{
    if (!MpWorldSync_IsLocalTutorialComplete()) {
        sTutorialCompletedNotified = false;
        return;
    }

    uint8_t badgesMask = sPendingBadgesMask;
    if (!sTutorialCompletedNotified) {
        const MpRunConfig *cfg = MpNet_GetRunConfig();
        sTutorialCompletedNotified = true;
        MpSoulLink_OnTutorialCompleted();
        if (cfg)
            badgesMask |= cfg->badgesMask;
    }

    if (sPendingFlagCount == 0 && sPendingVarCount == 0 && badgesMask == 0)
        return;

    uint16_t flagCount = sPendingFlagCount;
    uint16_t varCount = sPendingVarCount;

    sPendingFlagCount = 0;
    sPendingVarCount = 0;
    sPendingBadgesMask = 0;

    for (uint16_t i = 0; i < flagCount; i++)
        ApplyRemoteFlagSync(sPendingFlags[i].flagId, sPendingFlags[i].value);
    for (uint16_t i = 0; i < varCount; i++)
        ApplyRemoteVarSync(sPendingVars[i].varId, sPendingVars[i].value);
    for (int i = 0; i < 8; i++) {
        if ((badgesMask & (1u << i)) != 0 && !FlagGet(kBadgeHmTable[i].badgeFlag))
            GrantSingleBadgeAndHm(i);
    }
}

bool MpWorldSync_IsGlobalFlag(uint16_t flagId)
{
    if (flagId == 0 || flagId >= SPECIAL_FLAGS_START)
        return false;
    if (IsPerPlayerLocalPickupFlag(flagId))
        return false;
    if (FindBadgeIndexForFlag(flagId) >= 0)
        return true;

    for (size_t i = 0; i < 8; i++) {
        if (flagId == kBadgeHmTable[i].hmFlag ||
            flagId == kBadgeHmTable[i].tmFlag ||
            flagId == kBadgeHmTable[i].gymDefeatedFlag)
            return true;
    }
    for (size_t i = 0; i < sizeof(kGlobalStoryFlags) / sizeof(kGlobalStoryFlags[0]); i++) {
        if (kGlobalStoryFlags[i] != 0 && kGlobalStoryFlags[i] == flagId)
            return true;
    }
    return false;
}

bool MpWorldSync_IsGlobalVar(uint16_t varId)
{
    if (varId == 0)
        return false;
    for (size_t i = 0; i < sizeof(kGlobalStoryVars) / sizeof(kGlobalStoryVars[0]); i++) {
        if (kGlobalStoryVars[i] != 0 && kGlobalStoryVars[i] == varId)
            return true;
    }
    return false;
}

void MpWorldSync_ApplyRemoteFlagFromBridge(uint16_t flagId, bool value)
{
    if (!MpWorldSync_IsGlobalFlag(flagId))
        return;
    if (!MpWorldSync_IsLocalTutorialComplete())
        QueuePendingFlag(flagId, value);
    else if (FlagGet(flagId) != value)
        ApplyRemoteFlagSync(flagId, value);
}

void MpWorldSync_ApplyRemoteVarFromBridge(uint16_t varId, uint16_t value)
{
    if (!MpWorldSync_IsGlobalVar(varId))
        return;
    if (!MpWorldSync_IsLocalTutorialComplete())
        QueuePendingVar(varId, value);
    else if (VarGet(varId) != value)
        ApplyRemoteVarSync(varId, value);
}

void MpWorldSync_GrantBadgeHmsLocal(uint8_t badgesMask)
{
    if (!MpWorldSync_IsLocalTutorialComplete()) {
        sPendingBadgesMask |= badgesMask;
        return;
    }
    for (int i = 0; i < 8; i++) {
        if (badgesMask & (1u << i)) {
            GrantSingleBadgeAndHm(i);
        }
    }
}

void MpWorldSync_OnLocalFlagSet(uint16_t flagId, bool value)
{
    if (sApplyingRemote)
        return;

    if (value && (flagId == FLAG_SYS_POKEMON_GET
               || flagId == FLAG_DEFEATED_RIVAL_ROUTE103
               || flagId == FLAG_SYS_POKEDEX_GET
               || flagId == FLAG_ADVENTURE_STARTED
               || flagId == FLAG_RECEIVED_RUNNING_SHOES
               || flagId == FLAG_SYS_B_DASH))
        MpWorldSync_CheckFlushDeferred();

    if (!MpWorldSync_IsLocalTutorialComplete())
        return;

    if (!MpWorldSync_IsGlobalFlag(flagId))
        return;

    uint16_t targetPeer = MpNet_IsHost() ? MP_PEER_BROADCAST : MP_PEER_HOST;
    int badgeIdx = FindBadgeIndexForFlag(flagId);
    if (badgeIdx >= 0 && value) {
        if ((MpNet_GetRunConfig()->badgesMask & (1u << badgeIdx)) != 0)
            return;
        GrantSingleBadgeAndHm(badgeIdx);

        MpBadgeHmPayload badgePkt;
        badgePkt.badgeIndex = (uint8_t)badgeIdx;
        badgePkt.badgesMask = MpNet_GetRunConfig()->badgesMask;
        badgePkt.hmFlagsMask = (uint16_t)(1u << badgeIdx);
        MpNet_SendBinary(MP_CH_WORLD_SYNC, targetPeer, MP_PKT_WORLD_BADGE_HM, &badgePkt, sizeof(badgePkt));

        char json[96];
        snprintf(json, sizeof(json), "{\"t\":\"badge_unlock\",\"badgeIndex\":%d}", badgeIdx);
        MpNet_SendJson(MP_CH_WORLD_SYNC, MP_PEER_HOST, json);
        return;
    }

    MpFlagSyncPayload pkt;
    pkt.flagId = flagId;
    pkt.value = value ? 1u : 0u;
    MpNet_SendBinary(MP_CH_WORLD_SYNC, targetPeer, MP_PKT_WORLD_FLAG_SYNC, &pkt, sizeof(pkt));

    char json[96];
    snprintf(json, sizeof(json), "{\"t\":\"world_flag\",\"flagId\":%u,\"val\":%u}", (unsigned)flagId, value ? 1u : 0u);
    MpNet_SendJson(MP_CH_WORLD_SYNC, MP_PEER_HOST, json);
}

void MpWorldSync_OnLocalVarSet(uint16_t varId, uint16_t value)
{
    if (sApplyingRemote || !MpWorldSync_IsLocalTutorialComplete() || !MpWorldSync_IsGlobalVar(varId))
        return;

    uint16_t targetPeer = MpNet_IsHost() ? MP_PEER_BROADCAST : MP_PEER_HOST;
    MpVarSyncPayload pkt;
    pkt.varId = varId;
    pkt.value = value;
    MpNet_SendBinary(MP_CH_WORLD_SYNC, targetPeer, MP_PKT_WORLD_VAR_SYNC, &pkt, sizeof(pkt));

    char json[96];
    snprintf(json, sizeof(json), "{\"t\":\"world_var\",\"varId\":%u,\"val\":%u}", (unsigned)varId, (unsigned)value);
    MpNet_SendJson(MP_CH_WORLD_SYNC, MP_PEER_HOST, json);
}

void MpWorldSync_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    bool relayAsHost = MpNet_IsHost() && fromPeerId != 0 && fromPeerId != MpNet_GetLocalPeerId();

    switch (opcode) {
    case MP_PKT_WORLD_FLAG_SYNC:
        if (len >= sizeof(MpFlagSyncPayload)) {
            MpFlagSyncPayload pkt;
            memcpy(&pkt, payload, sizeof(pkt));
            if (MpWorldSync_IsGlobalFlag(pkt.flagId)) {
                bool val = (pkt.value != 0);
                if (!MpWorldSync_IsLocalTutorialComplete())
                    QueuePendingFlag(pkt.flagId, val);
                else
                    ApplyRemoteFlagSync(pkt.flagId, val);

                if (relayAsHost)
                    MpNet_SendBinary(MP_CH_WORLD_SYNC, MP_PEER_BROADCAST, MP_PKT_WORLD_FLAG_SYNC, &pkt, sizeof(pkt));
            }
        }
        break;
    case MP_PKT_WORLD_VAR_SYNC:
        if (len >= sizeof(MpVarSyncPayload)) {
            MpVarSyncPayload pkt;
            memcpy(&pkt, payload, sizeof(pkt));
            if (MpWorldSync_IsGlobalVar(pkt.varId)) {
                if (!MpWorldSync_IsLocalTutorialComplete())
                    QueuePendingVar(pkt.varId, pkt.value);
                else
                    ApplyRemoteVarSync(pkt.varId, pkt.value);

                if (relayAsHost)
                    MpNet_SendBinary(MP_CH_WORLD_SYNC, MP_PEER_BROADCAST, MP_PKT_WORLD_VAR_SYNC, &pkt, sizeof(pkt));
            }
        }
        break;
    case MP_PKT_WORLD_BADGE_HM:
        if (len >= sizeof(MpBadgeHmPayload)) {
            MpBadgeHmPayload pkt;
            memcpy(&pkt, payload, sizeof(pkt));
            uint8_t mask = pkt.badgesMask;
            if (pkt.badgeIndex < 8)
                mask |= (uint8_t)(1u << pkt.badgeIndex);
            MpWorldSync_GrantBadgeHmsLocal(mask);

            if (relayAsHost) {
                pkt.badgesMask = MpNet_GetRunConfig()->badgesMask;
                MpNet_SendBinary(MP_CH_WORLD_SYNC, MP_PEER_BROADCAST, MP_PKT_WORLD_BADGE_HM, &pkt, sizeof(pkt));
            }
        }
        break;
    case MP_PKT_WORLD_CUTSCENE:
        MpCutscene_HandlePacket(fromPeerId, opcode, payload, len);
        break;
    default:
        break;
    }
}
