#include "global.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_control_avatar.h"
#include "field_effect.h"
#include "field_effect_helpers.h"
#include "field_message_box.h"
#include "field_player_avatar.h"
#include "field_screen_effect.h"
#include "fieldmap.h"
#include "gba/m4a_internal.h"
#include "gpu_regs.h"
#include "item.h"
#include "load_save.h"
#include "main.h"
#include "main_menu.h"
#include "money.h"
#include "new_game.h"
#include "overworld.h"
#include "palette.h"
#include "play_time.h"
#include "pokemon.h"
#include "region_map.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "title_screen.h"
#include "window.h"
#include "constants/battle.h"
#include "constants/characters.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/field_effects.h"
#include "constants/flags.h"
#include "constants/heal_locations.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/pokeball.h"
#include "constants/region_map_sections.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/vars.h"
#include "mp_net.h"
#include "mp_overworld.h"
#include "mp_randomizer.h"
#include "mp_soul_link.h"
#include "mp_trade.h"
#include "mp_world_sync.h"
#include "mp_battle_2v2.h"
#include "battle.h"
#include "battle_setup.h"
#include "pokedex.h"
#include "pokemon_storage_system.h"
#include "script_pokemon_util.h"
#include "starter_choose.h"
#include "wild_encounter.h"
#include "constants/opponents.h"
#include "wasm_platform.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, js_mp_on_flash_saved, (void), {
    if (typeof window !== "undefined" && window.PokemonWasmBridge &&
        typeof window.PokemonWasmBridge._onFlashSavedInWasm === "function") {
        window.PokemonWasmBridge._onFlashSavedInWasm();
    }
});
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Distinct localIds (225..228 followers, 230..233 players) safely clear of map NPCs, Berry Blender (236..240), OWE (249..252), Player (255). */
#define MP_REMOTE_FOLLOWER_LOCALID_BASE 225u
#define MP_REMOTE_LOCALID_BASE          230u

static uint32_t sOwTick = 0;
static uint32_t sLastBroadcastTick = 0;
static uint32_t sLastReportTick = 0;
static uint32_t sLastFullStateSig = 0;
static uint32_t sLastFullReportTick = 0;
static bool sHasSentFullState = false;
static bool sHasLastSentPos = false;
static MpOverworldPosPayload sLastSentPos;
static char sStateJsonBuf[8192];

static bool sPendingWarpActive = false;
static int8_t sPendingWarpGroup = 0;
static int8_t sPendingWarpNum = 0;
static int16_t sPendingWarpX = 0;
static int16_t sPendingWarpY = 0;

static bool sSpawnedActive[MP_MAX_PLAYERS];
static int8_t sSpawnedGroup[MP_MAX_PLAYERS];
static int8_t sSpawnedNum[MP_MAX_PLAYERS];
static u8 sRemoteSurfBlobSpriteId[MP_MAX_PLAYERS];
static uint8_t sRemotePlayerStuckTicks[MP_MAX_PLAYERS];
static s16 sRemotePlayerLastX[MP_MAX_PLAYERS];
static s16 sRemotePlayerLastY[MP_MAX_PLAYERS];
static uint8_t sRemoteFollowerObjIds[MP_MAX_PLAYERS];
static uint8_t sRemoteFollowerStuckTicks[MP_MAX_PLAYERS];
static s16 sRemoteFollowerLastX[MP_MAX_PLAYERS];
static s16 sRemoteFollowerLastY[MP_MAX_PLAYERS];
static uint8_t sRemoteFollowerBobTicks[MP_MAX_PLAYERS];

static bool sHasAutoSaveBaseline = false;
static int8_t sLastAutoSaveGroup = -1;
static int8_t sLastAutoSaveNum = -1;
static bool sWasInBattle = false;
static uint16_t sPendingAutoSaveDelay = 0;
static bool sHasEnteredGameplay = false;

static bool IsInFreeOverworld(void)
{
    if (gMain.inBattle)
        return false;
    if (gMain.callback2 != CB2_Overworld)
        return false;
    return true;
}

static bool CanWarpSafelyNow(void)
{
    if (!IsInFreeOverworld())
        return false;
    if (gPaletteFade.active)
        return false;
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
        return false;
    return true;
}

static uint16_t SanitizeRemoteGfx(uint16_t gfxId, uint8_t gender, uint8_t movementState)
{
    enum Gender g = (gender == FEMALE) ? FEMALE : MALE;

    if (movementState & PLAYER_AVATAR_FLAG_SURFING)
        return GetPlayerAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_SURFING, g);
    if (movementState & PLAYER_AVATAR_FLAG_UNDERWATER)
        return GetPlayerAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_UNDERWATER, g);
    if (movementState & PLAYER_AVATAR_FLAG_MACH_BIKE)
        return GetPlayerAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_MACH_BIKE, g);
    if (movementState & PLAYER_AVATAR_FLAG_ACRO_BIKE)
        return GetPlayerAvatarGraphicsIdByStateIdAndGender(PLAYER_AVATAR_STATE_ACRO_BIKE, g);

    if (gfxId != 0 && gfxId < NUM_OBJ_EVENT_GFX)
        return gfxId;
    return (g == FEMALE) ? PLAYER_AVATAR_GFX_FEMALE_NORMAL : PLAYER_AVATAR_GFX_MALE_NORMAL;
}

static enum Direction SanitizeFacing(uint8_t facing)
{
    if (facing >= DIR_SOUTH && facing <= DIR_EAST)
        return (enum Direction)facing;
    return DIR_SOUTH;
}

static void DestroyRemoteSurfBlob(uint8_t slot)
{
    if (slot >= MP_MAX_PLAYERS)
        return;
    u8 blobId = sRemoteSurfBlobSpriteId[slot];
    if (blobId < MAX_SPRITES) {
        if (gSprites[blobId].inUse && gSprites[blobId].callback == UpdateSurfBlobFieldEffect)
            DestroySprite(&gSprites[blobId]);
        sRemoteSurfBlobSpriteId[slot] = MAX_SPRITES;
    }
}

static void ClearRemoteObjectMovement(struct ObjectEvent *obj)
{
    if (obj->spriteId < MAX_SPRITES)
        ClearObjectEventMovement(obj, &gSprites[obj->spriteId]);
    else {
        obj->singleMovementActive = FALSE;
        ObjectEventClearHeldMovementIfActive(obj);
    }
}

static bool ResolvePeerCoordsOnLocalMap(const MpPeerState *peer,
                                        int8_t localGroup,
                                        int8_t localNum,
                                        s16 peerX,
                                        s16 peerY,
                                        s16 *outLocalX,
                                        s16 *outLocalY)
{
    if (peer == NULL || !peer->connected || !peer->hasPosition)
        return false;

    if (peer->pos.mapGroup == localGroup && peer->pos.mapNum == localNum) {
        *outLocalX = peerX;
        *outLocalY = peerY;
        return true;
    }

    if (!gMapHeader.connections || !gMapHeader.mapLayout)
        return false;

    s32 localW = gMapHeader.mapLayout->width;
    s32 localH = gMapHeader.mapLayout->height;
    s32 count = gMapHeader.connections->count;
    const struct MapConnection *conns = gMapHeader.connections->connections;
    for (s32 i = 0; i < count; i++) {
        const struct MapConnection *c = &conns[i];
        if ((int8_t)c->mapGroup != peer->pos.mapGroup || (int8_t)c->mapNum != peer->pos.mapNum)
            continue;

        const struct MapHeader *connHeader = Overworld_GetMapHeaderByGroupAndId(c->mapGroup, c->mapNum);
        if (!connHeader || !connHeader->mapLayout)
            continue;

        s16 tx = 0;
        s16 ty = 0;
        switch (c->direction) {
        case CONNECTION_NORTH:
            tx = (s16)(peerX + c->offset);
            ty = (s16)(peerY - connHeader->mapLayout->height);
            break;
        case CONNECTION_SOUTH:
            tx = (s16)(peerX + c->offset);
            ty = (s16)(localH + peerY);
            break;
        case CONNECTION_WEST:
            tx = (s16)(peerX - connHeader->mapLayout->width);
            ty = (s16)(peerY + c->offset);
            break;
        case CONNECTION_EAST:
            tx = (s16)(localW + peerX);
            ty = (s16)(peerY + c->offset);
            break;
        default:
            continue;
        }

        if (tx >= -6 && tx < localW + 6 && ty >= -6 && ty < localH + 6) {
            *outLocalX = tx;
            *outLocalY = ty;
            return true;
        }
    }
    return false;
}

static void DespawnRemoteFollowerSlot(uint8_t slot)
{
    if (slot >= MP_MAX_PLAYERS)
        return;

    uint8_t localId = (uint8_t)(MP_REMOTE_FOLLOWER_LOCALID_BASE + slot);
    for (u8 i = 0; i < OBJECT_EVENTS_COUNT; i++) {
        if (gObjectEvents[i].active && !gObjectEvents[i].isPlayer && gObjectEvents[i].localId == localId)
            RemoveObjectEvent(&gObjectEvents[i]);
    }

    sRemoteFollowerObjIds[slot] = OBJECT_EVENTS_COUNT;
    sRemoteFollowerStuckTicks[slot] = 0;
    sRemoteFollowerLastX[slot] = -1;
    sRemoteFollowerLastY[slot] = -1;
    sRemoteFollowerBobTicks[slot] = 0;
}

static void DespawnRemoteSlot(uint8_t slot)
{
    if (slot >= MP_MAX_PLAYERS)
        return;

    DestroyRemoteSurfBlob(slot);
    DespawnRemoteFollowerSlot(slot);

    uint8_t localId = (uint8_t)(MP_REMOTE_LOCALID_BASE + slot);
    for (u8 i = 0; i < OBJECT_EVENTS_COUNT; i++) {
        if (gObjectEvents[i].active && !gObjectEvents[i].isPlayer && gObjectEvents[i].localId == localId)
            RemoveObjectEvent(&gObjectEvents[i]);
    }

    sSpawnedActive[slot] = false;
    sRemotePlayerStuckTicks[slot] = 0;
    sRemotePlayerLastX[slot] = -1;
    sRemotePlayerLastY[slot] = -1;
}

static void SyncRemoteSprites(void)
{
    if (!IsInFreeOverworld() || !gSaveBlock1Ptr)
        return;

    uint8_t localSlot = MpNet_GetLocalSlot();
    int8_t localGroup = gSaveBlock1Ptr->location.mapGroup;
    int8_t localNum = gSaveBlock1Ptr->location.mapNum;

    for (uint8_t slot = 0; slot < MP_MAX_PLAYERS; slot++) {
        if (slot == localSlot)
            continue;

        const MpPeerState *peer = MpNet_GetPeerState(slot);
        s16 resolvedX = 0;
        s16 resolvedY = 0;
        bool visibleOnMap = ResolvePeerCoordsOnLocalMap(peer, localGroup, localNum,
                                                        peer ? peer->pos.x : 0,
                                                        peer ? peer->pos.y : 0,
                                                        &resolvedX, &resolvedY);

        if (!visibleOnMap) {
            DespawnRemoteSlot(slot);
            continue;
        }

        if (sSpawnedActive[slot] &&
            (sSpawnedGroup[slot] != localGroup || sSpawnedNum[slot] != localNum)) {
            DespawnRemoteSlot(slot);
        }

        s16 mapShiftX = (s16)(resolvedX - peer->pos.x);
        s16 mapShiftY = (s16)(resolvedY - peer->pos.y);
        uint8_t localId = (uint8_t)(MP_REMOTE_LOCALID_BASE + slot);
        s16 shiftedX = (s16)(resolvedX + MAP_OFFSET);
        s16 shiftedY = (s16)(resolvedY + MAP_OFFSET);
        uint16_t gfxId = SanitizeRemoteGfx(peer->pos.graphicsId, peer->pos.gender, peer->pos.movementState);
        enum Direction facing = SanitizeFacing(peer->pos.facing);

        u8 objId = OBJECT_EVENTS_COUNT;
        bool spawnedNow = false;
        if (TryGetObjectEventIdByLocalIdAndMap(localId, (u8)localNum, (u8)localGroup, &objId) != FALSE ||
            objId >= OBJECT_EVENTS_COUNT) {
            objId = SpawnSpecialObjectEventParameterized(
                gfxId,
                MOVEMENT_TYPE_NONE,
                localId,
                shiftedX,
                shiftedY,
                peer->pos.elevation);
            if (objId >= OBJECT_EVENTS_COUNT)
                continue;
            sSpawnedActive[slot] = true;
            sSpawnedGroup[slot] = localGroup;
            sSpawnedNum[slot] = localNum;
            sRemoteSurfBlobSpriteId[slot] = MAX_SPRITES;
            sRemotePlayerStuckTicks[slot] = 0;
            sRemotePlayerLastX[slot] = shiftedX;
            sRemotePlayerLastY[slot] = shiftedY;
            spawnedNow = true;
        }

        struct ObjectEvent *obj = &gObjectEvents[objId];
        ObjectEventClearHeldMovementIfFinished(obj);

        bool gfxChanged = (obj->graphicsId != gfxId);
        if (gfxChanged) {
            ClearRemoteObjectMovement(obj);
            ObjectEventSetGraphicsId(obj, gfxId);
            if (obj->spriteId < MAX_SPRITES)
                MoveObjectEventToMapCoords(obj, obj->currentCoords.x, obj->currentCoords.y);
            obj->initialCoords = obj->currentCoords;
            ObjectEventTurn(obj, facing);
        }

        if (!ObjectEventIsHeldMovementActive(obj) && !obj->singleMovementActive) {
            obj->currentElevation = peer->pos.elevation;
            obj->previousElevation = peer->pos.elevation;
        }

        s16 dx = (s16)(shiftedX - obj->currentCoords.x);
        s16 dy = (s16)(shiftedY - obj->currentCoords.y);
        s16 adx = (dx < 0) ? (s16)(-dx) : dx;
        s16 ady = (dy < 0) ? (s16)(-dy) : dy;
        s16 dist = (s16)(adx + ady);

        bool playerStuck = false;
        if (!spawnedNow && dist >= 1) {
            if (obj->currentCoords.x == sRemotePlayerLastX[slot] &&
                obj->currentCoords.y == sRemotePlayerLastY[slot]) {
                if (++sRemotePlayerStuckTicks[slot] > 18u)
                    playerStuck = true;
            } else {
                sRemotePlayerLastX[slot] = obj->currentCoords.x;
                sRemotePlayerLastY[slot] = obj->currentCoords.y;
                sRemotePlayerStuckTicks[slot] = 0;
            }
        } else {
            sRemotePlayerLastX[slot] = obj->currentCoords.x;
            sRemotePlayerLastY[slot] = obj->currentCoords.y;
            sRemotePlayerStuckTicks[slot] = 0;
        }

        if (spawnedNow || dist > 4 || playerStuck || (dist >= 1 && peer->pos.inBattle != 0)) {
            ClearRemoteObjectMovement(obj);
            if (obj->spriteId < MAX_SPRITES)
                MoveObjectEventToMapCoords(obj, shiftedX, shiftedY);
            else
                ShiftObjectEventCoords(obj, shiftedX, shiftedY);
            obj->initialCoords = obj->currentCoords;
            sRemotePlayerLastX[slot] = obj->currentCoords.x;
            sRemotePlayerLastY[slot] = obj->currentCoords.y;
            sRemotePlayerStuckTicks[slot] = 0;
            ObjectEventTurn(obj, facing);
        } else if (dist >= 1) {
            obj->initialCoords = obj->currentCoords;
            if (!ObjectEventIsHeldMovementActive(obj) && !obj->singleMovementActive) {
                enum Direction stepDir = DIR_SOUTH;
                if (adx >= ady)
                    stepDir = (dx > 0) ? DIR_EAST : DIR_WEST;
                else
                    stepDir = (dy > 0) ? DIR_SOUTH : DIR_NORTH;

                bool isNormalFootGfx = (gfxId == PLAYER_AVATAR_GFX_MALE_NORMAL ||
                                        gfxId == PLAYER_AVATAR_GFX_FEMALE_NORMAL ||
                                        gfxId == OBJ_EVENT_GFX_BRENDAN_NORMAL ||
                                        gfxId == OBJ_EVENT_GFX_MAY_NORMAL ||
                                        gfxId == OBJ_EVENT_GFX_RIVAL_BRENDAN_NORMAL ||
                                        gfxId == OBJ_EVENT_GFX_RIVAL_MAY_NORMAL);
                u8 action;
                if ((peer->pos.movementState & PLAYER_AVATAR_FLAG_MACH_BIKE) && dist >= 2)
                    action = GetWalkFasterMovementAction(stepDir);
                else if ((peer->pos.movementState & PLAYER_AVATAR_FLAG_DASH) && isNormalFootGfx)
                    action = GetPlayerRunMovementAction(stepDir);
                else if ((peer->pos.movementState & (PLAYER_AVATAR_FLAG_BIKE | PLAYER_AVATAR_FLAG_SURFING)) || dist >= 2)
                    action = GetWalkFastMovementAction(stepDir);
                else
                    action = GetWalkNormalMovementAction(stepDir);

                ObjectEventSetHeldMovement(obj, action);
            }
        } else {
            obj->initialCoords = obj->currentCoords;
            if (!ObjectEventIsHeldMovementActive(obj) && !obj->singleMovementActive &&
                (enum Direction)obj->facingDirection != facing) {
                ObjectEventTurn(obj, facing);
            }
        }

        /* Validate and synchronize surf mount sprite when surfing */
        u8 existingBlob = sRemoteSurfBlobSpriteId[slot];
        if (existingBlob < MAX_SPRITES) {
            if (!gSprites[existingBlob].inUse ||
                gSprites[existingBlob].callback != UpdateSurfBlobFieldEffect ||
                (u8)gSprites[existingBlob].data[2] != objId) {
                sRemoteSurfBlobSpriteId[slot] = MAX_SPRITES;
            }
        }

        if ((peer->pos.movementState & PLAYER_AVATAR_FLAG_SURFING) != 0) {
            if (sRemoteSurfBlobSpriteId[slot] >= MAX_SPRITES && obj->spriteId < MAX_SPRITES) {
                gFieldEffectArguments[0] = obj->currentCoords.x;
                gFieldEffectArguments[1] = obj->currentCoords.y;
                gFieldEffectArguments[2] = objId;
                u8 blobId = (u8)FieldEffectStart(FLDEFF_SURF_BLOB);
                if (blobId < MAX_SPRITES) {
                    SetSurfBlob_BobState(blobId, BOB_PLAYER_AND_MON);
                    gSprites[blobId].oam.paletteNum = LoadPlayerObjectEventPalette((peer->pos.gender == FEMALE) ? FEMALE : MALE);
                    sRemoteSurfBlobSpriteId[slot] = blobId;
                }
            }
        } else {
            if (sRemoteSurfBlobSpriteId[slot] < MAX_SPRITES)
                DestroyRemoteSurfBlob(slot);
            if (obj->spriteId < MAX_SPRITES && !ObjectEventIsHeldMovementActive(obj))
                gSprites[obj->spriteId].y2 = 0;
        }

        /* Synchronize peer's Following Pokémon sprite, movement, walk-in-place idle, and bobbing */
        if ((peer->pos.followerGfxId & OBJ_EVENT_MON) == 0 ||
            SanitizeSpeciesId(peer->pos.followerGfxId & OBJ_EVENT_MON_SPECIES_MASK) == SPECIES_NONE) {
            DespawnRemoteFollowerSlot(slot);
            continue;
        }

        uint8_t fLocalId = (uint8_t)(MP_REMOTE_FOLLOWER_LOCALID_BASE + slot);
        s16 fTargetX = (s16)(peer->pos.followerX + mapShiftX + MAP_OFFSET);
        s16 fTargetY = (s16)(peer->pos.followerY + mapShiftY + MAP_OFFSET);
        if (fTargetX < 0 || fTargetY < 0 ||
            fTargetX >= gBackupMapLayout.width || fTargetY >= gBackupMapLayout.height) {
            DespawnRemoteFollowerSlot(slot);
            continue;
        }
        enum Direction fDir = SanitizeFacing(peer->pos.followerFacing);
        uint16_t fGfxId = peer->pos.followerGfxId;
        uint8_t fElev = peer->pos.followerElevation;

        u8 fObjId = OBJECT_EVENTS_COUNT;
        bool fSpawnedNow = false;
        if (TryGetObjectEventIdByLocalIdAndMap(fLocalId, (u8)localNum, (u8)localGroup, &fObjId) != FALSE ||
            fObjId >= OBJECT_EVENTS_COUNT) {
            fObjId = SpawnSpecialObjectEventParameterized(
                fGfxId,
                MOVEMENT_TYPE_NONE,
                fLocalId,
                fTargetX,
                fTargetY,
                fElev);
            if (fObjId >= OBJECT_EVENTS_COUNT)
                continue;
            sRemoteFollowerObjIds[slot] = fObjId;
            sRemoteFollowerStuckTicks[slot] = 0;
            sRemoteFollowerLastX[slot] = fTargetX;
            sRemoteFollowerLastY[slot] = fTargetY;
            sRemoteFollowerBobTicks[slot] = 0;
            fSpawnedNow = true;
        }

        struct ObjectEvent *fObj = &gObjectEvents[fObjId];
        sRemoteFollowerObjIds[slot] = fObjId;
        fObj->range.rangeX = 0;
        fObj->range.rangeY = 0;
        fObj->disableCoveringGroundEffects = FALSE;
        fObj->triggerGroundEffectsOnMove = TRUE;
        fObj->triggerGroundEffectsOnStop = TRUE;
        ObjectEventClearHeldMovementIfFinished(fObj);

        if (fSpawnedNow) {
            fObj->initialCoords = fObj->currentCoords;
            ObjectEventTurn(fObj, fDir);
        } else if (fObj->graphicsId != fGfxId) {
            ClearRemoteObjectMovement(fObj);
            ObjectEventSetGraphicsId(fObj, fGfxId);
            if (fObj->spriteId < MAX_SPRITES)
                MoveObjectEventToMapCoords(fObj, fObj->currentCoords.x, fObj->currentCoords.y);
            fObj->initialCoords = fObj->currentCoords;
            ObjectEventTurn(fObj, fDir);
        }

        if (!ObjectEventIsHeldMovementActive(fObj) && !fObj->singleMovementActive) {
            fObj->currentElevation = fElev;
            fObj->previousElevation = fElev;
        }

        s16 fDx = (s16)(fTargetX - fObj->currentCoords.x);
        s16 fDy = (s16)(fTargetY - fObj->currentCoords.y);
        s16 fAdx = (fDx < 0) ? (s16)(-fDx) : fDx;
        s16 fAdy = (fDy < 0) ? (s16)(-fDy) : fDy;
        s16 fDist = (s16)(fAdx + fAdy);

        bool followerStuck = false;
        if (!fSpawnedNow && fDist >= 1) {
            if (fObj->currentCoords.x == sRemoteFollowerLastX[slot] &&
                fObj->currentCoords.y == sRemoteFollowerLastY[slot]) {
                if (++sRemoteFollowerStuckTicks[slot] > 16u)
                    followerStuck = true;
            } else {
                sRemoteFollowerLastX[slot] = fObj->currentCoords.x;
                sRemoteFollowerLastY[slot] = fObj->currentCoords.y;
                sRemoteFollowerStuckTicks[slot] = 0;
            }
        } else {
            sRemoteFollowerLastX[slot] = fObj->currentCoords.x;
            sRemoteFollowerLastY[slot] = fObj->currentCoords.y;
            sRemoteFollowerStuckTicks[slot] = 0;
        }

        if (fSpawnedNow || fDist > 4 || followerStuck || (fDist >= 1 && peer->pos.inBattle != 0)) {
            ClearRemoteObjectMovement(fObj);
            if (fObj->spriteId < MAX_SPRITES)
                MoveObjectEventToMapCoords(fObj, fTargetX, fTargetY);
            else
                ShiftObjectEventCoords(fObj, fTargetX, fTargetY);
            fObj->initialCoords = fObj->currentCoords;
            sRemoteFollowerLastX[slot] = fObj->currentCoords.x;
            sRemoteFollowerLastY[slot] = fObj->currentCoords.y;
            ObjectEventTurn(fObj, fDir);
            sRemoteFollowerStuckTicks[slot] = 0;
        } else if (fDist >= 1) {
            fObj->initialCoords = fObj->currentCoords;
            if (!ObjectEventIsHeldMovementActive(fObj) && !fObj->singleMovementActive) {
                enum Direction fStepDir = DIR_SOUTH;
                u8 fAction;
                if (fAdx >= fAdy)
                    fStepDir = (fDx > 0) ? DIR_EAST : DIR_WEST;
                else
                    fStepDir = (fDy > 0) ? DIR_SOUTH : DIR_NORTH;

                if ((peer->pos.followerFlags & MP_FOLLOWER_F_JUMPING) && fDist == 2 && (fDx == 0 || fDy == 0))
                    fAction = GetJump2MovementAction(fStepDir);
                else if ((peer->pos.followerFlags & MP_FOLLOWER_F_JUMPING) && fDist == 1)
                    fAction = GetJumpMovementAction(fStepDir);
                else if ((peer->pos.movementState & PLAYER_AVATAR_FLAG_MACH_BIKE) && fDist >= 2)
                    fAction = GetWalkFasterMovementAction(fStepDir);
                else if (fDist >= 2 || (peer->pos.followerFlags & MP_FOLLOWER_F_FAST) || (peer->pos.movementState & PLAYER_AVATAR_FLAG_DASH))
                    fAction = GetWalkFastMovementAction(fStepDir);
                else
                    fAction = GetWalkNormalMovementAction(fStepDir);

                ObjectEventSetHeldMovement(fObj, fAction);
            }
        } else {
            fObj->initialCoords = fObj->currentCoords;
            sRemoteFollowerStuckTicks[slot] = 0;
            if (!ObjectEventIsHeldMovementActive(fObj) && !fObj->singleMovementActive &&
                (enum Direction)fObj->facingDirection != fDir) {
                ObjectEventTurn(fObj, fDir);
            }
        }

        if (fObj->spriteId < MAX_SPRITES) {
            struct Sprite *fSp = &gSprites[fObj->spriteId];
            u8 fActNow = fObj->movementActionId;
            bool localJumpNow = fObj->heldMovementActive &&
                ((fActNow >= MOVEMENT_ACTION_JUMP_2_DOWN && fActNow <= MOVEMENT_ACTION_JUMP_2_RIGHT) ||
                 (fActNow >= MOVEMENT_ACTION_JUMP_SPECIAL_DOWN && fActNow <= MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT) ||
                 (fActNow >= MOVEMENT_ACTION_JUMP_DOWN && fActNow <= MOVEMENT_ACTION_JUMP_RIGHT));
            sRemoteFollowerBobTicks[slot]++;
            if (!localJumpNow && (peer->pos.followerFlags & MP_FOLLOWER_F_JUMPING) == 0) {
                if (OW_FOLLOWERS_BOBBING == TRUE && (sRemoteFollowerBobTicks[slot] & 7u) == 2u) {
                    fSp->y2 ^= -1;
                }
            }
            if (peer->pos.inBattle != 0)
                fSp->invisible = ((sOwTick & 4u) != 0u);
            else
                fSp->invisible = FALSE;
        }
    }
}

void MpOverworld_Init(void)
{
    sOwTick = 0;
    sLastBroadcastTick = 0;
    sLastReportTick = 0;
    sHasLastSentPos = false;
    memset(&sLastSentPos, 0, sizeof(sLastSentPos));
    sPendingWarpActive = false;
    memset(sSpawnedActive, 0, sizeof(sSpawnedActive));
    memset(sSpawnedGroup, 0, sizeof(sSpawnedGroup));
    memset(sSpawnedNum, 0, sizeof(sSpawnedNum));
    for (uint8_t i = 0; i < MP_MAX_PLAYERS; i++) {
        sRemoteSurfBlobSpriteId[i] = MAX_SPRITES;
        sRemotePlayerStuckTicks[i] = 0;
        sRemotePlayerLastX[i] = -1;
        sRemotePlayerLastY[i] = -1;
        sRemoteFollowerObjIds[i] = OBJECT_EVENTS_COUNT;
        sRemoteFollowerStuckTicks[i] = 0;
        sRemoteFollowerLastX[i] = -1;
        sRemoteFollowerLastY[i] = -1;
        sRemoteFollowerBobTicks[i] = 0;
    }
    sHasAutoSaveBaseline = false;
    sLastAutoSaveGroup = -1;
    sLastAutoSaveNum = -1;
    sWasInBattle = false;
    sPendingAutoSaveDelay = 0;
    sHasEnteredGameplay = false;
    sHasSentFullState = false;
    sLastFullStateSig = 0;
    sLastFullReportTick = 0;
}

bool MpOverworld_GetLocalCoords(int8_t *outGroup, int8_t *outNum, int16_t *outX, int16_t *outY)
{
    if (!gSaveBlock1Ptr)
        return false;

    if (outGroup) *outGroup = gSaveBlock1Ptr->location.mapGroup;
    if (outNum) *outNum = gSaveBlock1Ptr->location.mapNum;

    s16 destX = gSaveBlock1Ptr->pos.x;
    s16 destY = gSaveBlock1Ptr->pos.y;
    if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT &&
        gObjectEvents[gPlayerAvatar.objectEventId].active) {
        PlayerGetDestCoords(&destX, &destY);
        destX = (s16)(destX - MAP_OFFSET);
        destY = (s16)(destY - MAP_OFFSET);
    }
    if (outX) *outX = destX;
    if (outY) *outY = destY;
    return true;
}

void MpOverworld_ClearRemoteSprites(void)
{
    for (uint8_t slot = 0; slot < MP_MAX_PLAYERS; slot++) {
        DespawnRemoteSlot(slot);
    }
}

void MpOverworld_OnMapLoad(void)
{
    MpOverworld_ClearRemoteSprites();
    SyncRemoteSprites();
    sHasLastSentPos = false; /* force fresh broadcast after transition */

    if (gSaveBlock1Ptr && IsInFreeOverworld()) {
        int8_t mg = gSaveBlock1Ptr->location.mapGroup;
        int8_t mn = gSaveBlock1Ptr->location.mapNum;
        if (!(mg == MAP_GROUP(MAP_INSIDE_OF_TRUCK) && mn == MAP_NUM(MAP_INSIDE_OF_TRUCK))) {
            if (!sHasAutoSaveBaseline || sLastAutoSaveGroup != mg || sLastAutoSaveNum != mn) {
                sPendingAutoSaveDelay = 12;
            }
        }
    }
}

bool MpOverworld_ExecuteWarp(int8_t mapGroup, int8_t mapNum, int16_t x, int16_t y)
{
    sPendingWarpGroup = mapGroup;
    sPendingWarpNum = mapNum;
    sPendingWarpX = x;
    sPendingWarpY = y;
    sPendingWarpActive = true;

    if (!CanWarpSafelyNow())
        return false;

    sPendingWarpActive = false;
    MpOverworld_ClearRemoteSprites();
    SetWarpDestination(mapGroup, mapNum, WARP_ID_NONE, (s8)x, (s8)y);
    DoTeleportTileWarp();
    return true;
}

bool MpOverworld_CanJoinSyncedBattleNow(void)
{
    if (!IsInFreeOverworld())
        return false;
    if (gPaletteFade.active)
        return false;
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
        return false;
    return true;
}

void MpOverworld_ForceClearFieldForSyncedBattle(void)
{
    if (!IsInFreeOverworld() || gPaletteFade.active)
        return;
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled()) {
        ScriptContext_Stop();
        HideFieldMessageBox();
        UnlockPlayerFieldControls();
    }
}

void MpOverworld_RequestAutoSave(uint16_t delayFrames)
{
    uint16_t d = (delayFrames > 0) ? delayFrames : 1u;
    if (sPendingAutoSaveDelay == 0 || d < sPendingAutoSaveDelay)
        sPendingAutoSaveDelay = d;
}

const char *MpOverworld_GetPeerBusyReason(uint8_t slot)
{
    const MpPeerState *peer = MpNet_GetPeerState(slot);
    if (peer == NULL || !peer->connected || !peer->hasPosition)
        return "not_ready";
    if (!MpWorldSync_IsLocalTutorialComplete() || (peer->pos.inBattle & MP_OW_STATUS_IN_TUTORIAL) != 0)
        return "in_tutorial";
    if ((peer->pos.inBattle & MP_OW_STATUS_IN_BATTLE) != 0)
        return "in_battle";
    if (gSaveBlock1Ptr == NULL
     || peer->pos.mapGroup != gSaveBlock1Ptr->location.mapGroup
     || peer->pos.mapNum != gSaveBlock1Ptr->location.mapNum)
        return "different_map";
    if ((peer->pos.inBattle & MP_OW_STATUS_BUSY) != 0)
        return "busy";
    return NULL;
}

bool MpOverworld_IsPeerBusyForBattle(uint8_t slot)
{
    return MpOverworld_GetPeerBusyReason(slot) != NULL;
}

bool MpOverworld_SaveGameNow(void)
{
    if (!gSaveBlock1Ptr || !gSaveBlock2Ptr)
        return false;
    if (gMain.inBattle || gMain.callback2 != CB2_Overworld) {
        MpOverworld_RequestAutoSave(10);
        return false;
    }

    int8_t mapGroup = gSaveBlock1Ptr->location.mapGroup;
    int8_t mapNum = gSaveBlock1Ptr->location.mapNum;
    if (mapGroup == MAP_GROUP(MAP_INSIDE_OF_TRUCK) && mapNum == MAP_NUM(MAP_INSIDE_OF_TRUCK))
        return false;

    if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT &&
        gObjectEvents[gPlayerAvatar.objectEventId].active) {
        s16 destX = 0;
        s16 destY = 0;
        PlayerGetDestCoords(&destX, &destY);
        gSaveBlock1Ptr->pos.x = (s16)(destX - MAP_OFFSET);
        gSaveBlock1Ptr->pos.y = (s16)(destY - MAP_OFFSET);
    }

    /* Strip transient remote peer object events before writing SaveBlock1 to Flash. */
    MpOverworld_ClearRemoteSprites();

    u8 mode = gDifferentSaveFile ? SAVE_OVERWRITE_DIFFERENT_FILE : SAVE_NORMAL;
    u8 status = TrySavingData(mode);
    SyncRemoteSprites();

    if (status == SAVE_STATUS_OK) {
        gDifferentSaveFile = FALSE;
        sHasAutoSaveBaseline = true;
        sLastAutoSaveGroup = mapGroup;
        sLastAutoSaveNum = mapNum;
#ifdef __EMSCRIPTEN__
        js_mp_on_flash_saved();
#endif
        return true;
    }
    return false;
}

static void ApplyLobbyNameIfPresent(void)
{
    const MpPeerState *selfPeer = MpNet_GetPeerState(MpNet_GetLocalSlot());
    if (!selfPeer || selfPeer->name[0] == '\0' || !gSaveBlock2Ptr)
        return;

    u8 i = 0;
    for (i = 0; i < PLAYER_NAME_LENGTH && selfPeer->name[i] != '\0'; i++) {
        char ch = selfPeer->name[i];
        if (ch >= 'A' && ch <= 'Z')
            gSaveBlock2Ptr->playerName[i] = (u8)(CHAR_A + (ch - 'A'));
        else if (ch >= 'a' && ch <= 'z')
            gSaveBlock2Ptr->playerName[i] = (u8)(CHAR_a + (ch - 'a'));
        else if (ch >= '0' && ch <= '9')
            gSaveBlock2Ptr->playerName[i] = (u8)(CHAR_0 + (ch - '0'));
        else
            gSaveBlock2Ptr->playerName[i] = CHAR_SPACE;
    }
    gSaveBlock2Ptr->playerName[i] = EOS;
}

static void ResetGraphicsAndSaveBlocks(void)
{
    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    StopMapMusic();
    ResetTasks();
    ResetSpriteData();
    FreeAllSpritePalettes();
    ResetPaletteFade();
    FreeAllWindowBuffers();
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    gMain.inBattle = FALSE;
    gMain.callback1 = NULL;

    SetSaveBlocksPointers(GetSaveBlocksPointersBaseOffset());
    ResetMenuAndMonGlobals();
    Save_ResetSaveCounters();
}

static void BootstrapFreshGameToLittleroot(void)
{
    /* No Flash save yet: bootstrap directly into Littleroot Town past Birch/Truck/Clock intro. */
    Sav2_ClearSetDefault();
    NewGameInitData();
    NewGameBirchSpeech_SetDefaultPlayerName(0);
    ApplyLobbyNameIfPresent();

    enum Gender g = (gSaveBlock2Ptr->playerGender == FEMALE) ? FEMALE : MALE;
    SetLastHealLocationWarp((g == FEMALE) ? HEAL_LOCATION_LITTLEROOT_TOWN_MAYS_HOUSE_2F
                                          : HEAL_LOCATION_LITTLEROOT_TOWN_BRENDANS_HOUSE_2F);
    FlagSet(FLAG_SYS_CLOCK_SET);
    VarSet(VAR_LITTLEROOT_INTRO_STATE, 7);
    VarSet(VAR_LITTLEROOT_TOWN_STATE, 1);
    FlagSet(FLAG_HIDE_LITTLEROOT_TOWN_MOM_OUTSIDE);
    FlagSet((g == FEMALE) ? FLAG_HIDE_LITTLEROOT_TOWN_BRENDANS_HOUSE_MOM
                          : FLAG_HIDE_LITTLEROOT_TOWN_MAYS_HOUSE_MOM);
    FlagSet(FLAG_HIDE_LITTLEROOT_TOWN_BRENDANS_HOUSE_TRUCK);
    FlagSet(FLAG_HIDE_LITTLEROOT_TOWN_MAYS_HOUSE_TRUCK);
    FlagSet(FLAG_HIDE_LITTLEROOT_TOWN_PLAYERS_BEDROOM_MOM);

    SetWarpDestination(MAP_GROUP(MAP_LITTLEROOT_TOWN), MAP_NUM(MAP_LITTLEROOT_TOWN), WARP_ID_NONE,
                       (s8)((g == FEMALE) ? 14 : 5), 8);
    WarpIntoMap();
    ResetInitialPlayerAvatarState();
    PlayTimeCounter_Start();
    ScriptContext_Init();
    UnlockPlayerFieldControls();
    gFieldCallback = FieldCB_WarpExitFadeFromBlack;
    gFieldCallback2 = NULL;
    SetMainCallback2(CB2_LoadMap);

    sHasAutoSaveBaseline = false;
    sPendingAutoSaveDelay = 20;
}

void MpOverworld_ResumeOrBootstrapFromSave(void)
{
    MpOverworld_ClearRemoteSprites();
    sPendingWarpActive = false;
    sPendingAutoSaveDelay = 0;
    sHasLastSentPos = false;

#ifdef WASM_PLATFORM
    WasmFlash_LoadFromStorage();
#endif

    ResetGraphicsAndSaveBlocks();
    LoadGameSave(SAVE_NORMAL);

    if (gSaveFileStatus == SAVE_STATUS_OK) {
        EnsureValidLastHealLocation();
        SetPokemonCryStereo(gSaveBlock2Ptr->optionsSound);
        sHasAutoSaveBaseline = true;
        sLastAutoSaveGroup = gSaveBlock1Ptr->location.mapGroup;
        sLastAutoSaveNum = gSaveBlock1Ptr->location.mapNum;
        SetMainCallback2(CB2_ContinueSavedGame);
        return;
    }

    BootstrapFreshGameToLittleroot();
}

void MpOverworld_ResetToNewGame(bool skipIntro)
{
    MpOverworld_ClearRemoteSprites();
    sPendingWarpActive = false;
    sPendingAutoSaveDelay = 0;
    sHasLastSentPos = false;
    memset(&sLastSentPos, 0, sizeof(sLastSentPos));
    sHasAutoSaveBaseline = false;
    sLastAutoSaveGroup = -1;
    sLastAutoSaveNum = -1;
    sWasInBattle = false;
    sHasEnteredGameplay = false;
    sHasSentFullState = false;
    sLastFullStateSig = 0;
    sLastFullReportTick = 0;

    MpBattle2v2_Init();
    MpNet_SetLocalInBattle(false);
    MpWorldSync_Init();
    MpCutscene_Init();
    MpSoulLink_ResetRun();

#ifdef WASM_PLATFORM
    WasmFlash_ClearStorage();
#endif

    ResetGraphicsAndSaveBlocks();
    LoadGameSave(SAVE_NORMAL);
    gSaveFileStatus = SAVE_STATUS_EMPTY;
    gDifferentSaveFile = TRUE;

    if (skipIntro) {
        BootstrapFreshGameToLittleroot();
        sHasAutoSaveBaseline = true;
        sLastAutoSaveGroup = MAP_GROUP(MAP_LITTLEROOT_TOWN);
        sLastAutoSaveNum = MAP_NUM(MAP_LITTLEROOT_TOWN);
        sPendingAutoSaveDelay = 0;
        sHasEnteredGameplay = true;
        return;
    }

    Sav2_ClearSetDefault();
    NewGameInitData();
    NewGameBirchSpeech_SetDefaultPlayerName(0);
    ApplyLobbyNameIfPresent();
    ScriptContext_Init();
    UnlockPlayerFieldControls();
    gFieldCallback = NULL;
    gFieldCallback2 = NULL;
    SetMainCallback2(CB2_InitTitleScreen);
}

static void ConvertGbaStringToAscii(const u8 *src, char *dst, size_t maxLen)
{
    size_t out = 0;
    if (dst == NULL || maxLen == 0)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; i < 64 && (out + 1) < maxLen; i++) {
        u8 c = src[i];
        char ch = '\0';

        if (c == EOS)
            break;
        if (c == EXT_CTRL_CODE_BEGIN || c == PLACEHOLDER_BEGIN)
            break;

        if (c == CHAR_SPACE || c == CHAR_NBSP || c == CHAR_SPACER || c == CHAR_NEWLINE)
            ch = ' ';
        else if (c == CHAR_ZWS)
            continue;
        else if (c >= CHAR_0 && c <= CHAR_9)
            ch = (char)('0' + (c - CHAR_0));
        else if (c >= CHAR_A && c <= CHAR_Z)
            ch = (char)('A' + (c - CHAR_A));
        else if (c >= CHAR_a && c <= CHAR_z)
            ch = (char)('a' + (c - CHAR_a));
        else if (c >= CHAR_A_GRAVE && c <= CHAR_A_CIRCUMFLEX)
            ch = 'A';
        else if (c == CHAR_C_CEDILLA)
            ch = 'C';
        else if (c >= CHAR_E_GRAVE && c <= CHAR_E_DIAERESIS)
            ch = 'E';
        else if ((c >= CHAR_I_GRAVE && c <= CHAR_I_DIAERESIS) || c == CHAR_I_ACUTE)
            ch = 'I';
        else if (c >= CHAR_O_GRAVE && c <= CHAR_O_CIRCUMFLEX)
            ch = 'O';
        else if (c >= CHAR_U_GRAVE && c <= CHAR_U_CIRCUMFLEX)
            ch = 'U';
        else if (c == CHAR_N_TILDE)
            ch = 'N';
        else if (c >= CHAR_a_GRAVE && c <= CHAR_a_CIRCUMFLEX)
            ch = 'a';
        else if (c == CHAR_c_CEDILLA)
            ch = 'c';
        else if (c >= CHAR_e_GRAVE && c <= CHAR_e_DIAERESIS)
            ch = 'e';
        else if ((c >= CHAR_i_GRAVE && c <= CHAR_i_DIAERESIS) || c == CHAR_i_ACUTE)
            ch = 'i';
        else if (c >= CHAR_o_GRAVE && c <= CHAR_o_CIRCUMFLEX)
            ch = 'o';
        else if (c >= CHAR_u_GRAVE && c <= CHAR_u_CIRCUMFLEX)
            ch = 'u';
        else if (c == CHAR_n_TILDE)
            ch = 'n';
        else {
            switch (c) {
            case CHAR_AMPERSAND:     ch = '&'; break;
            case CHAR_PLUS:          ch = '+'; break;
            case CHAR_EQUALS:        ch = '='; break;
            case CHAR_PERCENT:       ch = '%'; break;
            case CHAR_LEFT_PAREN:    ch = '('; break;
            case CHAR_RIGHT_PAREN:   ch = ')'; break;
            case CHAR_LESS_THAN:     ch = '<'; break;
            case CHAR_GREATER_THAN:  ch = '>'; break;
            case CHAR_EXCL_MARK:     ch = '!'; break;
            case CHAR_QUESTION_MARK: ch = '?'; break;
            case CHAR_PERIOD:        ch = '.'; break;
            case CHAR_HYPHEN:        ch = '-'; break;
            case CHAR_COMMA:         ch = ','; break;
            case CHAR_SLASH:         ch = '/'; break;
            case CHAR_COLON:         ch = ':'; break;
            case CHAR_SGL_QUOTE_LEFT:
            case CHAR_SGL_QUOTE_RIGHT:
            case CHAR_DBL_QUOTE_LEFT:
            case CHAR_DBL_QUOTE_RIGHT:
                ch = '\'';
                break;
            case CHAR_MALE:          ch = 'M'; break;
            case CHAR_FEMALE:        ch = 'F'; break;
            default:                 ch = ' '; break;
            }
        }
        dst[out++] = ch;
    }

    while (out > 0 && dst[out - 1] == ' ')
        out--;
    dst[out] = '\0';
}

static size_t Base64EncodeBytes(const uint8_t *src, size_t len, char *out, size_t outCap)
{
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t pos = 0;
    if (out == NULL || outCap == 0)
        return 0;

    while (i < len && (pos + 4) < outCap) {
        uint32_t octetA = src[i++];
        uint32_t octetB = (i < len) ? src[i++] : 0u;
        uint32_t octetC = (i < len) ? src[i++] : 0u;

        uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;
        out[pos++] = kB64[(triple >> 18) & 0x3Fu];
        out[pos++] = kB64[(triple >> 12) & 0x3Fu];
        out[pos++] = kB64[(triple >> 6) & 0x3Fu];
        out[pos++] = kB64[triple & 0x3Fu];
    }

    /* Fix padding accurately for final partial block if any. */
    if (len > 0 && pos >= 4 && (len % 3u) != 0u) {
        if ((len % 3u) == 1u) {
            out[pos - 2] = '=';
            out[pos - 1] = '=';
        } else if ((len % 3u) == 2u) {
            out[pos - 1] = '=';
        }
    }
    out[pos] = '\0';
    return pos;
}

static const char *BallIdToHudTag(uint8_t ballId)
{
    switch (ballId) {
    case BALL_MASTER: return "masterball";
    case BALL_ULTRA:  return "ultraball";
    case BALL_GREAT:  return "greatball";
    default:          return "pokeball";
    }
}

static const char *StatusToHudTag(uint32_t status, uint16_t hp)
{
    if (hp == 0)
        return "FNT";
    if (status & STATUS1_PSN_ANY)
        return "PSN";
    if (status & STATUS1_SLEEP)
        return "SLP";
    if (status & STATUS1_BURN)
        return "BRN";
    if (status & STATUS1_ICY_ANY)
        return "FRZ";
    if (status & STATUS1_PARALYSIS)
        return "PAR";
    return "";
}

static void JsonAppendEscapedStr(char *buf, size_t cap, size_t *pos, const char *s)
{
    if (*pos < cap)
        buf[(*pos)++] = '"';
    if (s != NULL) {
        for (size_t i = 0; s[i] != '\0' && (*pos + 2) < cap; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '"' || ch == '\\') {
                buf[(*pos)++] = '\\';
                buf[(*pos)++] = (char)ch;
            } else if (ch >= 0x20u && ch < 0x7Fu) {
                buf[(*pos)++] = (char)ch;
            }
        }
    }
    if (*pos < cap)
        buf[(*pos)++] = '"';
    if (*pos < cap)
        buf[*pos] = '\0';
    else if (cap > 0)
        buf[cap - 1] = '\0';
}

static uint32_t Fnv1aMix32(uint32_t hash, uint32_t val)
{
    hash ^= (val & 0xFFu);        hash *= 16777619u;
    hash ^= ((val >> 8) & 0xFFu); hash *= 16777619u;
    hash ^= ((val >> 16) & 0xFFu); hash *= 16777619u;
    hash ^= ((val >> 24) & 0xFFu); hash *= 16777619u;
    return hash;
}

static uint32_t ComputeLocalStateSignature(void)
{
    static const enum Pocket kTradePockets[] = {
        POCKET_ITEMS, POCKET_POKE_BALLS, POCKET_BERRIES, POCKET_TM_HM
    };
    uint32_t h = 2166136261u;
    if (!gSaveBlock1Ptr)
        return h;

    h = Fnv1aMix32(h, GetMoney(&gSaveBlock1Ptr->money));
    h = Fnv1aMix32(h, MpWorldSync_IsLocalTutorialComplete() ? 1u : 0u);
    u8 count = gPartiesCount[B_TRAINER_PLAYER];
    if (count > PARTY_SIZE)
        count = PARTY_SIZE;
    h = Fnv1aMix32(h, count);

    for (u8 i = 0; i < count; i++) {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        u32 species = GetMonData(mon, MON_DATA_SPECIES);
        const MpSoulLinkEntry *entry = MpSoulLink_LookupMon(mon);
        h = Fnv1aMix32(h, species);
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_IS_EGG));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_LEVEL));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_HP));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_MAX_HP));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_STATUS));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_HELD_ITEM));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_POKEBALL));
        h = Fnv1aMix32(h, GetMonData(mon, MON_DATA_PERSONALITY));
        h = Fnv1aMix32(h, entry ? entry->linkId : 0u);
        h = Fnv1aMix32(h, MpSoulLink_IsMonDead(mon) ? 1u : 0u);
    }

    uint32_t exported = 0;
    for (size_t p = 0; p < ARRAY_COUNT(kTradePockets) && exported < 24u; p++) {
        enum Pocket pocket = kTradePockets[p];
        u16 cap = gBagPockets[pocket].capacity;
        for (u16 s = 0; s < cap && exported < 24u; s++) {
            struct ItemSlot slot = GetBagItemIdAndQuantity(pocket, s);
            if (slot.itemId == ITEM_NONE || slot.quantity == 0)
                continue;
            if (!MpRandomizer_IsItemRandomizable((uint16_t)slot.itemId))
                continue;
            h = Fnv1aMix32(h, (uint32_t)slot.itemId);
            h = Fnv1aMix32(h, (uint32_t)slot.quantity);
            exported++;
        }
    }
    return h;
}

static void ReportStateToBridge(const MpOverworldPosPayload *cur, bool inBattle, bool busy, uint32_t stateSig, bool includeFull)
{
    static const enum Pocket kTradePockets[] = {
        POCKET_ITEMS, POCKET_POKE_BALLS, POCKET_BERRIES, POCKET_TM_HM
    };
    u8 gbaMapBuf[36];
    char mapNameAscii[40];
    mapsec_u16_t curMapsec = GetCurrentRegionMapSectionId();

    if (curMapsec < MAPSEC_NONE) {
        GetMapNameGeneric(gbaMapBuf, curMapsec);
        ConvertGbaStringToAscii(gbaMapBuf, mapNameAscii, sizeof(mapNameAscii));
    } else {
        strcpy(mapNameAscii, "Hoenn");
    }
    if (mapNameAscii[0] == '\0')
        strcpy(mapNameAscii, "Hoenn");

    uint32_t money = gSaveBlock1Ptr ? GetMoney(&gSaveBlock1Ptr->money) : 0u;
    size_t pos = 0;
    size_t cap = sizeof(sStateJsonBuf);

    int n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0,
                     "{\"mapGroup\":%d,\"mapNum\":%d,\"x\":%d,\"y\":%d,\"mapName\":",
                     (int)cur->mapGroup, (int)cur->mapNum, (int)cur->x, (int)cur->y);
    if (n > 0) pos += (size_t)n;
    JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, mapNameAscii);

    bool tutorialComplete = MpWorldSync_IsLocalTutorialComplete();
    n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0,
                 ",\"inBattle\":%s,\"busy\":%s,\"tutorialComplete\":%s,\"money\":%u",
                 inBattle ? "true" : "false", busy ? "true" : "false",
                 tutorialComplete ? "true" : "false", (unsigned)money);
    if (n > 0) pos += (size_t)n;

    if (includeFull && gSaveBlock1Ptr) {
        u8 partyCount = gPartiesCount[B_TRAINER_PLAYER];
        if (partyCount > PARTY_SIZE)
            partyCount = PARTY_SIZE;

        n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"party\":[");
        if (n > 0) pos += (size_t)n;

        bool firstMon = true;
        for (u8 i = 0; i < partyCount; i++) {
            struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
            u32 species = GetMonData(mon, MON_DATA_SPECIES);
            if (species == SPECIES_NONE || species >= NUM_SPECIES)
                continue;

            bool isEgg = (GetMonData(mon, MON_DATA_IS_EGG) != 0);
            u32 personality = GetMonData(mon, MON_DATA_PERSONALITY);
            u32 otId = GetMonData(mon, MON_DATA_OT_ID);
            u8 level = isEgg ? 1u : (u8)GetMonData(mon, MON_DATA_LEVEL);
            u16 hp = isEgg ? 1u : (u16)GetMonData(mon, MON_DATA_HP);
            u16 maxHp = isEgg ? 1u : (u16)GetMonData(mon, MON_DATA_MAX_HP);
            if (maxHp == 0)
                maxHp = 1;
            u32 status1 = isEgg ? 0u : GetMonData(mon, MON_DATA_STATUS);
            u8 ballId = (u8)GetMonData(mon, MON_DATA_POKEBALL);
            u16 heldItemId = isEgg ? ITEM_NONE : (u16)GetMonData(mon, MON_DATA_HELD_ITEM);
            u16 metLoc = (u16)GetMonData(mon, MON_DATA_MET_LOCATION);
            bool isDead = !isEgg && (MpSoulLink_IsMonDead(mon) || hp == 0);
            const MpSoulLinkEntry *entry = isEgg ? NULL : MpSoulLink_LookupMon(mon);
            uint16_t linkNum = entry ? entry->linkId : 0u;
            uint16_t originSec = entry ? entry->originMapsec : metLoc;

            char uidBuf[32];
            char speciesAscii[32];
            char nickAscii[32];
            char heldAscii[36];
            char originAscii[36];
            char linkBuf[20];
            char monB64[160];
            u8 gbaNick[24];
            u8 gbaOrigin[36];
            uint8_t rawMon[MP_SERIALIZED_MON_BYTES];

            snprintf(uidBuf, sizeof(uidBuf), "mon-%08x-%08x", (unsigned)personality, (unsigned)otId);

            if (isEgg) {
                strcpy(speciesAscii, "Egg");
                strcpy(nickAscii, "Egg");
                heldAscii[0] = '\0';
            } else {
                ConvertGbaStringToAscii(GetSpeciesName((enum Species)species), speciesAscii, sizeof(speciesAscii));
                GetMonData(mon, MON_DATA_NICKNAME, gbaNick);
                ConvertGbaStringToAscii(gbaNick, nickAscii, sizeof(nickAscii));
                if (nickAscii[0] == '\0')
                    strcpy(nickAscii, speciesAscii);
                if (heldItemId != ITEM_NONE && heldItemId < ITEMS_COUNT)
                    ConvertGbaStringToAscii(GetItemName((enum Item)heldItemId), heldAscii, sizeof(heldAscii));
                else
                    heldAscii[0] = '\0';
            }

            if (originSec == MP_STARTER_MAPSEC) {
                strcpy(originAscii, "Starter Bag");
            } else if (originSec < MAPSEC_NONE) {
                GetMapNameGeneric(gbaOrigin, originSec);
                ConvertGbaStringToAscii(gbaOrigin, originAscii, sizeof(originAscii));
            } else {
                strcpy(originAscii, "Hoenn");
            }
            if (originAscii[0] == '\0')
                strcpy(originAscii, "Hoenn");

            if (linkNum != 0)
                snprintf(linkBuf, sizeof(linkBuf), "LINK-%02u", (unsigned)linkNum);
            else
                linkBuf[0] = '\0';

            monB64[0] = '\0';
            if (MpTrade_SerializePokemon(mon, rawMon))
                Base64EncodeBytes(rawMon, MP_SERIALIZED_MON_BYTES, monB64, sizeof(monB64));

            if (!firstMon) {
                n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",");
                if (n > 0) pos += (size_t)n;
            }
            firstMon = false;

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, "{\"uid\":");
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, uidBuf);

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"species\":");
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, speciesAscii);

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"nickname\":");
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, nickAscii);

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0,
                         ",\"speciesId\":%u,\"level\":%u,\"hp\":%u,\"maxHp\":%u,\"ball\":",
                         (unsigned)(isEgg ? 0u : species), (unsigned)level, (unsigned)hp, (unsigned)maxHp);
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, BallIdToHudTag(ballId));

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"heldItem\":");
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, heldAscii);

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"status\":");
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, StatusToHudTag(status1, hp));

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"isDead\":%s,\"linkId\":",
                         isDead ? "true" : "false");
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, linkBuf);

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"mapsec\":%u,\"originMap\":",
                         (unsigned)originSec);
            if (n > 0) pos += (size_t)n;
            JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, originAscii);

            if (monB64[0] != '\0') {
                n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"monBytesB64\":");
                if (n > 0) pos += (size_t)n;
                JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, monB64);
            }

            n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, "}");
            if (n > 0) pos += (size_t)n;
        }

        n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, "],\"bagItems\":[");
        if (n > 0) pos += (size_t)n;

        uint32_t exportedItems = 0;
        for (size_t p = 0; p < ARRAY_COUNT(kTradePockets) && exportedItems < 24u; p++) {
            enum Pocket pocket = kTradePockets[p];
            u16 pCap = gBagPockets[pocket].capacity;
            for (u16 s = 0; s < pCap && exportedItems < 24u; s++) {
                struct ItemSlot slot = GetBagItemIdAndQuantity(pocket, s);
                char itemNameAscii[40];
                if (slot.itemId == ITEM_NONE || slot.quantity == 0)
                    continue;
                if (!MpRandomizer_IsItemRandomizable((uint16_t)slot.itemId))
                    continue;

                ConvertGbaStringToAscii(GetItemName(slot.itemId), itemNameAscii, sizeof(itemNameAscii));
                if (itemNameAscii[0] == '\0')
                    snprintf(itemNameAscii, sizeof(itemNameAscii), "Item %u", (unsigned)slot.itemId);

                if (exportedItems > 0) {
                    n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",");
                    if (n > 0) pos += (size_t)n;
                }

                n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0,
                             "{\"id\":\"ITEM_%u\",\"itemId\":%u,\"name\":",
                             (unsigned)slot.itemId, (unsigned)slot.itemId);
                if (n > 0) pos += (size_t)n;
                JsonAppendEscapedStr(sStateJsonBuf, cap, &pos, itemNameAscii);

                n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, ",\"qty\":%u}",
                             (unsigned)slot.quantity);
                if (n > 0) pos += (size_t)n;
                exportedItems++;
            }
        }

        n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, "]}");
        if (n > 0) pos += (size_t)n;

        sLastFullStateSig = stateSig;
        sHasSentFullState = true;
        sLastFullReportTick = sOwTick;
    } else {
        n = snprintf(sStateJsonBuf + pos, (pos < cap) ? (cap - pos) : 0, "}");
        if (n > 0) pos += (size_t)n;
    }

    if (pos >= cap)
        sStateJsonBuf[cap - 1] = '\0';
    WasmMp_ReportLocalState(sStateJsonBuf);
}

void MpOverworld_OnFrame(void)
{
    sOwTick++;

    bool inBattle = (gMain.inBattle != 0);
    bool busy = !inBattle && !MpOverworld_CanJoinSyncedBattleNow();
    MpNet_SetLocalInBattle(inBattle);

    if (sPendingWarpActive && CanWarpSafelyNow()) {
        MpOverworld_ExecuteWarp(sPendingWarpGroup, sPendingWarpNum, sPendingWarpX, sPendingWarpY);
    }

    if (!gSaveBlock1Ptr || !gSaveBlock2Ptr)
        return;

    if (!sHasEnteredGameplay) {
        if (gMain.callback2 == CB2_Overworld || gMain.inBattle)
            sHasEnteredGameplay = true;
        else
            return;
    }

    if ((sOwTick % 30u) == 0u && IsInFreeOverworld())
        MpWorldSync_CheckFlushDeferred();

    int8_t mapGroup = 0;
    int8_t mapNum = 0;
    int16_t mapX = 0;
    int16_t mapY = 0;
    MpOverworld_GetLocalCoords(&mapGroup, &mapNum, &mapX, &mapY);

    /* Schedule Flash auto-save after battles or map transitions outside the intro truck. */
    if (sWasInBattle && !inBattle && IsInFreeOverworld()) {
        sPendingAutoSaveDelay = 15;
    } else if (IsInFreeOverworld() &&
               !(mapGroup == MAP_GROUP(MAP_INSIDE_OF_TRUCK) && mapNum == MAP_NUM(MAP_INSIDE_OF_TRUCK)) &&
               (!sHasAutoSaveBaseline || sLastAutoSaveGroup != mapGroup || sLastAutoSaveNum != mapNum)) {
        if (sPendingAutoSaveDelay == 0)
            sPendingAutoSaveDelay = 15;
    }
    sWasInBattle = inBattle;

    if (sPendingAutoSaveDelay > 0 && IsInFreeOverworld()) {
        sPendingAutoSaveDelay--;
        if (sPendingAutoSaveDelay == 0) {
            if (MpOverworld_CanJoinSyncedBattleNow()) {
                MpOverworld_SaveGameNow();
            } else {
                sPendingAutoSaveDelay = 10;
            }
        }
    }

    uint8_t moveState = gPlayerAvatar.flags;
    if (gPlayerAvatar.runningState == MOVING && (moveState & PLAYER_AVATAR_FLAG_ON_FOOT)) {
        if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT &&
            gObjectEvents[gPlayerAvatar.objectEventId].active) {
            u8 act = gObjectEvents[gPlayerAvatar.objectEventId].movementActionId;
            if (act >= MOVEMENT_ACTION_PLAYER_RUN_DOWN && act <= MOVEMENT_ACTION_PLAYER_RUN_RIGHT)
                moveState |= PLAYER_AVATAR_FLAG_DASH;
        }
    }

    MpOverworldPosPayload cur;
    memset(&cur, 0, sizeof(cur));
    cur.slotIndex = MpNet_GetLocalSlot();
    cur.mapGroup = mapGroup;
    cur.mapNum = mapNum;
    cur.elevation = PlayerGetElevation();
    cur.x = mapX;
    cur.y = mapY;
    cur.facing = (uint8_t)GetPlayerFacingDirection();
    cur.movementState = moveState;
    cur.graphicsId = GetPlayerAvatarGraphicsIdByCurrentState();
    cur.gender = gSaveBlock2Ptr->playerGender;
    bool tutorialComplete = MpWorldSync_IsLocalTutorialComplete();
    cur.inBattle = (uint8_t)((inBattle ? MP_OW_STATUS_IN_BATTLE : 0u)
                           | (busy ? MP_OW_STATUS_BUSY : 0u)
                           | (!tutorialComplete ? MP_OW_STATUS_IN_TUTORIAL : 0u));
    const MpPeerState *selfPeer = MpNet_GetPeerState(cur.slotIndex);
    cur.partyTag = selfPeer ? selfPeer->partyPairTag : 0u;

    if (IsInFreeOverworld()) {
        struct ObjectEvent *localFollower = GetFollowerObject();
        if (localFollower != NULL && localFollower->active && !localFollower->invisible &&
            (localFollower->graphicsId & OBJ_EVENT_MON)) {
            cur.followerGfxId = localFollower->graphicsId;
            cur.followerX = (int16_t)(localFollower->currentCoords.x - MAP_OFFSET);
            cur.followerY = (int16_t)(localFollower->currentCoords.y - MAP_OFFSET);
            cur.followerElevation = localFollower->currentElevation;
            cur.followerFacing = localFollower->facingDirection;
            uint8_t fFlags = 0u;
            if (localFollower->singleMovementActive) {
                u8 fAct = localFollower->movementActionId;
                if ((fAct >= MOVEMENT_ACTION_JUMP_2_DOWN && fAct <= MOVEMENT_ACTION_JUMP_2_RIGHT) ||
                    (fAct >= MOVEMENT_ACTION_JUMP_SPECIAL_DOWN && fAct <= MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT) ||
                    (fAct >= MOVEMENT_ACTION_JUMP_DOWN && fAct <= MOVEMENT_ACTION_JUMP_RIGHT)) {
                    fFlags |= MP_FOLLOWER_F_MOVING | MP_FOLLOWER_F_JUMPING;
                } else if ((fAct >= MOVEMENT_ACTION_WALK_FAST_DOWN && fAct <= MOVEMENT_ACTION_WALK_FAST_RIGHT) ||
                           (fAct >= MOVEMENT_ACTION_WALK_FASTER_DOWN && fAct <= MOVEMENT_ACTION_WALK_FASTER_RIGHT) ||
                           (fAct >= MOVEMENT_ACTION_PLAYER_RUN_DOWN && fAct <= MOVEMENT_ACTION_PLAYER_RUN_RIGHT)) {
                    fFlags |= MP_FOLLOWER_F_MOVING | MP_FOLLOWER_F_FAST;
                } else if ((fAct >= MOVEMENT_ACTION_WALK_SLOW_DOWN && fAct <= MOVEMENT_ACTION_WALK_NORMAL_RIGHT) ||
                           (fAct >= MOVEMENT_ACTION_SLIDE_DOWN && fAct <= MOVEMENT_ACTION_SLIDE_RIGHT)) {
                    fFlags |= MP_FOLLOWER_F_MOVING;
                }
            }
            if (moveState & PLAYER_AVATAR_FLAG_DASH)
                fFlags |= MP_FOLLOWER_F_FAST;
            cur.followerFlags = fFlags;
        }
    } else if (sHasLastSentPos && cur.mapGroup == sLastSentPos.mapGroup && cur.mapNum == sLastSentPos.mapNum) {
        cur.followerGfxId = sLastSentPos.followerGfxId;
        cur.followerX = sLastSentPos.followerX;
        cur.followerY = sLastSentPos.followerY;
        cur.followerElevation = sLastSentPos.followerElevation;
        cur.followerFacing = sLastSentPos.followerFacing;
        cur.followerFlags = (uint8_t)(sLastSentPos.followerFlags & (uint8_t)~MP_FOLLOWER_F_MOVING);
    }

    bool movedOrChanged = !sHasLastSentPos || (memcmp(&cur, &sLastSentPos, sizeof(cur)) != 0);
    bool urgentChange = !sHasLastSentPos ||
                        cur.inBattle != sLastSentPos.inBattle ||
                        cur.movementState != sLastSentPos.movementState ||
                        cur.followerGfxId != sLastSentPos.followerGfxId ||
                        cur.mapGroup != sLastSentPos.mapGroup ||
                        cur.mapNum != sLastSentPos.mapNum;
    bool dueForPosSync = movedOrChanged && ((sOwTick - sLastBroadcastTick) >= 2u);
    bool heartbeatDue = (sOwTick - sLastBroadcastTick) >= 30u;

    if (urgentChange || dueForPosSync || heartbeatDue) {
        MpNet_SendBinary(MP_CH_OVERWORLD, MP_PEER_BROADCAST, MP_PKT_OW_PLAYER_POS, &cur, sizeof(cur));
        sLastSentPos = cur;
        sHasLastSentPos = true;
        sLastBroadcastTick = sOwTick;
    }

    uint32_t stateSig = ComputeLocalStateSignature();
    bool fullChanged = !sHasSentFullState || (stateSig != sLastFullStateSig);
    bool fullPeriodic = (sOwTick - sLastFullReportTick) >= 180u;
    if (urgentChange || fullChanged || fullPeriodic ||
        (movedOrChanged && (sOwTick - sLastReportTick) >= 15u) ||
        (sOwTick - sLastReportTick) >= 60u) {
        ReportStateToBridge(&cur, inBattle, busy, stateSig, fullChanged || fullPeriodic);
        sLastReportTick = sOwTick;
    }

    SyncRemoteSprites();
}

void MpOverworld_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    if (opcode == MP_PKT_OW_PLAYER_POS && len >= sizeof(MpOverworldPosPayload)) {
        MpOverworldPosPayload pkt;
        memcpy(&pkt, payload, sizeof(pkt));
        if (pkt.slotIndex < MP_MAX_PLAYERS && pkt.slotIndex != MpNet_GetLocalSlot()) {
            MpPeerState *peer = MpNet_GetPeerStateMutable(pkt.slotIndex);
            if (peer) {
                peer->connected = true;
                if (fromPeerId != 0 && (fromPeerId != 1 || pkt.slotIndex == 0))
                    peer->peerId = fromPeerId;
                peer->hasPosition = true;
                peer->pos = pkt;
                if (!MpNet_HasAuthoritativePairTag(pkt.slotIndex))
                    peer->partyPairTag = pkt.partyTag;
            }
        }
    } else if (opcode == MP_PKT_OW_PLAYER_LEAVE && len >= 1) {
        uint8_t slot = ((const uint8_t *)payload)[0];
        if (slot < MP_MAX_PLAYERS && slot != MpNet_GetLocalSlot()) {
            MpPeerState *peer = MpNet_GetPeerStateMutable(slot);
            if (peer) {
                peer->connected = false;
                peer->hasPosition = false;
            }
            DespawnRemoteSlot(slot);
        }
    }
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_mp_interact_remote_player, (int peerSlot), {
    if (typeof window === "undefined") return;
    const now = Date.now();
    if (window._lastPvpInteractAt && (now - window._lastPvpInteractAt) < 1500) return;
    window._lastPvpInteractAt = now;
    if (window.PokemonNet && typeof window.PokemonNet.sendBattleInvite === "function") {
        window.PokemonNet.sendBattleInvite(peerSlot, "pvp");
    }
});
#endif

bool MpOverworld_IsRemotePlayerObject(uint8_t objectEventId, uint8_t *outPeerSlot)
{
    uint8_t localId;

    if (objectEventId >= OBJECT_EVENTS_COUNT || !gObjectEvents[objectEventId].active)
        return false;

    localId = gObjectEvents[objectEventId].localId;
    if (localId >= MP_REMOTE_LOCALID_BASE && localId < MP_REMOTE_LOCALID_BASE + MP_MAX_PLAYERS)
    {
        if (outPeerSlot != NULL)
            *outPeerSlot = (uint8_t)(localId - MP_REMOTE_LOCALID_BASE);
        return true;
    }
    return false;
}

void MpOverworld_InteractWithRemotePlayer(uint8_t peerSlot)
{
    const MpPeerState *peer;

    if (peerSlot >= MP_MAX_PLAYERS)
        return;
    peer = MpNet_GetPeerState(peerSlot);
    if (peer == NULL || !peer->connected)
        return;

    PlaySE(SE_SELECT);
#ifdef __EMSCRIPTEN__
    js_mp_interact_remote_player((int)peerSlot);
#endif
}

static void ForceReportStateNow(void)
{
    sHasSentFullState = false;
    sHasLastSentPos = false;
    sLastReportTick = 0;
    sLastBroadcastTick = 0;
    MpOverworld_OnFrame();
}

#ifndef WASM_KEEPALIVE
#ifdef __EMSCRIPTEN__
#define WASM_KEEPALIVE __attribute__((used))
#else
#define WASM_KEEPALIVE
#endif
#endif

WASM_KEEPALIVE int mp_test_query(int kind, int arg0, int arg1)
{
    switch (kind) {
    case 0:
        if (gMain.inBattle)
            return 2;
        if (gMain.callback2 == CB2_Overworld)
            return 1;
        return 0;
    case 1:
        switch (arg0) {
        case 0: return gSaveBlock1Ptr ? (int)gSaveBlock1Ptr->location.mapGroup : -1;
        case 1: return gSaveBlock1Ptr ? (int)gSaveBlock1Ptr->location.mapNum : -1;
        case 2: return gSaveBlock1Ptr ? (int)gSaveBlock1Ptr->pos.x : 0;
        case 3: return gSaveBlock1Ptr ? (int)gSaveBlock1Ptr->pos.y : 0;
        case 4: return (int)GetCurrentRegionMapSectionId();
        case 5: return gSaveBlock2Ptr ? (int)gSaveBlock2Ptr->playerGender : 0;
        case 6:
            if (!gSaveBlock2Ptr) return 0;
            return MpSoulLink_IsSetModeEnforced() ? OPTIONS_BATTLE_STYLE_SET : (int)gSaveBlock2Ptr->optionsBattleStyle;
        default: return 0;
        }
    case 2:
        return FlagGet((u16)arg0) ? 1 : 0;
    case 3:
        return (int)VarGet((u16)arg0);
    case 4:
        return (int)CalculatePlayerPartyCount();
    case 5: {
        if (arg0 < 0 || arg0 >= PARTY_SIZE)
            return 0;
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][arg0];
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE && arg1 != 8)
            return 0;
        const MpSoulLinkEntry *entry = MpSoulLink_LookupMon(mon);
        switch (arg1) {
        case 0:  return (int)GetMonData(mon, MON_DATA_SPECIES);
        case 1:  return (int)GetMonData(mon, MON_DATA_LEVEL);
        case 2:  return (int)GetMonData(mon, MON_DATA_HP);
        case 3:  return (int)GetMonData(mon, MON_DATA_MAX_HP);
        case 4:  return (int)GetMonData(mon, MON_DATA_EXP);
        case 5:  return (int)GetMonData(mon, MON_DATA_HELD_ITEM);
        case 6:  return entry ? (int)entry->linkId : 0;
        case 7:  return entry ? (int)entry->originMapsec : (int)GetMonData(mon, MON_DATA_MET_LOCATION);
        case 8:  return MpSoulLink_IsMonDead(mon) ? 1 : 0;
        case 9:  return (int)GetMonData(mon, MON_DATA_PERSONALITY);
        case 10: return (int)GetMonData(mon, MON_DATA_OT_ID);
        default: return 0;
        }
    }
    case 6:
        return (int)CountTotalItemQuantityInBag((enum Item)arg0);
    case 7:
        return gSaveBlock1Ptr ? (int)GetMoney(&gSaveBlock1Ptr->money) : 0;
    case 8:
        switch (arg0) {
        case 0: return (int)MpSoulLink_GetCurrentLevelCap();
        case 1: return MpSoulLink_IsRouteConsumed((uint16_t)arg1) ? 1 : 0;
        case 2: {
            uint8_t battler = (uint8_t)(arg1 & 0xFF);
            uint16_t sec = (uint16_t)((arg1 >> 8) & 0xFFFF);
            if (sec == 0xFFFFu)
                sec = (uint16_t)GetCurrentRegionMapSectionId();
            return MpSoulLink_CanThrowBallAtBattler(battler, sec) ? 1 : 0;
        }
        case 3: return MpSoulLink_IsBattleItemBlocked((uint16_t)arg1) ? 0 : 1;
        case 4: {
            if (arg1 < 0 || arg1 >= PARTY_SIZE)
                return 0;
            struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][arg1];
            u32 curExp = GetMonData(mon, MON_DATA_EXP);
            u32 clamped = MpSoulLink_ClampExpToCap(mon, curExp + 100u);
            return (clamped <= curExp) ? 1 : 0;
        }
        case 5: return MpSoulLink_IsEnabled() ? 1 : 0;
        case 6: {
            int count = 0;
            u8 graveBox = (u8)(TOTAL_BOXES_COUNT - 1);
            for (u8 pos = 0; pos < IN_BOX_COUNT; pos++) {
                struct BoxPokemon *bm = GetBoxedMonPtr(graveBox, pos);
                if (bm != NULL && GetBoxMonData(bm, MON_DATA_SPECIES) != SPECIES_NONE)
                    count++;
            }
            return count;
        }
        default: return 0;
        }
    case 9: {
        u8 graveBox = (u8)(TOTAL_BOXES_COUNT - 1);
        if (arg0 < 0 || arg0 >= IN_BOX_COUNT)
            return 0;
        struct BoxPokemon *bm = GetBoxedMonPtr(graveBox, (u8)arg0);
        if (bm == NULL)
            return 0;
        if (arg1 == 0)
            return (int)GetBoxMonData(bm, MON_DATA_SPECIES);
        if (arg1 == 1) {
            const MpSoulLinkEntry *entry = MpSoulLink_LookupBoxMon(bm);
            return entry ? (int)entry->linkId : 0;
        }
        return 0;
    }
    case 10:
        switch (arg0) {
        case 0: return (int)gBattleTypeFlags;
        case 1: return (int)gBattleOutcome;
        case 2:
            if (arg1 < 0 || arg1 >= PARTY_SIZE) return 0;
            return (int)GetMonData(&gParties[B_TRAINER_OPPONENT_A][arg1], MON_DATA_SPECIES);
        case 3:
            if (arg1 < 0 || arg1 >= PARTY_SIZE) return 0;
            return (int)GetMonData(&gParties[B_TRAINER_OPPONENT_A][arg1], MON_DATA_HP);
        default: return 0;
        }
    default:
        return 0;
    }
}

WASM_KEEPALIVE int mp_test_action(int kind, int arg0, int arg1, int arg2)
{
    if (!gSaveBlock1Ptr || !gSaveBlock2Ptr)
        return 0;

    switch (kind) {
    case 0: {
        enum Gender gender = (arg0 == 1) ? FEMALE : MALE;
        NewGameInitData();
        gSaveBlock2Ptr->playerGender = gender;
        SetLastHealLocationWarp((gender == FEMALE) ? HEAL_LOCATION_LITTLEROOT_TOWN_MAYS_HOUSE_2F
                                                   : HEAL_LOCATION_LITTLEROOT_TOWN_BRENDANS_HOUSE_2F);
        FlagSet((gender == FEMALE) ? FLAG_HIDE_LITTLEROOT_TOWN_BRENDANS_HOUSE_MOM
                                   : FLAG_HIDE_LITTLEROOT_TOWN_MAYS_HOUSE_MOM);
        ApplyLobbyNameIfPresent();
        if (MpSoulLink_IsSetModeEnforced())
            gSaveBlock2Ptr->optionsBattleStyle = OPTIONS_BATTLE_STYLE_SET;

        SetWarpDestination(MAP_GROUP(MAP_LITTLEROOT_TOWN), MAP_NUM(MAP_LITTLEROOT_TOWN), WARP_ID_NONE,
                           (s8)((gender == FEMALE) ? 14 : 5), 8);
        WarpIntoMap();
        ResetInitialPlayerAvatarState();
        PlayTimeCounter_Start();
        ScriptContext_Init();
        UnlockPlayerFieldControls();
        gFieldCallback = NULL;
        gFieldCallback2 = NULL;
        sHasEnteredGameplay = true;
        gMain.inBattle = FALSE;
        SetMainCallback2(CB2_LoadMap);
        ForceReportStateNow();
        return 1;
    }
    case 1: {
        s16 tx = (s16)((arg2 >> 16) & 0xFFFF);
        s16 ty = (s16)(arg2 & 0xFFFF);
        SetWarpDestination((s8)arg0, (s8)arg1, WARP_ID_NONE, (s8)tx, (s8)ty);
        WarpIntoMap();
        gSaveBlock1Ptr->location.mapGroup = (s8)arg0;
        gSaveBlock1Ptr->location.mapNum = (s8)arg1;
        gSaveBlock1Ptr->pos.x = tx;
        gSaveBlock1Ptr->pos.y = ty;
        gFieldCallback = NULL;
        gFieldCallback2 = NULL;
        SetMainCallback2(CB2_LoadMap);
        ForceReportStateNow();
        return 1;
    }
    case 2: {
        if (arg0 == 1) {
            FlagSet(FLAG_SYS_CLOCK_SET);
            FlagSet(FLAG_HIDE_LITTLEROOT_TOWN_BRENDANS_HOUSE_TRUCK);
            FlagSet(FLAG_HIDE_LITTLEROOT_TOWN_MAYS_HOUSE_TRUCK);
            VarSet(VAR_LITTLEROOT_INTRO_STATE, 7);
            VarSet(VAR_LITTLEROOT_TOWN_STATE, 2);
            ForceReportStateNow();
            return 1;
        }
        if (arg0 == 2) {
            u16 starterChoice = (u16)(arg1 >= 0 && arg1 <= 2 ? arg1 : 2);
            enum Species starterSpecies = GetStarterPokemon(starterChoice);
            VarSet(VAR_STARTER_MON, starterChoice);
            FlagSet(FLAG_SYS_POKEMON_GET);
            FlagSet(FLAG_RESCUED_BIRCH);
            FlagSet(FLAG_HIDE_ROUTE_101_BIRCH_ZIGZAGOON_BATTLE);
            FlagSet(FLAG_HIDE_ROUTE_101_BIRCH_STARTERS_BAG);
            VarSet(VAR_ROUTE101_STATE, 3);
            MpRandomizer_PushSuppressStatic();
            MpSoulLink_SetSuppressGiftHook(true);
            ScriptGiveMon(starterSpecies, 5, ITEM_NONE);
            MpSoulLink_SetSuppressGiftHook(false);
            MpRandomizer_PopSuppressStatic();
            MpSoulLink_RegisterStarter(&gParties[B_TRAINER_PLAYER][0]);
            ForceReportStateNow();
            return (int)GetMonData(&gParties[B_TRAINER_PLAYER][0], MON_DATA_SPECIES);
        }
        if (arg0 == 3) {
            FlagSet(FLAG_DEFEATED_RIVAL_ROUTE103);
            FlagSet(FLAG_HIDE_ROUTE_103_RIVAL);
            VarSet(VAR_BIRCH_LAB_STATE, 5);
            FlagSet(FLAG_SYS_POKEDEX_GET);
            FlagSet(FLAG_ADVENTURE_STARTED);
            AddBagItem(ITEM_POKE_BALL, 5);
            FlagSet(FLAG_RECEIVED_RUNNING_SHOES);
            FlagSet(FLAG_SYS_B_DASH);
            VarSet(VAR_LITTLEROOT_TOWN_STATE, 4);
            MpWorldSync_CheckFlushDeferred();
            ForceReportStateNow();
            return 1;
        }
        return 0;
    }
    case 3: {
        struct Pokemon savedSlot0;
        enum Species spLeft = (enum Species)arg0;
        u8 lvLeft = (u8)((arg1 > 0) ? arg1 : 3);
        enum Species spRight = (enum Species)((arg2 >> 8) & 0xFFFF);
        u8 lvRight = (u8)(arg2 & 0xFF);
        if (spRight == SPECIES_NONE) spRight = spLeft;
        if (lvRight == 0) lvRight = lvLeft;

        CreateWildMon(spLeft, lvLeft);
        savedSlot0 = gParties[B_TRAINER_OPPONENT_A][0];
        CreateWildMon(spRight, lvRight);
        gParties[B_TRAINER_OPPONENT_A][1] = gParties[B_TRAINER_OPPONENT_A][0];
        gParties[B_TRAINER_OPPONENT_A][0] = savedSlot0;
        gPartiesCount[B_TRAINER_OPPONENT_A] = 2;

        gBattlersCount = 4;
        gBattlerPositions[0] = B_POSITION_PLAYER_LEFT;
        gBattlerPositions[1] = B_POSITION_OPPONENT_LEFT;
        gBattlerPositions[2] = B_POSITION_PLAYER_RIGHT;
        gBattlerPositions[3] = B_POSITION_OPPONENT_RIGHT;
        gBattlerPartyIndexes[0] = 0;
        gBattlerPartyIndexes[1] = 0;
        gBattlerPartyIndexes[2] = (CalculatePlayerPartyCount() > 1) ? 1 : 0;
        gBattlerPartyIndexes[3] = 1;

        memset(&gBattleMons[1], 0, sizeof(gBattleMons[1]));
        memset(&gBattleMons[3], 0, sizeof(gBattleMons[3]));
        gBattleMons[1].species = (enum Species)GetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_SPECIES);
        gBattleMons[1].level = (u8)GetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_LEVEL);
        gBattleMons[1].hp = (u16)GetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_HP);
        gBattleMons[1].maxHP = (u16)GetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_MAX_HP);
        gBattleMons[3].species = (enum Species)GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_SPECIES);
        gBattleMons[3].level = (u8)GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_LEVEL);
        gBattleMons[3].hp = (u16)GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_HP);
        gBattleMons[3].maxHP = (u16)GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_MAX_HP);

        gBattleTypeFlags = BATTLE_TYPE_DOUBLE;
        gBattleOutcome = 0;
        gMain.inBattle = TRUE;
        MpSoulLink_OnWildEncounterStarted((uint16_t)GetCurrentRegionMapSectionId());
        ForceReportStateNow();
        return 1;
    }
    case 4: {
        uint16_t mapsec = (uint16_t)GetCurrentRegionMapSectionId();
        if (arg0 == 0) {
            u32 zeroHp = 0;
            u32 oneHp = 1;
            SetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_HP, &zeroHp);
            gBattleMons[GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT)].hp = 0;
            SetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_HP, &oneHp);
            gBattleMons[GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT)].hp = 1;
            return 1;
        }
        if (arg0 == 1) {
            enum BattlerId leftOpp = GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
            enum BattlerId rightOpp = GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT);
            bool leftAlive = GetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_HP) > 0;
            bool rightAlive = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) && (GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_HP) > 0);
            if (!MpSoulLink_CanThrowBallAtBattler((uint8_t)leftOpp, mapsec)
             && (!rightAlive || !MpSoulLink_CanThrowBallAtBattler((uint8_t)rightOpp, mapsec)))
                return -2;
            if (leftAlive && rightAlive)
                return -1;
            enum Item ballId = (arg1 > 0) ? (enum Item)arg1 : ITEM_POKE_BALL;
            if (!RemoveBagItem(ballId, 1))
                return -3;
            struct Pokemon *targetMon = &gParties[B_TRAINER_OPPONENT_A][0];
            u8 ballByte = (u8)ballId;
            SetMonData(targetMon, MON_DATA_POKEBALL, &ballByte);
            SetMonData(targetMon, MON_DATA_MET_LOCATION, &mapsec);
            GiveCapturedMonToPlayer(targetMon);
            enum NationalDexOrder natDex = SpeciesToNationalPokedexNum((enum Species)GetMonData(targetMon, MON_DATA_SPECIES));
            GetSetPokedexFlag(natDex, FLAG_SET_SEEN);
            GetSetPokedexFlag(natDex, FLAG_SET_CAUGHT);
            MpSoulLink_OnWildEncounterFinished(mapsec, true, false);
            gBattleOutcome = B_OUTCOME_CAUGHT;
            gMain.inBattle = FALSE;
            gBattleTypeFlags = 0;
            ForceReportStateNow();
            return 1;
        }
        if (arg0 == 2 || arg0 == 3) {
            bool sp0Dupe = MpSoulLink_IsSpeciesLineOwned((u16)GetMonData(&gParties[B_TRAINER_OPPONENT_A][0], MON_DATA_SPECIES));
            bool sp1Present = GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_SPECIES) != SPECIES_NONE;
            bool sp1Dupe = !sp1Present || MpSoulLink_IsSpeciesLineOwned((u16)GetMonData(&gParties[B_TRAINER_OPPONENT_A][1], MON_DATA_SPECIES));
            bool allDupes = sp0Dupe && sp1Dupe;
            if (arg0 == 2 && arg1 > 0 && CalculatePlayerPartyCount() > 0) {
                struct Pokemon *lead = &gParties[B_TRAINER_PLAYER][0];
                u32 expVal = GetMonData(lead, MON_DATA_EXP) + (u32)arg1;
                SetMonData(lead, MON_DATA_EXP, &expVal);
                CalculateMonStats(lead);
            }
            MpSoulLink_OnWildEncounterFinished(mapsec, false, allDupes);
            gBattleOutcome = (arg0 == 2) ? B_OUTCOME_WON : B_OUTCOME_RAN;
            gMain.inBattle = FALSE;
            gBattleTypeFlags = 0;
            ForceReportStateNow();
            return 1;
        }
        return 0;
    }
    case 5: {
        if (arg0 < 0 || arg0 >= PARTY_SIZE)
            return 0;
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][arg0];
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
            return 0;
        if (arg1 == 1 && arg2 >= 1 && arg2 <= MAX_LEVEL) {
            enum Species sp = (enum Species)GetMonData(mon, MON_DATA_SPECIES);
            u32 targetExp = gExperienceTables[gSpeciesInfo[sp].growthRate][arg2];
            SetMonData(mon, MON_DATA_EXP, &targetExp);
            CalculateMonStats(mon);
        } else if (arg1 == 2) {
            u32 hpVal = (arg2 >= 0) ? (u32)arg2 : 0u;
            SetMonData(mon, MON_DATA_HP, &hpVal);
            if (hpVal == 0) {
                MpSoulLink_OnMonFainted(mon, false, MP_SL_CAUSE_BATTLE_FAINT);
                MpSoulLink_TransferDeadPartyMonsToGraveyard();
            }
        } else if (arg1 == 3 && arg2 > 0) {
            u32 expVal = GetMonData(mon, MON_DATA_EXP) + (u32)arg2;
            SetMonData(mon, MON_DATA_EXP, &expVal);
            CalculateMonStats(mon);
        } else if (arg1 == 4) {
            u16 itemId = (u16)arg2;
            SetMonData(mon, MON_DATA_HELD_ITEM, &itemId);
        }
        ForceReportStateNow();
        return 1;
    }
    case 6:
        HealPlayerParty();
        ForceReportStateNow();
        return 1;
    case 7:
        if (arg2 > 0 && !IsEnoughMoney(&gSaveBlock1Ptr->money, (u32)arg2))
            return 0;
        if (arg2 > 0)
            RemoveMoney(&gSaveBlock1Ptr->money, (u32)arg2);
        AddBagItem((enum Item)arg0, (u16)(arg1 > 0 ? arg1 : 1));
        ForceReportStateNow();
        return 1;
    case 8:
        if (arg2 > 0 && FlagGet((u16)arg2))
            return 0;
        AddBagItem((enum Item)arg0, (u16)(arg1 > 0 ? arg1 : 1));
        if (arg2 > 0)
            FlagSet((u16)arg2);
        ForceReportStateNow();
        return 1;
    case 9: {
        u8 partyCount = CalculatePlayerPartyCount();
        gBattleTypeFlags = BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE;
        gMain.inBattle = TRUE;
        int potionBlocked = MpSoulLink_IsBattleItemBlocked(ITEM_POTION) ? 1 : 0;
        if (partyCount > 0 && arg1 > 0) {
            struct Pokemon *lead = &gParties[B_TRAINER_PLAYER][0];
            u32 hp = GetMonData(lead, MON_DATA_HP);
            if (arg1 >= 9999)
                hp = 0;
            else if (hp > (u32)arg1 + 1u)
                hp -= (u32)arg1;
            else if (hp > 1u)
                hp = 1u;
            SetMonData(lead, MON_DATA_HP, &hp);
            if (hp == 0) {
                MpSoulLink_OnMonFainted(lead, false, MP_SL_CAUSE_BATTLE_FAINT);
                MpSoulLink_TransferDeadPartyMonsToGraveyard();
            }
        }
        partyCount = CalculatePlayerPartyCount();
        if (arg2 > 0) {
            for (u8 i = 0; i < partyCount; i++) {
                struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
                if (GetMonData(mon, MON_DATA_HP) == 0)
                    continue;
                u32 newExp = GetMonData(mon, MON_DATA_EXP) + (u32)arg2;
                SetMonData(mon, MON_DATA_EXP, &newExp);
                CalculateMonStats(mon);
            }
        }
        SetTrainerFlag((u16)arg0);
        if ((u16)arg0 == TRAINER_ROXANNE_1) {
            FlagSet(FLAG_DEFEATED_RUSTBORO_GYM);
            FlagSet(FLAG_BADGE01_GET);
            VarSet(VAR_RUSTBORO_CITY_STATE, 1);
            VarSet(VAR_PETALBURG_GYM_STATE, 1);
            if (!FlagGet(FLAG_RECEIVED_TM_ROCK_TOMB)) {
                FlagSet(FLAG_RECEIVED_TM_ROCK_TOMB);
                AddBagItem(ITEM_TM_ROCK_TOMB, 1);
            }
        }
        gBattleOutcome = B_OUTCOME_WON;
        gMain.inBattle = FALSE;
        gBattleTypeFlags = 0;
        ForceReportStateNow();
        return potionBlocked;
    }
    case 10:
        MpSoulLink_StepFrame();
        MpTrade_StepFrame();
        MpSoulLink_TransferDeadPartyMonsToGraveyard();
        ForceReportStateNow();
        return 1;
    default:
        return 0;
    }
}
