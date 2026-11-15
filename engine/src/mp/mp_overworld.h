#ifndef GUARD_MP_OVERWORLD_H
#define GUARD_MP_OVERWORLD_H

#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read local player's current unshifted map coordinates and map location. */
bool MpOverworld_GetLocalCoords(int8_t *outGroup, int8_t *outNum, int16_t *outX, int16_t *outY);

/** Despawn any remote peer sprites currently tracked on the local map. */
void MpOverworld_ClearRemoteSprites(void);

/** Check whether an overworld objectEventId corresponds to a synced remote player. */
bool MpOverworld_IsRemotePlayerObject(uint8_t objectEventId, uint8_t *outPeerSlot);

/** Trigger a friendly PvP invite when interacting with a remote player in the overworld. */
void MpOverworld_InteractWithRemotePlayer(uint8_t peerSlot);

/** Immediately save current overworld progress into GBA Flash (synced to localStorage). */
bool MpOverworld_SaveGameNow(void);

/** Load an existing GBA Flash save directly into the world, or bootstrap Littleroot Town if none exists. */
void MpOverworld_ResumeOrBootstrapFromSave(void);

/** Erase GBA Flash save and reset runtime/MP state into either Title Screen or quick Littleroot start. */
void MpOverworld_ResetToNewGame(bool skipIntro);

/** Return true when the local player is in free overworld and can safely enter a synced 2v2 battle. */
bool MpOverworld_CanJoinSyncedBattleNow(void);

/** Stop any overworld dialogue/script blocking a forced transition into a synced battle. */
void MpOverworld_ForceClearFieldForSyncedBattle(void);

/** Schedule an immediate Flash + IndexedDB auto-save once in free overworld. */
void MpOverworld_RequestAutoSave(uint16_t delayFrames);

/** Return NULL if peer is ready in the same map, else exact fallback reason ("not_ready", "in_tutorial", "in_battle", "different_map", "busy"). */
const char *MpOverworld_GetPeerBusyReason(uint8_t slot);

/** Return true when the target peer is disconnected, in battle, or locked in a cutscene/NPC/solo activity. */
bool MpOverworld_IsPeerBusyForBattle(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_OVERWORLD_H */
