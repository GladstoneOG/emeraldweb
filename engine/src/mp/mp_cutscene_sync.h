#ifndef GUARD_MP_CUTSCENE_SYNC_H
#define GUARD_MP_CUTSCENE_SYNC_H

#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns true if a specific global cutscene ID has already completed in this run. */
bool MpCutscene_IsCompleted(uint16_t cutsceneId);

/** Mark a global cutscene ID completed locally (e.g. from loaded snapshot). */
void MpCutscene_MarkCompleted(uint16_t cutsceneId);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_CUTSCENE_SYNC_H */
