#ifndef GUARD_MP_BATTLE_2V2_H
#define GUARD_MP_BATTLE_2V2_H

#include <stdbool.h>
#include <stdint.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pokemon;
struct Sfc32State;

enum MpBattleSubOpcode {
    MP_PKT_BAT_PARTY_MON = 0x34,
};

typedef struct MP_PACKED MpBattlePartyMonPayload {
    uint32_t battleSeed;
    uint8_t slotIndex;
    uint8_t monIndex;
    uint8_t totalMons;
    uint8_t mode;
    uint8_t monBytes[MP_SERIALIZED_MON_BYTES];
} MpBattlePartyMonPayload;

void MpBattle2v2_Init(void);
void MpBattle2v2_StepFrame(void);

bool MpBattle2v2_ShouldForceWildDouble(void);
void MpBattle2v2_ConfigureTrainerBattleFlags(uint16_t trainerId, uint32_t *battleTypeFlags);
void MpBattle2v2_EnsureNpcDoubleParty(uint16_t trainerId, struct Pokemon *enemyParty);
void MpBattle2v2_EnsureReliableMove(struct Pokemon *mon);
uint8_t MpBattle2v2_CountAlivePlayerMons(void);
void MpBattle2v2_AttachWildAiPartner(uint32_t *battleTypeFlags);

bool MpBattle2v2_IsFallbackAiPartner(void);
bool MpBattle2v2_IsFriendlyPvP(void);
bool MpBattle2v2_IsCoopMulti(void);
bool MpBattle2v2_IsSyncedSession(void);
const uint8_t *MpBattle2v2_GetPvpOpponentName(void);
bool MpBattle2v2_IsRemoteControlledBattler(uint8_t battlerId);
bool MpBattle2v2_ShouldWaitForRemoteAction(uint8_t battlerId);
bool MpBattle2v2_WasLastBattleFriendlyPvP(void);
bool MpBattle2v2_WasLastBattleBridgeSynced(void);
uint16_t MpBattle2v2_GetSyncedTrainerId(void);
bool MpBattle2v2_ReadyForBattleStart(void);
bool MpBattle2v2_WaitForBusyTrainerPrompt(void);
void MpBattle2v2_ResolveBusyPrompt(int choice);
void MpBattle2v2_VerifySyncedSeedBeforeTrainerBuild(void);
uint8_t MpBattle2v2_GetRemotePartnerGender(void);
uint32_t MpBattle2v2_GetBattleSeed(void);
bool MpBattle2v2_IsInitiator(void);
bool MpBattle2v2_ReadyToCommitLocalAction(uint8_t battlerId);
bool MpBattle2v2_ShouldUseLocalOpponentAi(void);
bool MpBattle2v2_AreRemoteAiActionsReady(void);
bool MpBattle2v2_IsWaitingForRemotePartner(void);

bool MpBattle2v2_ShouldWaitForRemoteSwitch(uint8_t battlerId);
void MpBattle2v2_CommitLocalSwitchChoice(uint8_t battlerId, uint8_t caseId, uint8_t chosenMonId);
bool MpBattle2v2_TryConsumeRemoteSwitchChoice(uint8_t battlerId, uint8_t caseId, uint8_t *outChosenMonId);
void MpBattle2v2_CommitLearnedMove(uint8_t partyIndex, uint8_t moveSlot, uint16_t moveId);

void MpBattle2v2_SaveRngState(struct Sfc32State *outRng1, struct Sfc32State *outRng2);
void MpBattle2v2_RestoreRngState(const struct Sfc32State *rng1, const struct Sfc32State *rng2);
void MpBattle2v2_ReseedSyncedBattleRng(uint32_t contextTag);

void MpBattle2v2_StartSyncedBattle(uint32_t battleSeed, uint8_t mode, uint16_t trainerId);
void MpBattle2v2_OnBattleInit(void);
void MpBattle2v2_OnTurnStart(void);
void MpBattle2v2_CommitLocalAction(uint8_t battlerId);
bool MpBattle2v2_TryApplyRemoteAction(uint8_t battlerId);
void MpBattle2v2_OnAllActionsLocked(void);
void MpBattle2v2_OnBattleEnd(uint8_t battleOutcome);

/* Implemented in battle_setup.c to launch battle transition safely */
void BattleSetup_LaunchMpSyncedBattle(void);

/* Required callbacks invoked by mp_net.c */
void MpBattle2v2_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpBattle2v2_StartSyncedFromBridge(uint32_t battleSeed, uint8_t mode, uint8_t partnerSlot, uint16_t partnerPeerId);

#ifdef WASM_PLATFORM
void WasmMp_PromptTeammateBusy(int partnerSlot, const char *reason);
void WasmMp_NotifyBusyPromptResolved(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_BATTLE_2V2_H */
