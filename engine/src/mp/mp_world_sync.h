#ifndef GUARD_MP_WORLD_SYNC_H
#define GUARD_MP_WORLD_SYNC_H

#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return true once all 6 mandatory intro/tutorial milestone flags are set locally. */
bool MpWorldSync_IsLocalTutorialComplete(void);

/** True while applying a replicated remote flag/var locally (prevents echo loops). */
bool MpWorldSync_IsApplyingRemote(void);

/** Flushes deferred remote story packets once local tutorial/starter is complete. */
void MpWorldSync_CheckFlushDeferred(void);

/** Apply or queue a replicated global story flag received from the JS bridge/snapshot. */
void MpWorldSync_ApplyRemoteFlagFromBridge(uint16_t flagId, bool value);

/** Apply or queue a replicated global story var received from the JS bridge/snapshot. */
void MpWorldSync_ApplyRemoteVarFromBridge(uint16_t varId, uint16_t value);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_WORLD_SYNC_H */
