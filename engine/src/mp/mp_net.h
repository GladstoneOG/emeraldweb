#ifndef GUARD_MP_NET_H
#define GUARD_MP_NET_H

#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compute deterministic 64-bit FNV-1a hash from an ASCII seed string. */
uint64_t MpNet_HashSeedString(const char *seedStr);

/** Update peer tracking metadata for a given player slot (0..3). */
void MpNet_UpdatePeerMeta(uint8_t slotIndex, uint16_t peerId, bool connected, uint8_t pairTag, const char *name);

/** Returns true once JS/control metadata has set an authoritative pair tag for slotIndex. */
bool MpNet_HasAuthoritativePairTag(uint8_t slotIndex);

/** Mutable peer slot accessor for sibling engine-side sync modules. */
MpPeerState *MpNet_GetPeerStateMutable(uint8_t slotIndex);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_NET_H */
