#ifndef GUARD_MP_RANDOMIZER_H
#define GUARD_MP_RANDOMIZER_H

#include <stdbool.h>
#include <stdint.h>
#include "gba/types.h"
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct LevelUpMove;

enum MpRandomizerSalt {
    MP_RANDO_SALT_WILD        = 0x57494C44u,
    MP_RANDO_SALT_STARTER     = 0x53545254u,
    MP_RANDO_SALT_STATIC      = 0x53544154u,
    MP_RANDO_SALT_TRAINER     = 0x54524E52u,
    MP_RANDO_SALT_NPC_PARTNER = 0x5052544Eu,
    MP_RANDO_SALT_GYM_TYPE    = 0x47594D54u,
    MP_RANDO_SALT_ITEM        = 0x4954454Du,
    MP_RANDO_SALT_TM          = 0x544D484Du,
    MP_RANDO_SALT_MART        = 0x4D415254u,
    MP_RANDO_SALT_HELD_ITEM   = 0x48454C44u,
    MP_RANDO_SALT_ABILITY     = 0x4142494Cu,
    MP_RANDO_SALT_LEARNSET    = 0x4C524E53u,
    MP_RANDO_SALT_TYPE        = 0x54595045u,
    MP_RANDO_SALT_EVO         = 0x45564F4Cu,
};

uint64_t MpRand_SplitMix64(uint64_t runSeed, uint32_t categorySalt, uint32_t entityId);
uint64_t MpRandomizer_HashSeedString(const char *seedStr);
uint64_t MpRandomizer_GetRunSeed(void);
uint32_t MpRandomizer_GetFlags(void);
bool MpRandomizer_HasFlag(uint32_t flag);

void MpRandomizer_PushSuppressStatic(void);
void MpRandomizer_PopSuppressStatic(void);
bool MpRandomizer_IsStaticSuppressed(void);

uint16_t MpRandomizer_GetWildSpecies(uint8_t mapGroup, uint8_t mapNum, uint8_t area, uint8_t slot, uint16_t origSpecies);
uint16_t MpRandomizer_GetStarterSpecies(uint16_t starterIndex, uint16_t origSpecies);
uint16_t MpRandomizer_GetStaticSpecies(uint32_t entityId, uint16_t origSpecies);
uint16_t MpRandomizer_GetTrainerSpecies(uint16_t trainerId, uint8_t monSlot, uint8_t trainerClass, uint16_t origSpecies);
uint16_t MpRandomizer_GetNpcPartnerSpecies(uint16_t trainerId, uint8_t trainerClass, uint16_t aceSpecies, uint8_t aceLevel);
u16 MpRandomizer_GetPartnerSpecies(u32 seed, u8 slotIdx, u16 excludeSpecies, u8 level);

bool MpRandomizer_IsItemRandomizable(uint16_t itemId);
uint16_t MpRandomizer_GetItemReplacement(uint32_t entityId, uint16_t origItem, bool martOnly);
uint16_t MpRandomizer_GetHeldItem(uint32_t entityId, uint16_t origItem);
void MpRandomizer_TransformShopItems(uint16_t *items, uint16_t count, uint8_t mapGroup, uint8_t mapNum);

uint16_t MpRandomizer_GetSpeciesAbility(uint16_t species, uint8_t slot, uint16_t origAbility);
uint8_t MpRandomizer_GetSpeciesType(uint16_t species, uint8_t slot, uint8_t origType);
const struct LevelUpMove *MpRandomizer_GetLevelUpLearnset(uint16_t species, const struct LevelUpMove *origLearnset);
uint16_t MpRandomizer_GetEvolutionTarget(uint16_t fromSpecies, uint8_t evoIndex, uint16_t origTarget);

/* Required callback invoked by mp_net.c on config updates */
void MpRandomizer_OnRunConfigChanged(const MpRunConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_RANDOMIZER_H */
