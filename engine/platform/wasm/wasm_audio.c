#include "wasm_platform.h"
#include <math.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define WASM_AUDIO_SAMPLE_RATE 32768
#define WASM_AUDIO_FRAME_SAMPLES 547 /* ~32768 / 59.7275 Hz */
#define WASM_AUDIO_RING_FRAMES 8192

static int16_t sAudioRing[WASM_AUDIO_RING_FRAMES * 2];
static volatile uint32_t sRingWritePos = 0;
static volatile uint32_t sRingReadPos = 0;
static bool sAudioMuted = false;

void WasmAudio_Init(void)
{
    memset(sAudioRing, 0, sizeof(sAudioRing));
    sRingWritePos = 0;
    sRingReadPos = 0;
}

void WasmAudio_SetMuted(bool muted)
{
    sAudioMuted = muted;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_push_pcm_audio, (uintptr_t ptr, int frameCount, int sampleRate), {
    if (typeof window === "undefined") return;
    if (!window._pokeAudioCtx) {
        const AudioCtx = window.AudioContext || window.webkitAudioContext;
        if (!AudioCtx) return;
        /* Created lazily; resumed on first user gesture inside hud_controller.js */
        window._pokeAudioCtx = new AudioCtx({ sampleRate: sampleRate });
        window._pokeNextAudioTime = 0;
    }
    const ctx = window._pokeAudioCtx;
    if (!window._pokeAudioGain || window._pokeAudioGain.context !== ctx) {
        window._pokeAudioGain = ctx.createGain();
        window._pokeAudioGain.connect(ctx.destination);
    }
    const vol = (typeof window.POKEMON_AUDIO_VOLUME === "number")
        ? Math.max(0, Math.min(1, window.POKEMON_AUDIO_VOLUME))
        : 0.8;
    const isMuted = Boolean(globalThis.POKEMON_AUDIO_MUTED || window.POKEMON_AUDIO_MUTED);
    window._pokeAudioGain.gain.value = isMuted ? 0 : vol;
    if (isMuted) {
        window._pokeNextAudioTime = 0;
        return;
    }
    if (ctx.state !== "running") return;

    const buf = ctx.createBuffer(2, frameCount, sampleRate);
    const chL = buf.getChannelData(0);
    const chR = buf.getChannelData(1);
    const pcm = HEAP16;
    const base = ptr >> 1;
    for (let i = 0; i < frameCount; i++) {
        chL[i] = pcm[base + i * 2] / 32768.0;
        chR[i] = pcm[base + i * 2 + 1] / 32768.0;
    }
    const src = ctx.createBufferSource();
    src.buffer = buf;
    src.connect(window._pokeAudioGain);
    const now = ctx.currentTime;
    if (window._pokeNextAudioTime < now) {
        window._pokeNextAudioTime = now + 0.02;
    }
    if (window._pokeNextAudioTime - now < 0.25) {
        src.start(window._pokeNextAudioTime);
        window._pokeNextAudioTime += buf.duration;
    }
});
#endif

static void PushSamples(const int16_t *interleaved, uint32_t frameCount)
{
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t idx = (sRingWritePos % WASM_AUDIO_RING_FRAMES) * 2u;
        sAudioRing[idx] = sAudioMuted ? 0 : interleaved[i * 2u];
        sAudioRing[idx + 1u] = sAudioMuted ? 0 : interleaved[i * 2u + 1u];
        sRingWritePos++;
    }
#ifdef __EMSCRIPTEN__
    if (!sAudioMuted) {
        js_push_pcm_audio((uintptr_t)interleaved, (int)frameCount, WASM_AUDIO_SAMPLE_RATE);
    }
#endif
}

uint32_t umul3232H32(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}

uint32_t WasmAudio_ReadSamples(int16_t *outInterleaved, uint32_t maxFrames)
{
    uint32_t avail = sRingWritePos - sRingReadPos;
    if (avail > maxFrames)
        avail = maxFrames;
    for (uint32_t i = 0; i < avail; i++) {
        uint32_t idx = ((sRingReadPos + i) % WASM_AUDIO_RING_FRAMES) * 2u;
        outInterleaved[i * 2u] = sAudioRing[idx];
        outInterleaved[i * 2u + 1u] = sAudioRing[idx + 1u];
    }
    sRingReadPos += avail;
    return avail;
}

#ifdef WASM_SMOKE_TEST

void WasmAudio_MixFrame(void)
{
    int16_t frameBuf[WASM_AUDIO_FRAME_SAMPLES * 2];
    memset(frameBuf, 0, sizeof(frameBuf));
    PushSamples(frameBuf, WASM_AUDIO_FRAME_SAMPLES);
}

#else

#include "global.h"
#include "gba/m4a_internal.h"

#define TONEDATA_TYPE_REV 0x10
#define TONEDATA_TYPE_CMP 0x20
#define SOUND_CHANNEL_SF_SPECIAL 0x20
#define WAVE_DATA_FLAG_LOOP 0xC0

extern void *const gMPlayJumpTableTemplate[];
extern const u8 gClockTable[];
extern const s8 gDeltaEncodingTable[];
extern const u8 gCgb3Vol[];
extern u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

/* =========================================================================
 * Per-Channel High-Precision Resampler + BDPCM Block Decoder State
 * ========================================================================= */
typedef struct WasmDsChanState {
    double pos;
    uint32_t decodedBlock;
    s8 bdpcmBuf[64];
} WasmDsChanState;

static WasmDsChanState sDsChanState[MAX_DIRECTSOUND_CHANNELS];

/* CGB oscillator state */
static double sCgbPhase[3] = {0.0, 0.0, 0.0};
static double sCgbNoiseAccum = 0.0;
static uint16_t sCgbLfsr = 0x7FFFu;

static void ClearModMInternal(struct MusicPlayerTrack *track)
{
    track->modM = 0;
    track->lfoSpeedC = 0;
    if (track->modT == 0)
        track->flags |= MPT_FLG_PITCHG;
    else
        track->flags |= MPT_FLG_VOLCHG;
}

static void ChnVolSetAsmInternal(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    s32 pan = (s8)chan->rhythmPan;
    u32 vel = chan->velocity;
    s32 right = ((s32)track->volMR * (0x80 + pan) * (s32)vel) >> 14;
    s32 left = ((s32)track->volML * (0x7F - pan) * (s32)vel) >> 14;
    if (right < 0)
        right = 0;
    else if (right > 0xFF)
        right = 0xFF;
    if (left < 0)
        left = 0;
    else if (left > 0xFF)
        left = 0xFF;
    chan->rightVolume = (u8)right;
    chan->leftVolume = (u8)left;
}

static inline s8 DecodeBdpcmSample(int chanIdx, const struct WaveData *wav, uint32_t sampleIdx)
{
    if (sampleIdx >= wav->size)
        return 0;

    uint32_t blockIdx = sampleIdx >> 6;
    WasmDsChanState *st = &sDsChanState[chanIdx];
    if (st->decodedBlock != blockIdx) {
        st->decodedBlock = blockIdx;
        const u8 *blk = (const u8 *)wav + 0x10 + (size_t)blockIdx * 0x21u;
        s8 *dst = st->bdpcmBuf;
        u8 cur = blk[0];
        *dst++ = (s8)cur;
        u8 b1 = blk[1];
        cur = (u8)(cur + gDeltaEncodingTable[b1 & 0x0Fu]);
        *dst++ = (s8)cur;
        for (int i = 2; i < 0x21; i++) {
            u8 b = blk[i];
            cur = (u8)(cur + gDeltaEncodingTable[b >> 4]);
            *dst++ = (s8)cur;
            cur = (u8)(cur + gDeltaEncodingTable[b & 0x0Fu]);
            *dst++ = (s8)cur;
        }
    }
    return st->bdpcmBuf[sampleIdx & 0x3Fu];
}

static inline s8 ReadWaveSample(int chanIdx, const struct WaveData *wav, uint32_t idx)
{
    if (wav->type != 0)
        return DecodeBdpcmSample(chanIdx, wav, idx);
    return wav->data[idx];
}

static void StepDirectSoundEnvelopes(struct SoundInfo *soundInfo)
{
    int maxChans = soundInfo->maxChans;
    if (maxChans > MAX_DIRECTSOUND_CHANNELS)
        maxChans = MAX_DIRECTSOUND_CHANNELS;

    for (int i = 0; i < maxChans; i++) {
        struct SoundChannel *chan = &soundInfo->chans[i];
        struct WaveData *wav = chan->wav;
        u8 sf = chan->statusFlags;

        if (!(sf & SOUND_CHANNEL_SF_ON))
            continue;

        if (wav == NULL || wav->size == 0) {
            chan->statusFlags = 0;
            continue;
        }

        if (sf & SOUND_CHANNEL_SF_START) {
            if (sf & SOUND_CHANNEL_SF_STOP) {
                chan->statusFlags = 0;
                continue;
            }
            sf = SOUND_CHANNEL_SF_ENV_ATTACK;
            if (((wav->status >> 8) & WAVE_DATA_FLAG_LOOP) != 0)
                sf |= SOUND_CHANNEL_SF_LOOP;
            chan->statusFlags = sf;

            u32 startOff = chan->count;
            if (startOff >= wav->size)
                startOff = 0;
            if (chan->type & TONEDATA_TYPE_REV)
                sDsChanState[i].pos = (double)(wav->size - 1u - startOff);
            else
                sDsChanState[i].pos = (double)startOff;
            sDsChanState[i].decodedBlock = 0xFFFFFFFFu;

            chan->envelopeVolume = 0;
            u32 env = chan->attack;
            if (env >= 0xFFu) {
                env = 0xFFu;
                sf--;
                chan->statusFlags = sf;
            }
            chan->envelopeVolume = (u8)env;
        } else {
            u32 env = chan->envelopeVolume;
            if (sf & SOUND_CHANNEL_SF_IEC) {
                if (chan->pseudoEchoLength <= 1) {
                    chan->statusFlags = 0;
                    continue;
                }
                chan->pseudoEchoLength--;
            } else if (sf & SOUND_CHANNEL_SF_STOP) {
                env = (env * chan->release) >> 8;
                if (env <= chan->pseudoEchoVolume) {
                start_pseudo_echo:
                    env = chan->pseudoEchoVolume;
                    if (env == 0) {
                        chan->statusFlags = 0;
                        continue;
                    }
                    sf |= SOUND_CHANNEL_SF_IEC;
                    chan->statusFlags = sf;
                }
                chan->envelopeVolume = (u8)env;
            } else if ((sf & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY) {
                env = (env * chan->decay) >> 8;
                if (env <= chan->sustain) {
                    env = chan->sustain;
                    if (env == 0)
                        goto start_pseudo_echo;
                    sf--;
                    chan->statusFlags = sf;
                }
                chan->envelopeVolume = (u8)env;
            } else if ((sf & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_ATTACK) {
                env += chan->attack;
                if (env >= 0xFFu) {
                    env = 0xFFu;
                    sf--;
                    chan->statusFlags = sf;
                }
                chan->envelopeVolume = (u8)env;
            }
        }

        u32 masterScaled = ((u32)(soundInfo->masterVolume + 1u) * (u32)chan->envelopeVolume) >> 4;
        chan->envelopeVolumeRight = (u8)(((u32)chan->rightVolume * masterScaled) >> 8);
        chan->envelopeVolumeLeft = (u8)(((u32)chan->leftVolume * masterScaled) >> 8);
    }
}

void m4aSoundVSync(void)
{
}

void SoundMainBTM(void)
{
}

void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable)
{
    for (int i = 0; i < 36; i++)
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

void RealClearChain(void *x)
{
    struct SoundChannel *chan = (struct SoundChannel *)x;
    if (chan == NULL)
        return;

    struct MusicPlayerTrack *track = chan->track;
    if (track == NULL)
        return;

    struct SoundChannel *next = (struct SoundChannel *)chan->nextChannelPointer;
    struct SoundChannel *prev = (struct SoundChannel *)chan->prevChannelPointer;
    if (prev != NULL)
        prev->nextChannelPointer = next;
    else
        track->chan = next;

    if (next != NULL)
        next->prevChannelPointer = prev;

    chan->track = NULL;
}

void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    if (track == NULL || !(track->flags & MPT_FLG_EXIST))
        return;

    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct SoundChannel *chan = track->chan;
    while (chan != NULL) {
        struct SoundChannel *next = (struct SoundChannel *)chan->nextChannelPointer;
        if (chan->statusFlags != 0) {
            u8 cgbType = chan->type & TONEDATA_TYPE_CGB;
            if (cgbType != 0 && soundInfo != NULL && soundInfo->CgbOscOff != NULL)
                soundInfo->CgbOscOff(cgbType);
            chan->statusFlags = 0;
        }
        chan->track = NULL;
        chan = next;
    }
    track->chan = NULL;
}

void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    if (mplayInfo == NULL || mplayInfo->ident != ID_NUMBER)
        return;

    mplayInfo->ident++;

    if (mplayInfo->MPlayMainNext != NULL)
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);

    if ((s32)mplayInfo->status < 0) {
        mplayInfo->ident = ID_NUMBER;
        return;
    }

    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0) {
        mplayInfo->ident = ID_NUMBER;
        return;
    }

    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    mplayInfo->tempoC += mplayInfo->tempoI;

    while (mplayInfo->tempoC >= 150) {
        int trackCount = mplayInfo->trackCount;
        if (trackCount > MAX_MUSICPLAYER_TRACKS)
            trackCount = MAX_MUSICPLAYER_TRACKS;

        u32 activeBits = 0;
        struct MusicPlayerTrack *track = mplayInfo->tracks;

        for (int t = 0; t < trackCount; t++, track++) {
            if (!(track->flags & MPT_FLG_EXIST))
                continue;

            activeBits |= (1u << t);

            struct SoundChannel *chan = track->chan;
            while (chan != NULL) {
                struct SoundChannel *next = (struct SoundChannel *)chan->nextChannelPointer;
                if (chan->statusFlags & SOUND_CHANNEL_SF_ON) {
                    if (chan->gateTime != 0 && --chan->gateTime == 0)
                        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                } else {
                    ClearChain(chan);
                }
                chan = next;
            }

            if (track->flags & MPT_FLG_START) {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 64;
                track->lfoSpeed = 22;
                track->tone.type = 1;
            }

            int safety = 256;
            while (track->wait == 0 && safety-- > 0) {
                if (track->cmdPtr == NULL) {
                    TrackStop(mplayInfo, track);
                    track->flags = 0;
                    break;
                }

                u8 cmd = *track->cmdPtr;
                if (cmd < 0x80) {
                    cmd = track->runningStatus;
                } else {
                    track->cmdPtr++;
                    if (cmd >= 0xBD)
                        track->runningStatus = cmd;
                }

                if (cmd >= 0xCF) {
                    if (soundInfo != NULL && soundInfo->plynote != NULL)
                        soundInfo->plynote(cmd - 0xCF, mplayInfo, track);
                    else
                        ply_note(cmd - 0xCF, mplayInfo, track);
                } else if (cmd > 0xB0) {
                    mplayInfo->cmd = cmd - 0xB1;
                    if (soundInfo != NULL && soundInfo->MPlayJumpTable != NULL) {
#if __STDC_VERSION__ < 202311L
                        void (*fn)(struct MusicPlayerInfo *, struct MusicPlayerTrack *) =
                            (void (*)(struct MusicPlayerInfo *, struct MusicPlayerTrack *))
                            soundInfo->MPlayJumpTable[mplayInfo->cmd];
#else
                        void (*fn)(...) = (void (*)(...))soundInfo->MPlayJumpTable[mplayInfo->cmd];
#endif
                        fn(mplayInfo, track);
                    }
                    if (track->flags == 0)
                        break;
                } else {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }

            if (track->flags == 0)
                continue;

            track->wait--;

            if (track->lfoSpeed != 0 && track->mod != 0) {
                if (track->lfoDelayC != 0) {
                    track->lfoDelayC--;
                } else {
                    u8 lfoSpeedC = (u8)(track->lfoSpeedC + track->lfoSpeed);
                    track->lfoSpeedC = lfoSpeedC;
                    s32 tri = ((s8)(lfoSpeedC - 0x40) >= 0) ? (0x80 - (s32)lfoSpeedC) : (s32)(s8)lfoSpeedC;
                    s8 newModM = (s8)(((s32)track->mod * tri) >> 6);
                    if (newModM != track->modM) {
                        track->modM = newModM;
                        if (track->modT == 0)
                            track->flags |= MPT_FLG_PITCHG;
                        else
                            track->flags |= MPT_FLG_VOLCHG;
                    }
                }
            }
        }

        mplayInfo->clock++;
        if (activeBits == 0) {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            mplayInfo->ident = ID_NUMBER;
            return;
        }
        mplayInfo->status = activeBits;
        mplayInfo->tempoC -= 150;
    }

    int trackCount = mplayInfo->trackCount;
    if (trackCount > MAX_MUSICPLAYER_TRACKS)
        trackCount = MAX_MUSICPLAYER_TRACKS;
    struct MusicPlayerTrack *track = mplayInfo->tracks;

    for (int t = 0; t < trackCount; t++, track++) {
        if ((track->flags & MPT_FLG_EXIST) && (track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG))) {
            u8 savedFlags = track->flags;
            TrkVolPitSet(mplayInfo, track);

            struct SoundChannel *chan = track->chan;
            while (chan != NULL) {
                struct SoundChannel *next = (struct SoundChannel *)chan->nextChannelPointer;
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON)) {
                    ClearChain(chan);
                    chan = next;
                    continue;
                }

                u8 cgbType = chan->type & TONEDATA_TYPE_CGB;
                if (savedFlags & MPT_FLG_VOLCHG) {
                    ChnVolSetAsmInternal(chan, track);
                    if (cgbType != 0)
                        ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
                }
                if (savedFlags & MPT_FLG_PITCHG) {
                    s32 key = (s32)chan->key + (s8)track->keyM;
                    if (key < 0)
                        key = 0;
                    if (cgbType != 0 && soundInfo != NULL && soundInfo->MidiKeyToCgbFreq != NULL) {
                        struct CgbChannel *cgbChan = (struct CgbChannel *)chan;
                        cgbChan->frequency = soundInfo->MidiKeyToCgbFreq(cgbType, (u8)key, track->pitM);
                        cgbChan->modify |= CGB_CHANNEL_MO_PIT;
                    } else if (cgbType == 0 && chan->wav != NULL) {
                        chan->frequency = MidiKeyToFreq(chan->wav, (u8)key, track->pitM);
                    }
                }
                chan = next;
            }
            track->flags &= 0xF0;
        }
    }

    mplayInfo->ident = ID_NUMBER;
}

void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    if (soundInfo == NULL || soundInfo->ident != ID_NUMBER)
        return;

    soundInfo->ident++;

    if (soundInfo->MPlayMainHead != NULL)
        soundInfo->MPlayMainHead(soundInfo->musicPlayerHead);

    if (soundInfo->CgbSound != NULL)
        soundInfo->CgbSound();

    StepDirectSoundEnvelopes(soundInfo);

    soundInfo->ident = ID_NUMBER;
}

void ply_note(u32 note_cmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    if (soundInfo == NULL)
        return;

    track->gateTime = gClockTable[note_cmd];
    if (track->cmdPtr != NULL && *track->cmdPtr < 0x80) {
        track->key = *track->cmdPtr++;
        if (*track->cmdPtr < 0x80) {
            track->velocity = *track->cmdPtr++;
            if (*track->cmdPtr < 0x80) {
                track->gateTime = (u8)(track->gateTime + *track->cmdPtr++);
            }
        }
    }

    s32 rhythmPan = 0;
    const struct ToneData *resolvedTone = &track->tone;
    u8 key = track->key;

    if (track->tone.type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) {
        u8 splitIdx;
        if (track->tone.type & TONEDATA_TYPE_SPL) {
            const u8 *keySplitTable;
            memcpy(&keySplitTable, &track->tone.attack, sizeof(keySplitTable));
            if (keySplitTable == NULL)
                return;
            splitIdx = keySplitTable[track->key];
        } else {
            splitIdx = track->key;
        }
        const struct ToneData *subTones = (const struct ToneData *)track->tone.wav;
        if (subTones == NULL)
            return;
        resolvedTone = &subTones[splitIdx];
        if (resolvedTone->type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
            return;
        if (track->tone.type & TONEDATA_TYPE_RHY) {
            if (resolvedTone->pan_sweep & 0x80)
                rhythmPan = ((s32)resolvedTone->pan_sweep - TONEDATA_P_S_PAN) * 2;
            key = resolvedTone->key;
        }
    }

    u32 prio = (u32)track->priority + (u32)mplayInfo->priority;
    if (prio > 0xFFu)
        prio = 0xFFu;

    u8 cgbType = resolvedTone->type & TONEDATA_TYPE_CGB;
    struct SoundChannel *chan = NULL;

    if (cgbType != 0) {
        if (soundInfo->cgbChans == NULL)
            return;
        struct CgbChannel *cgbChan = &soundInfo->cgbChans[cgbType - 1];
        if ((cgbChan->statusFlags & SOUND_CHANNEL_SF_ON) && !(cgbChan->statusFlags & SOUND_CHANNEL_SF_STOP)) {
            if (cgbChan->priority > prio)
                return;
            if (cgbChan->priority == prio && cgbChan->track < track)
                return;
        }
        chan = (struct SoundChannel *)cgbChan;
    } else {
        u32 bestPrio = prio;
        struct MusicPlayerTrack *bestTrack = track;
        bool foundStopped = false;
        int maxChans = soundInfo->maxChans;
        if (maxChans > MAX_DIRECTSOUND_CHANNELS)
            maxChans = MAX_DIRECTSOUND_CHANNELS;

        for (int i = 0; i < maxChans; i++) {
            struct SoundChannel *cand = &soundInfo->chans[i];
            if (!(cand->statusFlags & SOUND_CHANNEL_SF_ON)) {
                chan = cand;
                break;
            }
            if (cand->statusFlags & SOUND_CHANNEL_SF_STOP) {
                if (!foundStopped) {
                    foundStopped = true;
                    bestPrio = cand->priority;
                    bestTrack = cand->track;
                    chan = cand;
                    continue;
                }
            } else if (foundStopped) {
                continue;
            }

            if (cand->priority < bestPrio) {
                bestPrio = cand->priority;
                bestTrack = cand->track;
                chan = cand;
            } else if (cand->priority == bestPrio && cand->track >= bestTrack) {
                bestTrack = cand->track;
                chan = cand;
            }
        }
        if (chan == NULL)
            return;
    }

    ClearChain(chan);

    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL)
        track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        ClearModMInternal(track);

    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = (u8)prio;
    chan->key = key;
    chan->rhythmPan = (u8)rhythmPan;
    chan->type = resolvedTone->type;
    chan->wav = resolvedTone->wav;
    chan->attack = resolvedTone->attack;
    chan->decay = resolvedTone->decay;
    chan->sustain = resolvedTone->sustain;
    chan->release = resolvedTone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;

    ChnVolSetAsmInternal(chan, track);

    s32 midiKey = (s32)chan->key + (s8)track->keyM;
    if (midiKey < 0)
        midiKey = 0;

    if (cgbType != 0) {
        struct CgbChannel *cgbChan = (struct CgbChannel *)chan;
        cgbChan->length = resolvedTone->length;
        u8 sweep = resolvedTone->pan_sweep;
        if ((sweep & 0x80) || (sweep & 0x70) == 0)
            sweep = 0x08;
        cgbChan->sweep = sweep;
        cgbChan->frequency = soundInfo->MidiKeyToCgbFreq(cgbType, (u8)midiKey, track->pitM);
    } else {
        chan->count = track->unk_3C;
        if (chan->wav != NULL)
            chan->frequency = MidiKeyToFreq(chan->wav, (u8)midiKey, track->pitM);
    }

    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    struct SoundChannel *chan = track->chan;
    while (chan != NULL) {
        struct SoundChannel *next = (struct SoundChannel *)chan->nextChannelPointer;
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
        chan = next;
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 *dest = NULL;
    memcpy(&dest, track->cmdPtr, sizeof(dest));
    track->cmdPtr = dest;
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->patternLevel >= 3) {
        ply_fine(mplayInfo, track);
        return;
    }
    track->patternStack[track->patternLevel++] = track->cmdPtr + 4;
    ply_goto(mplayInfo, track);
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    if (track->patternLevel == 0)
        return;
    track->cmdPtr = track->patternStack[--track->patternLevel];
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 count = *track->cmdPtr;
    if (count == 0) {
        track->cmdPtr += 1;
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN++;
    if (track->repN < count) {
        track->cmdPtr += 1;
        ply_goto(mplayInfo, track);
    } else {
        track->repN = 0;
        track->cmdPtr += 5;
    }
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->priority = *track->cmdPtr++;
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u16 tempo = (u16)(*track->cmdPtr++) << 1;
    mplayInfo->tempoD = tempo;
    mplayInfo->tempoI = (tempo * mplayInfo->tempoU) >> 8;
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->keyShift = (s8)*track->cmdPtr++;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 voice = *track->cmdPtr++;
    if (mplayInfo->tone != NULL)
        track->tone = mplayInfo->tone[voice];
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->vol = *track->cmdPtr++;
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->pan = (s8)((s32)*track->cmdPtr++ - C_V);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bend = (s8)((s32)*track->cmdPtr++ - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bendRange = *track->cmdPtr++;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoSpeed = *track->cmdPtr++;
    if (track->lfoSpeed == 0)
        ClearModMInternal(track);
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoDelay = *track->cmdPtr++;
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->mod = *track->cmdPtr++;
    if (track->mod == 0)
        ClearModMInternal(track);
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 modt = *track->cmdPtr++;
    if (track->modT != modt) {
        track->modT = modt;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->tune = (s8)((s32)*track->cmdPtr++ - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 regOff = *track->cmdPtr++;
    u8 val = *track->cmdPtr++;
    if (regOff < 0x50)
        *((vu8 *)REG_ADDR_SOUND1CNT_L + regOff) = val;
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 key;
    if (*track->cmdPtr < 0x80) {
        key = *track->cmdPtr++;
        track->key = key;
    } else {
        key = track->key;
    }

    struct SoundChannel *chan = track->chan;
    while (chan != NULL) {
        if ((chan->statusFlags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV))
            && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP)
            && chan->midiKey == key) {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
            break;
        }
        chan = (struct SoundChannel *)chan->nextChannelPointer;
    }
}

/* =========================================================================
 * Real-Time DirectSound + CGB Frame Mixer (32768 Hz Stereo)
 * ========================================================================= */
void WasmAudio_MixFrame(void)
{
    int16_t frameBuf[WASM_AUDIO_FRAME_SAMPLES * 2];
    int32_t mixL[WASM_AUDIO_FRAME_SAMPLES];
    int32_t mixR[WASM_AUDIO_FRAME_SAMPLES];

    memset(mixL, 0, sizeof(mixL));
    memset(mixR, 0, sizeof(mixR));

    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    if (soundInfo != NULL && soundInfo->ident == ID_NUMBER) {
        /* 1. DirectSound channels */
        int maxChans = soundInfo->maxChans;
        if (maxChans > MAX_DIRECTSOUND_CHANNELS)
            maxChans = MAX_DIRECTSOUND_CHANNELS;

        double fixStep = (double)(soundInfo->pcmFreq > 0 ? soundInfo->pcmFreq : 13379) / (double)WASM_AUDIO_SAMPLE_RATE;

        for (int ch = 0; ch < maxChans; ch++) {
            struct SoundChannel *chan = &soundInfo->chans[ch];
            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON) || (chan->statusFlags & SOUND_CHANNEL_SF_START))
                continue;

            const struct WaveData *wav = chan->wav;
            if (wav == NULL || wav->size == 0) {
                chan->statusFlags = 0;
                continue;
            }

            s32 volL = chan->envelopeVolumeLeft;
            s32 volR = chan->envelopeVolumeRight;
            bool isRev = (chan->type & TONEDATA_TYPE_REV) != 0;
            bool isLoop = (chan->statusFlags & SOUND_CHANNEL_SF_LOOP) != 0 && wav->loopStart < wav->size;
            double step = (chan->type & TONEDATA_TYPE_FIX)
                        ? fixStep
                        : ((double)chan->frequency / (double)WASM_AUDIO_SAMPLE_RATE);
            if (step <= 0.0)
                step = fixStep;

            double pos = sDsChanState[ch].pos;
            uint32_t size = wav->size;
            uint32_t loopLen = isLoop ? (size - wav->loopStart) : 0;

            for (uint32_t s = 0; s < WASM_AUDIO_FRAME_SAMPLES; s++) {
                if (!isRev && pos >= (double)size) {
                    if (isLoop && loopLen > 0) {
                        pos = (double)wav->loopStart + fmod(pos - (double)wav->loopStart, (double)loopLen);
                    } else {
                        chan->statusFlags = 0;
                        break;
                    }
                } else if (isRev && pos < 0.0) {
                    chan->statusFlags = 0;
                    break;
                }

                if (volL != 0 || volR != 0) {
                    uint32_t idx0 = (uint32_t)pos;
                    if (idx0 >= size)
                        idx0 = size - 1u;
                    uint32_t idx1;
                    if (!isRev) {
                        idx1 = idx0 + 1u;
                        if (idx1 >= size)
                            idx1 = (isLoop && loopLen > 0) ? wav->loopStart : idx0;
                    } else {
                        idx1 = (idx0 > 0) ? (idx0 - 1u) : 0u;
                    }

                    double frac = pos - (double)idx0;
                    s32 smp0 = ReadWaveSample(ch, wav, idx0);
                    s32 smp1 = ReadWaveSample(ch, wav, idx1);
                    s32 smp = smp0 + (s32)((double)(smp1 - smp0) * frac);

                    /* Scale 8-bit sample * 8-bit vol to 16-bit headroom (~4 full-volume channels before clip) */
                    mixL[s] += (smp * volL) >> 1;
                    mixR[s] += (smp * volR) >> 1;
                }

                pos += isRev ? -step : step;
            }
            sDsChanState[ch].pos = pos;
        }

        /* 2. CGB channels (Square 1, Square 2, Wave 3, Noise 4) */
        if (soundInfo->cgbChans != NULL && (REG_SOUNDCNT_X & SOUND_MASTER_ENABLE)) {
            static const double kDutyFrac[4] = {0.125, 0.25, 0.5, 0.75};
            u16 soundCntH = REG_SOUNDCNT_H;
            int cgbShift = (soundCntH & 3u) == 0 ? 2 : ((soundCntH & 3u) == 1 ? 1 : 0);
            u8 nr51 = REG_NR51;

            /* Channels 1 & 2: Pulse */
            for (int c = 0; c < 2; c++) {
                struct CgbChannel *cgb = &soundInfo->cgbChans[c];
                if (!(cgb->statusFlags & SOUND_CHANNEL_SF_ON) || cgb->envelopeVolume == 0)
                    continue;

                uint32_t rawFreq = cgb->frequency & 0x7FFu;
                if (rawFreq >= 2048u)
                    continue;
                double freqHz = 131072.0 / (double)(2048u - rawFreq);
                double phaseStep = freqHz / (double)WASM_AUDIO_SAMPLE_RATE;
                u8 dutyIdx = ((c == 0 ? REG_NR11 : REG_NR21) >> 6) & 3u;
                double duty = kDutyFrac[dutyIdx];
                s32 amp = (((s32)cgb->envelopeVolume * 1400) >> cgbShift);
                bool enR = (nr51 & (1u << c)) != 0;
                bool enL = (nr51 & (1u << (c + 4))) != 0;
                if (!enL && !enR)
                    continue;

                double ph = sCgbPhase[c];
                for (uint32_t s = 0; s < WASM_AUDIO_FRAME_SAMPLES; s++) {
                    s32 smp = (ph < duty) ? amp : -amp;
                    if (enL) mixL[s] += smp;
                    if (enR) mixR[s] += smp;
                    ph += phaseStep;
                    if (ph >= 1.0)
                        ph -= floor(ph);
                }
                sCgbPhase[c] = ph;
            }

            /* Channel 3: 4-bit 32-sample Wave RAM */
            {
                struct CgbChannel *cgb = &soundInfo->cgbChans[2];
                if ((cgb->statusFlags & SOUND_CHANNEL_SF_ON) && (REG_NR30 & 0x80)) {
                    uint32_t rawFreq = cgb->frequency & 0x7FFu;
                    u8 nr32 = REG_NR32 & 0xE0u;
                    s32 waveGainNum = 0;
                    if (nr32 == 0x20u) waveGainNum = 4;
                    else if (nr32 == 0x80u) waveGainNum = 3;
                    else if (nr32 == 0x40u) waveGainNum = 2;
                    else if (nr32 == 0x60u) waveGainNum = 1;

                    if (rawFreq < 2048u && waveGainNum > 0) {
                        bool enR = (nr51 & (1u << 2)) != 0;
                        bool enL = (nr51 & (1u << 6)) != 0;
                        if (enL || enR) {
                            u32 waveWords[4] = {
                                REG_WAVE_RAM0,
                                REG_WAVE_RAM1,
                                REG_WAVE_RAM2,
                                REG_WAVE_RAM3
                            };
                            const u8 *waveBytes = (const u8 *)waveWords;
                            double freqHz = 65536.0 / (double)(2048u - rawFreq);
                            double phaseStep = freqHz / (double)WASM_AUDIO_SAMPLE_RATE;
                            double ph = sCgbPhase[2];

                            for (uint32_t s = 0; s < WASM_AUDIO_FRAME_SAMPLES; s++) {
                                uint32_t stepIdx = (uint32_t)(ph * 32.0) & 31u;
                                u8 byte = waveBytes[stepIdx >> 1];
                                s32 nib = (stepIdx & 1u) ? (byte & 0x0F) : (byte >> 4);
                                s32 centered = nib * 2 - 15; /* -15..+15 */
                                s32 smp = ((centered * 280 * waveGainNum) >> cgbShift);
                                if (enL) mixL[s] += smp;
                                if (enR) mixR[s] += smp;
                                ph += phaseStep;
                                if (ph >= 1.0)
                                    ph -= floor(ph);
                            }
                            sCgbPhase[2] = ph;
                        }
                    }
                }
            }

            /* Channel 4: LFSR Noise */
            {
                struct CgbChannel *cgb = &soundInfo->cgbChans[3];
                if ((cgb->statusFlags & SOUND_CHANNEL_SF_ON) && cgb->envelopeVolume > 0) {
                    u8 nr43 = REG_NR43;
                    u32 shift = nr43 >> 4;
                    bool width7 = (nr43 & 0x08) != 0;
                    u32 divCode = nr43 & 0x07u;
                    bool enR = (nr51 & (1u << 3)) != 0;
                    bool enL = (nr51 & (1u << 7)) != 0;
                    if (shift < 14 && (enL || enR)) {
                        double divVal = (divCode == 0) ? 0.5 : (double)divCode;
                        double clockHz = 524288.0 / (divVal * (double)(1u << (shift + 1u)));
                        double stepTicks = clockHz / (double)WASM_AUDIO_SAMPLE_RATE;
                        if (stepTicks > 16.0)
                            stepTicks = 16.0;
                        s32 amp = (((s32)cgb->envelopeVolume * 1100) >> cgbShift);

                        for (uint32_t s = 0; s < WASM_AUDIO_FRAME_SAMPLES; s++) {
                            sCgbNoiseAccum += stepTicks;
                            while (sCgbNoiseAccum >= 1.0) {
                                sCgbNoiseAccum -= 1.0;
                                u16 bit = (sCgbLfsr ^ (sCgbLfsr >> 1)) & 1u;
                                sCgbLfsr = (u16)((sCgbLfsr >> 1) | (bit << 14));
                                if (width7)
                                    sCgbLfsr = (u16)((sCgbLfsr & ~(1u << 6)) | (bit << 6));
                            }
                            s32 smp = (sCgbLfsr & 1u) ? -amp : amp;
                            if (enL) mixL[s] += smp;
                            if (enR) mixR[s] += smp;
                        }
                    }
                }
            }
        }
    }

    for (uint32_t i = 0; i < WASM_AUDIO_FRAME_SAMPLES; i++) {
        int32_t l = mixL[i];
        int32_t r = mixR[i];
        if (l > 32767) l = 32767;
        else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        else if (r < -32768) r = -32768;
        frameBuf[i * 2u] = (int16_t)l;
        frameBuf[i * 2u + 1u] = (int16_t)r;
    }

    PushSamples(frameBuf, WASM_AUDIO_FRAME_SAMPLES);
}

#endif
