#ifndef GUARD_MP_TRADE_H
#define GUARD_MP_TRADE_H

#include <stdbool.h>
#include <stdint.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pokemon;

enum MpTradeResult {
    MP_TRADE_OK                = 0,
    MP_TRADE_ERR_INVALID       = 1,
    MP_TRADE_ERR_ALREADY_DONE  = 2,
    MP_TRADE_ERR_NO_MON        = 3,
    MP_TRADE_ERR_NO_ITEM       = 4,
    MP_TRADE_ERR_NO_BAG_SPACE  = 5,
    MP_TRADE_ERR_NO_MONEY      = 6,
    MP_TRADE_ERR_KEY_OR_HM     = 7,
    MP_TRADE_ERR_PARTY_EMPTY   = 8,
};

typedef struct MpTradeSession {
    bool active;
    uint32_t sessionId;
    uint16_t partnerPeerId;
    uint8_t partnerSlot;
    uint8_t offeredPartyIndex;
    bool hasLocalOffer;
    bool hasRemoteOffer;
    bool localLocked;
    bool remoteLocked;
    struct MpTradeOfferPayload localOffer;
    struct MpTradeOfferPayload remoteOffer;
    bool hasPendingCommit;
    struct MpTradeCommitPayload pendingCommit;
} MpTradeSession;

void MpTrade_Init(void);
void MpTrade_StepFrame(void);

bool MpTrade_SerializePokemon(const struct Pokemon *mon, uint8_t outBytes[MP_SERIALIZED_MON_BYTES]);
bool MpTrade_DeserializePokemon(const uint8_t inBytes[MP_SERIALIZED_MON_BYTES], struct Pokemon *outMon);

int MpTrade_BuildOffer(uint32_t sessionId, uint8_t partyIndex, uint16_t itemId, uint16_t itemQty, uint32_t money, struct MpTradeOfferPayload *outOffer);
bool MpTrade_SendOffer(uint16_t partnerPeerId, uint32_t sessionId, uint8_t partyIndex, uint16_t itemId, uint16_t itemQty, uint32_t money);
bool MpTrade_SetLockAndSend(uint16_t partnerPeerId, bool locked);
bool MpTrade_SubmitCommitToPeer(uint16_t partnerPeerId);

uint16_t MpTrade_ApplyTradeEvolution(struct Pokemon *mon, struct Pokemon *partnerMon);
int MpTrade_ApplyCommit(const struct MpTradeCommitPayload *commit);
const MpTradeSession *MpTrade_GetSession(void);

/* Required callbacks invoked by mp_net.c */
void MpTrade_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len);
void MpTrade_ApplyCommitFromBridge(void);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_MP_TRADE_H */
