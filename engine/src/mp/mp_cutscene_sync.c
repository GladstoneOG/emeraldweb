#include "mp_cutscene_sync.h"
#include "mp_overworld.h"

#include <stdio.h>
#include <string.h>

#define MP_MAX_CUTSCENES 512u

static struct {
    bool active;
    uint16_t cutsceneId;
    uint8_t phase;
    bool deferred;
    bool hasSavedReturnWarp;
    int8_t returnMapGroup;
    int8_t returnMapNum;
    int16_t returnX;
    int16_t returnY;
    char title[32];
} sCutscene;

static uint8_t sCompletedBits[MP_MAX_CUTSCENES / 8u];

static void SendCutsceneBinary(uint8_t phase, uint16_t cutsceneId, bool deferred, const char *title)
{
    MpCutscenePayload pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cutsceneId = cutsceneId;
    pkt.phase = phase;
    pkt.deferred = deferred ? 1u : 0u;
    pkt.fromSlot = MpNet_GetLocalSlot();
    pkt.returnMapGroup = sCutscene.returnMapGroup;
    pkt.returnMapNum = sCutscene.returnMapNum;
    pkt.returnX = sCutscene.returnX;
    pkt.returnY = sCutscene.returnY;
    if (title && title[0] != '\0')
        snprintf(pkt.title, sizeof(pkt.title), "%.31s", title);

    MpNet_SendBinary(MP_CH_WORLD_SYNC, MP_PEER_BROADCAST, MP_PKT_WORLD_CUTSCENE, &pkt, sizeof(pkt));
}

static void SendCutsceneAckJson(bool inBattleDeferred)
{
    char json[96];
    snprintf(json, sizeof(json),
             "{\"t\":\"cutscene_ack\",\"status\":\"%s\"}",
             inBattleDeferred ? "in_battle_deferred" : "watching");
    MpNet_SendJson(MP_CH_WORLD_SYNC, MP_PEER_HOST, json);
}

static void SavePreCutsceneReturnLocation(void)
{
    int8_t g = 0, n = 0;
    int16_t x = 0, y = 0;
    if (MpOverworld_GetLocalCoords(&g, &n, &x, &y)) {
        sCutscene.hasSavedReturnWarp = true;
        sCutscene.returnMapGroup = g;
        sCutscene.returnMapNum = n;
        sCutscene.returnX = x;
        sCutscene.returnY = y;
    }
}

void MpCutscene_Init(void)
{
    memset(&sCutscene, 0, sizeof(sCutscene));
    sCutscene.phase = MP_CUTSCENE_IDLE;
    memset(sCompletedBits, 0, sizeof(sCompletedBits));
}

bool MpCutscene_IsCompleted(uint16_t cutsceneId)
{
    if (cutsceneId >= MP_MAX_CUTSCENES)
        return false;
    return (sCompletedBits[cutsceneId >> 3] & (1u << (cutsceneId & 7u))) != 0;
}

void MpCutscene_MarkCompleted(uint16_t cutsceneId)
{
    if (cutsceneId >= MP_MAX_CUTSCENES)
        return;
    sCompletedBits[cutsceneId >> 3] |= (uint8_t)(1u << (cutsceneId & 7u));
}

bool MpCutscene_IsActive(void)
{
    return sCutscene.active;
}

bool MpCutscene_TriggerGlobal(uint16_t cutsceneId, const char *title)
{
    if (sCutscene.active || MpCutscene_IsCompleted(cutsceneId))
        return false;

    const char *safeTitle = (title && title[0] != '\0') ? title : "Story Event";
    SavePreCutsceneReturnLocation();

    sCutscene.active = true;
    sCutscene.cutsceneId = cutsceneId;
    sCutscene.deferred = MpNet_IsLocalInBattle();
    snprintf(sCutscene.title, sizeof(sCutscene.title), "%.31s", safeTitle);

    if (MpNet_IsHost()) {
        sCutscene.phase = MP_CUTSCENE_START;
        SendCutsceneBinary(MP_CUTSCENE_START, cutsceneId, sCutscene.deferred, sCutscene.title);
    } else {
        sCutscene.phase = MP_CUTSCENE_TRIGGER;
        SendCutsceneBinary(MP_CUTSCENE_TRIGGER, cutsceneId, sCutscene.deferred, sCutscene.title);
    }

    char json[160];
    snprintf(json, sizeof(json),
             "{\"t\":\"cutscene_trigger\",\"cutsceneId\":%u,\"title\":\"%.31s\"}",
             (unsigned)cutsceneId, sCutscene.title);
    MpNet_SendJson(MP_CH_WORLD_SYNC, MP_PEER_HOST, json);
    return true;
}

static void ReturnToSavedLocationIfMoved(void)
{
    if (!sCutscene.hasSavedReturnWarp)
        return;
    int8_t curG = 0, curN = 0;
    int16_t curX = 0, curY = 0;
    if (!MpOverworld_GetLocalCoords(&curG, &curN, &curX, &curY))
        return;
    if (curG != sCutscene.returnMapGroup || curN != sCutscene.returnMapNum ||
        curX != sCutscene.returnX || curY != sCutscene.returnY) {
        MpOverworld_ExecuteWarp(sCutscene.returnMapGroup, sCutscene.returnMapNum,
                                sCutscene.returnX, sCutscene.returnY);
    }
}

void MpCutscene_FinishActive(void)
{
    if (!sCutscene.active)
        return;

    uint16_t finishedId = sCutscene.cutsceneId;
    MpCutscene_MarkCompleted(finishedId);

    if (MpNet_IsHost()) {
        SendCutsceneBinary(MP_CUTSCENE_END, finishedId, false, sCutscene.title);
    }

    ReturnToSavedLocationIfMoved();
    memset(&sCutscene, 0, sizeof(sCutscene));
    sCutscene.phase = MP_CUTSCENE_IDLE;
}

void MpCutscene_OnFrame(void)
{
    if (!sCutscene.active)
        return;

    if (sCutscene.deferred && !MpNet_IsLocalInBattle()) {
        sCutscene.deferred = false;
        sCutscene.phase = MP_CUTSCENE_ACK;
        SendCutsceneBinary(MP_CUTSCENE_ACK, sCutscene.cutsceneId, false, sCutscene.title);
        SendCutsceneAckJson(false);
    }
}

void MpCutscene_HandlePacket(uint16_t fromPeerId, uint8_t opcode, const void *payload, uint16_t len)
{
    (void)fromPeerId;
    if (opcode != MP_PKT_WORLD_CUTSCENE || len < sizeof(MpCutscenePayload))
        return;

    MpCutscenePayload pkt;
    memcpy(&pkt, payload, sizeof(pkt));

    switch (pkt.phase) {
    case MP_CUTSCENE_TRIGGER:
        if (MpNet_IsHost() && !MpCutscene_IsCompleted(pkt.cutsceneId) && !sCutscene.active) {
            SavePreCutsceneReturnLocation();
            sCutscene.active = true;
            sCutscene.cutsceneId = pkt.cutsceneId;
            sCutscene.phase = MP_CUTSCENE_START;
            sCutscene.deferred = MpNet_IsLocalInBattle();
            snprintf(sCutscene.title, sizeof(sCutscene.title), "%.31s",
                     pkt.title[0] ? pkt.title : "Story Event");
            SendCutsceneBinary(MP_CUTSCENE_START, pkt.cutsceneId, sCutscene.deferred, sCutscene.title);
        }
        break;
    case MP_CUTSCENE_START:
        if (!MpCutscene_IsCompleted(pkt.cutsceneId)) {
            if (sCutscene.active && sCutscene.cutsceneId == pkt.cutsceneId)
                break;
            SavePreCutsceneReturnLocation();
            sCutscene.active = true;
            sCutscene.cutsceneId = pkt.cutsceneId;
            snprintf(sCutscene.title, sizeof(sCutscene.title), "%.31s",
                     pkt.title[0] ? pkt.title : "Story Event");
            sCutscene.deferred = MpNet_IsLocalInBattle();
            sCutscene.phase = sCutscene.deferred ? MP_CUTSCENE_START : MP_CUTSCENE_ACK;
            SendCutsceneBinary(MP_CUTSCENE_ACK, pkt.cutsceneId, sCutscene.deferred, sCutscene.title);
            SendCutsceneAckJson(sCutscene.deferred);
        }
        break;
    case MP_CUTSCENE_END:
        MpCutscene_MarkCompleted(pkt.cutsceneId);
        if (sCutscene.active && sCutscene.cutsceneId == pkt.cutsceneId) {
            ReturnToSavedLocationIfMoved();
            memset(&sCutscene, 0, sizeof(sCutscene));
            sCutscene.phase = MP_CUTSCENE_IDLE;
        }
        break;
    default:
        break;
    }
}
