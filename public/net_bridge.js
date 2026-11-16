/* ============================================================================
 * Pokémon Emerald Web Multiplayer — Binary Relay Bridge, Host Authority & IDB
 * Protocol matches server.js / cloudflare_worker.js (PokemonRelayHub):
 *   Frame: [frameType(1B), peerId(2B BE), channel(1B), ...payload]
 *   Channels: 0=Control, 1=Overworld, 2=WorldSync, 3=Battle2v2, 4=SoulLink, 5=Trade
 * ============================================================================ */
(function (global) {
  "use strict";

  const FRAME_DATA = 1;
  const FRAME_PEER_CONNECTED = 2;
  const FRAME_PEER_DISCONNECTED = 3;
  const FRAME_KICK_PEER = 4;
  const FRAME_ROOM_INFO = 5;
  const FRAME_HOST_ASSIGNED = 6;
  const FRAME_REJECTED = 7;

  const CH_CONTROL = 0;
  const CH_OVERWORLD = 1;
  const CH_WORLD_SYNC = 2;
  const CH_BATTLE = 3;
  const CH_SOUL_LINK = 4;
  const CH_TRADE = 5;

  const STORAGE_KEYS = {
    playerName: "pokemon_web_player_name",
    clientToken: "pokemon_web_client_token",
    sessionToken: "pokemon_web_session_token",
    relayUrl: "pokemon_web_relay_url",
    keybinds: "pokemon_web_keybinds",
    audioMuted: "pokemon_web_audio_muted",
    runsFallback: "pokemon_web_runs_fallback_v1",
    flashSave: "pokemon_web_flash_save_v1",
    flashSaveMeta: "pokemon_web_flash_save_meta_v1",
  };

  function computePartyPairTag(partyId) {
    if (!partyId || typeof partyId !== "string") return 0;
    if (partyId === "CO-OP 1+2" || partyId === "CO-OP 2+1") return 3;
    if (partyId === "CO-OP 1+3" || partyId === "CO-OP 3+1") return 4;
    if (partyId === "CO-OP 1+4" || partyId === "CO-OP 4+1") return 5;
    if (partyId === "CO-OP 2+3" || partyId === "CO-OP 3+2") return 6;
    if (partyId === "CO-OP 2+4" || partyId === "CO-OP 4+2") return 7;
    if (partyId === "CO-OP 3+4" || partyId === "CO-OP 4+3") return 8;
    let hash = 0;
    for (let i = 0; i < partyId.length; i++) {
      hash = (hash * 31 + partyId.charCodeAt(i)) >>> 0;
    }
    return 9 + (hash % 6);
  }

  function decodeFlashBase64(b64) {
    if (!b64 || typeof b64 !== "string") return null;
    try {
      const bin = atob(b64);
      const len = bin.length;
      if (len < 4096) return null;
      const out = new Uint8Array(131072);
      out.fill(0xff);
      const copyLen = Math.min(out.length, len);
      for (let i = 0; i < copyLen; i++) {
        out[i] = bin.charCodeAt(i) & 0xff;
      }
      return out;
    } catch (_) {
      return null;
    }
  }

  function encodeFlashBase64(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length === 0) return "";
    const parts = [];
    for (let i = 0; i < bytes.length; i += 8192) {
      parts.push(String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + 8192, bytes.length))));
    }
    return btoa(parts.join(""));
  }

  function hasValidSectorSignature(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 4096) return false;
    for (let off = 0; off + 4096 <= bytes.length; off += 4096) {
      // SECTOR_SIGNATURE (0x08012025) little-endian at offset + 0xFF8 (offset 4088 inside sector footer)
      if (
        bytes[off + 0xff8] === 0x25 &&
        bytes[off + 0xff9] === 0x20 &&
        bytes[off + 0xffa] === 0x01 &&
        bytes[off + 0xffb] === 0x08
      ) {
        return true;
      }
    }
    return false;
  }

  function hasValidWasmFlashSave() {
    try {
      const b64 = localStorage.getItem(STORAGE_KEYS.flashSave);
      const bytes = decodeFlashBase64(b64);
      return hasValidSectorSignature(bytes);
    } catch (_) {
      return false;
    }
  }

  function exportWasmFlashBase64() {
    try {
      const m = global.PokemonWasmBridge && global.PokemonWasmBridge._getWasmModule
        ? global.PokemonWasmBridge._getWasmModule()
        : null;
      if (m && typeof m._wasm_get_flash_ptr === "function" && m.HEAPU8) {
        const ptr = m._wasm_get_flash_ptr();
        if (ptr) {
          const bytes = m.HEAPU8.slice(ptr, ptr + 131072);
          if (hasValidSectorSignature(bytes)) {
            const b64 = encodeFlashBase64(bytes);
            try {
              localStorage.setItem(STORAGE_KEYS.flashSave, b64);
            } catch (_) {}
            return b64;
          }
        }
      }
      const b64 = localStorage.getItem(STORAGE_KEYS.flashSave) || "";
      const bytes = decodeFlashBase64(b64);
      return hasValidSectorSignature(bytes) ? b64 : "";
    } catch (_) {
      return "";
    }
  }

  function importWasmFlashBase64(b64) {
    const bytes = decodeFlashBase64(b64);
    if (!bytes) return false;
    try {
      const cleanB64 = encodeFlashBase64(bytes);
      localStorage.setItem(STORAGE_KEYS.flashSave, cleanB64);
      const m = global.PokemonWasmBridge && global.PokemonWasmBridge._getWasmModule
        ? global.PokemonWasmBridge._getWasmModule()
        : null;
      if (m && typeof m._wasm_get_flash_ptr === "function" && m.HEAPU8) {
        const ptr = m._wasm_get_flash_ptr();
        if (ptr) m.HEAPU8.set(bytes, ptr);
      }
      return true;
    } catch (_) {
      return false;
    }
  }

  function readFlashSaveMeta() {
    try {
      const raw = localStorage.getItem(STORAGE_KEYS.flashSaveMeta);
      const parsed = raw ? JSON.parse(raw) : null;
      if (parsed && typeof parsed === "object") {
        return {
          runId: typeof parsed.runId === "string" ? parsed.runId : "",
          savedAtMs: Number(parsed.savedAtMs) || 0,
          userId: typeof parsed.userId === "string" ? parsed.userId : "guest",
          slotIndex: Number.isInteger(Number(parsed.slotIndex)) ? Number(parsed.slotIndex) : 0,
          flashRev: Math.max(0, Math.floor(Number(parsed.flashRev) || 0)),
        };
      }
    } catch (_) {}
    return { runId: "", savedAtMs: 0, userId: "guest", slotIndex: 0, flashRev: 0 };
  }

  function writeFlashSaveMeta(runId, savedAtMs, extra) {
    try {
      const prev = readFlashSaveMeta();
      const ex = extra && typeof extra === "object" ? extra : {};
      localStorage.setItem(
        STORAGE_KEYS.flashSaveMeta,
        JSON.stringify({
          runId: typeof runId === "string" ? runId : prev.runId,
          savedAtMs: Number(savedAtMs) || Date.now(),
          userId: typeof ex.userId === "string" && ex.userId ? ex.userId : prev.userId || "guest",
          slotIndex:
            ex.slotIndex !== undefined && Number.isInteger(Number(ex.slotIndex))
              ? Number(ex.slotIndex)
              : prev.slotIndex || 0,
          flashRev:
            ex.flashRev !== undefined && Number.isFinite(Number(ex.flashRev))
              ? Math.max(0, Math.floor(Number(ex.flashRev)))
              : prev.flashRev || 0,
        })
      );
    } catch (_) {}
  }

  function clearLocalFlashSave(options) {
    const keepMeta = Boolean(options && options.keepMeta);
    try {
      localStorage.removeItem(STORAGE_KEYS.flashSave);
      if (!keepMeta) {
        localStorage.removeItem(STORAGE_KEYS.flashSaveMeta);
      }
    } catch (_) {}
    try {
      const m = global.PokemonWasmBridge && global.PokemonWasmBridge._getWasmModule
        ? global.PokemonWasmBridge._getWasmModule()
        : null;
      if (m && typeof m._wasm_get_flash_ptr === "function" && m.HEAPU8) {
        const ptr = m._wasm_get_flash_ptr();
        if (ptr) m.HEAPU8.fill(0xff, ptr, ptr + 131072);
      }
    } catch (_) {}
  }

  const ASSET_DB_NAME = "pokemon_asset_cache_db_v1";
  const ASSET_STORE_NAME = "asset_packs";
  const RUN_DB_NAME = "pokemon_campaign_runs_db_v1";
  const RUN_STORE_NAME = "campaign_runs";
  const FLASH_CACHE_DB_NAME = "pokemon_slot_flash_cache_v1";
  const FLASH_CACHE_STORE_NAME = "slot_flash";

  const TRADE_EVOLUTIONS = {
    Kadabra: "Alakazam",
    Machoke: "Machamp",
    Graveler: "Golem",
    Haunter: "Gengar",
    Poliwhirl: "Politoed",
    Slowpoke: "Slowking",
    Onix: "Steelix",
    Scyther: "Scizor",
    Seadra: "Kingdra",
    Porygon: "Porygon2",
    Clamperl: "Huntail",
    Dusclops: "Dusknoir",
    Rhydon: "Rhyperior",
    Electabuzz: "Electivire",
    Magmar: "Magmortar",
  };

  const RANDOMIZER_PRESETS = {
    off: {
      wildEncounters: false,
      startersAndStatics: false,
      trainers2v2: false,
      itemsAndMarts: false,
      abilities: false,
      banWonderGuard: true,
      movesets: false,
      types: false,
      evolutions: false,
      preserveGymThemes: true,
      similarBst: true,
    },
    balanced: {
      wildEncounters: true,
      startersAndStatics: true,
      trainers2v2: true,
      itemsAndMarts: false,
      abilities: false,
      banWonderGuard: true,
      movesets: false,
      types: false,
      evolutions: false,
      preserveGymThemes: true,
      similarBst: true,
    },
    classic: {
      wildEncounters: true,
      startersAndStatics: true,
      trainers2v2: true,
      itemsAndMarts: true,
      abilities: true,
      banWonderGuard: true,
      movesets: false,
      types: false,
      evolutions: false,
      preserveGymThemes: false,
      similarBst: false,
    },
    chaos: {
      wildEncounters: true,
      startersAndStatics: true,
      trainers2v2: true,
      itemsAndMarts: true,
      abilities: true,
      banWonderGuard: true,
      movesets: true,
      types: true,
      evolutions: true,
      preserveGymThemes: false,
      similarBst: false,
    },
  };

  const NUZLOCKE_PRESETS = {
    standard_soullink: {
      enabled: true,
      dupesClause: true,
      shinyClause: true,
      levelCaps: false,
      setMode: false,
      noBattleItems: false,
      whiteoutGameOver: false,
    },
    hardcore: {
      enabled: true,
      dupesClause: true,
      shinyClause: true,
      levelCaps: true,
      setMode: true,
      noBattleItems: true,
      whiteoutGameOver: true,
    },
    casual_coop: {
      enabled: false,
      dupesClause: false,
      shinyClause: true,
      levelCaps: false,
      setMode: false,
      noBattleItems: false,
      whiteoutGameOver: false,
    },
  };

  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder();

  function safeParseJson(str) {
    try {
      return JSON.parse(str);
    } catch (_) {
      return null;
    }
  }

  function cloneData(obj) {
    return JSON.parse(JSON.stringify(obj));
  }

  function generateId(prefix) {
    const rand = Math.floor(Math.random() * 0xffffffff)
      .toString(16)
      .padStart(8, "0");
    return `${prefix}_${Date.now().toString(36)}_${rand}`;
  }

  function normalizeSeedString(raw) {
    return String(raw || "")
      .replace(/[^\x20-\x7e]/g, "")
      .trim()
      .slice(0, 31);
  }

  function randomSeedString() {
    const words = ["RAYQUAZA", "KYOGRE", "GROUDON", "HOENN", "EMERALD", "SOOTOPOLIS", "METEOR", "LATIOS"];
    const pick = words[Math.floor(Math.random() * words.length)];
    const num = Math.floor(1000 + Math.random() * 8999);
    return `${pick}-${num}`.slice(0, 31);
  }

  function normalizeRunConfig(cfg, fallbackCfg) {
    const base = fallbackCfg || makeDefaultRunConfig();
    const src = cfg && typeof cfg === "object" ? cfg : {};
    const presetKey =
      typeof src.randomizerPreset === "string" && RANDOMIZER_PRESETS[src.randomizerPreset]
        ? src.randomizerPreset
        : typeof base.randomizerPreset === "string" && RANDOMIZER_PRESETS[base.randomizerPreset]
          ? base.randomizerPreset
          : "balanced";
    const presetToggles = RANDOMIZER_PRESETS[presetKey] || RANDOMIZER_PRESETS.balanced;
    const rawToggles = src.randomizerToggles && typeof src.randomizerToggles === "object" ? src.randomizerToggles : {};
    const normalizedToggles = {};
    for (const k of Object.keys(RANDOMIZER_PRESETS.balanced)) {
      normalizedToggles[k] = k in rawToggles ? Boolean(rawToggles[k]) : Boolean(presetToggles[k]);
    }
    const nuzPresetKey =
      typeof src.nuzlockePreset === "string" && NUZLOCKE_PRESETS[src.nuzlockePreset]
        ? src.nuzlockePreset
        : typeof base.nuzlockePreset === "string" && NUZLOCKE_PRESETS[base.nuzlockePreset]
          ? base.nuzlockePreset
          : "standard_soullink";
    const nuzPresetToggles = NUZLOCKE_PRESETS[nuzPresetKey] || NUZLOCKE_PRESETS.standard_soullink;
    const rawNuzToggles = src.nuzlockeToggles && typeof src.nuzlockeToggles === "object" ? src.nuzlockeToggles : {};
    const normalizedNuzToggles = {};
    for (const k of Object.keys(NUZLOCKE_PRESETS.standard_soullink)) {
      normalizedNuzToggles[k] = k in rawNuzToggles ? Boolean(rawNuzToggles[k]) : Boolean(nuzPresetToggles[k]);
    }
    const cleanSeed = normalizeSeedString(src.seed) || normalizeSeedString(base.seed) || randomSeedString();
    return {
      ...base,
      ...src,
      runId: String(src.runId || base.runId || generateId("run")).slice(0, 64),
      roomName: String(src.roomName || base.roomName || "Hoenn Run").slice(0, 48),
      seed: cleanSeed,
      randomizerPreset: presetKey,
      randomizerToggles: normalizedToggles,
      nuzlockePreset: nuzPresetKey,
      nuzlockeToggles: normalizedNuzToggles,
      soulLinkMode: src.soulLinkMode === "group" ? "group" : "pairs",
      battleAutoPull: Boolean(src.battleAutoPull),
      speedMultiplier: [1, 2, 3].includes(Number(src.speedMultiplier)) ? Number(src.speedMultiplier) : 1,
      gameOver: Boolean(src.gameOver),
    };
  }

  function getOrCreateClientToken() {
    let tok = localStorage.getItem(STORAGE_KEYS.clientToken);
    if (!tok) {
      tok = generateId("tr");
      localStorage.setItem(STORAGE_KEYS.clientToken, tok);
    }
    return tok;
  }

  function getOrCreatePlayerName() {
    if (
      typeof client !== "undefined" &&
      client &&
      client.state &&
      client.state.auth &&
      client.state.auth.status === "signed_in" &&
      client.state.auth.username
    ) {
      const authName = String(client.state.auth.username).trim().slice(0, 20);
      if (authName) {
        try {
          localStorage.setItem(STORAGE_KEYS.playerName, authName);
        } catch (_) {}
        return authName;
      }
    }
    let name = "";
    try {
      name = localStorage.getItem(STORAGE_KEYS.playerName) || "";
    } catch (_) {}
    return name || "Trainer";
  }

  // --- Relay URL Configuration (Compatible with brotatoweb & /relay_config.js) ---
  function applyRelayConfig(rawUrl) {
    let clean = String(rawUrl || "")
      .trim()
      .replace(/\/+$/, "");
    if (clean.endsWith("/ws")) clean = clean.slice(0, -3);
    global.POKEMON_RELAY_URL = clean;
    if (!clean) {
      const proto = global.location && global.location.protocol === "https:" ? "wss://" : "ws://";
      const host = global.location ? global.location.host : "localhost:8080";
      global.POKEMON_WS_URL = proto + host + "/ws";
      return;
    }
    if (clean.startsWith("wss://") || clean.startsWith("ws://")) {
      global.POKEMON_WS_URL = clean + "/ws";
    } else if (clean.startsWith("https://")) {
      global.POKEMON_WS_URL = "wss://" + clean.slice(8) + "/ws";
    } else if (clean.startsWith("http://")) {
      global.POKEMON_WS_URL = "ws://" + clean.slice(7) + "/ws";
    } else {
      global.POKEMON_WS_URL = "wss://" + clean + "/ws";
    }
  }

  function getRelayHttpBase() {
    const clean = global.POKEMON_RELAY_URL || "";
    if (!clean) return "";
    if (clean.startsWith("https://") || clean.startsWith("http://")) return clean;
    if (clean.startsWith("wss://")) return "https://" + clean.slice(6);
    if (clean.startsWith("ws://")) return "http://" + clean.slice(5);
    return "https://" + clean;
  }

  function initRelayFromEnvironment() {
    if (typeof global.location !== "undefined" && global.location.search) {
      const params = new URLSearchParams(global.location.search);
      const relayOverride = params.get("relay");
      if (relayOverride !== null && relayOverride.trim()) {
        localStorage.setItem(STORAGE_KEYS.relayUrl, relayOverride.trim());
      }
    }
    const stored = typeof localStorage !== "undefined" ? localStorage.getItem(STORAGE_KEYS.relayUrl) : "";
    const defaultRelay = stored || global.POKEMON_DEFAULT_RELAY || "";
    applyRelayConfig(defaultRelay);
  }

  // --- 4-Byte Binary Framing ---
  function encodeFrame(type, peerId, channel, payloadBytes) {
    const payload = payloadBytes ? new Uint8Array(payloadBytes) : new Uint8Array(0);
    const out = new Uint8Array(4 + payload.byteLength);
    out[0] = type & 0xff;
    out[1] = (peerId >> 8) & 0xff;
    out[2] = peerId & 0xff;
    out[3] = channel & 0xff;
    if (payload.byteLength > 0) out.set(payload, 4);
    return out;
  }

  function decodeFrame(buffer) {
    const buf = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
    if (buf.byteLength < 4) return null;
    return {
      frameType: buf[0],
      peerId: (buf[1] << 8) | buf[2],
      channel: buf[3],
      payload: buf.subarray(4),
    };
  }

  function encodeJsonPayload(obj) {
    return textEncoder.encode(JSON.stringify(obj));
  }

  function tryDecodeJsonPayload(payloadBytes) {
    if (!payloadBytes || payloadBytes.byteLength === 0) return null;
    // 0x7b is '{'
    if (payloadBytes[0] !== 0x7b) return null;
    return safeParseJson(textDecoder.decode(payloadBytes));
  }

  function toNumericCutsceneId(rawId) {
    if (typeof rawId === "number" && Number.isFinite(rawId)) {
      const n = Math.floor(rawId);
      return n > 0 ? n % 512 : 0;
    }
    const str = String(rawId || "").trim();
    if (/^\d+$/.test(str)) {
      const n = parseInt(str, 10);
      return n > 0 ? n % 512 : 0;
    }
    if (!str) return 0;
    let hash = 14695981039346656037n;
    const prime = 1099511628211n;
    const mask = 0xffffffffffffffffn;
    const bytes = textEncoder.encode(str);
    for (let i = 0; i < bytes.length; i++) {
      hash ^= BigInt(bytes[i]);
      hash = (hash * prime) & mask;
    }
    return Number((hash % 511n) + 1n);
  }

  function buildWasmJsonBytes(msg, rawBytes) {
    if (!msg || typeof msg.t !== "string") return rawBytes || null;
    if (
      msg.t === "cutscene_trigger" ||
      msg.t === "cutscene_start" ||
      msg.t === "cutscene_update"
    ) {
      const rawCut = msg.cutsceneId ?? (msg.cutscene && msg.cutscene.cutsceneId);
      const numericId = toNumericCutsceneId(rawCut);
      const rawTitle = msg.title || (msg.cutscene && msg.cutscene.title) || "";
      if (typeof msg.cutsceneId === "number" && rawBytes && rawBytes.byteLength <= 900) {
        return rawBytes;
      }
      return encodeJsonPayload({
        t: msg.t,
        cutsceneId: numericId,
        title: String(rawTitle).slice(0, 31),
      });
    }
    if (msg.t === "cutscene_end") {
      const rawDone = msg.completedId ?? msg.cutsceneId ?? msg.completedKey;
      const numericId = toNumericCutsceneId(rawDone);
      if (typeof msg.completedId === "number" && rawBytes && rawBytes.byteLength <= 900) {
        return rawBytes;
      }
      return encodeJsonPayload({
        t: "cutscene_end",
        completedId: numericId,
        title: String(msg.title || "").slice(0, 31),
      });
    }
    if (msg.t === "world_sync") {
      const worldObj = msg.world && typeof msg.world === "object" ? msg.world : msg;
      const badges = Array.isArray(msg.badges)
        ? msg.badges
        : Array.isArray(worldObj.badges)
          ? worldObj.badges
          : null;
      const rawFlags = worldObj.globalFlags && typeof worldObj.globalFlags === "object" ? worldObj.globalFlags : {};
      const rawVars = worldObj.globalVars && typeof worldObj.globalVars === "object" ? worldObj.globalVars : {};
      const flags = [];
      for (const [k, v] of Object.entries(rawFlags)) {
        const id = Number(k);
        if (Number.isInteger(id) && id > 0 && v) flags.push([id, 1]);
      }
      const vars = [];
      for (const [k, v] of Object.entries(rawVars)) {
        const id = Number(k);
        const val = Number(v);
        if (Number.isInteger(id) && id > 0 && Number.isFinite(val) && val > 0) {
          vars.push([id, Math.floor(val)]);
        }
      }
      if (badges || flags.length > 0 || vars.length > 0) {
        const out = { t: "world_sync" };
        if (badges) out.badges = badges.slice(0, 8).map(Boolean);
        if (flags.length > 0) out.flags = flags;
        if (vars.length > 0) out.vars = vars;
        let bytes = encodeJsonPayload(out);
        while (bytes.byteLength > 1900 && (out.flags && out.flags.length > 0 || out.vars && out.vars.length > 0)) {
          if (out.vars && out.vars.length > 0) out.vars.pop();
          else if (out.flags && out.flags.length > 0) out.flags.pop();
          bytes = encodeJsonPayload(out);
        }
        return bytes;
      }
    }
    return rawBytes || encodeJsonPayload(msg);
  }

  // --- IndexedDB Helpers ---
  function openIdb(dbName, storeName) {
    return new Promise((resolve) => {
      if (typeof indexedDB === "undefined") {
        resolve(null);
        return;
      }
      const req = indexedDB.open(dbName, 1);
      req.onupgradeneeded = (ev) => {
        const db = ev.target.result;
        if (!db.objectStoreNames.contains(storeName)) {
          db.createObjectStore(storeName);
        }
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => resolve(null);
    });
  }

  async function idbGet(dbName, storeName, key) {
    const db = await openIdb(dbName, storeName);
    if (!db) return null;
    return new Promise((resolve) => {
      try {
        const tx = db.transaction(storeName, "readonly");
        const req = tx.objectStore(storeName).get(key);
        req.onsuccess = () => resolve(req.result ?? null);
        req.onerror = () => resolve(null);
      } catch (_) {
        resolve(null);
      }
    });
  }

  async function idbPut(dbName, storeName, key, val) {
    const db = await openIdb(dbName, storeName);
    if (!db) return false;
    return new Promise((resolve) => {
      try {
        const tx = db.transaction(storeName, "readwrite");
        tx.objectStore(storeName).put(val, key);
        tx.oncomplete = () => resolve(true);
        tx.onerror = () => resolve(false);
      } catch (_) {
        resolve(false);
      }
    });
  }

  async function idbGetAll(dbName, storeName) {
    const db = await openIdb(dbName, storeName);
    if (!db) return [];
    return new Promise((resolve) => {
      try {
        const tx = db.transaction(storeName, "readonly");
        const store = tx.objectStore(storeName);
        const req = store.getAll();
        req.onsuccess = () => resolve(Array.isArray(req.result) ? req.result : []);
        req.onerror = () => resolve([]);
      } catch (_) {
        resolve([]);
      }
    });
  }

  async function idbDelete(dbName, storeName, key) {
    const db = await openIdb(dbName, storeName);
    if (!db) return false;
    return new Promise((resolve) => {
      try {
        const tx = db.transaction(storeName, "readwrite");
        tx.objectStore(storeName).delete(key);
        tx.oncomplete = () => resolve(true);
        tx.onerror = () => resolve(false);
      } catch (_) {
        resolve(false);
      }
    });
  }

  function makeFlashCacheKey(userId, runId, slotIndex) {
    const u = String(userId || "guest").trim() || "guest";
    const r = String(runId || "solo").trim() || "solo";
    const s = Number.isInteger(Number(slotIndex)) ? Number(slotIndex) : 0;
    return `${u}::${r}::${s}`;
  }

  async function compressFlashBase64Gzip(rawB64) {
    const bytes = decodeFlashBase64(rawB64);
    if (!bytes) return "";
    if (typeof CompressionStream === "undefined" || typeof Response === "undefined") {
      return rawB64;
    }
    try {
      const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("gzip"));
      const compressed = new Uint8Array(await new Response(stream).arrayBuffer());
      return "gz:" + encodeFlashBase64(compressed);
    } catch (_) {
      return rawB64;
    }
  }

  async function decompressFlashPayloadToBase64(payloadStr) {
    if (!payloadStr || typeof payloadStr !== "string") return "";
    if (!payloadStr.startsWith("gz:")) {
      const bytes = decodeFlashBase64(payloadStr);
      return bytes ? encodeFlashBase64(bytes) : "";
    }
    if (typeof DecompressionStream === "undefined" || typeof Response === "undefined") {
      return "";
    }
    try {
      const bin = atob(payloadStr.slice(3));
      const gzBytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) gzBytes[i] = bin.charCodeAt(i) & 0xff;
      const stream = new Blob([gzBytes]).stream().pipeThrough(new DecompressionStream("gzip"));
      const rawBytes = new Uint8Array(await new Response(stream).arrayBuffer());
      return hasValidSectorSignature(rawBytes) ? encodeFlashBase64(rawBytes) : "";
    } catch (_) {
      return "";
    }
  }

  // --- Chunked Asset Pack Loader (supports tools/chunk_assets.py + brotatoweb manifests) ---
  async function loadSingleChunkedAsset(entry, statusCb, progressCb) {
    if (!entry || !Array.isArray(entry.chunks) || typeof entry.totalBytes !== "number") {
      return null;
    }
    const cacheKey = entry.file || entry.fileName || "pokemon_assets.pack";
    const cached = await idbGet(ASSET_DB_NAME, ASSET_STORE_NAME, cacheKey);
    if (
      cached &&
      cached.version === entry.version &&
      cached.bytes &&
      cached.bytes.byteLength === entry.totalBytes
    ) {
      if (progressCb) progressCb(100);
      if (statusCb) statusCb(`Loaded ${cacheKey} from local PC cache!`);
      return cached.bytes;
    }

    const totalBytes = entry.totalBytes;
    const chunkSize = entry.chunkSize || Math.ceil(totalBytes / Math.max(1, entry.chunks.length));
    const assembled = new Uint8Array(totalBytes);
    let loadedBytes = 0;
    let nextIdx = 0;
    const mbTotal = (totalBytes / (1024 * 1024)).toFixed(1);

    async function worker() {
      while (nextIdx < entry.chunks.length) {
        const idx = nextIdx++;
        const chunkPath = "/" + String(entry.chunks[idx]).replace(/^\/+/, "");
        const res = await fetch(chunkPath);
        if (!res.ok) throw new Error(`Chunk fetch failed (${res.status}): ${chunkPath}`);
        const buf = new Uint8Array(await res.arrayBuffer());
        assembled.set(buf, idx * chunkSize);
        loadedBytes += buf.byteLength;
        const pct = totalBytes > 0 ? Math.min(100, Math.round((loadedBytes / totalBytes) * 100)) : 100;
        const mbCur = (loadedBytes / (1024 * 1024)).toFixed(1);
        if (progressCb) progressCb(pct);
        if (statusCb) statusCb(`Downloading ${cacheKey}... ${mbCur} MB / ${mbTotal} MB (${pct}%)`);
      }
    }

    const workers = [];
    for (let i = 0; i < Math.min(4, entry.chunks.length); i++) {
      workers.push(worker());
    }
    await Promise.all(workers);

    if (statusCb) statusCb(`Caching ${cacheKey} to IndexedDB...`);
    await idbPut(ASSET_DB_NAME, ASSET_STORE_NAME, cacheKey, {
      version: entry.version,
      bytes: assembled,
    });
    return assembled;
  }

  async function loadChunkedAssetPack(statusCb, progressCb) {
    const candidateManifests = [
      "/chunks/assets.manifest.json",
      "/chunks/index.wasm.manifest.json",
      "/chunks/wasm.manifest.json",
      "/chunks/pck.manifest.json",
    ];
    let manifest = null;
    let manifestName = "";
    for (const url of candidateManifests) {
      try {
        const res = await fetch(url, { cache: "no-cache" });
        if (res.ok) {
          manifest = await res.json();
          manifestName = url;
          break;
        }
      } catch (_) {}
    }
    if (!manifest) return null;

    // Multi-asset manifest generated by tools/chunk_assets.py: { assets: { "index.wasm": {...}, ... } }
    if (manifest.assets && typeof manifest.assets === "object") {
      const files = {};
      for (const [fileName, entry] of Object.entries(manifest.assets)) {
        const bytes = await loadSingleChunkedAsset(entry, statusCb, progressCb);
        if (bytes) files[fileName] = bytes;
      }
      const primary = files["index.wasm"] || files["index.data"] || Object.values(files)[0] || null;
      return { manifest, manifestName, files, bytes: primary };
    }

    // Single-asset legacy manifest format
    const bytes = await loadSingleChunkedAsset(manifest, statusCb, progressCb);
    if (!bytes) return null;
    const key = manifest.file || manifest.fileName || "index.pck";
    return { manifest, manifestName, files: { [key]: bytes }, bytes };
  }

  const KNOWN_ITEM_IDS = {
    ITEM_NONE: 0,
    ITEM_POKE_BALL: 1,
    ITEM_GREAT_BALL: 2,
    ITEM_ULTRA_BALL: 3,
    ITEM_MASTER_BALL: 4,
    ITEM_POTION: 28,
    ITEM_SUPER_POTION: 29,
    ITEM_HYPER_POTION: 30,
    ITEM_MAX_POTION: 31,
  };

  function resolveNumericItemId(item) {
    if (!item || typeof item !== "object") return 0;
    const rawNum = Number(item.itemId);
    if (Number.isInteger(rawNum) && rawNum > 0 && rawNum <= 65535) return rawNum;
    const key = String(item.id || "").trim();
    if (Object.prototype.hasOwnProperty.call(KNOWN_ITEM_IDS, key)) {
      return KNOWN_ITEM_IDS[key];
    }
    return 0;
  }

  function decodeBase64Fixed100(b64) {
    if (typeof b64 !== "string" || b64.length === 0 || b64.length > 160) return null;
    try {
      if (typeof atob === "function") {
        const bin = atob(b64);
        if (bin.length !== 100) return null;
        const out = new Uint8Array(100);
        for (let i = 0; i < 100; i++) out[i] = bin.charCodeAt(i) & 0xff;
        return out;
      }
      const clean = b64.replace(/[\r\n\s]/g, "");
      if (clean.length % 4 !== 0) return null;
      const alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      const bytes = [];
      for (let i = 0; i < clean.length; i += 4) {
        const c0 = alpha.indexOf(clean[i]);
        const c1 = alpha.indexOf(clean[i + 1]);
        const c2 = clean[i + 2] === "=" ? -1 : alpha.indexOf(clean[i + 2]);
        const c3 = clean[i + 3] === "=" ? -1 : alpha.indexOf(clean[i + 3]);
        if (c0 < 0 || c1 < 0 || (clean[i + 2] !== "=" && c2 < 0) || (clean[i + 3] !== "=" && c3 < 0)) {
          return null;
        }
        const n = (c0 << 18) | (c1 << 12) | ((c2 >= 0 ? c2 : 0) << 6) | (c3 >= 0 ? c3 : 0);
        bytes.push((n >> 16) & 0xff);
        if (c2 >= 0) bytes.push((n >> 8) & 0xff);
        if (c3 >= 0) bytes.push(n & 0xff);
      }
      return bytes.length === 100 ? new Uint8Array(bytes) : null;
    } catch (_) {
      return null;
    }
  }

  function toNumericTradeSessionId(sessionId) {
    if (typeof sessionId === "number" && Number.isFinite(sessionId) && sessionId > 0) {
      return (Math.floor(sessionId) >>> 0) || 1;
    }
    const str = String(sessionId || "").trim();
    if (/^\d+$/.test(str)) {
      return (parseInt(str, 10) >>> 0) || 1;
    }
    let hash = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
      hash ^= str.charCodeAt(i);
      hash = Math.imul(hash, 0x01000193) >>> 0;
    }
    return hash || 1;
  }

  function encodeBinaryTradeCommitFrame(sessionIdNum, slotA, slotB, offerA, offerB, itemIdx) {
    const buf = new ArrayBuffer(254);
    const u8 = new Uint8Array(buf);
    const dv = new DataView(buf);
    const chTrade = 5;
    const opcodeCommit = 0x52; // MP_PKT_TRD_COMMIT

    // MpPacketHeader (8 bytes)
    dv.setUint16(0, 0x4d50, true); // MP_BINARY_MAGIC ('MP')
    u8[2] = chTrade;
    u8[3] = opcodeCommit;
    dv.setUint16(4, 1, true);
    u8[6] = 0; // Host slot 0 (matching fromPeerId=1 so non-Host slotA doesn't overwrite sPeers[slotA].peerId)
    u8[7] = 1; // host-authoritative flag

    // MpTradeCommitPayload (246 bytes starting at offset 8)
    const base = 8;
    dv.setUint32(base + 0, sessionIdNum >>> 0, true);
    u8[base + 4] = slotA & 0xff;
    u8[base + 5] = slotB & 0xff;

    const writeOffer = (off, slotIdx, offer) => {
      const hasMon = itemIdx === 0 && offer && offer.partyIndex >= 0 && offer.mon ? 1 : 0;
      const monBytes = hasMon ? decodeBase64Fixed100(offer.mon.monBytesB64) : null;
      if (hasMon && !monBytes) return false;

      const items = offer && Array.isArray(offer.items) ? offer.items : [];
      const item = items[itemIdx] || null;
      const itemId = item ? resolveNumericItemId(item) : 0;
      const itemQty = item && itemId > 0 ? Math.max(0, Math.min(65535, Math.floor(Number(item.qty) || 0))) : 0;
      const money = itemIdx === 0 && offer ? Math.max(0, Math.floor(Number(offer.money) || 0)) : 0;
      const linkNum = offer && offer.mon && offer.mon.linkId
        ? parseInt(String(offer.mon.linkId).replace(/\D/g, ""), 10) || 0
        : 0;
      const mapsec = offer && offer.mon && Number.isInteger(Number(offer.mon.mapsec))
        ? Number(offer.mon.mapsec) & 0xffff
        : 0xffff;

      dv.setUint32(off + 0, sessionIdNum >>> 0, true);
      u8[off + 4] = slotIdx & 0xff;
      u8[off + 5] = hasMon;
      u8[off + 6] = 1; // locked
      u8[off + 7] = 0;
      dv.setUint16(off + 8, linkNum & 0xffff, true);
      dv.setUint16(off + 10, mapsec, true);
      dv.setUint16(off + 12, itemId & 0xffff, true);
      dv.setUint16(off + 14, itemQty & 0xffff, true);
      dv.setUint32(off + 16, money >>> 0, true);
      if (monBytes) u8.set(monBytes, off + 20);
      return true;
    };

    if (!writeOffer(base + 6, slotA, offerA)) return null;
    if (!writeOffer(base + 126, slotB, offerB)) return null;
    return u8;
  }

  function parseMonUidKeys(monUid, fallbackOtId, fallbackPersonality) {
    let otId = Number.isFinite(Number(fallbackOtId)) ? (Number(fallbackOtId) >>> 0) : 0;
    let personality = Number.isFinite(Number(fallbackPersonality)) ? (Number(fallbackPersonality) >>> 0) : 0;
    let valid = Boolean(otId || personality);
    if (typeof monUid === "string") {
      const m = /^mon-([0-9a-fA-F]{1,8})-([0-9a-fA-F]{1,8})$/.exec(monUid.trim());
      if (m) {
        personality = (parseInt(m[1], 16) >>> 0);
        otId = (parseInt(m[2], 16) >>> 0);
        valid = true;
      }
    }
    return { otId, personality, valid };
  }

  function parseRouteMapsec(routeId) {
    if (typeof routeId === "number" && Number.isInteger(routeId) && routeId >= 0 && routeId <= 0xffff) {
      return routeId;
    }
    if (typeof routeId === "string") {
      const m = /^mapsec:(\d+)$/i.exec(routeId.trim());
      if (m) {
        const n = parseInt(m[1], 10);
        if (Number.isInteger(n) && n >= 0 && n <= 0xffff) return n;
      }
      if (routeId.trim().toUpperCase() === "STARTER") return 190;
    }
    return 0xffff;
  }

  function parseNumericLinkId(linkId) {
    const n = parseInt(String(linkId || "").replace(/\D/g, ""), 10);
    return Number.isFinite(n) && n > 0 ? n : 0;
  }

  function sanitizeMon(mon) {
    if (!mon || typeof mon !== "object") return null;
    const maxHp = Math.max(1, Math.floor(Number(mon.maxHp) || 1));
    const hp = Math.max(0, Math.min(maxHp, Math.floor(Number(mon.hp) || 0)));
    const validStatus = ["BRN", "PAR", "PSN", "SLP", "FRZ", "FNT"].includes(mon.status) ? mon.status : "";
    const validBall = ["pokeball", "greatball", "ultraball", "masterball"].includes(mon.ball)
      ? mon.ball
      : "pokeball";
    const clean = {
      uid: String(mon.uid || generateId("mon")).slice(0, 64),
      species: String(mon.species || "MissingNo").slice(0, 32),
      nickname: String(mon.nickname || mon.species || "Pokemon").slice(0, 32),
      level: Math.max(1, Math.min(100, Math.floor(Number(mon.level) || 1))),
      hp,
      maxHp,
      status: validStatus,
      ball: validBall,
      heldItem: String(mon.heldItem || "").slice(0, 40),
      linkId: String(mon.linkId || "").slice(0, 32),
      originMap: String(mon.originMap || "Hoenn").slice(0, 40),
      isDead: Boolean(mon.isDead),
    };
    if (decodeBase64Fixed100(mon.monBytesB64)) {
      clean.monBytesB64 = mon.monBytesB64;
    }
    if (Number.isInteger(Number(mon.speciesId)) && Number(mon.speciesId) > 0) {
      clean.speciesId = Number(mon.speciesId);
    }
    if (Number.isInteger(Number(mon.mapsec)) && Number(mon.mapsec) >= 0 && Number(mon.mapsec) <= 0xffff) {
      clean.mapsec = Number(mon.mapsec);
    }
    return clean;
  }

  function sanitizeBagItems(items) {
    if (!Array.isArray(items)) return [];
    const out = [];
    for (const it of items.slice(0, 128)) {
      if (!it || typeof it !== "object" || !it.id) continue;
      const qty = Math.floor(Number(it.qty) || 0);
      if (qty <= 0) continue;
      const itemId = resolveNumericItemId(it);
      const entry = {
        id: String(it.id).slice(0, 48),
        name: String(it.name || it.id).slice(0, 48),
        qty,
      };
      if (itemId > 0) entry.itemId = itemId;
      out.push(entry);
    }
    return out;
  }

  function sanitizePlayerSlot(p, forcedIndex) {
    const base = makeEmptyPlayerSlot(forcedIndex);
    if (!p || typeof p !== "object") return base;
    return {
      ...base,
      slotIndex: forcedIndex,
      peerId: Math.max(0, Math.floor(Number(p.peerId) || 0)),
      token: String(p.token || "").slice(0, 64),
      userId: p.userId ? String(p.userId).slice(0, 64) : "",
      username: p.username ? String(p.username).slice(0, 32) : "",
      flashRev: Math.max(0, Math.floor(Number(p.flashRev) || 0)),
      name: String(p.name || base.name).slice(0, 20),
      connected: Boolean(p.connected),
      isHost: forcedIndex === 0,
      partyId: p.partyId ? String(p.partyId).slice(0, 24) : null,
      soulLinkGroup: p.soulLinkGroup ? String(p.soulLinkGroup).slice(0, 24) : null,
      mapGroup: Math.floor(Number(p.mapGroup) || 0),
      mapNum: Math.floor(Number(p.mapNum) || 0),
      mapName: String(p.mapName || base.mapName).slice(0, 48),
      x: Math.floor(Number(p.x) || 0),
      y: Math.floor(Number(p.y) || 0),
      inBattle: Boolean(p.inBattle),
      busy: Boolean(p.busy),
      tutorialComplete: Boolean(p.tutorialComplete),
      money: Math.max(0, Math.floor(Number(p.money) || 0)),
      bagItems: Array.isArray(p.bagItems) ? sanitizeBagItems(p.bagItems) : base.bagItems,
      party: Array.isArray(p.party) ? p.party.slice(0, 6).map(sanitizeMon).filter(Boolean) : [],
    };
  }

  // --- Default Starter & Demo State Factories ---
  function makeEmptyPlayerSlot(slotIndex) {
    return {
      slotIndex,
      peerId: slotIndex === 0 ? 1 : 0,
      token: "",
      userId: "",
      username: "",
      flashRev: 0,
      name: slotIndex === 0 ? getOrCreatePlayerName() : `Slot ${slotIndex + 1}`,
      connected: false,
      isHost: slotIndex === 0,
      partyId: null, // e.g., "party_A" or "party_B"
      soulLinkGroup: null,
      mapGroup: 0,
      mapNum: 0,
      mapName: "Littleroot Town",
      x: 10,
      y: 8,
      inBattle: false,
      busy: false,
      tutorialComplete: false,
      money: 0,
      bagItems: [],
      party: [],
    };
  }

  function makeDefaultRunConfig() {
    return {
      runId: generateId("run"),
      roomName: `${getOrCreatePlayerName()}'s Hoenn Run`,
      seed: randomSeedString(),
      randomizerPreset: "balanced",
      randomizerToggles: cloneData(RANDOMIZER_PRESETS.balanced),
      nuzlockePreset: "standard_soullink",
      nuzlockeToggles: cloneData(NUZLOCKE_PRESETS.standard_soullink),
      soulLinkMode: "pairs", // "pairs" (P1+P2 / P3+P4) or "group" (All 4)
      battleAutoPull: false,
      speedMultiplier: 1,
      gameOver: false,
    };
  }

  function makeInitialState() {
    const p0 = makeEmptyPlayerSlot(0);
    p0.connected = true;
    p0.token = getOrCreateClientToken();
    const runConfig = makeDefaultRunConfig();
    const hasAutoJoinRoom =
      typeof global.location !== "undefined" &&
      global.location.search &&
      Number(new URLSearchParams(global.location.search).get("room")) > 0;
    if (!hasAutoJoinRoom && hasValidWasmFlashSave()) {
      const localMeta = readFlashSaveMeta();
      if (localMeta.runId) {
        runConfig.runId = localMeta.runId;
      }
    }

    return {
      auth: {
        status: "checking", // checking | signed_out | signed_in
        userId: "",
        username: "",
        sessionToken: (typeof localStorage !== "undefined" && localStorage.getItem(STORAGE_KEYS.sessionToken)) || "",
      },
      connection: {
        status: "offline", // offline | connecting | hosting | connected
        role: "solo",
        roomPort: 0,
        myPeerId: 1,
        mySlotIndex: 0,
        myToken: p0.token,
        error: "",
      },
      wasmReady: false,
      runConfig,
      players: [p0, makeEmptyPlayerSlot(1), makeEmptyPlayerSlot(2), makeEmptyPlayerSlot(3)],
      world: {
        badges: [false, false, false, false, false, false, false, false],
        hms: [],
        unlockedMaps: ["0:0", "0:1", "0:9", "0:16", "0:17", "0:18", "0:19"],
        defeatedTrainers: [],
        completedCutscenes: {},
        globalFlags: {},
        globalVars: {},
      },
      soulLink: {
        nextLinkNum: 1,
        routeLocks: {}, // key: `${groupKey}:${routeId}` -> { linkId, routeName, members: { slotIndex: monUid } }
        graveyard: [],
      },
      activeInvites: [], // incoming battle / pvp / trade invites
      activeCutscene: null,
      trade: {
        active: false,
        sessionId: "",
        partnerPeerId: 0,
        partnerSlotIndex: -1,
        partnerName: "",
        myOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
        partnerOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
        myLocked: false,
        partnerLocked: false,
        myConfirmed: false,
        partnerConfirmed: false,
        statusText: "Select an online partner to initiate a Quick-Trade.",
      },
      eventLog: [
        {
          id: generateId("ev"),
          time: new Date().toLocaleTimeString(),
          kind: "info",
          text: "Emerald Companion HUD initialized. Host or join a room to begin multiplayer!",
        },
      ],
      highlightedLinkId: null,
    };
  }

  // --- Main Bridge Controller ---
  class PokemonRelayClient {
    constructor() {
      initRelayFromEnvironment();
      this.state = makeInitialState();
      this.ws = null;
      this.listeners = new Set();
      this.wasmModule = null;
      this.wasmCallbacks = {};
      this.remoteRooms = [];
      this.lanInfo = null;
      this.cloudRuns = { hostedRuns: [], joinedRuns: [] };
      this._roomInfoTimer = null;
      this._pendingLateJoin = null;
      this._pendingSlotClaim = null;
      this._pendingRunConfigSync = null;
      this._pendingWorldSync = null;
      this._joinOptions = { confirmedProgressed: false };
      this._suppressLocalFlashSync = false;
      this._upsertedRunIds = new Set();
      this._inFlightUpserts = new Map();
    }

    isSignedIn() {
      return Boolean(
        this.state.auth &&
          this.state.auth.status === "signed_in" &&
          this.state.auth.userId &&
          this.state.auth.username
      );
    }

    _syncAccountTrainerName() {
      if (!this.isSignedIn()) return;
      const cleanName = String(this.state.auth.username || "").trim().slice(0, 20) || "Trainer";
      try {
        localStorage.setItem(STORAGE_KEYS.playerName, cleanName);
      } catch (_) {}
      const myIdx = this.state.connection ? Number(this.state.connection.mySlotIndex) || 0 : 0;
      const mySlot = this.state.players[myIdx];
      if (mySlot) {
        mySlot.userId = this.state.auth.userId;
        mySlot.username = this.state.auth.username;
        mySlot.name = cleanName;
      }
      if (this.state.runConfig) {
        const rn = String(this.state.runConfig.roomName || "").trim();
        if (!rn || /^(Trainer|Guest|May\d+)'s Hoenn Run$/i.test(rn)) {
          this.state.runConfig.roomName = `${cleanName}'s Hoenn Run`;
        }
      }
      if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._syncPeersToWasm === "function") {
        global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      }
    }

    _handleSignedInFlashIsolation(userId) {
      const meta = readFlashSaveMeta();
      if (hasValidWasmFlashSave() && meta.userId && meta.userId !== "guest" && meta.userId !== userId) {
        clearLocalFlashSave();
        if (
          global.PokemonWasmBridge &&
          typeof global.PokemonWasmBridge._returnToTitleScreenInWasm === "function"
        ) {
          global.PokemonWasmBridge._returnToTitleScreenInWasm({ autosave: false });
        }
      }
    }

    _getEffectiveUserId() {
      return (this.state.auth && this.state.auth.userId) || "guest";
    }

    async _apiFetch(path, options) {
      const base = getRelayHttpBase();
      const opts = options && typeof options === "object" ? { ...options } : {};
      const headers = new Headers(opts.headers || {});
      const tok =
        (this.state.auth && this.state.auth.sessionToken) ||
        (typeof localStorage !== "undefined" ? localStorage.getItem(STORAGE_KEYS.sessionToken) : "") ||
        "";
      if (tok && !headers.has("Authorization")) {
        headers.set("Authorization", `Bearer ${tok}`);
      }
      if (opts.body && typeof opts.body === "object" && !(opts.body instanceof Uint8Array)) {
        if (!headers.has("Content-Type")) headers.set("Content-Type", "application/json");
        opts.body = JSON.stringify(opts.body);
      }
      opts.headers = headers;
      opts.cache = "no-store";
      try {
        const res = await fetch(`${base}${path}`, opts);
        const text = await res.text();
        const data = text ? safeParseJson(text) : null;
        return {
          ok: res.ok,
          status: res.status,
          data,
          error: (data && (data.error || data.message)) || (!res.ok ? `HTTP ${res.status}` : ""),
        };
      } catch (err) {
        return { ok: false, status: 0, data: null, error: err && err.message ? err.message : "Network error" };
      }
    }

    validateSession() {
      const run = async () => {
        const tok = typeof localStorage !== "undefined" ? localStorage.getItem(STORAGE_KEYS.sessionToken) || "" : "";
        if (!tok) {
          this.state.auth = { status: "signed_out", userId: "", username: "", sessionToken: "" };
          this.emit("auth");
          return false;
        }
        this.state.auth.sessionToken = tok;
        const res = await this._apiFetch("/api/auth/me", { method: "GET" });
        if (res.ok && res.data && res.data.userId) {
          this.state.auth = {
            status: "signed_in",
            userId: String(res.data.userId),
            username: String(res.data.username || ""),
            sessionToken: tok,
          };
          this._syncAccountTrainerName();
          this._handleSignedInFlashIsolation(this.state.auth.userId);
          this.emit("auth");
          this.emit("players");
          await this.refreshCloudRuns();
          return true;
        }
        if (res.status === 401 || res.status === 403) {
          try {
            localStorage.removeItem(STORAGE_KEYS.sessionToken);
          } catch (_) {}
          this.state.auth = { status: "signed_out", userId: "", username: "", sessionToken: "" };
        } else {
          this.state.auth.status = "signed_out";
        }
        this.emit("auth");
        return false;
      };
      this._validateSessionPromise = run().finally(() => {
        this._validateSessionPromise = null;
      });
      return this._validateSessionPromise;
    }

    async registerAccount(username, password) {
      const res = await this._apiFetch("/api/auth/register", {
        method: "POST",
        body: { username: String(username || "").trim(), password: String(password || "") },
      });
      if (!res.ok || !res.data || !res.data.sessionToken) {
        return { ok: false, error: res.error || "Registration failed" };
      }
      const { userId, username: cleanName, sessionToken } = res.data;
      try {
        localStorage.setItem(STORAGE_KEYS.sessionToken, sessionToken);
      } catch (_) {}
      this.state.auth = {
        status: "signed_in",
        userId: String(userId || ""),
        username: String(cleanName || username || ""),
        sessionToken: String(sessionToken),
      };
      this._syncAccountTrainerName();
      this._handleSignedInFlashIsolation(this.state.auth.userId);
      this.emit("auth");
      this.emit("players");
      await this.refreshCloudRuns();
      return { ok: true, userId: this.state.auth.userId, username: this.state.auth.username };
    }

    async loginAccount(username, password) {
      const res = await this._apiFetch("/api/auth/login", {
        method: "POST",
        body: { username: String(username || "").trim(), password: String(password || "") },
      });
      if (!res.ok || !res.data || !res.data.sessionToken) {
        return { ok: false, error: res.error || "Invalid credentials" };
      }
      const { userId, username: cleanName, sessionToken } = res.data;
      try {
        localStorage.setItem(STORAGE_KEYS.sessionToken, sessionToken);
      } catch (_) {}
      this.state.auth = {
        status: "signed_in",
        userId: String(userId || ""),
        username: String(cleanName || username || ""),
        sessionToken: String(sessionToken),
      };
      this._syncAccountTrainerName();
      this._handleSignedInFlashIsolation(this.state.auth.userId);
      this.emit("auth");
      this.emit("players");
      await this.refreshCloudRuns();
      return { ok: true, userId: this.state.auth.userId, username: this.state.auth.username };
    }

    async logoutAccount() {
      if (this._wasmResumeTimer) {
        clearTimeout(this._wasmResumeTimer);
        this._wasmResumeTimer = null;
      }
      this._pendingWasmResume = false;
      this._pendingWasmResetMode = "";
      if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge.setInputMask === "function") {
        global.PokemonWasmBridge.setInputMask(0, "gamepad");
        global.PokemonWasmBridge.setKeyboardMask(0);
      }
      // Autosave gameplay & return WASM to Title Screen while auth token is still valid.
      let didSaveWasm = false;
      if (
        global.PokemonWasmBridge &&
        typeof global.PokemonWasmBridge._returnToTitleScreenInWasm === "function"
      ) {
        didSaveWasm = global.PokemonWasmBridge._returnToTitleScreenInWasm({ autosave: true });
      } else if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._saveGameInWasm === "function") {
        didSaveWasm = global.PokemonWasmBridge._saveGameInWasm();
      }
      if (this.isSignedIn() && (didSaveWasm || hasValidWasmFlashSave())) {
        try {
          await this.saveRunSnapshotToDb(undefined, { silentBroadcast: true, skipWasmSave: true });
          const myRunId = (this.state.runConfig && this.state.runConfig.runId) || "";
          const myIdx = this.state.connection ? Number(this.state.connection.mySlotIndex) || 0 : 0;
          const mySlot = this.state.players[myIdx];
          const b64 = exportWasmFlashBase64();
          if (myRunId && b64) {
            await this.cacheSlotFlashLocal(
              myRunId,
              myIdx,
              b64,
              (mySlot && mySlot.flashRev) || 0,
              Date.now()
            );
            if (this.isHost()) {
              await this.syncRunUpsertToBackend().catch(() => null);
            }
            await this.syncSlotFlashToBackend(
              myRunId,
              myIdx,
              b64,
              (mySlot && mySlot.flashRev) || 0,
              mySlot
            ).catch(() => null);
          }
        } catch (_) {}
      }
      this.disconnect();
      if (this.state.auth && this.state.auth.sessionToken) {
        await this._apiFetch("/api/auth/logout", { method: "POST" }).catch(() => {});
      }
      try {
        localStorage.removeItem(STORAGE_KEYS.sessionToken);
        localStorage.removeItem(STORAGE_KEYS.playerName);
      } catch (_) {}
      clearLocalFlashSave();
      this.state.auth = { status: "signed_out", userId: "", username: "", sessionToken: "" };
      this.cloudRuns = { hostedRuns: [], joinedRuns: [] };
      const mySlot = this.state.players[this.state.connection.mySlotIndex];
      if (mySlot) {
        mySlot.userId = "";
        mySlot.username = "";
        mySlot.name = "Trainer";
        mySlot.party = [];
        mySlot.bagItems = [];
        mySlot.tutorialComplete = false;
        mySlot.flashRev = 0;
      }
      if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._syncPeersToWasm === "function") {
        global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      }
      this.emit("auth");
      this.emit("players");
      this.emit("saves");
      this.emit("all");
    }

    async refreshCloudRuns() {
      if (!this.state.auth || this.state.auth.status !== "signed_in") {
        this.cloudRuns = { hostedRuns: [], joinedRuns: [] };
        return this.cloudRuns;
      }
      const res = await this._apiFetch("/api/runs", { method: "GET" });
      if (res.ok && res.data) {
        this.cloudRuns = {
          hostedRuns: Array.isArray(res.data.hostedRuns) ? res.data.hostedRuns : [],
          joinedRuns: Array.isArray(res.data.joinedRuns) ? res.data.joinedRuns : [],
        };
        this.emit("saves");
      }
      return this.cloudRuns;
    }

    async cacheSlotFlashLocal(runId, slotIndex, rawFlashB64, flashRev, savedAtMs) {
      if (!runId || !rawFlashB64) return false;
      const bytes = decodeFlashBase64(rawFlashB64);
      if (!hasValidSectorSignature(bytes)) return false;
      const userId = this._getEffectiveUserId();
      const sIdx = Number.isInteger(Number(slotIndex)) ? Number(slotIndex) : 0;
      const key = makeFlashCacheKey(userId, runId, sIdx);
      const existing = await idbGet(FLASH_CACHE_DB_NAME, FLASH_CACHE_STORE_NAME, key);
      const nextRev = Math.max(
        Number.isFinite(Number(flashRev)) ? Math.floor(Number(flashRev)) : 0,
        existing ? Number(existing.flashRev) || 0 : 0
      );
      return idbPut(FLASH_CACHE_DB_NAME, FLASH_CACHE_STORE_NAME, key, {
        key,
        userId,
        runId,
        slotIndex: sIdx,
        flashSaveB64: encodeFlashBase64(bytes),
        flashRev: nextRev,
        savedAtMs: Number(savedAtMs) || Date.now(),
      });
    }

    async getCachedSlotFlashLocal(runId, slotIndex, explicitUserId) {
      if (!runId) return null;
      const userId = explicitUserId || this._getEffectiveUserId();
      const sIdx = Number.isInteger(Number(slotIndex)) ? Number(slotIndex) : 0;
      const primary = await idbGet(
        FLASH_CACHE_DB_NAME,
        FLASH_CACHE_STORE_NAME,
        makeFlashCacheKey(userId, runId, sIdx)
      );
      if (primary && primary.flashSaveB64) return primary;
      if (userId !== "guest") {
        const guestFallback = await idbGet(
          FLASH_CACHE_DB_NAME,
          FLASH_CACHE_STORE_NAME,
          makeFlashCacheKey("guest", runId, sIdx)
        );
        if (guestFallback && guestFallback.flashSaveB64) return guestFallback;
      }
      return null;
    }

    async flushActiveFlashToCache() {
      const meta = readFlashSaveMeta();
      if (!meta.runId) return false;
      try {
        const b64 = localStorage.getItem(STORAGE_KEYS.flashSave) || "";
        const bytes = decodeFlashBase64(b64);
        if (!hasValidSectorSignature(bytes)) return false;
        const userId = meta.userId || this._getEffectiveUserId();
        const sIdx = Number.isInteger(meta.slotIndex) ? meta.slotIndex : 0;
        const key = makeFlashCacheKey(userId, meta.runId, sIdx);
        return idbPut(FLASH_CACHE_DB_NAME, FLASH_CACHE_STORE_NAME, key, {
          key,
          userId,
          runId: meta.runId,
          slotIndex: sIdx,
          flashSaveB64: encodeFlashBase64(bytes),
          flashRev: meta.flashRev || 0,
          savedAtMs: meta.savedAtMs || Date.now(),
        });
      } catch (_) {
        return false;
      }
    }

    async syncRunUpsertToBackend(explicitSnapshot) {
      if (this._validateSessionPromise) {
        await this._validateSessionPromise.catch(() => {});
      }
      if (!this.state.auth || this.state.auth.status !== "signed_in") return null;
      if (!this.isHost() && !explicitSnapshot) return null;
      const snap = explicitSnapshot || null;
      const cfg = (snap && snap.runConfig) || this.state.runConfig;
      if (!cfg || !cfg.runId) return null;
      const runId = cfg.runId;
      if (this._inFlightUpserts && this._inFlightUpserts.has(runId)) {
        return this._inFlightUpserts.get(runId);
      }
      const world = (snap && snap.world) || this.state.world;
      const soulLink = (snap && snap.soulLink) || this.state.soulLink;
      const players = (snap && snap.playerSlots) || this.state.players;
      const hostSlotObj = players && players[0] ? players[0] : null;
      const doUpsert = async () => {
        const res = await this._apiFetch("/api/runs/upsert", {
          method: "POST",
          body: {
            runId,
            title: cfg.roomName || "Hoenn Run",
            seed: cfg.seed || "",
            randomizerPreset: cfg.randomizerPreset || "balanced",
            runConfig: cfg,
            worldState: { world, soulLink },
            hostSlot: hostSlotObj
              ? {
                  slotIdx: 0,
                  userId: hostSlotObj.userId || this.state.auth.userId,
                  characterName: hostSlotObj.name || getOrCreatePlayerName(),
                  tutorialComplete: Boolean(hostSlotObj.tutorialComplete),
                  party: Array.isArray(hostSlotObj.party) ? hostSlotObj.party : [],
                  money: Number(hostSlotObj.money) || 0,
                  bagItems: Array.isArray(hostSlotObj.bagItems) ? hostSlotObj.bagItems : [],
                  flashRev: Number(hostSlotObj.flashRev) || 0,
                }
              : undefined,
            slots: (players || [])
              .map((p, idx) => ({
                slotIdx: idx,
                userId: p.userId || (idx === 0 ? this.state.auth.userId : ""),
                characterName: p.name || `Slot ${idx + 1}`,
                tutorialComplete: Boolean(p.tutorialComplete),
                party: Array.isArray(p.party) ? p.party : [],
                money: Number(p.money) || 0,
                bagItems: Array.isArray(p.bagItems) ? p.bagItems : [],
                flashRev: Number(p.flashRev) || 0,
                connected: Boolean(p.connected),
              }))
              .filter(
                (s) =>
                  s.slotIdx === 0 ||
                  s.userId ||
                  s.connected ||
                  s.party.length > 0 ||
                  s.tutorialComplete ||
                  s.flashRev > 0
              ),
          },
        });
        if (res.ok) {
          if (this._upsertedRunIds) this._upsertedRunIds.add(runId);
          this.refreshCloudRuns().catch(() => {});
        }
        return res;
      };
      const promise = doUpsert().finally(() => {
        if (this._inFlightUpserts) this._inFlightUpserts.delete(runId);
      });
      if (this._inFlightUpserts) this._inFlightUpserts.set(runId, promise);
      return promise;
    }

    async syncSlotFlashToBackend(runId, slotIndex, rawFlashB64, flashRev, slotObj, opts) {
      if (this._validateSessionPromise) {
        await this._validateSessionPromise.catch(() => {});
      }
      if (!this.state.auth || this.state.auth.status !== "signed_in") return null;
      if (!runId || !rawFlashB64) return null;
      const sIdx = Number.isInteger(Number(slotIndex)) ? Number(slotIndex) : 0;
      if (this._inFlightUpserts && this._inFlightUpserts.has(runId)) {
        await this._inFlightUpserts.get(runId).catch(() => {});
      }
      const compressed = await compressFlashBase64Gzip(rawFlashB64);
      const isGzip = Boolean(compressed && compressed.startsWith("gz:"));
      const slot = slotObj || this.state.players[sIdx] || {};
      const buildBody = () => {
        const b = {
          flashSaveB64: isGzip ? compressed.slice(3) : rawFlashB64,
          flashEncoding: isGzip ? "gzip" : "raw",
          characterName: slot.name || getOrCreatePlayerName(),
          tutorialComplete: Boolean(slot.tutorialComplete),
          party: Array.isArray(slot.party) ? slot.party : [],
          money: Number(slot.money) || 0,
          bagItems: Array.isArray(slot.bagItems) ? slot.bagItems : [],
          force: Boolean(opts && opts.force),
        };
        if (opts && Number.isFinite(Number(opts.expectedRev))) {
          b.expectedRev = Math.max(0, Math.floor(Number(opts.expectedRev)));
        }
        if (slot.userId) {
          b.userId = slot.userId;
        }
        return b;
      };

      let res = await this._apiFetch(`/api/runs/${encodeURIComponent(runId)}/slot/${sIdx}/flash`, {
        method: "POST",
        body: buildBody(),
      });
      if (res.status === 404 && this.isHost()) {
        const upRes = await this.syncRunUpsertToBackend().catch(() => null);
        if (upRes && upRes.ok) {
          res = await this._apiFetch(`/api/runs/${encodeURIComponent(runId)}/slot/${sIdx}/flash`, {
            method: "POST",
            body: buildBody(),
          });
        }
      } else if (res.status === 403 && sIdx > 0 && this.isClient()) {
        const claimRes = await this.claimRunSlotOnBackend(runId, sIdx, slot.name).catch(() => null);
        if (claimRes && claimRes.ok) {
          res = await this._apiFetch(`/api/runs/${encodeURIComponent(runId)}/slot/${sIdx}/flash`, {
            method: "POST",
            body: buildBody(),
          });
        }
      }
      if (res.ok && res.data && Number.isFinite(Number(res.data.flashRev))) {
        const nextRev = Number(res.data.flashRev);
        if (slot) slot.flashRev = Math.max(slot.flashRev || 0, nextRev);
        if (sIdx === (this.state.connection.mySlotIndex || 0)) {
          writeFlashSaveMeta(runId, Date.now(), {
            userId: this._getEffectiveUserId(),
            slotIndex: sIdx,
            flashRev: nextRev,
          });
        }
      } else if (res.status === 409 && res.data && Number.isFinite(Number(res.data.currentRev || res.data.flashRev))) {
        const serverRev = Number(res.data.currentRev ?? res.data.flashRev);
        if (slot) slot.flashRev = Math.max(slot.flashRev || 0, serverRev);
      }
      return res;
    }

    async fetchSlotFlashFromBackend(runId, slotIndex) {
      if (!this.state.auth || this.state.auth.status !== "signed_in" || !runId) return null;
      const sIdx = Number.isInteger(Number(slotIndex)) ? Number(slotIndex) : 0;
      const res = await this._apiFetch(`/api/runs/${encodeURIComponent(runId)}/slot/${sIdx}/flash?encoding=gzip`, {
        method: "GET",
      });
      if (!res.ok || !res.data || !res.data.flashSaveB64) return null;
      const payload =
        res.data.flashEncoding === "gzip" && !String(res.data.flashSaveB64).startsWith("gz:")
          ? `gz:${res.data.flashSaveB64}`
          : res.data.flashSaveB64;
      const rawB64 = await decompressFlashPayloadToBase64(payload);
      if (!rawB64) return null;
      return {
        flashSaveB64: rawB64,
        flashRev: Math.max(0, Math.floor(Number(res.data.flashRev) || 0)),
        slotMeta: res.data.slotMeta || null,
      };
    }

    async claimRunSlotOnBackend(runId, slotIndex, characterName) {
      if (!this.state.auth || this.state.auth.status !== "signed_in" || !runId) {
        return { ok: false };
      }
      return this._apiFetch(`/api/runs/${encodeURIComponent(runId)}/slot/${Number(slotIndex)}/claim`, {
        method: "POST",
        body: { characterName: characterName || getOrCreatePlayerName() },
      });
    }

    async deleteCloudRun(runId) {
      if (!this.state.auth || this.state.auth.status !== "signed_in" || !runId) {
        return { ok: false };
      }
      const res = await this._apiFetch(`/api/runs/${encodeURIComponent(runId)}`, { method: "DELETE" });
      if (res.ok) {
        await this.refreshCloudRuns();
      }
      return res;
    }

    async hasReturningSlotForRun(runId) {
      if (!runId) return false;
      if (this._validateSessionPromise) {
        await this._validateSessionPromise.catch(() => {});
      }
      const myUserId = this._getEffectiveUserId();
      const myTok = this.state.connection && this.state.connection.myToken;
      const allCloud = [
        ...(this.cloudRuns && Array.isArray(this.cloudRuns.hostedRuns) ? this.cloudRuns.hostedRuns : []),
        ...(this.cloudRuns && Array.isArray(this.cloudRuns.joinedRuns) ? this.cloudRuns.joinedRuns : []),
      ];
      if (allCloud.some((r) => r && (r.runId === runId || r.run_id === runId))) {
        return true;
      }
      const meta = readFlashSaveMeta();
      if (meta.runId === runId && hasValidWasmFlashSave()) return true;
      for (let idx = 0; idx < 4; idx++) {
        const cached = await this.getCachedSlotFlashLocal(runId, idx);
        if (cached && cached.flashSaveB64) return true;
      }
      const runs = await this.listSavedRuns();
      const snap = runs.find((r) => r && r.runId === runId);
      if (snap && Array.isArray(snap.playerSlots)) {
        return snap.playerSlots.some(
          (p) =>
            p &&
            ((myUserId !== "guest" && p.userId === myUserId) || (myTok && p.token === myTok))
        );
      }
      return false;
    }

    subscribe(fn) {
      this.listeners.add(fn);
      return () => this.listeners.delete(fn);
    }

    emit(reason, payload) {
      for (const fn of this.listeners) {
        try {
          fn(this.state, reason, payload);
        } catch (err) {
          console.error("[PokemonRelayClient] listener error:", err);
        }
      }
    }

    isHost() {
      return this.state.connection.status === "offline" || this.state.connection.status === "hosting";
    }

    isNetworkHost() {
      return this.state.connection.status === "hosting";
    }

    isClient() {
      return this.state.connection.status === "connected";
    }

    addLog(text, kind) {
      this.state.eventLog.unshift({
        id: generateId("ev"),
        time: new Date().toLocaleTimeString(),
        kind: kind || "info",
        text,
      });
      if (this.state.eventLog.length > 60) {
        this.state.eventLog.length = 60;
      }
      this.emit("log", { text, kind });
    }

    setPlayerName(newName) {
      const clean = String(newName || "").trim().slice(0, 20) || "Trainer";
      localStorage.setItem(STORAGE_KEYS.playerName, clean);
      const mySlot = this.state.players[this.state.connection.mySlotIndex];
      if (mySlot) mySlot.name = clean;
      if (this.isNetworkHost()) {
        this.pushRoomInfo();
        this.broadcastControl({ t: "players_sync", players: this.state.players });
      } else if (this.isClient()) {
        this.sendJsonToHost(CH_CONTROL, { t: "rename", name: clean });
      }
      this.emit("players");
    }

    setRelayUrl(rawUrl) {
      const clean = String(rawUrl || "").trim();
      localStorage.setItem(STORAGE_KEYS.relayUrl, clean);
      applyRelayConfig(clean);
      this.refreshRooms();
      this.emit("relay");
    }

    buildInviteUrl(customPort) {
      const port = Number(customPort) || this.state.connection.roomPort || 27462;
      const params = new URLSearchParams();
      if (global.POKEMON_RELAY_URL && global.location && global.POKEMON_RELAY_URL !== global.location.origin) {
        params.set("relay", global.POKEMON_RELAY_URL);
      }
      params.set("room", String(port));
      const origin = global.location ? global.location.origin : "http://localhost:8080";
      return `${origin}/?${params.toString()}`;
    }

    async refreshRooms() {
      try {
        const base = getRelayHttpBase();
        const res = await fetch(`${base}/api/info`, { cache: "no-store" });
        if (!res.ok) return null;
        const data = await res.json();
        this.lanInfo = data;
        this.remoteRooms = Array.isArray(data.rooms) ? data.rooms : [];
        this.emit("rooms", data);
        return data;
      } catch (_) {
        return null;
      }
    }

    disconnect() {
      if (this._roomInfoTimer) {
        clearInterval(this._roomInfoTimer);
        this._roomInfoTimer = null;
      }
      if (this._autoSaveDbTimer) {
        clearTimeout(this._autoSaveDbTimer);
        this._autoSaveDbTimer = null;
      }
      if (this.ws) {
        try {
          this.ws.onopen = null;
          this.ws.onmessage = null;
          this.ws.onclose = null;
          this.ws.onerror = null;
          this.ws.close(1000, "user_disconnect");
        } catch (_) {}
        this.ws = null;
      }
      this.state.connection.status = "offline";
      this.state.connection.role = "solo";
      this.state.connection.roomPort = 0;
      this.state.connection.myPeerId = 1;
      this.state.connection.mySlotIndex = 0;
      this._pendingLateJoin = null;
      this._pendingSlotClaim = null;
      this._suppressLocalFlashSync = false;
      this._sendClientHello = null;
      this.state.players[0].busy = false;
      this.state.players[0].inBattle = false;
      for (let i = 1; i < 4; i++) {
        this.state.players[i].connected = false;
        this.state.players[i].peerId = 0;
        this.state.players[i].partyId = null;
        this.state.players[i].busy = false;
        this.state.players[i].inBattle = false;
        this.state.players[i].tutorialComplete = false;
      }
      this._flashSavesBySlot = {};
      if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._syncPeersToWasm === "function") {
        global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      }
      this.emit("connection");
    }

    async hostRoom(customConfig, resumeSnapshot) {
      if (this._validateSessionPromise) {
        await this._validateSessionPromise.catch(() => {});
      }
      if (!this.isSignedIn()) {
        this.emit("auth_required", { action: "host" });
        this.emit("toast", {
          kind: "warn",
          title: "Sign In Required",
          message: "Please sign in or create an account before hosting a room.",
        });
        return false;
      }
      this.disconnect();
      let freshResetMode = "";
      if (resumeSnapshot) {
        await this.applySnapshotLocal(resumeSnapshot, true);
      } else if (customConfig) {
        const prevMeta = readFlashSaveMeta();
        if (prevMeta.runId) {
          await this.flushActiveFlashToCache();
        }
        const { freshReset, freshMode, ...cleanConfig } = customConfig;
        if (freshReset) {
          freshResetMode = freshMode === "littleroot" ? "littleroot" : "title";
        }
        this.state.runConfig = normalizeRunConfig(
          {
            ...cleanConfig,
            runId: cleanConfig.runId || generateId("run"),
          },
          this.state.runConfig
        );
      } else {
        this.state.runConfig = normalizeRunConfig(this.state.runConfig, makeDefaultRunConfig());
      }
      if (freshResetMode) {
        this.resetNewGame({ mode: freshResetMode });
      }
      this._pendingRunConfigSync = { fromPeerId: 1, msg: { t: "run_config_sync" } };
      if (global.PokemonWasmBridge._dispatchWasmJson(CH_CONTROL, 1, { t: "run_config_sync" })) {
        this._pendingRunConfigSync = null;
      }

      const mySlot = this.state.players[0];
      mySlot.connected = true;
      mySlot.isHost = true;
      mySlot.peerId = 1;
      mySlot.token = this.state.connection.myToken;
      mySlot.userId = (this.state.auth && this.state.auth.userId) || "";
      mySlot.username = (this.state.auth && this.state.auth.username) || "";
      mySlot.name = getOrCreatePlayerName();
      global.PokemonWasmBridge._syncPeersToWasm(this.state.players);

      const requestedPort = Number(customConfig && customConfig.roomPort) || 27462;
      const wsUrl = `${global.POKEMON_WS_URL}?role=host&room=${requestedPort}${this.state.auth && this.state.auth.sessionToken ? `&sessionToken=${encodeURIComponent(this.state.auth.sessionToken)}` : ""}`;

      this.state.connection.status = "connecting";
      this.state.connection.role = "host";
      this.state.connection.error = "";
      this.emit("connection");

      const ws = new WebSocket(wsUrl);
      ws.binaryType = "arraybuffer";
      this.ws = ws;

      ws.onopen = () => {
        this.addLog(`Connecting to relay as Host...`, "info");
      };

      ws.onmessage = (ev) => {
        if (typeof ev.data === "string") return;
        const frame = decodeFrame(ev.data);
        if (!frame) return;

        if (frame.frameType === FRAME_HOST_ASSIGNED) {
          // Host assigned room port in peerId field
          const assignedPort = frame.peerId || requestedPort;
          this.state.connection.status = "hosting";
          this.state.connection.roomPort = assignedPort;
          this.state.connection.myPeerId = 1;
          this.state.connection.mySlotIndex = 0;
          if (global.PokemonWasmBridge._dispatchWasmJson(CH_CONTROL, 1, { t: "run_config_sync" })) {
            this._pendingRunConfigSync = null;
          }
          this.broadcastControl({ t: "run_config_sync", runConfig: this.state.runConfig });
          this.pushRoomInfo();
          if (this._roomInfoTimer) clearInterval(this._roomInfoTimer);
          this._roomInfoTimer = setInterval(() => this.pushRoomInfo(), 4000);
          this.addLog(`Hosting Room #${assignedPort} (Seed: ${this.state.runConfig.seed})`, "gold");
          this.saveRunSnapshotToDb(undefined, { skipWasmSave: true });
          this.syncRunUpsertToBackend().catch(() => {});
          if (resumeSnapshot) {
            global.PokemonWasmBridge._loadSnapshotIntoWasm(resumeSnapshot);
          }
          global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
          this.emit("connection");
          this.refreshRooms();
          return;
        }

        if (frame.frameType === FRAME_PEER_CONNECTED) {
          this._onHostPeerConnected(frame.peerId);
          return;
        }

        if (frame.frameType === FRAME_PEER_DISCONNECTED) {
          this._onHostPeerDisconnected(frame.peerId);
          return;
        }

        if (frame.frameType === FRAME_DATA) {
          this._handleIncomingDataFrame(frame.peerId, frame.channel, frame.payload);
        }
      };

      ws.onclose = (ev) => {
        if (this.ws !== ws) return;
        this.ws = null;
        if (this._roomInfoTimer) {
          clearInterval(this._roomInfoTimer);
          this._roomInfoTimer = null;
        }
        this.state.connection.status = "offline";
        this.state.connection.error = ev.reason || "Relay disconnected";
        this.addLog(`Host socket closed (${ev.code} ${ev.reason || ""})`, "death");
        this.emit("connection");
      };

      ws.onerror = () => {
        this.state.connection.error = "WebSocket relay connection failed";
        this.emit("connection");
      };
    }

    joinRoom(roomPort, options) {
      if (!this.isSignedIn()) {
        this.emit("auth_required", { action: "join", port: Number(roomPort) || 0 });
        this.emit("toast", {
          kind: "warn",
          title: "Sign In Required",
          message: "Please sign in or create an account before joining a room.",
        });
        return false;
      }
      const port = Number(roomPort) || 27462;
      const joinOpts = options && typeof options === "object" ? options : {};
      this.disconnect();
      this._pendingSlotClaim = null;
      this._suppressLocalFlashSync = true;
      this._joinOptions = {
        confirmedProgressed: Boolean(joinOpts.confirmedProgressed),
        runId: typeof joinOpts.runId === "string" ? joinOpts.runId : "",
        claimSlotIndex:
          Number.isInteger(Number(joinOpts.claimSlotIndex)) && Number(joinOpts.claimSlotIndex) >= 1
            ? Number(joinOpts.claimSlotIndex)
            : -1,
        joinAsNew: Boolean(joinOpts.joinAsNew),
      };
      const sessionTok = (this.state.auth && this.state.auth.sessionToken) || "";
      const wsUrl = `${global.POKEMON_WS_URL}?role=client&room=${port}${sessionTok ? `&sessionToken=${encodeURIComponent(sessionTok)}` : ""}`;

      this.state.connection.status = "connecting";
      this.state.connection.role = "client";
      this.state.connection.roomPort = port;
      this.state.connection.error = "";
      this.emit("connection");

      const ws = new WebSocket(wsUrl);
      ws.binaryType = "arraybuffer";
      this.ws = ws;
      let helloSent = false;

      const sendClientHello = (extraOpts) => {
        if (ws.readyState !== WebSocket.OPEN) return;
        const ex = extraOpts && typeof extraOpts === "object" ? extraOpts : {};
        if (!ex.force && helloSent) return;
        helloSent = true;
        const localMeta = readFlashSaveMeta();
        const myAuth = this.state.auth || {};
        const roomMeta = (this.remoteRooms || []).find((r) => Number(r && r.game_port) === port);
        const targetRunId = (this._joinOptions && this._joinOptions.runId) || (roomMeta && roomMeta.run_id) || "";
        const joinedCloudRun =
          targetRunId && this.cloudRuns && Array.isArray(this.cloudRuns.joinedRuns)
            ? this.cloudRuns.joinedRuns.find((r) => r && r.runId === targetRunId)
            : null;
        const preferredSlotIdx =
          joinedCloudRun &&
          Number.isInteger(Number(joinedCloudRun.mySlotIdx)) &&
          Number(joinedCloudRun.mySlotIdx) >= 1 &&
          Number(joinedCloudRun.mySlotIdx) <= 3
            ? Number(joinedCloudRun.mySlotIdx)
            : -1;
        this.sendJsonToHost(CH_CONTROL, {
          t: "hello",
          token: this.state.connection.myToken,
          sessionToken: myAuth.sessionToken || "",
          userId: myAuth.status === "signed_in" ? myAuth.userId : "",
          username: myAuth.status === "signed_in" ? myAuth.username : "",
          name: getOrCreatePlayerName(),
          localRunId: localMeta.runId || "",
          localParty: localMeta.runId ? this.state.players[0].party : [],
          money: localMeta.runId ? this.state.players[0].money : 0,
          bagItems: localMeta.runId ? this.state.players[0].bagItems : [],
          tutorialComplete: localMeta.runId ? Boolean(this.state.players[0].tutorialComplete) : false,
          preferredSlotIndex: preferredSlotIdx,
          claimSlotIndex:
            ex.claimSlotIndex !== undefined ? ex.claimSlotIndex : this._joinOptions.claimSlotIndex,
          joinAsNew: ex.joinAsNew !== undefined ? Boolean(ex.joinAsNew) : this._joinOptions.joinAsNew,
        });
      };
      this._sendClientHello = sendClientHello;

      ws.onopen = () => {
        // Wait for relay FRAME_PEER_CONNECTED(1) or FRAME_REJECTED before marking connected
        this.addLog(`WebSocket opened for Room #${port}; awaiting relay handshake...`, "info");
      };

      ws.onmessage = (ev) => {
        if (typeof ev.data === "string") return;
        const frame = decodeFrame(ev.data);
        if (!frame) return;

        if (frame.frameType === FRAME_REJECTED) {
          const rawReason = textDecoder.decode(frame.payload || new Uint8Array());
          const parsedReason = tryDecodeJsonPayload(frame.payload);
          const code = parsedReason && parsedReason.code ? String(parsedReason.code) : "ROOM_REJECTED";
          const message = (parsedReason && parsedReason.message) || rawReason || "Rejected by room";
          this.state.connection.error = message;
          this.addLog(`Join rejected (${code}): ${message}`, "death");
          this.emit("join_rejected", { port, code, reason: code, message });
          this.disconnect();
          return;
        }

        if (frame.frameType === FRAME_PEER_CONNECTED && frame.peerId === 1) {
          const handshake = tryDecodeJsonPayload(frame.payload);
          if (handshake && handshake.self_peer_id) {
            this.state.connection.myPeerId = Number(handshake.self_peer_id);
          }
          if (handshake && handshake.room_port) {
            this.state.connection.roomPort = Number(handshake.room_port);
          }
          this.state.connection.status = "connected";
          this.addLog(`Connected to Room #${this.state.connection.roomPort}! Syncing campaign state...`, "info");
          sendClientHello();
          this.emit("connection");
          return;
        }

        if (frame.frameType === FRAME_PEER_DISCONNECTED && frame.peerId === 1) {
          this.addLog("Host disconnected from room.", "death");
          this.emit("toast", { kind: "danger", title: "Host Disconnected", message: "The Host closed the room." });
          this.disconnect();
          return;
        }

        if (frame.frameType === FRAME_DATA) {
          this._handleIncomingDataFrame(frame.peerId, frame.channel, frame.payload);
        }
      };

      ws.onclose = (ev) => {
        if (this.ws !== ws) return;
        this.ws = null;
        this._pendingLateJoin = null;
        for (let i = 1; i < 4; i++) {
          this.state.players[i].connected = false;
          this.state.players[i].peerId = 0;
          this.state.players[i].busy = false;
          this.state.players[i].inBattle = false;
          this.state.players[i].tutorialComplete = false;
        }
        if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._syncPeersToWasm === "function") {
          global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
        }
        if (this.state.connection.status !== "offline") {
          this.state.connection.status = "offline";
          this.state.connection.error = ev.reason || "Disconnected";
          this.addLog(`Disconnected from Room #${port}`, "info");
          this.emit("connection");
        }
      };

      ws.onerror = () => {
        this.state.connection.error = "Unable to connect to relay";
        this.emit("connection");
      };
    }

    confirmLateJoin() {
      const pending = this._pendingLateJoin;
      if (!pending) return false;
      this._pendingLateJoin = null;
      this._joinOptions = { confirmedProgressed: true };
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        this.state.connection.error = "Connection closed before late join confirmation";
        this.emit("connection");
        return false;
      }
      this._finalizeClientWelcome(pending.msg, pending.meta);
      return true;
    }

    cancelLateJoin() {
      const hadPending = Boolean(this._pendingLateJoin);
      this._pendingLateJoin = null;
      this._joinOptions = { confirmedProgressed: false };
      this.disconnect();
      return hadPending;
    }

    _getRunProgressSummary(worldObj, soulLinkObj) {
      const world = worldObj || this.state.world || {};
      const sl = soulLinkObj || this.state.soulLink || {};
      const locks = sl.routeLocks || {};
      const badgeCount = Math.max(0, Number(world.badgesCount) || 0);
      const graveyard = Array.isArray(sl.graveyard) ? sl.graveyard : [];
      const deathCount = graveyard.length;
      const caughtRouteIds = new Set();
      const deadRoutes = [];
      for (const lock of Object.values(locks)) {
        if (!lock) continue;
        const hasMembers = lock.members && Object.keys(lock.members).length > 0;
        if (hasMembers && lock.routeId) {
          caughtRouteIds.add(lock.routeId);
        }
        if (lock.dead || lock.failed) {
          const trimmedStr = typeof lock.linkId === "string" ? lock.linkId.trim() : "";
          const linkIdVal =
            typeof lock.linkId === "string"
              ? (trimmedStr && trimmedStr !== "0" ? trimmedStr : "")
              : Number.isFinite(Number(lock.linkId)) && Number(lock.linkId) > 0
                ? Number(lock.linkId)
                : "";
          deadRoutes.push({
            routeId: lock.routeId,
            routeName: lock.routeName || `Route ${lock.routeId}`,
            linkId: linkIdVal,
            dead: Boolean(lock.dead),
            failed: Boolean(lock.failed),
          });
        }
      }
      return {
        badgeCount,
        deathCount,
        caughtRoutesCount: caughtRouteIds.size,
        graveyard,
        deadRoutes,
      };
    }

    pushRoomInfo() {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN || !this.isNetworkHost()) return;
      const connectedCount = this.state.players.filter((p) => p.connected).length;
      const cfg = this.state.runConfig;
      const summary = this._getRunProgressSummary();
      const gameOver = Boolean(cfg.gameOver);
      const info = {
        host_name: this.state.players[0].name,
        host_user_id: (this.state.auth && this.state.auth.userId) || "",
        room_name: cfg.roomName || `${this.state.players[0].name}'s Room`,
        seed: cfg.seed,
        randomizer_preset: cfg.randomizerPreset,
        nuzlocke_preset: cfg.nuzlockePreset || "none",
        soul_link_mode: cfg.soulLinkMode,
        speed_multiplier: cfg.speedMultiplier,
        run_id: cfg.runId,
        member_count: connectedCount,
        member_limit: 4,
        badge_count: summary.badgeCount,
        death_count: summary.deathCount,
        caught_routes_count: summary.caughtRoutesCount,
        game_over: gameOver,
        state: gameOver ? "game_over" : "roaming_hoenn",
        joinable: !gameOver && connectedCount < 4,
      };
      this.ws.send(encodeFrame(FRAME_ROOM_INFO, 0, CH_CONTROL, encodeJsonPayload(info)));
    }

    kickPlayer(targetSlotIndex) {
      if (!this.isNetworkHost() || !this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
      const idx = Number(targetSlotIndex);
      if (!Number.isInteger(idx) || idx <= 0 || idx >= 4) return false;
      const target = this.state.players[idx];
      if (!target || !target.connected || !target.peerId) return false;
      const kickedPeerId = target.peerId;
      try {
        this.ws.send(encodeFrame(FRAME_KICK_PEER, kickedPeerId, 0));
      } catch (_) {}
      this._onHostPeerDisconnected(kickedPeerId);
      return true;
    }

    // --- Raw Frame Senders ---
    sendBinary(channel, targetPeerId, uint8Bytes) {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
      const numericTarget = Number(targetPeerId);
      const target = this.isNetworkHost()
        ? (Number.isFinite(numericTarget) ? numericTarget : 0)
        : (Number.isFinite(numericTarget) && numericTarget >= 0 ? numericTarget : 1);
      this.ws.send(encodeFrame(FRAME_DATA, target, channel, uint8Bytes));
      return true;
    }

    sendJson(channel, targetPeerId, obj) {
      return this.sendBinary(channel, targetPeerId, encodeJsonPayload(obj));
    }

    sendJsonToHost(channel, obj) {
      return this.sendJson(channel, 1, obj);
    }

    broadcastControl(obj) {
      if (!this.isNetworkHost()) return;
      this.sendJson(CH_CONTROL, 0, obj);
    }

    broadcastChannel(channel, obj) {
      if (!this.isNetworkHost()) return;
      this.sendJson(channel, 0, obj);
    }

    // --- Host Peer Lifecycle ---
    _onHostPeerConnected(peerId) {
      this.addLog(`Peer #${peerId} joined the relay.`, "info");
      this.pushRoomInfo();
    }

    _onHostPeerDisconnected(peerId) {
      const slot = this.state.players.find((p) => p.peerId === peerId);
      if (slot) {
        slot.connected = false;
        slot.peerId = 0;
        slot.inBattle = false;
        slot.busy = false;
        slot.tutorialComplete = false;
        global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
        this.addLog(`${slot.name} (P${slot.slotIndex + 1}) disconnected.`, "info");
        if (this.state.trade.active && this.state.trade.partnerPeerId === peerId) {
          this.state.trade.active = false;
          this.state.trade.statusText = "Partner disconnected; trade cancelled.";
          this.emit("trade");
        }
        this.broadcastControl({ t: "players_sync", players: this.state.players });
        this.pushRoomInfo();
        this.emit("players");
      }
    }

    // --- Incoming Frame Router ---
    _handleIncomingDataFrame(fromPeerId, channel, payloadBytes) {
      const msg = tryDecodeJsonPayload(payloadBytes);
      if (!msg || typeof msg.t !== "string") {
        // Pure binary engine packet -> forward directly to WASM
        global.PokemonWasmBridge._dispatchBinaryToWasm(channel, fromPeerId, payloadBytes);
        if (this.isNetworkHost() && channel === CH_SOUL_LINK) {
          this._handleBinarySoulLinkPacket(fromPeerId, payloadBytes);
        }
        return;
      }

      if (this.isNetworkHost()) {
        this._handleHostMessage(fromPeerId, channel, msg, { rawBytes: payloadBytes });
      } else {
        this._handleClientMessage(fromPeerId, channel, msg, { rawBytes: payloadBytes });
      }
    }

    _getHostSlotGateReason(slotA, slotB, opts) {
      const options = opts && typeof opts === "object" ? opts : {};
      const requireTargetTutorial = options.requireTargetTutorial !== false;
      if (this.state.runConfig && this.state.runConfig.gameOver) {
        return "This Soul-Link Nuzlocke run has ended (Game Over).";
      }
      if (!slotA || !slotA.connected) {
        return "Requester is not connected.";
      }
      if (!slotA.tutorialComplete) {
        return `${slotA.name || "You"} must finish the Littleroot tutorial first.`;
      }
      if (slotB) {
        if (!slotB.connected) {
          return `${slotB.name || "Target trainer"} is not connected.`;
        }
        if (requireTargetTutorial && !slotB.tutorialComplete) {
          return `${slotB.name || "Target trainer"} is still in the Littleroot tutorial.`;
        }
      }
      return null;
    }

    _notifyHostGateRejection(requesterPeerId, action, reason) {
      const message = String(reason || "Action restricted during tutorial or Game Over.");
      this.addLog(message, "warn");
      if (requesterPeerId === 1) {
        this.emit("feature_gated", { action, reason: message, message });
        this.emit("toast", { kind: "warn", title: "Action Restricted", message });
      } else if (requesterPeerId > 1) {
        this.sendJson(CH_CONTROL, requesterPeerId, { t: "feature_gated", action, reason: message, message });
      }
    }

    _markRunGameOver(who, opts) {
      const options = opts && typeof opts === "object" ? opts : {};
      const alreadyOver = Boolean(this.state.runConfig && this.state.runConfig.gameOver);
      if (!this.state.runConfig) this.state.runConfig = makeDefaultRunConfig();
      this.state.runConfig.gameOver = true;
      const label = who ? `${who} ` : "";
      if (!this._gameOverToastShown) {
        this._gameOverToastShown = true;
        this.addLog(`${label}NUZLOCKE WHITE OUT! Run ended by Nuzlocke Game Over rule.`, "death");
        if (!options.silentToast) {
          this.emit("toast", {
            kind: "danger",
            title: "Nuzlocke Game Over",
            message: who ? `${who} whited out under Nuzlocke Game Over rules!` : "Whiteout ended the Nuzlocke run!",
          });
        }
      }
      if (!alreadyOver && this.isNetworkHost()) {
        this.broadcastControl({ t: "run_config_sync", runConfig: this.state.runConfig });
        this.pushRoomInfo();
        this.saveRunSnapshotToDb();
      }
      this.emit("config");
      this.emit("all");
    }

    _handleLocalBroadcastJsonNotification(channel, msg) {
      if (!msg || typeof msg.t !== "string") return;
      if (channel === CH_SOUL_LINK && msg.t === "nuzlocke_gameover") {
        const mySlot = this.state.players[this.state.connection.mySlotIndex];
        this._markRunGameOver(mySlot ? mySlot.name : "");
      }
    }

    _handleHostMessage(fromPeerId, channel, msg, meta) {
      const fromLocalWasm = Boolean(meta && meta.fromLocalWasm);
      const senderSlot = this.state.players.find((p) => p.peerId === fromPeerId);

      if (channel === CH_CONTROL) {
        switch (msg.t) {
          case "hello": {
            const myRunId = (this.state.runConfig && this.state.runConfig.runId) || "";
            const verifiedUserId =
              msg.authVerified && msg.authUserId
                ? String(msg.authUserId).slice(0, 64)
                : "";
            const verifiedUsername =
              msg.authVerified && msg.authUsername
                ? String(msg.authUsername).slice(0, 32)
                : "";
            const incomingToken = String(msg.token || "").slice(0, 64);
            const requestedClaimIdx =
              Number.isInteger(Number(msg.claimSlotIndex)) ? Number(msg.claimSlotIndex) : -1;
            const joinAsNew = Boolean(msg.joinAsNew);

            // 1) Primary match by verified userId; fallback to clientToken ONLY for legacy unauthenticated slots.
            let existingIdx = -1;
            if (verifiedUserId) {
              existingIdx = this.state.players.findIndex(
                (p, idx) => idx > 0 && p.userId && p.userId === verifiedUserId
              );
            }
            if (existingIdx === -1 && !verifiedUserId && incomingToken) {
              existingIdx = this.state.players.findIndex(
                (p, idx) => idx > 0 && !p.userId && p.token && p.token === incomingToken
              );
            }
            // If a previously unauthenticated local slot matches by token and caller now signed in, upgrade it if unclaimed
            if (existingIdx === -1 && verifiedUserId && incomingToken) {
              const legacyIdx = this.state.players.findIndex(
                (p, idx) => idx > 0 && !p.connected && !p.userId && p.token && p.token === incomingToken
              );
              if (legacyIdx !== -1) existingIdx = legacyIdx;
            }

            if (this.state.runConfig && this.state.runConfig.gameOver && existingIdx === -1) {
              this.sendJson(CH_CONTROL, fromPeerId, {
                t: "join_rejected",
                code: "RUN_GAME_OVER",
                reason: "RUN_GAME_OVER",
                message: "This Soul-Link Nuzlocke run has ended (Game Over).",
              });
              return;
            }

            let targetIdx = existingIdx;
            let isClaimedSlot = false;
            const preferredSlotIdx = Number.isInteger(Number(msg.preferredSlotIndex))
              ? Number(msg.preferredSlotIndex)
              : -1;
            if (targetIdx === -1 && requestedClaimIdx >= 1 && requestedClaimIdx <= 3) {
              const candidate = this.state.players[requestedClaimIdx];
              if (candidate && !candidate.connected) {
                targetIdx = requestedClaimIdx;
                isClaimedSlot = true;
              }
            }
            if (targetIdx === -1 && verifiedUserId && preferredSlotIdx >= 1 && preferredSlotIdx <= 3) {
              const pref = this.state.players[preferredSlotIdx];
              if (pref && !pref.connected && (!pref.userId || pref.userId === verifiedUserId)) {
                targetIdx = preferredSlotIdx;
                isClaimedSlot = true;
              }
            }

            const emptyIdx = this.state.players.findIndex(
              (p, idx) =>
                idx > 0 &&
                !p.connected &&
                !p.token &&
                !p.userId &&
                (!p.party || p.party.length === 0) &&
                !p.tutorialComplete &&
                !(this._flashSavesBySlot && this._flashSavesBySlot[idx])
            );

            if (targetIdx === -1) {
              const offlineClaimable = this.state.players.filter(
                (p, idx) =>
                  idx > 0 &&
                  !p.connected &&
                  (p.userId ||
                    p.token ||
                    (p.party && p.party.length > 0) ||
                    p.tutorialComplete ||
                    (this._flashSavesBySlot && this._flashSavesBySlot[idx]))
              );
              if (!joinAsNew && offlineClaimable.length > 0) {
                this.sendJson(CH_CONTROL, fromPeerId, {
                  t: "slot_claim_required",
                  runId: myRunId,
                  roomName: (this.state.runConfig && this.state.runConfig.roomName) || "Hoenn Run",
                  hasEmptySlot: emptyIdx !== -1,
                  offlineSlots: offlineClaimable.map((p) => ({
                    slotIndex: p.slotIndex,
                    name: p.name,
                    username: p.username || "",
                    userId: p.userId || "",
                    tutorialComplete: Boolean(p.tutorialComplete),
                    flashRev: Number(p.flashRev) || 0,
                    party: Array.isArray(p.party) ? p.party : [],
                  })),
                });
                return;
              }
              targetIdx = emptyIdx;
            }

            if (targetIdx === -1) {
              this.sendJson(CH_CONTROL, fromPeerId, {
                t: "join_rejected",
                code: "ROOM_FULL",
                reason: "ROOM_FULL",
                message: "Room is full (4/4 trainers).",
              });
              return;
            }

            const slot = this.state.players[targetIdx];
            const isReturningSlot = existingIdx !== -1 || isClaimedSlot;
            const isSameRunClient =
              isReturningSlot || Boolean(myRunId && msg.localRunId && msg.localRunId === myRunId);
            slot.peerId = fromPeerId;
            slot.connected = true;
            slot.isHost = false;
            slot.inBattle = false;
            slot.busy = false;
            if (verifiedUserId) {
              slot.userId = verifiedUserId;
              slot.username = verifiedUsername || slot.username || "";
            }
            slot.token = String(incomingToken || slot.token || generateId("tok")).slice(0, 64);
            if (!isReturningSlot || !slot.name || slot.name === `Slot ${targetIdx + 1}`) {
              slot.name = String(msg.name || verifiedUsername || `Trainer ${targetIdx + 1}`).slice(0, 20);
            } else if (msg.name && !isClaimedSlot) {
              slot.name = String(msg.name).slice(0, 20);
            }
            if (!isSameRunClient) {
              slot.party = [];
              slot.money = 0;
              slot.bagItems = [];
              slot.tutorialComplete = false;
              slot.flashRev = 0;
            } else {
              slot.tutorialComplete = Boolean(slot.tutorialComplete || msg.tutorialComplete);
              if ((!slot.party || slot.party.length === 0) && Array.isArray(msg.localParty)) {
                slot.party = msg.localParty.slice(0, 6).map(sanitizeMon).filter(Boolean);
              }
              if (typeof msg.money === "number" && Number.isFinite(msg.money) && slot.money === 0) {
                slot.money = Math.max(0, Math.floor(msg.money));
              }
              if (Array.isArray(msg.bagItems) && msg.bagItems.length > 0 && slot.bagItems.length === 0) {
                slot.bagItems = sanitizeBagItems(msg.bagItems);
              }
            }

            const flashBySlot = this._flashSavesBySlot || {};
            const slotFlashSaveB64 =
              isReturningSlot && typeof flashBySlot[targetIdx] === "string" ? flashBySlot[targetIdx] : "";
            if (!isReturningSlot && flashBySlot[targetIdx]) {
              delete flashBySlot[targetIdx];
            }
            global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
            this.sendJson(CH_CONTROL, fromPeerId, {
              t: "welcome",
              assignedPeerId: fromPeerId,
              assignedSlot: targetIdx,
              isReturningSlot,
              slotFlashRev: Number(slot.flashRev) || 0,
              runConfig: this.state.runConfig,
              players: this.state.players,
              world: this.state.world,
              soulLink: this.state.soulLink,
              slotFlashSaveB64,
              hostFlashSaveB64: "",
            });
            this.broadcastControl({ t: "players_sync", players: this.state.players });
            this.pushRoomInfo();
            this.saveRunSnapshotToDb(undefined, { silentBroadcast: true, skipWasmSave: true }).catch(() => {});
            this.syncRunUpsertToBackend().catch(() => {});
            this.addLog(`${slot.name} ${isReturningSlot ? "resumed" : "claimed"} Trainer Slot P${targetIdx + 1}.`, "gold");
            this.emit("players");
            break;
          }
          case "rename": {
            if (!senderSlot) return;
            senderSlot.name = String(msg.name || senderSlot.name).slice(0, 20);
            global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
            this.broadcastControl({ t: "players_sync", players: this.state.players });
            this.pushRoomInfo();
            this.emit("players");
            break;
          }
          case "speed_set": {
            // Host-only invariant: clients CANNOT mutate global speed!
            this.sendJson(CH_CONTROL, fromPeerId, {
              t: "speed_set",
              speed: this.state.runConfig.speedMultiplier,
              rejected: true,
            });
            break;
          }
          case "party_pair": {
            this._hostHandlePartyPair(senderSlot ? senderSlot.slotIndex : 0, Number(msg.targetSlot));
            break;
          }
          case "party_leave": {
            this._hostHandlePartyLeave(senderSlot ? senderSlot.slotIndex : 0);
            break;
          }
          case "teleport_request": {
            this._hostHandleTeleportRequest(fromPeerId, Number(msg.targetPeerId));
            break;
          }
          case "state_sync": {
            if (!senderSlot) return;
            const prevBusy = senderSlot.busy;
            const prevInBattle = senderSlot.inBattle;
            const prevTutorial = senderSlot.tutorialComplete;
            const hasSavedProgress =
              (senderSlot.flashRev || 0) > 0 ||
              (Array.isArray(senderSlot.party) && senderSlot.party.length > 0) ||
              Boolean(senderSlot.tutorialComplete) ||
              Boolean(this._flashSavesBySlot && this._flashSavesBySlot[senderSlot.slotIndex]);
            const isBlankTutorialSync =
              Array.isArray(msg.party) &&
              msg.party.length === 0 &&
              !msg.tutorialComplete &&
              !msg.explicitReset;
            if (hasSavedProgress && isBlankTutorialSync) {
              // Never allow a blank/tutorial state_sync to overwrite a slot that already has saved progress.
              break;
            }
            if (Array.isArray(msg.party)) {
              senderSlot.party = msg.party.slice(0, 6).map(sanitizeMon).filter(Boolean);
            }
            if (typeof msg.mapName === "string") senderSlot.mapName = msg.mapName.slice(0, 48);
            if (typeof msg.mapGroup === "number" && Number.isFinite(msg.mapGroup)) senderSlot.mapGroup = Math.floor(msg.mapGroup);
            if (typeof msg.mapNum === "number" && Number.isFinite(msg.mapNum)) senderSlot.mapNum = Math.floor(msg.mapNum);
            if (typeof msg.x === "number" && Number.isFinite(msg.x)) senderSlot.x = Math.floor(msg.x);
            if (typeof msg.y === "number" && Number.isFinite(msg.y)) senderSlot.y = Math.floor(msg.y);
            if (typeof msg.inBattle === "boolean") senderSlot.inBattle = msg.inBattle;
            if (typeof msg.busy === "boolean") senderSlot.busy = msg.busy;
            if (typeof msg.tutorialComplete === "boolean") senderSlot.tutorialComplete = msg.tutorialComplete;
            if (typeof msg.money === "number" && Number.isFinite(msg.money)) senderSlot.money = Math.max(0, Math.floor(msg.money));
            if (Array.isArray(msg.bagItems)) senderSlot.bagItems = sanitizeBagItems(msg.bagItems);
            this._unlockMapKey(senderSlot.mapGroup, senderSlot.mapNum);
            if (
              prevBusy !== senderSlot.busy ||
              prevInBattle !== senderSlot.inBattle ||
              prevTutorial !== senderSlot.tutorialComplete
            ) {
              global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
            }
            this.broadcastControl({ t: "players_sync", players: this.state.players });
            this.emit("players");
            break;
          }
          case "flash_save_sync": {
            if (!senderSlot || typeof msg.flashSaveB64 !== "string" || msg.flashSaveB64.length < 4096) break;
            const bytes = decodeFlashBase64(msg.flashSaveB64);
            if (!hasValidSectorSignature(bytes)) break;
            const prevRev = Math.max(0, Math.floor(Number(senderSlot.flashRev) || 0));
            const incomingRev = Number.isFinite(Number(msg.flashRev))
              ? Math.max(1, Math.floor(Number(msg.flashRev)))
              : prevRev + 1;
            if (incomingRev < prevRev) break;
            senderSlot.flashRev = Math.max(prevRev + 1, incomingRev);
            if (!this._flashSavesBySlot) this._flashSavesBySlot = {};
            this._flashSavesBySlot[senderSlot.slotIndex] = msg.flashSaveB64;
            if (this._hostFlashPersistTimer) clearTimeout(this._hostFlashPersistTimer);
            this._hostFlashPersistTimer = setTimeout(() => {
              this._hostFlashPersistTimer = null;
              this.saveRunSnapshotToDb(undefined, { silentBroadcast: true, skipWasmSave: true }).catch(() => {});
            }, 150);
            const curRunId = this.state.runConfig && this.state.runConfig.runId;
            if (curRunId && msg.needsHostBackup && this.state.auth && this.state.auth.status === "signed_in") {
              this.syncSlotFlashToBackend(
                curRunId,
                senderSlot.slotIndex,
                msg.flashSaveB64,
                senderSlot.flashRev,
                senderSlot,
                { expectedRev: prevRev }
              ).catch(() => {});
            }
            break;
          }
          case "flash_save_clear": {
            if (senderSlot) {
              senderSlot.flashRev = 0;
              if (this._flashSavesBySlot) {
                delete this._flashSavesBySlot[senderSlot.slotIndex];
              }
            }
            break;
          }
        }
      } else if (channel === CH_OVERWORLD) {
        if (msg.t === "pos" && senderSlot) {
          senderSlot.mapGroup = msg.mapGroup ?? senderSlot.mapGroup;
          senderSlot.mapNum = msg.mapNum ?? senderSlot.mapNum;
          senderSlot.mapName = msg.mapName || senderSlot.mapName;
          senderSlot.x = msg.x ?? senderSlot.x;
          senderSlot.y = msg.y ?? senderSlot.y;
          senderSlot.inBattle = Boolean(msg.inBattle);
          this._unlockMapKey(senderSlot.mapGroup, senderSlot.mapNum);
          this.broadcastChannel(CH_OVERWORLD, {
            t: "pos",
            slotIndex: senderSlot.slotIndex,
            mapGroup: senderSlot.mapGroup,
            mapNum: senderSlot.mapNum,
            mapName: senderSlot.mapName,
            x: senderSlot.x,
            y: senderSlot.y,
            inBattle: senderSlot.inBattle,
          });
          this.emit("overworld");
        }
      } else if (channel === CH_WORLD_SYNC) {
        if (msg.t === "badge_unlock") {
          this.unlockBadge(Number(msg.badgeIndex), { fromPeerId, fromLocalWasm });
        } else if (msg.t === "world_flag") {
          const flagId = Number(msg.flagId);
          if (Number.isInteger(flagId) && flagId > 0) {
            const valBool = Boolean(msg.val);
            if (!this.state.world.globalFlags || typeof this.state.world.globalFlags !== "object") {
              this.state.world.globalFlags = {};
            }
            const prevBool = Boolean(this.state.world.globalFlags[String(flagId)]);
            this.state.world.globalFlags[String(flagId)] = valBool;
            if (!fromLocalWasm) {
              global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, fromPeerId || 1, {
                t: "world_flag",
                flagId,
                val: valBool ? 1 : 0,
              });
            }
            if (prevBool !== valBool) {
              if (this.isNetworkHost()) {
                this.broadcastChannel(CH_WORLD_SYNC, { t: "world_flag", flagId, val: valBool ? 1 : 0 });
              }
              this.saveRunSnapshotToDb(undefined, { silentBroadcast: true, skipWasmSave: true }).catch(() => {});
              this.emit("world");
            }
          }
        } else if (msg.t === "world_var") {
          const varId = Number(msg.varId);
          const valNum = Math.max(0, Math.floor(Number(msg.val) || 0));
          if (Number.isInteger(varId) && varId > 0) {
            if (!this.state.world.globalVars || typeof this.state.world.globalVars !== "object") {
              this.state.world.globalVars = {};
            }
            const prevVal = this.state.world.globalVars[String(varId)];
            this.state.world.globalVars[String(varId)] = valNum;
            if (!fromLocalWasm) {
              global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, fromPeerId || 1, {
                t: "world_var",
                varId,
                val: valNum,
              });
            }
            if (prevVal !== valNum) {
              if (this.isNetworkHost()) {
                this.broadcastChannel(CH_WORLD_SYNC, { t: "world_var", varId, val: valNum });
              }
              this.saveRunSnapshotToDb(undefined, { silentBroadcast: true, skipWasmSave: true }).catch(() => {});
              this.emit("world");
            }
          }
        } else if (msg.t === "cutscene_trigger") {
          this._hostStartCutscene(
            msg.cutsceneId,
            msg.title,
            senderSlot ? senderSlot.name : "Peer",
            { fromPeerId, fromLocalWasm }
          );
        } else if (msg.t === "cutscene_ack" && this.state.activeCutscene && senderSlot) {
          const nextStatus = String(msg.status || "watching").slice(0, 32);
          if (nextStatus === "done") {
            const expectedNumId =
              this.state.activeCutscene.cutsceneNumericId ||
              toNumericCutsceneId(this.state.activeCutscene.cutsceneId);
            const msgNumId = msg.cutsceneId !== undefined ? toNumericCutsceneId(msg.cutsceneId) : expectedNumId;
            if (msgNumId === expectedNumId || String(msg.cutsceneId || "") === String(this.state.activeCutscene.cutsceneId)) {
              this.finishActiveCutscene({ fromPeerId, fromLocalWasm });
            }
            return;
          }
          const prevStatus = this.state.activeCutscene.peerStates[senderSlot.slotIndex];
          this.state.activeCutscene.peerStates[senderSlot.slotIndex] = nextStatus;
          if (prevStatus !== nextStatus) {
            this.broadcastChannel(CH_WORLD_SYNC, {
              t: "cutscene_update",
              cutsceneId:
                this.state.activeCutscene.cutsceneNumericId ||
                toNumericCutsceneId(this.state.activeCutscene.cutsceneId),
              title: this.state.activeCutscene.title,
              cutscene: this.state.activeCutscene,
            });
          }
          this.emit("cutscene");
        }
      } else if (channel === CH_BATTLE) {
        if (msg.t === "battle_invite" || msg.t === "battle_accept" || msg.t === "battle_decline") {
          const targetPeer = Number(msg.targetPeerId);
          if (msg.t === "battle_invite" || msg.t === "battle_accept") {
            const targetSlot = this.state.players.find((p) => p.peerId === targetPeer);
            const gateReason = this._getHostSlotGateReason(senderSlot, targetSlot);
            if (gateReason) {
              this._notifyHostGateRejection(fromPeerId, "battle_invite", gateReason);
              return;
            }
          }
          if (targetPeer === 1) {
            this._receiveBattleControlMsg(fromPeerId, msg);
          } else if (targetPeer > 1) {
            this.sendJson(CH_BATTLE, targetPeer, { ...msg, fromPeerId, fromName: senderSlot ? senderSlot.name : "Peer" });
          }
        }
      } else if (channel === CH_SOUL_LINK) {
        if (msg.t === "route_catch" && senderSlot) {
          this.registerRouteCatch(senderSlot.slotIndex, msg.routeId, msg.routeName, msg.mon);
        } else if (msg.t === "mon_fainted" && senderSlot) {
          this.reportMonFainted(senderSlot.slotIndex, msg.monUid, msg.cause);
        } else if (msg.t === "nuzlocke_gameover" && senderSlot) {
          const who = senderSlot.name || "Trainer";
          this._markRunGameOver(who);
          if (fromPeerId !== 1 && this.isNetworkHost()) {
            this.broadcastChannel(CH_SOUL_LINK, { t: "nuzlocke_gameover", fromName: who });
          }
        }
      } else if (channel === CH_TRADE) {
        this._hostHandleTradeMsg(fromPeerId, senderSlot, msg);
      }
    }

    async confirmSlotClaim(slotIndex) {
      const pending = this._pendingSlotClaim;
      if (!pending || !this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
      const cleanIdx = Math.max(1, Math.min(3, Math.floor(Number(slotIndex) || 1)));
      this._pendingSlotClaim = null;
      this._joinOptions = {
        confirmedProgressed: true,
        claimSlotIndex: cleanIdx,
        joinAsNew: false,
      };
      if (pending.runId && this.state.auth && this.state.auth.status === "signed_in") {
        await this.claimRunSlotOnBackend(pending.runId, cleanIdx).catch(() => {});
      }
      if (typeof this._sendClientHello === "function") {
        this._sendClientHello({ force: true, claimSlotIndex: cleanIdx, joinAsNew: false });
      }
      return true;
    }

    joinAsNewFromClaim() {
      const pending = this._pendingSlotClaim;
      if (!pending || !this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
      this._pendingSlotClaim = null;
      this._joinOptions = {
        ...this._joinOptions,
        claimSlotIndex: -1,
        joinAsNew: true,
      };
      if (typeof this._sendClientHello === "function") {
        this._sendClientHello({ force: true, claimSlotIndex: -1, joinAsNew: true });
      }
      return true;
    }

    cancelSlotClaim() {
      const hadPending = Boolean(this._pendingSlotClaim);
      this._pendingSlotClaim = null;
      this.disconnect();
      return hadPending;
    }

    async _resolveBestWelcomeFlash(msg, meta) {
      if (this._validateSessionPromise) {
        await this._validateSessionPromise.catch(() => {});
      }
      const myIdx = Math.max(0, Math.min(3, Math.floor(Number(msg.assignedSlot) || 0)));
      const welcomeRunId = (msg.runConfig && msg.runConfig.runId) || "";
      const hostFlashB64 = typeof msg.slotFlashSaveB64 === "string" ? msg.slotFlashSaveB64 : "";
      const hostRev = Math.max(0, Math.floor(Number(msg.slotFlashRev) || 0));
      const hostValid = hostFlashB64 && hasValidSectorSignature(decodeFlashBase64(hostFlashB64)) ? hostFlashB64 : "";

      let idbEntry = welcomeRunId ? await this.getCachedSlotFlashLocal(welcomeRunId, myIdx) : null;
      const cloudMatch =
        welcomeRunId && this.cloudRuns
          ? [...(this.cloudRuns.joinedRuns || []), ...(this.cloudRuns.hostedRuns || [])].find(
              (r) => r && r.runId === welcomeRunId
            )
          : null;
      const cloudAltSlot =
        cloudMatch && Number.isInteger(Number(cloudMatch.mySlotIdx)) && Number(cloudMatch.mySlotIdx) >= 0
          ? Number(cloudMatch.mySlotIdx)
          : -1;
      if (!idbEntry && welcomeRunId) {
        if (cloudAltSlot >= 0 && cloudAltSlot !== myIdx) {
          idbEntry = await this.getCachedSlotFlashLocal(welcomeRunId, cloudAltSlot);
        }
        if (!idbEntry) {
          for (let s = 1; s < 4; s++) {
            if (s === myIdx) continue;
            const candidate = await this.getCachedSlotFlashLocal(welcomeRunId, s);
            if (candidate && candidate.flashSaveB64) {
              idbEntry = candidate;
              break;
            }
          }
        }
      }
      const idbB64 =
        idbEntry && idbEntry.flashSaveB64 && hasValidSectorSignature(decodeFlashBase64(idbEntry.flashSaveB64))
          ? idbEntry.flashSaveB64
          : "";
      const idbRev = idbEntry ? Math.max(0, Math.floor(Number(idbEntry.flashRev) || 0)) : 0;

      let backendEntry = meta && meta._backendFlash !== undefined ? meta._backendFlash : undefined;
      if (backendEntry === undefined && welcomeRunId && this.state.auth && this.state.auth.status === "signed_in") {
        const playersArr = Array.isArray(msg.players) ? msg.players : [];
        const assignedPlayer = playersArr[myIdx];
        const assignedOwnerMatches =
          !assignedPlayer || !assignedPlayer.userId || assignedPlayer.userId === this.state.auth.userId;
        if (assignedOwnerMatches) {
          backendEntry = await this.fetchSlotFlashFromBackend(welcomeRunId, myIdx);
        }
        if (!backendEntry && cloudAltSlot >= 0 && cloudAltSlot !== myIdx) {
          backendEntry = await this.fetchSlotFlashFromBackend(welcomeRunId, cloudAltSlot);
        }
        if (meta) meta._backendFlash = backendEntry || null;
      }
      const backendB64 =
        backendEntry && backendEntry.flashSaveB64 && hasValidSectorSignature(decodeFlashBase64(backendEntry.flashSaveB64))
          ? backendEntry.flashSaveB64
          : "";
      const backendRev = backendEntry ? Math.max(0, Math.floor(Number(backendEntry.flashRev) || 0)) : 0;

      // Also check active localStorage flash if it matches welcomeRunId
      const localMeta = readFlashSaveMeta();
      let activeLocalB64 = "";
      if (welcomeRunId && localMeta.runId === welcomeRunId && hasValidWasmFlashSave()) {
        try {
          activeLocalB64 = localStorage.getItem(STORAGE_KEYS.flashSave) || "";
        } catch (_) {}
      }
      const activeLocalRev = localMeta.runId === welcomeRunId ? Math.max(0, Number(localMeta.flashRev) || 0) : 0;

      const candidates = [];
      if (hostValid) candidates.push({ source: "host", b64: hostValid, rev: hostRev, prio: 3 });
      if (backendB64) candidates.push({ source: "backend", b64: backendB64, rev: backendRev, prio: 4 });
      if (idbB64) candidates.push({ source: "idb", b64: idbB64, rev: idbRev, prio: 2 });
      if (activeLocalB64) candidates.push({ source: "local", b64: activeLocalB64, rev: activeLocalRev, prio: 1 });

      candidates.sort((a, b) => (b.rev !== a.rev ? b.rev - a.rev : b.prio - a.prio));
      return {
        chosen: candidates[0] || null,
        hostRev,
        hasAnySavedFlash: candidates.length > 0,
      };
    }

    async _handleWelcomeAsync(msg, meta) {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      if (msg.runConfig && msg.runConfig.gameOver) {
        const message = "This Soul-Link Nuzlocke run has ended (Game Over).";
        this.state.connection.error = message;
        this.addLog(`Join rejected (RUN_GAME_OVER): ${message}`, "death");
        this.emit("join_rejected", {
          port: this.state.connection.roomPort,
          code: "RUN_GAME_OVER",
          reason: "RUN_GAME_OVER",
          message,
        });
        this.disconnect();
        return;
      }
      const resolved = await this._resolveBestWelcomeFlash(msg, meta);
      if (meta) meta._resolvedFlash = resolved;
      const isReturning = Boolean(msg.isReturningSlot || resolved.hasAnySavedFlash);
      const isBrandNewJoiner = !isReturning;
      const summary = this._getRunProgressSummary(msg.world, msg.soulLink);
      const isProgressedRoom =
        summary.badgeCount > 0 ||
        summary.deathCount > 0 ||
        summary.caughtRoutesCount > 0 ||
        summary.deadRoutes.length > 0;
      if (isBrandNewJoiner && isProgressedRoom && !(this._joinOptions && this._joinOptions.confirmedProgressed)) {
        const hostSlot = Array.isArray(msg.players) && msg.players[0] ? msg.players[0] : null;
        this._pendingLateJoin = { msg, meta };
        this.emit("late_join_confirm_needed", {
          port: this.state.connection.roomPort,
          roomName: (msg.runConfig && msg.runConfig.roomName) || `${(hostSlot && hostSlot.name) || "Host"}'s Room`,
          hostName: (hostSlot && hostSlot.name) || "Host",
          nuzlockePreset: (msg.runConfig && msg.runConfig.nuzlockePreset) || "none",
          badgeCount: summary.badgeCount,
          deathCount: summary.deathCount,
          caughtRoutesCount: summary.caughtRoutesCount,
          graveyard: summary.graveyard,
          deadRoutes: summary.deadRoutes,
          runConfig: msg.runConfig || this.state.runConfig,
        });
        return;
      }
      await this._finalizeClientWelcome(msg, meta);
    }

    async _finalizeClientWelcome(msg, meta) {
      const fromPeerId = 1;
      const myIdx = Math.max(0, Math.min(3, Math.floor(Number(msg.assignedSlot) || 0)));
      const prevMeta = readFlashSaveMeta();
      const welcomeRunId = (msg.runConfig && msg.runConfig.runId) || "";
      if (prevMeta.runId && prevMeta.runId !== welcomeRunId) {
        await this.flushActiveFlashToCache();
      }

      this.state.connection.myPeerId = Number(msg.assignedPeerId) || this.state.connection.myPeerId;
      this.state.connection.mySlotIndex = myIdx;
      if (msg.runConfig) this.state.runConfig = normalizeRunConfig(msg.runConfig, makeDefaultRunConfig());
      if (Array.isArray(msg.players)) {
        this.state.players = [0, 1, 2, 3].map((idx) => sanitizePlayerSlot(msg.players[idx], idx));
      }
      if (msg.world) this.state.world = msg.world;
      if (msg.soulLink) this.state.soulLink = msg.soulLink;
      if (!this._flashSavesBySlot) this._flashSavesBySlot = {};

      const resolved =
        (meta && meta._resolvedFlash) || (await this._resolveBestWelcomeFlash(msg, meta || {}));
      const chosen = resolved.chosen;
      const mySlot = this.state.players[myIdx];

      if (
        myIdx > 0 &&
        welcomeRunId &&
        this.state.auth &&
        this.state.auth.status === "signed_in" &&
        (!mySlot || !mySlot.userId || mySlot.userId === this.state.auth.userId)
      ) {
        if (mySlot && !mySlot.userId) {
          mySlot.userId = this.state.auth.userId;
          mySlot.username = this.state.auth.username;
        }
        this.claimRunSlotOnBackend(welcomeRunId, myIdx, (mySlot && mySlot.name) || getOrCreatePlayerName()).catch(
          () => {}
        );
      }

      if (chosen && chosen.b64) {
        const chosenRev = Math.max(chosen.rev || 0, Number(mySlot && mySlot.flashRev) || 0);
        this._flashSavesBySlot[myIdx] = chosen.b64;
        if (mySlot) mySlot.flashRev = chosenRev;
        if (importWasmFlashBase64(chosen.b64)) {
          writeFlashSaveMeta(welcomeRunId, Date.now(), {
            userId: this._getEffectiveUserId(),
            slotIndex: myIdx,
            flashRev: chosenRev,
          });
          await this.cacheSlotFlashLocal(welcomeRunId, myIdx, chosen.b64, chosenRev, Date.now());
          this._suppressLocalFlashSync = false;
          this._resumeFallbackOccurred = false;
          global.PokemonWasmBridge._loadSnapshotIntoWasm();
        }
        if (chosen.source !== "host" && chosen.rev > resolved.hostRev) {
          this.sendJsonToHost(CH_CONTROL, {
            t: "flash_save_sync",
            flashSaveB64: chosen.b64,
            flashRev: Math.max(1, chosenRev),
          });
        }
      } else {
        // Brand-new late joiner: clear mismatched local Flash and start fresh in Littleroot tutorial.
        delete this._flashSavesBySlot[myIdx];
        clearLocalFlashSave({ keepMeta: true });
        if (mySlot) {
          mySlot.tutorialComplete = false;
          mySlot.party = [];
          mySlot.money = 0;
          mySlot.bagItems = [];
          mySlot.flashRev = 0;
        }
        if (welcomeRunId) {
          writeFlashSaveMeta(welcomeRunId, Date.now(), {
            userId: this._getEffectiveUserId(),
            slotIndex: myIdx,
            flashRev: 0,
          });
        }
        this._ignoreBlankSaveUntilMs = Date.now() + 1500;
        this._suppressLocalFlashSync = false;
        global.PokemonWasmBridge._resetNewGameInWasm("littleroot");
      }

      global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
      this._applySpeed(this.state.runConfig.speedMultiplier);
      this.saveRunSnapshotToDb(undefined, { skipWasmSave: true }).catch(() => {});
      this._pendingRunConfigSync = { fromPeerId, msg: { t: "run_config_sync" } };
      if (global.PokemonWasmBridge._dispatchWasmJson(CH_CONTROL, fromPeerId, { t: "run_config_sync" })) {
        this._pendingRunConfigSync = null;
      }
      if (
        msg.world &&
        ((Array.isArray(msg.world.badges) && msg.world.badges.some(Boolean)) ||
          (msg.world.globalFlags && Object.keys(msg.world.globalFlags).length > 0) ||
          (msg.world.globalVars && Object.keys(msg.world.globalVars).length > 0))
      ) {
        const worldMsg = { t: "world_sync", badges: msg.world.badges, world: msg.world };
        this._pendingWorldSync = { fromPeerId, msg: worldMsg };
        if (global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, fromPeerId, worldMsg)) {
          this._pendingWorldSync = null;
        }
      }
      if (this.state.runConfig && this.state.runConfig.gameOver) {
        this._markRunGameOver("", { silentToast: true });
      }
      this.emit("all");
    }

    _handleClientMessage(fromPeerId, channel, msg, meta) {
      const rawBytes = meta && meta.rawBytes;
      if (channel === CH_CONTROL) {
        switch (msg.t) {
          case "slot_claim_required": {
            this._pendingSlotClaim = {
              port: this.state.connection.roomPort,
              runId: String(msg.runId || ""),
              roomName: String(msg.roomName || "Hoenn Run"),
              hasEmptySlot: Boolean(msg.hasEmptySlot),
              offlineSlots: Array.isArray(msg.offlineSlots) ? msg.offlineSlots : [],
            };
            this.emit("slot_claim_required", this._pendingSlotClaim);
            break;
          }
          case "welcome": {
            this._handleWelcomeAsync(msg, meta || {});
            break;
          }
          case "join_rejected": {
            const code = String(msg.code || msg.reason || "JOIN_REJECTED");
            const message = String(msg.message || "Join rejected by host.");
            this.state.connection.error = message;
            this.addLog(`Join rejected (${code}): ${message}`, "death");
            this.emit("join_rejected", {
              port: this.state.connection.roomPort,
              code,
              reason: code,
              message,
            });
            this.disconnect();
            break;
          }
          case "feature_gated": {
            const message = String(msg.message || msg.reason || "Action restricted right now.");
            this.addLog(message, "warn");
            this.emit("feature_gated", { action: msg.action || "", reason: message, message });
            this.emit("toast", { kind: "warn", title: "Action Restricted", message });
            break;
          }
          case "players_sync": {
            if (Array.isArray(msg.players)) {
              this.state.players = [0, 1, 2, 3].map((idx) => sanitizePlayerSlot(msg.players[idx], idx));
              global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
              this.emit("players");
            }
            break;
          }
          case "speed_set": {
            const spd = Number(msg.speed) || 1;
            this.state.runConfig.speedMultiplier = spd;
            this._applySpeed(spd);
            global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
            if (!msg.rejected) {
              this.addLog(`Host set global game speed to ${spd}x.`, "info");
              this.emit("toast", { kind: "info", title: "Global Speed Synced", message: `Host set speed to ${spd}x` });
            }
            this.emit("speed");
            break;
          }
          case "run_config_sync": {
            if (msg.runConfig) {
              this.state.runConfig = normalizeRunConfig(msg.runConfig, makeDefaultRunConfig());
              this._applySpeed(this.state.runConfig.speedMultiplier);
              this._pendingRunConfigSync = { fromPeerId, msg: { t: "run_config_sync" }, rawBytes };
              if (global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes)) {
                this._pendingRunConfigSync = null;
              }
              if (this.state.runConfig.gameOver) {
                this._markRunGameOver("", { silentToast: true });
              }
              this.emit("config");
            }
            break;
          }
          case "snapshot_sync": {
            if (msg.snapshot) {
              this.applySnapshotLocal(msg.snapshot, false, { fromBroadcast: true });
              this.saveRunSnapshotToDb(msg.snapshot);
              global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
              this.emit("toast", { kind: "ok", title: "Run Snapshot Synced", message: `Saved Run ${msg.snapshot.runId}` });
            }
            break;
          }
          case "teleport_approved": {
            this.addLog(`Warping to ${msg.targetName} at ${msg.mapName} (${msg.x}, ${msg.y})!`, "gold");
            this.emit("toast", {
              kind: "ok",
              title: "Teleport-to-Friend",
              message: `Warped to ${msg.targetName} in ${msg.mapName}`,
            });
            const mySlot = this.state.players[this.state.connection.mySlotIndex];
            if (mySlot) {
              mySlot.mapGroup = msg.mapGroup;
              mySlot.mapNum = msg.mapNum;
              mySlot.mapName = msg.mapName;
              mySlot.x = msg.x;
              mySlot.y = msg.y;
            }
            this._unlockMapKey(msg.mapGroup, msg.mapNum);
            global.PokemonWasmBridge._triggerWarpInWasm(msg.mapGroup, msg.mapNum, msg.x, msg.y);
            this.emit("players");
            break;
          }
          case "teleport_denied": {
            this.emit("toast", { kind: "danger", title: "Teleport Blocked", message: msg.reason || "Map not unlocked yet." });
            break;
          }
        }
      } else if (channel === CH_OVERWORLD) {
        if (msg.t === "pos") {
          const slot = this.state.players[msg.slotIndex];
          if (slot) {
            slot.mapGroup = msg.mapGroup;
            slot.mapNum = msg.mapNum;
            slot.mapName = msg.mapName;
            slot.x = msg.x;
            slot.y = msg.y;
            slot.inBattle = Boolean(msg.inBattle);
            this._unlockMapKey(msg.mapGroup, msg.mapNum);
            this.emit("overworld");
          }
        }
      } else if (channel === CH_WORLD_SYNC) {
        if (msg.t === "world_sync" && msg.world) {
          this.state.world = msg.world;
          global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
          this.emit("world");
          if (msg.announcement) {
            this.addLog(msg.announcement, "gold");
            this.emit("toast", { kind: "gold", title: "World Sync", message: msg.announcement });
          }
        } else if (msg.t === "badge_unlock") {
          global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
        } else if (msg.t === "world_flag") {
          const flagId = Number(msg.flagId);
          if (Number.isInteger(flagId) && flagId > 0) {
            if (!this.state.world.globalFlags || typeof this.state.world.globalFlags !== "object") {
              this.state.world.globalFlags = {};
            }
            this.state.world.globalFlags[String(flagId)] = Boolean(msg.val);
            global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
            this.emit("world");
          }
        } else if (msg.t === "world_var") {
          const varId = Number(msg.varId);
          if (Number.isInteger(varId) && varId > 0) {
            if (!this.state.world.globalVars || typeof this.state.world.globalVars !== "object") {
              this.state.world.globalVars = {};
            }
            this.state.world.globalVars[String(varId)] = Math.max(0, Math.floor(Number(msg.val) || 0));
            global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
            this.emit("world");
          }
        } else if (msg.t === "cutscene_start" || msg.t === "cutscene_update") {
          const mySlotIdx = this.state.connection.mySlotIndex;
          const prevCutscene = this.state.activeCutscene;
          const prevNumId = prevCutscene
            ? toNumericCutsceneId(prevCutscene.cutsceneNumericId ?? prevCutscene.cutsceneId)
            : -1;
          const incomingCut = msg.cutscene || {
            cutsceneId: String(msg.cutsceneId ?? "cutscene"),
            cutsceneNumericId: toNumericCutsceneId(msg.cutsceneId),
            title: String(msg.title || msg.cutsceneId || "Story Event"),
            triggeredBy: "Host",
            startedAt: Date.now(),
            peerStates: {},
          };
          const incomingNumId = toNumericCutsceneId(
            msg.cutsceneId ?? incomingCut.cutsceneNumericId ?? incomingCut.cutsceneId
          );
          const sameActiveCutscene = Boolean(prevCutscene && prevNumId === incomingNumId);
          this.state.activeCutscene = incomingCut;

          const mySlot = this.state.players[mySlotIdx];
          const status = mySlot && mySlot.inBattle ? "in_battle_deferred" : "watching";
          const prevAckedStatus =
            prevCutscene && prevCutscene.peerStates ? prevCutscene.peerStates[mySlotIdx] : null;
          if (msg.t === "cutscene_start" || !sameActiveCutscene || prevAckedStatus !== status) {
            this.sendJsonToHost(CH_WORLD_SYNC, { t: "cutscene_ack", status });
          }
          if (msg.t === "cutscene_start" || !sameActiveCutscene) {
            global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
          }
          this.emit("cutscene");
        } else if (msg.t === "cutscene_end") {
          if (msg.completedKey !== undefined) this.state.world.completedCutscenes[msg.completedKey] = true;
          if (msg.completedId !== undefined) this.state.world.completedCutscenes[msg.completedId] = true;
          this.state.activeCutscene = null;
          global.PokemonWasmBridge._dispatchWasmJson(channel, fromPeerId, msg, rawBytes);
          this.addLog(`Cutscene "${msg.title || msg.completedKey || msg.completedId}" completed globally.`, "info");
          this.emit("cutscene");
        }
      } else if (channel === CH_BATTLE) {
        this._receiveBattleControlMsg(fromPeerId, msg);
      } else if (channel === CH_SOUL_LINK) {
        if (msg.t === "soullink_sync") {
          if (msg.soulLink) this.state.soulLink = msg.soulLink;
          if (Array.isArray(msg.players)) {
            this.state.players = [0, 1, 2, 3].map((idx) => sanitizePlayerSlot(msg.players[idx], idx));
            global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
          }
          global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
          if (msg.deadLinkId) {
            global.PokemonWasmBridge._notifyWasmLinkDeath(msg.deadLinkId);
          }
          if (Array.isArray(msg.killMons)) {
            const myIdx = this.state.connection.mySlotIndex;
            for (const km of msg.killMons) {
              if (!km) continue;
              if (km.slotIndex === undefined || Number(km.slotIndex) === myIdx) {
                global.PokemonWasmBridge._killMonByUidInWasm(km.otId, km.personality);
              }
            }
          }
          if (msg.paritySummary || (Array.isArray(msg.parityVictims) && msg.parityVictims.length > 0)) {
            const summaryText = msg.paritySummary || msg.deathAlert || "Soul-Link death parity enforced.";
            this.addLog(summaryText, "death");
            this.emit("soullink_death_parity", {
              partyTag: msg.partyTag || "",
              victims: msg.parityVictims || [],
              summary: summaryText,
            });
          } else if (msg.deathAlert) {
            this.addLog(msg.deathAlert, "death");
            this.emit("toast", { kind: "danger", title: "Soul-Link Broken!", message: msg.deathAlert });
          } else if (msg.linkAlert) {
            this.addLog(msg.linkAlert, "gold");
            this.emit("toast", { kind: "ok", title: "Soul-Link Formed", message: msg.linkAlert });
          }
          this.emit("players");
        } else if (msg.t === "dead_route_reject") {
          const myIdx = this.state.connection.mySlotIndex;
          if (msg.targetSlot === undefined || Number(msg.targetSlot) === myIdx) {
            global.PokemonWasmBridge._killMonByUidInWasm(msg.otId, msg.personality);
            const message =
              msg.message ||
              `Rejected catch on dead/locked route ${msg.routeName || msg.mapsec || ""}.`;
            this.addLog(message, "death");
            this.emit("dead_route_reject", {
              targetSlot: Number(msg.targetSlot ?? myIdx),
              mapsec: Number(msg.mapsec) || 0,
              otId: Number(msg.otId) >>> 0,
              personality: Number(msg.personality) >>> 0,
              routeName: String(msg.routeName || ""),
              reason: String(msg.reason || "dead_route"),
              message,
            });
          }
        } else if (msg.t === "nuzlocke_gameover") {
          this._markRunGameOver(msg.fromName || "");
        }
      } else if (channel === CH_TRADE) {
        this._clientHandleTradeMsg(msg);
      }
    }

    // --- Host-Only Speed Control (1x / 2x / 3x) ---
    setGlobalSpeed(multiplier) {
      const spd = [1, 2, 3].includes(Number(multiplier)) ? Number(multiplier) : 1;
      if (!this.isHost()) {
        this.emit("toast", {
          kind: "warn",
          title: "Host Only",
          message: "Only the Room Host can change global emulation speed.",
        });
        return false;
      }
      this.state.runConfig.speedMultiplier = spd;
      this._applySpeed(spd);
      global.PokemonWasmBridge._dispatchWasmJson(CH_CONTROL, 1, { t: "speed_set", speed: spd });
      this.addLog(`Global speed set to ${spd}x.`, "info");
      if (this.isNetworkHost()) {
        this.broadcastControl({ t: "speed_set", speed: spd });
        this.pushRoomInfo();
      }
      this.emit("speed");
      return true;
    }

    _applySpeed(spd) {
      global.PokemonWasmBridge._applySpeedToWasm(spd);
    }

    // --- Co-op Feature Gating & Party Pairing (Max 2 per party) ---
    getFeatureGateReason(targetSlotIndex) {
      if (this.state.runConfig && this.state.runConfig.gameOver) {
        return "This Soul-Link Nuzlocke run has ended (Game Over).";
      }
      const myIdx = this.state.connection ? this.state.connection.mySlotIndex : 0;
      const me = this.state.players[myIdx];
      if (me && !me.tutorialComplete) {
        return "Finish the Littleroot tutorial (Starter, May Battle, Pokédex, Running Shoes) first.";
      }
      if (targetSlotIndex !== undefined && targetSlotIndex !== null && targetSlotIndex !== "") {
        const idx = Number(targetSlotIndex);
        const target = Number.isInteger(idx) ? this.state.players[idx] : null;
        if (!target || !target.connected) {
          return "Target trainer is not connected.";
        }
        if (!target.tutorialComplete) {
          return `${target.name || `Trainer ${idx + 1}`} is still in the Littleroot tutorial.`;
        }
      }
      return null;
    }

    _emitLocalGateRejection(action, reason) {
      const message = String(reason || "Action restricted right now.");
      this.addLog(message, "warn");
      this.emit("feature_gated", { action, reason: message, message });
      this.emit("toast", { kind: "warn", title: "Action Restricted", message });
    }

    requestPairWith(targetSlotIndex) {
      const gateReason = this.getFeatureGateReason(targetSlotIndex);
      if (gateReason) {
        this._emitLocalGateRejection("party_pair", gateReason);
        return false;
      }
      if (this.isHost()) {
        return this._hostHandlePartyPair(this.state.connection.mySlotIndex, targetSlotIndex);
      }
      this.sendJsonToHost(CH_CONTROL, { t: "party_pair", targetSlot: targetSlotIndex });
      return true;
    }

    leaveParty() {
      if (this.isHost()) {
        this._hostHandlePartyLeave(this.state.connection.mySlotIndex);
      } else {
        this.sendJsonToHost(CH_CONTROL, { t: "party_leave" });
      }
    }

    _hostHandlePartyPair(slotAIdx, slotBIdx) {
      const pA = this.state.players[slotAIdx];
      const pB = this.state.players[slotBIdx];
      if (!pA || !pB || slotAIdx === slotBIdx) return false;

      const requesterPeer = slotAIdx === 0 ? 1 : pA.peerId;
      const gateReason = this._getHostSlotGateReason(pA, pB);
      if (gateReason) {
        this._notifyHostGateRejection(requesterPeer, "party_pair", gateReason);
        return false;
      }

      const oldKeyA = this._getLinkGroupKey(slotAIdx);
      const oldKeyB = this._getLinkGroupKey(slotBIdx);
      const partyTag = `CO-OP ${slotAIdx + 1}+${slotBIdx + 1}`;
      const routeLocks = (this.state.soulLink && this.state.soulLink.routeLocks) || {};
      const oldPrefixes = [oldKeyA, oldKeyB, partyTag].filter(
        (k, idx, arr) => k && arr.indexOf(k) === idx
      );

      // Group existing route locks belonging to either side by routeId before mutating
      const routeBuckets = new Map();
      for (const prefix of oldPrefixes) {
        const prefixDot = `${prefix}:`;
        for (const lockKey of Object.keys(routeLocks)) {
          if (!lockKey.startsWith(prefixDot)) continue;
          const routeId = lockKey.slice(prefixDot.length);
          if (!routeBuckets.has(routeId)) {
            routeBuckets.set(routeId, []);
          }
          routeBuckets.get(routeId).push({ lockKey, lock: routeLocks[lockKey] });
        }
      }

      // Compute pending Soul-Link death parity victims BEFORE committing the merge
      const pendingKills = [];
      const seenVictimUids = new Set();
      const deadRouteNames = new Set();
      for (const [routeId, entries] of routeBuckets.entries()) {
        const anyDead = entries.some((e) => e.lock && (e.lock.dead || e.lock.failed));
        if (!anyDead) continue;
        const memberUids = new Set();
        const routeLinkIds = new Set();
        let routeName = routeId;
        for (const e of entries) {
          if (!e.lock) continue;
          if (e.lock.routeName) routeName = e.lock.routeName;
          if (e.lock.linkId) routeLinkIds.add(e.lock.linkId);
          if (e.lock.members) {
            for (const uid of Object.values(e.lock.members)) {
              if (uid) memberUids.add(uid);
            }
          }
        }
        for (const slot of [pA, pB]) {
          if (!Array.isArray(slot.party)) continue;
          for (const mon of slot.party) {
            if (!mon || mon.isDead) continue;
            const matchesUid = mon.uid && memberUids.has(mon.uid);
            const matchesLink = mon.linkId && routeLinkIds.has(mon.linkId);
            if ((matchesUid || matchesLink) && !seenVictimUids.has(mon.uid)) {
              seenVictimUids.add(mon.uid);
              deadRouteNames.add(routeName);
              pendingKills.push({ slot, mon, routeId, routeName });
            }
          }
        }
      }

      // Guard: reject pairing if enforcing death parity would eliminate a player's last alive Pokemon
      for (const slot of [pA, pB]) {
        const party = Array.isArray(slot.party) ? slot.party : [];
        const aliveBefore = party.filter((m) => m && !m.isDead).length;
        if (aliveBefore === 0) continue;
        const killedInSlot = pendingKills.filter((k) => k.slot.slotIndex === slot.slotIndex).length;
        if (killedInSlot > 0 && aliveBefore - killedInSlot <= 0) {
          const routesStr = Array.from(deadRouteNames).join(", ") || "a dead route";
          const reason = `Cannot pair: Soul-Link death parity on ${routesStr} would knock out ${slot.name}'s last surviving Pokémon! Catch a backup Pokémon first.`;
          this._notifyHostGateRejection(requesterPeer, "party_pair", reason);
          if (slotBIdx !== slotAIdx && pB.peerId > 1 && pB.peerId !== requesterPeer) {
            this._notifyHostGateRejection(pB.peerId, "party_pair", reason);
          }
          return false;
        }
      }

      // Clear previous parties of A and B and form new CO-OP party
      for (const p of this.state.players) {
        if (p.partyId && (p.partyId === pA.partyId || p.partyId === pB.partyId)) {
          p.partyId = null;
        }
      }
      pA.partyId = partyTag;
      pB.partyId = partyTag;
      pA.soulLinkGroup = partyTag;
      pB.soulLinkGroup = partyTag;

      // Migrate prior solo/pair route locks & linkIds into the shared Soul-Link group
      let migratedLocks = false;
      const deadLinkIds = new Set();
      for (const [routeId, entries] of routeBuckets.entries()) {
        const newLockKey = `${partyTag}:${routeId}`;
        const anyDead = entries.some((e) => e.lock && (e.lock.dead || e.lock.failed));
        const anyFailed = entries.some((e) => e.lock && e.lock.failed);
        let chosenLinkId = "";
        let chosenRouteName = routeId;
        const mergedMembers = {};
        const oldLinkIds = [];
        for (const e of entries) {
          if (!e.lock) continue;
          if (!chosenLinkId && e.lock.linkId) chosenLinkId = e.lock.linkId;
          if (e.lock.routeName) chosenRouteName = e.lock.routeName;
          if (e.lock.linkId && !oldLinkIds.includes(e.lock.linkId)) {
            oldLinkIds.push(e.lock.linkId);
          }
          Object.assign(mergedMembers, e.lock.members || {});
          if (e.lockKey !== newLockKey) {
            delete routeLocks[e.lockKey];
            migratedLocks = true;
          }
        }
        routeLocks[newLockKey] = {
          linkId: chosenLinkId,
          groupKey: partyTag,
          routeId,
          routeName: chosenRouteName,
          members: mergedMembers,
          dead: anyDead,
          failed: anyFailed,
        };
        if (anyDead && chosenLinkId) {
          deadLinkIds.add(chosenLinkId);
        }
        // Align both players' party mon linkIds when both caught on the same route
        if (chosenLinkId && oldLinkIds.length > 1) {
          for (const slot of [pA, pB]) {
            if (!Array.isArray(slot.party)) continue;
            for (const m of slot.party) {
              if (m && m.linkId && oldLinkIds.includes(m.linkId)) {
                m.linkId = chosenLinkId;
              }
            }
          }
          migratedLocks = true;
        }
      }

      // Apply death parity kills on surviving mons whose route lock is dead/failed on partner's side
      const killMons = [];
      const parityVictims = [];
      const myIdx = this.state.connection.mySlotIndex || 0;
      for (const item of pendingKills) {
        const { slot, mon, routeName } = item;
        mon.isDead = true;
        mon.hp = 0;
        mon.status = "FNT";
        if (mon.linkId) deadLinkIds.add(mon.linkId);
        const uidParts = parseMonUidKeys(mon.uid);
        if (uidParts) {
          killMons.push({
            slotIndex: slot.slotIndex,
            otId: uidParts.otId,
            personality: uidParts.personality,
            monUid: mon.uid,
            linkId: mon.linkId || "",
          });
          if (slot.slotIndex === myIdx) {
            global.PokemonWasmBridge._killMonByUidInWasm(uidParts.otId, uidParts.personality);
          }
        }
        parityVictims.push({
          slotIndex: slot.slotIndex,
          trainerName: slot.name,
          species: mon.species,
          nickname: mon.nickname,
          linkId: mon.linkId || "UNLINKED",
          routeName,
        });
        this.state.soulLink.graveyard.unshift({
          id: generateId("rip"),
          trainerName: slot.name,
          slotIndex: slot.slotIndex,
          species: mon.species,
          nickname: mon.nickname,
          level: mon.level,
          linkId: mon.linkId || "UNLINKED",
          originMap: mon.originMap || routeName || "Hoenn",
          cause: "Soul-Link death parity on pairing",
          diedAt: new Date().toLocaleTimeString(),
        });
        if (this.state.runConfig.nuzlockeToggles.enabled && Array.isArray(slot.party)) {
          const idx = slot.party.indexOf(mon);
          if (idx !== -1) slot.party.splice(idx, 1);
        }
        migratedLocks = true;
      }

      let paritySummary = "";
      if (parityVictims.length > 0) {
        const names = parityVictims.map((v) => `${v.trainerName}'s ${v.nickname || v.species}`).join(" & ");
        paritySummary = `Death parity on pairing (${partyTag}): ${names} perished because that route is already dead.`;
        this.addLog(paritySummary, "death");
        for (const dl of deadLinkIds) {
          global.PokemonWasmBridge._notifyWasmLinkDeath(dl);
        }
        this.emit("soullink_death_parity", {
          partyTag,
          victims: parityVictims,
          summary: paritySummary,
        });
      }

      global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
      this.addLog(`${pA.name} and ${pB.name} paired up (${partyTag}) for 2v2 Co-op!`, "gold");
      this.broadcastControl({ t: "players_sync", players: this.state.players });
      if ((migratedLocks || parityVictims.length > 0) && this.isNetworkHost()) {
        const firstDeadLink = deadLinkIds.values().next().value || "";
        this.broadcastChannel(CH_SOUL_LINK, {
          t: "soullink_sync",
          soulLink: this.state.soulLink,
          players: this.state.players,
          deadLinkId: firstDeadLink,
          killMons,
          partyTag,
          parityVictims: parityVictims.length > 0 ? parityVictims : undefined,
          paritySummary: paritySummary || undefined,
        });
      }
      if (parityVictims.length > 0 || migratedLocks) {
        this.saveRunSnapshotToDb();
      }
      this.emit("players");
      return true;
    }

    _hostHandlePartyLeave(slotIdx) {
      const p = this.state.players[slotIdx];
      if (!p || !p.partyId) return;
      const oldTag = p.partyId;
      for (const pl of this.state.players) {
        if (pl.partyId === oldTag) {
          pl.soulLinkGroup = pl.soulLinkGroup || oldTag;
          pl.partyId = null;
        }
      }
      global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      this.addLog(`${p.name} dissolved ${oldTag}.`, "info");
      this.broadcastControl({ t: "players_sync", players: this.state.players });
      this.emit("players");
    }

    _unlockMapKey(mapGroup, mapNum) {
      const g = Number(mapGroup);
      const n = Number(mapNum);
      if (!Number.isFinite(g) || !Number.isFinite(n)) return;
      const mapKey = `${Math.floor(g)}:${Math.floor(n)}`;
      if (!Array.isArray(this.state.world.unlockedMaps)) {
        this.state.world.unlockedMaps = [];
      }
      if (!this.state.world.unlockedMaps.includes(mapKey)) {
        this.state.world.unlockedMaps.push(mapKey);
      }
    }

    // --- Teleport-to-Friend ---
    requestTeleport(targetSlotIndex) {
      const target = this.state.players[targetSlotIndex];
      if (!target || !target.connected) return false;
      const gateReason = this.getFeatureGateReason();
      if (gateReason) {
        this._emitLocalGateRejection("teleport", gateReason);
        return false;
      }
      if (this.isHost()) {
        this._hostHandleTeleportRequest(1, target.peerId);
      } else {
        this.sendJsonToHost(CH_CONTROL, { t: "teleport_request", targetPeerId: target.peerId });
      }
      return true;
    }

    _hostHandleTeleportRequest(requesterPeerId, targetPeerId) {
      const requester =
        requesterPeerId === 1
          ? this.state.players[0]
          : this.state.players.find((p) => p.peerId === requesterPeerId);
      const target = this.state.players.find((p) => p.peerId === targetPeerId);
      if (!target) return;
      const gateReason = this._getHostSlotGateReason(requester, target, { requireTargetTutorial: false });
      if (gateReason) {
        this._notifyHostGateRejection(requesterPeerId, "teleport", gateReason);
        if (requesterPeerId > 1) {
          this.sendJson(CH_CONTROL, requesterPeerId, { t: "teleport_denied", reason: gateReason });
        }
        return;
      }
      const mapKey = `${target.mapGroup}:${target.mapNum}`;
      const unlocked =
        this.state.world.unlockedMaps.includes(mapKey) || this.state.world.unlockedMaps.length === 0;
      if (!unlocked) {
        if (requesterPeerId === 1) {
          this.emit("toast", { kind: "danger", title: "Teleport Denied", message: `${target.mapName} is not unlocked yet.` });
        } else {
          this.sendJson(CH_CONTROL, requesterPeerId, { t: "teleport_denied", reason: `${target.mapName} is locked.` });
        }
        return;
      }
      const payload = {
        t: "teleport_approved",
        targetName: target.name,
        mapGroup: target.mapGroup,
        mapNum: target.mapNum,
        mapName: target.mapName,
        x: target.x,
        y: target.y,
      };
      if (requesterPeerId === 1) {
        const mySlot = this.state.players[0];
        mySlot.mapGroup = target.mapGroup;
        mySlot.mapNum = target.mapNum;
        mySlot.mapName = target.mapName;
        mySlot.x = target.x;
        mySlot.y = target.y;
        this.addLog(`Warped to ${target.name} in ${target.mapName}!`, "gold");
        this.emit("toast", { kind: "ok", title: "Teleport-to-Friend", message: `Warped to ${target.name} (${target.mapName})` });
        global.PokemonWasmBridge._triggerWarpInWasm(target.mapGroup, target.mapNum, target.x, target.y);
        this.broadcastControl({ t: "players_sync", players: this.state.players });
        this.emit("players");
      } else {
        this.sendJson(CH_CONTROL, requesterPeerId, payload);
      }
    }

    // --- Badges & World Sync ---
    unlockBadge(badgeIdx, opts) {
      const cleanIdx = Number(badgeIdx);
      if (!Number.isInteger(cleanIdx) || cleanIdx < 0 || cleanIdx >= 8) return;
      if (this.isHost()) {
        if (this.state.world.badges[cleanIdx]) return;
        this.state.world.badges[cleanIdx] = true;
        const badgeNames = ["Stone", "Knuckle", "Dynamo", "Heat", "Balance", "Feather", "Mind", "Rain"];
        const msg = `${badgeNames[cleanIdx]} Badge unlocked & synced to all players!`;
        this.addLog(msg, "gold");
        this.emit("toast", { kind: "gold", title: "Badge Unlocked", message: msg });
        if (this.isNetworkHost()) {
          this.broadcastChannel(CH_WORLD_SYNC, {
            t: "world_sync",
            badges: this.state.world.badges,
            world: this.state.world,
            announcement: msg,
          });
        }
        if (!opts || !opts.fromLocalWasm) {
          global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, (opts && opts.fromPeerId) || 1, {
            t: "badge_unlock",
            badgeIndex: cleanIdx,
          });
        }
        this.saveRunSnapshotToDb();
        this.emit("world");
      } else {
        this.sendJsonToHost(CH_WORLD_SYNC, { t: "badge_unlock", badgeIndex: cleanIdx });
      }
    }

    // --- Stardew-Style Global Cutscene Sync ---
    triggerGlobalCutscene(cutsceneId, title) {
      if (this.isHost()) {
        this._hostStartCutscene(cutsceneId, title, this.state.players[0].name);
      } else {
        this.sendJsonToHost(CH_WORLD_SYNC, { t: "cutscene_trigger", cutsceneId, title });
      }
    }

    _hostStartCutscene(cutsceneId, title, triggeredBy, opts) {
      const rawKey = String(cutsceneId ?? "").trim() || "cutscene";
      const numericId = toNumericCutsceneId(cutsceneId);
      if (this.state.world.completedCutscenes[rawKey] || this.state.world.completedCutscenes[numericId]) return;
      const peerStates = {};
      for (const p of this.state.players) {
        if (p.connected) {
          peerStates[p.slotIndex] = p.inBattle ? "in_battle_deferred" : "watching";
        }
      }
      this.state.activeCutscene = {
        cutsceneId: rawKey,
        cutsceneNumericId: numericId,
        title: title || rawKey,
        triggeredBy,
        startedAt: Date.now(),
        peerStates,
      };
      this.addLog(`Global Cutscene started by ${triggeredBy}: ${this.state.activeCutscene.title}`, "gold");
      if (this.isNetworkHost()) {
        this.broadcastChannel(CH_WORLD_SYNC, {
          t: "cutscene_start",
          cutsceneId: numericId,
          title: this.state.activeCutscene.title,
          cutscene: this.state.activeCutscene,
        });
      }
      if (!opts || !opts.fromLocalWasm) {
        global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, (opts && opts.fromPeerId) || 1, {
          t: "cutscene_trigger",
          cutsceneId: numericId,
          title: String(this.state.activeCutscene.title || "").slice(0, 31),
        });
      }
      this.emit("cutscene");
    }

    finishActiveCutscene(opts) {
      if (!this.state.activeCutscene) return;
      if (this.isHost()) {
        const c = this.state.activeCutscene;
        const numericId = c.cutsceneNumericId || toNumericCutsceneId(c.cutsceneId);
        this.state.world.completedCutscenes[String(c.cutsceneId)] = true;
        this.state.world.completedCutscenes[numericId] = true;
        this.state.activeCutscene = null;
        this.addLog(`Global Cutscene "${c.title}" synced & returned players to saved positions.`, "info");
        if (this.isNetworkHost()) {
          this.broadcastChannel(CH_WORLD_SYNC, {
            t: "cutscene_end",
            completedId: numericId,
            cutsceneId: numericId,
            completedKey: String(c.cutsceneId),
            title: c.title,
          });
        }
        if (!opts || !opts.fromLocalWasm) {
          global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, 1, {
            t: "cutscene_end",
            completedId: numericId,
            title: String(c.title || "").slice(0, 31),
          });
        }
        this.saveRunSnapshotToDb();
        this.emit("cutscene");
        this.emit("world");
      } else {
        this.sendJsonToHost(CH_WORLD_SYNC, { t: "cutscene_ack", status: "done" });
      }
    }

    // --- Cross-Map Hybrid 2v2 Co-op Multi-Battle & PvP Invites ---
    _buildTeammateBusyInfo(partnerSlot, reasonStr) {
      const idx = Number(partnerSlot);
      const partner = Number.isInteger(idx) && idx >= 0 && idx < 4 ? this.state.players[idx] : null;
      const who = partner && partner.name ? partner.name : "Your teammate";
      const mapName = (partner && partner.mapName) || "";
      const validReasons = ["busy", "different_map", "in_battle", "in_tutorial", "not_ready"];
      const reason = typeof reasonStr === "string" && validReasons.includes(reasonStr) ? reasonStr : "busy";
      let title = "Partner Busy";
      let desc = `${who} is busy in a cutscene or dialogue.`;
      if (reason === "different_map") {
        title = "Partner Elsewhere";
        desc = `${who} is in a different area${mapName ? ` (${mapName})` : ""}.`;
      } else if (reason === "in_tutorial") {
        title = "Partner in Tutorial";
        desc = `${who} is still in the Littleroot tutorial.`;
      } else if (reason === "in_battle") {
        title = "Partner in Battle";
        desc = `${who} is already in another battle.`;
      } else if (reason === "not_ready") {
        title = "Partner Not Ready";
        desc = `${who} is not connected or not ready yet.`;
      }
      return {
        targetSlotIndex: Number.isInteger(idx) && idx >= 0 && idx < 4 ? idx : -1,
        targetName: who,
        reason,
        mapName,
        title,
        desc,
      };
    }

    onTeammateBusyFallback(partnerSlot, reasonStr) {
      const info = this._buildTeammateBusyInfo(partnerSlot, reasonStr);
      const fallbackTitle = info.reason === "busy" ? "AI Partner Fallback" : info.title;
      const msg = `${info.desc.replace(/\.$/, "")} — continuing with AI partner.`;
      this.addLog(msg, "warn");
      this.emit("teammate_busy", {
        targetSlotIndex: info.targetSlotIndex,
        targetName: info.targetName,
        reason: info.reason,
        mapName: info.mapName,
        title: fallbackTitle,
        message: msg,
      });
    }

    onTeammateBusyPrompt(partnerSlot, reasonStr) {
      const info = this._buildTeammateBusyInfo(partnerSlot, reasonStr);
      this.addLog(`${info.desc} Choose Wait for Partner or Proceed with AI Partner.`, "warn");
      this.emit("teammate_busy_prompt", {
        targetSlotIndex: info.targetSlotIndex,
        targetName: info.targetName,
        reason: info.reason,
        mapName: info.mapName,
        title: info.title,
        message: info.desc,
      });
    }

    onTeammateBusyResolved() {
      this.emit("teammate_busy_resolved", {});
    }

    sendBattleInvite(targetSlotIndex, mode, options) {
      const target = this.state.players[targetSlotIndex];
      const me = this.state.players[this.state.connection.mySlotIndex];
      if (!target || !target.connected || !me) return;
      const gateReason = this.getFeatureGateReason(targetSlotIndex);
      if (gateReason) {
        this._emitLocalGateRejection("battle_invite", gateReason);
        return;
      }

      const resolvedMode = mode || "coop_2v2";
      const forceWait = Boolean(options && options.forceWait);
      if (!forceWait && (target.inBattle || target.busy)) {
        const busyReason = target.inBattle ? "in a battle" : "in a cutscene, NPC dialogue, or solo activity";
        if (resolvedMode === "coop_2v2") {
          this.emit("busy_battle_prompt", {
            targetSlotIndex,
            targetName: target.name,
            mode: resolvedMode,
            busyReason,
          });
          return;
        }
        this.emit("toast", {
          kind: "gold",
          title: "Player Busy",
          message: `${target.name} is currently ${busyReason}. Invite paused so their scene isn't interrupted.`,
        });
        return;
      }

      const invite = {
        t: "battle_invite",
        inviteId: generateId("bat"),
        mode: resolvedMode, // "coop_2v2" | "pvp"
        fromPeerId: this.state.connection.myPeerId,
        fromSlotIndex: me.slotIndex,
        fromName: me.name,
        targetPeerId: target.peerId,
        targetSlotIndex: target.slotIndex,
        mapName: me.mapName,
        enemyDesc: resolvedMode === "pvp" ? "Friendly PvP (Nuzlocke Safe)" : "Wild Lv.14 Zigzagoon & Electrike",
        battleSeed: Math.floor(Math.random() * 0xffffffff),
      };

      if (target.peerId === this.state.connection.myPeerId) {
        this._receiveBattleControlMsg(target.peerId, invite);
        return;
      }
      if (this.isNetworkHost()) {
        this.sendJson(CH_BATTLE, target.peerId, invite);
      } else {
        this.sendJsonToHost(CH_BATTLE, invite);
      }
      this.addLog(`Sent ${mode === "pvp" ? "Friendly PvP" : "Co-op 2v2"} invite to ${target.name}.`, "info");
    }

    _receiveBattleControlMsg(fromPeerId, msg) {
      if (msg.t === "battle_invite") {
        this.state.activeInvites.push({ ...msg, receivedAt: Date.now() });
        this.emit("invites");
      } else if (msg.t === "battle_accept") {
        this.addLog(`${msg.fromName || "Partner"} accepted ${msg.mode} battle! Seed=${msg.battleSeed}`, "gold");
        this.emit("toast", {
          kind: "ok",
          title: "2v2 Battle Synced!",
          message: `${msg.fromName || "Partner"} joined the battle!`,
        });
        const resolvedPeerId = Number(msg.fromPeerId) || Number(fromPeerId) || 0;
        let resolvedSlotIdx = Number.isInteger(Number(msg.fromSlotIndex)) ? Number(msg.fromSlotIndex) : 0xff;
        if ((resolvedSlotIdx < 0 || resolvedSlotIdx >= 4) && resolvedPeerId > 0) {
          const matchedPeer = this.state.players.find((p) => p && p.peerId === resolvedPeerId);
          if (matchedPeer) resolvedSlotIdx = matchedPeer.slotIndex;
        }
        global.PokemonWasmBridge._startSyncedBattleInWasm({
          ...msg,
          partnerSlotIndex: resolvedSlotIdx,
          partnerPeerId: resolvedPeerId,
        });
      } else if (msg.t === "battle_decline") {
        this.addLog(`${msg.fromName || "Partner"} declined the battle invite.`, "info");
      }
    }

    respondBattleInvite(inviteId, accepted) {
      const idx = this.state.activeInvites.findIndex((i) => i.inviteId === inviteId);
      if (idx === -1) return;
      const invite = this.state.activeInvites.splice(idx, 1)[0];
      const me = this.state.players[this.state.connection.mySlotIndex];

      const reply = {
        t: accepted ? "battle_accept" : "battle_decline",
        inviteId: invite.inviteId,
        mode: invite.mode,
        targetPeerId: invite.fromPeerId,
        targetSlotIndex: invite.fromSlotIndex,
        fromPeerId: this.state.connection.myPeerId,
        fromSlotIndex: me ? me.slotIndex : this.state.connection.mySlotIndex,
        fromName: me ? me.name : "Partner",
        battleSeed: invite.battleSeed,
      };
      if (invite.fromPeerId !== this.state.connection.myPeerId) {
        if (this.isNetworkHost()) {
          this.sendJson(CH_BATTLE, invite.fromPeerId, reply);
        } else {
          this.sendJsonToHost(CH_BATTLE, reply);
        }
      }
      if (accepted) {
        this.addLog(`Joined ${invite.fromName}'s ${invite.mode === "pvp" ? "Friendly PvP" : "Co-op 2v2"} battle!`, "gold");
        global.PokemonWasmBridge._startSyncedBattleInWasm({
          ...invite,
          partnerSlotIndex: invite.fromSlotIndex,
          partnerPeerId: invite.fromPeerId,
        });
      }
      this.emit("invites");
    }

    // --- Soul-Link & Nuzlocke Tracking ---
    _getLinkGroupKey(slotIndex) {
      if (this.state.runConfig.soulLinkMode === "group") return "ALL";
      const player = this.state.players[slotIndex];
      if (player && player.partyId) return player.partyId;
      if (player && player.soulLinkGroup) return player.soulLinkGroup;
      return slotIndex < 2 ? "PAIR_1_2" : "PAIR_3_4";
    }

    _rejectDeadOrDupRouteCatch(slot, fromPeerId, opts) {
      const options = opts && typeof opts === "object" ? opts : {};
      const myIdx = this.state.connection ? this.state.connection.mySlotIndex : 0;
      const mapsec = Number(options.mapsec) || 0;
      const otId = Number(options.otId) >>> 0;
      const personality = Number(options.personality) >>> 0;
      const routeName = String(options.routeName || (mapsec ? `Area #${mapsec}` : "Locked Route"));
      const reason = String(options.reason || "dead_route");
      const message = String(options.message || `Catch rejected: ${routeName} is dead or already locked.`);
      this.addLog(message, "death");
      const eventPayload = {
        targetSlot: slot.slotIndex,
        mapsec,
        otId,
        personality,
        routeName,
        reason,
        message,
      };
      if (slot.slotIndex === myIdx) {
        if (otId || personality) {
          global.PokemonWasmBridge._killMonByUidInWasm(otId, personality);
        }
        this.emit("dead_route_reject", eventPayload);
      } else {
        const targetPeer = slot.peerId > 1 ? slot.peerId : Number(fromPeerId) || 0;
        if (this.isNetworkHost() && targetPeer > 1) {
          this.sendJson(CH_SOUL_LINK, targetPeer, { t: "dead_route_reject", ...eventPayload });
        }
        this.emit("dead_route_reject", eventPayload);
      }
    }

    _handleBinarySoulLinkPacket(fromPeerId, payloadBytes) {
      if (!(payloadBytes instanceof Uint8Array) || payloadBytes.byteLength < 8 || !this.isHost()) return;
      const dv = new DataView(payloadBytes.buffer, payloadBytes.byteOffset, payloadBytes.byteLength);
      const magic = dv.getUint16(0, true);
      const channel = payloadBytes[2];
      const opcode = payloadBytes[3];
      if (magic !== 0x4d50 || channel !== CH_SOUL_LINK) return;

      if (opcode === 0x40 && payloadBytes.byteLength >= 25) {
        const slotIndex = payloadBytes[8];
        const mapsec = dv.getUint16(10, true);
        const linkIdNum = dv.getUint16(12, true);
        const speciesId = dv.getUint16(14, true);
        const level = Math.max(1, payloadBytes[16] || 1);
        const otId = dv.getUint32(17, true);
        const personality = dv.getUint32(21, true);
        const slot = this.state.players[slotIndex];
        if (!slot) return;

        const isStarterCatch = mapsec === 190 || linkIdNum === 1;
        if (!isStarterCatch && !slot.tutorialComplete) {
          return;
        }

        const monUid = `mon-${personality.toString(16).padStart(8, "0")}-${otId.toString(16).padStart(8, "0")}`;
        const groupKey = this._getLinkGroupKey(slotIndex);
        const routeId = `mapsec:${mapsec}`;
        const lockKey = `${groupKey}:${routeId}`;
        const existingMon = Array.isArray(slot.party) ? slot.party.find((m) => m.uid === monUid) : null;
        const existingLock = this.state.soulLink.routeLocks[lockKey];
        const routeName = (existingMon && existingMon.originMap) || (existingLock && existingLock.routeName) || `Area #${mapsec}`;

        if (existingLock) {
          const isDeadOrFailed = Boolean(existingLock.dead || existingLock.failed);
          const slotAlreadyCaught = Boolean(
            existingLock.members && existingLock.members[slotIndex] && existingLock.members[slotIndex] !== monUid
          );
          if (isDeadOrFailed || slotAlreadyCaught) {
            const aliveOtherMons = Array.isArray(slot.party)
              ? slot.party.filter((m) => m && !m.isDead && m.uid !== monUid).length
              : 0;
            if (isStarterCatch && isDeadOrFailed && !slotAlreadyCaught && aliveOtherMons === 0) {
              // Preserve late joiner's unlinked starter if host starter died before tutorial completion
              this.addLog(
                `${slot.name}'s starter (${routeName}) kept unlinked so solo run isn't softlocked.`,
                "warn"
              );
              return;
            }
            const reason = isDeadOrFailed ? "dead_route" : "route_locked";
            const message = isDeadOrFailed
              ? `${slot.name}'s catch on ${routeName} was rejected because that route is dead/failed!`
              : `${slot.name}'s catch on ${routeName} was rejected (route already caught)!`;
            this._rejectDeadOrDupRouteCatch(slot, fromPeerId, {
              mapsec,
              otId,
              personality,
              routeName,
              reason,
              message,
            });
            return;
          }
        }

        const linkIdStr = linkIdNum
          ? `LINK-${String(linkIdNum).padStart(2, "0")}`
          : `LINK-${String(this.state.soulLink.nextLinkNum++).padStart(2, "0")}`;
        if (linkIdNum >= this.state.soulLink.nextLinkNum) {
          this.state.soulLink.nextLinkNum = linkIdNum + 1;
        }
        if (!existingLock) {
          this.state.soulLink.routeLocks[lockKey] = {
            linkId: linkIdStr,
            groupKey,
            routeId,
            routeName,
            members: { [slotIndex]: monUid },
            dead: false,
            failed: false,
          };
        } else {
          existingLock.linkId = existingLock.linkId || linkIdStr;
          existingLock.members = existingLock.members || {};
          existingLock.members[slotIndex] = monUid;
        }
        const lock = this.state.soulLink.routeLocks[lockKey];
        const speciesLabel = (existingMon && existingMon.species) || `Pokemon #${speciesId}`;
        if (existingMon) {
          existingMon.linkId = lock.linkId;
        } else if (slot.party.length < 6) {
          const maxHp = Math.max(10, level * 3 + 10);
          slot.party.push({
            uid: monUid,
            species: speciesLabel,
            speciesId,
            nickname: speciesLabel,
            level,
            hp: maxHp,
            maxHp,
            status: "",
            ball: "pokeball",
            heldItem: "",
            linkId: lock.linkId,
            originMap: routeName,
            isDead: false,
          });
        }

        const linkAlert = `${slot.name} caught ${speciesLabel} on ${routeName} [${lock.linkId}]`;
        this.addLog(linkAlert, "gold");
        global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
        if (this.isNetworkHost()) {
          this.broadcastChannel(CH_SOUL_LINK, {
            t: "soullink_sync",
            soulLink: this.state.soulLink,
            players: this.state.players,
            linkAlert,
          });
        }
        this.saveRunSnapshotToDb();
        this.emit("players");
      } else if ((opcode === 0x41 || opcode === 0x42) && payloadBytes.byteLength >= 21) {
        const slotIndex = payloadBytes[8];
        const linkIdNum = dv.getUint16(10, true);
        const causeCode = payloadBytes[12];
        const otId = dv.getUint32(13, true);
        const personality = dv.getUint32(17, true);
        const monUid = `mon-${personality.toString(16).padStart(8, "0")}-${otId.toString(16).padStart(8, "0")}`;
        const linkIdStr = linkIdNum ? `LINK-${String(linkIdNum).padStart(2, "0")}` : "";
        const cause = causeCode === 1 ? "Whiteout" : "Fainted in battle";
        this.reportMonFainted(slotIndex, monUid, cause, linkIdStr, { skipLocalWasmNotify: fromPeerId === 1 });
      } else if (opcode === 0x43 && payloadBytes.byteLength >= 12) {
        const slotIndex = payloadBytes[8];
        const mapsec = dv.getUint16(10, true);
        const linkIdNum = payloadBytes.byteLength >= 14 ? dv.getUint16(12, true) : 0;
        const slot = this.state.players[slotIndex];
        if (!slot) return;
        if (mapsec !== 190 && !slot.tutorialComplete) return;

        const groupKey = this._getLinkGroupKey(slotIndex);
        const routeId = `mapsec:${mapsec}`;
        const lockKey = `${groupKey}:${routeId}`;
        let lock = this.state.soulLink.routeLocks[lockKey];
        const linkIdStr =
          (lock && lock.linkId) || (linkIdNum ? `LINK-${String(linkIdNum).padStart(2, "0")}` : "");
        if (!lock) {
          lock = {
            linkId: linkIdStr,
            groupKey,
            routeId,
            routeName: `Area #${mapsec}`,
            members: {},
            dead: true,
            failed: true,
          };
          this.state.soulLink.routeLocks[lockKey] = lock;
        } else {
          lock.dead = true;
          lock.failed = true;
        }
        const routeName = lock.routeName || `Area #${mapsec}`;

        // Death parity: if another linked member in the same group already caught on this route/linkId, faint it too
        const memberEntries = Object.entries(lock.members || {});
        let triggeredParity = false;
        for (const [mSlotStr, mUid] of memberEntries) {
          const mSlot = Number(mSlotStr);
          if (mSlot === slotIndex) continue;
          const mPlayer = this.state.players[mSlot];
          const aliveMon =
            mPlayer && Array.isArray(mPlayer.party)
              ? mPlayer.party.find((m) => m && !m.isDead && (m.uid === mUid || (linkIdStr && m.linkId === linkIdStr)))
              : null;
          if (aliveMon) {
            triggeredParity = true;
            this.reportMonFainted(
              mSlot,
              aliveMon.uid || mUid,
              `${slot.name} failed the encounter on ${routeName}`,
              linkIdStr,
              { skipLocalWasmNotify: fromPeerId === 1 }
            );
            break;
          }
        }
        if (!triggeredParity && linkIdStr) {
          for (const pl of this.state.players) {
            if (pl.slotIndex === slotIndex) continue;
            const plGroup = this._getLinkGroupKey(pl.slotIndex);
            const sameGroup =
              this.state.runConfig.soulLinkMode === "group" || plGroup === groupKey;
            if (!sameGroup || !Array.isArray(pl.party)) continue;
            const linkedAlive = pl.party.find((m) => m && !m.isDead && m.linkId === linkIdStr);
            if (linkedAlive) {
              triggeredParity = true;
              this.reportMonFainted(
                pl.slotIndex,
                linkedAlive.uid,
                `${slot.name} failed the encounter on ${routeName}`,
                linkIdStr,
                { skipLocalWasmNotify: fromPeerId === 1 }
              );
              break;
            }
          }
        }
        if (!triggeredParity) {
          this.addLog(`${slot.name} lost the encounter on ${routeName} — route is now dead.`, "death");
          global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
          if (this.isNetworkHost()) {
            this.broadcastChannel(CH_SOUL_LINK, {
              t: "soullink_sync",
              soulLink: this.state.soulLink,
              players: this.state.players,
            });
          }
          this.saveRunSnapshotToDb();
          this.emit("players");
        }
      }
    }

    registerRouteCatch(slotIndex, routeId, routeName, mon) {
      if (!this.isHost()) {
        this.sendJsonToHost(CH_SOUL_LINK, { t: "route_catch", routeId, routeName, mon });
        return;
      }
      const slot = this.state.players[slotIndex];
      if (!slot) return;
      const isStarter = routeId === "mapsec:190";
      if (!isStarter && !slot.tutorialComplete) return;

      const groupKey = this._getLinkGroupKey(slotIndex);
      const lockKey = `${groupKey}:${routeId}`;
      let lock = this.state.soulLink.routeLocks[lockKey];
      const monUid = (mon && mon.uid) || generateId("mon");
      if (lock) {
        const isDeadOrFailed = Boolean(lock.dead || lock.failed);
        const alreadyCaught = Boolean(lock.members && lock.members[slotIndex] && lock.members[slotIndex] !== monUid);
        if (isDeadOrFailed || alreadyCaught) {
          const uidParts = parseMonUidKeys(monUid);
          const mapsec = parseRouteMapsec(routeId);
          this._rejectDeadOrDupRouteCatch(slot, slot.peerId || 1, {
            mapsec,
            otId: uidParts ? uidParts.otId : 0,
            personality: uidParts ? uidParts.personality : 0,
            routeName: routeName || lock.routeName || routeId,
            reason: isDeadOrFailed ? "dead_route" : "route_locked",
            message: isDeadOrFailed
              ? `${slot.name}'s catch on ${routeName || lock.routeName || routeId} was rejected (dead route)!`
              : `${slot.name}'s catch on ${routeName || lock.routeName || routeId} was rejected (route locked)!`,
          });
          return;
        }
      }
      if (!lock) {
        const linkId = `LINK-${String(this.state.soulLink.nextLinkNum++).padStart(2, "0")}`;
        lock = { linkId, groupKey, routeId, routeName, members: {}, dead: false, failed: false };
        this.state.soulLink.routeLocks[lockKey] = lock;
      }

      const newMon = {
        uid: monUid,
        species: (mon && mon.species) || "Poochyena",
        nickname: (mon && mon.nickname) || (mon && mon.species) || "Poochyena",
        level: (mon && mon.level) || 6,
        hp: (mon && mon.hp) || 22,
        maxHp: (mon && mon.maxHp) || 22,
        status: "",
        ball: (mon && mon.ball) || "pokeball",
        heldItem: (mon && mon.heldItem) || "",
        linkId: lock.linkId,
        originMap: routeName || routeId,
        isDead: false,
      };

      if (slot.party.length < 6) {
        slot.party.push(newMon);
      }
      lock.members[slotIndex] = newMon.uid;

      const linkAlert = `${slot.name} caught ${newMon.species} on ${newMon.originMap} [${lock.linkId}]`;
      this.addLog(linkAlert, "gold");
      global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
      if (this.isNetworkHost()) {
        this.broadcastChannel(CH_SOUL_LINK, {
          t: "soullink_sync",
          soulLink: this.state.soulLink,
          players: this.state.players,
          linkAlert,
        });
      }
      this.saveRunSnapshotToDb();
      this.emit("players");
    }

    reportMonFainted(slotIndex, monUid, cause, optionalLinkId, opts) {
      if (!this.isHost()) {
        this.sendJsonToHost(CH_SOUL_LINK, { t: "mon_fainted", monUid, cause });
        return;
      }
      const owner = this.state.players[slotIndex];
      if (!owner) return;
      let mon = owner.party.find((m) => m.uid === monUid);
      if (!mon && optionalLinkId) {
        mon = owner.party.find((m) => m.linkId === optionalLinkId && !m.isDead) || null;
      }
      if (mon && mon.isDead) return;

      const linkId = (mon && mon.linkId) || optionalLinkId || "";
      if (!mon && !linkId) return;
      const groupKey = this._getLinkGroupKey(slotIndex);
      if (this.state.soulLink && this.state.soulLink.routeLocks) {
        for (const rk of Object.keys(this.state.soulLink.routeLocks)) {
          const lockObj = this.state.soulLink.routeLocks[rk];
          if (!lockObj) continue;
          const matchLink = linkId && lockObj.linkId === linkId;
          const matchMember =
            monUid && lockObj.members && Object.values(lockObj.members).includes(monUid);
          if (matchLink || matchMember) {
            lockObj.dead = true;
          }
        }
      }
      const victims = [];

      for (const pl of this.state.players) {
        if (!pl.connected && pl.party.length === 0) continue;
        const plGroup = this._getLinkGroupKey(pl.slotIndex);
        const sameGroup =
          this.state.runConfig.soulLinkMode === "group" || plGroup === groupKey || pl.slotIndex === slotIndex;
        if (!sameGroup) continue;

        for (let i = pl.party.length - 1; i >= 0; i--) {
          const candidate = pl.party[i];
          if (candidate.uid === monUid || (linkId && candidate.linkId === linkId && !candidate.isDead)) {
            candidate.isDead = true;
            candidate.hp = 0;
            candidate.status = "FNT";
            victims.push(`${pl.name}'s ${candidate.nickname} (${candidate.species})`);
            this.state.soulLink.graveyard.unshift({
              id: generateId("rip"),
              trainerName: pl.name,
              slotIndex: pl.slotIndex,
              species: candidate.species,
              nickname: candidate.nickname,
              level: candidate.level,
              linkId: candidate.linkId || "UNLINKED",
              originMap: candidate.originMap || "Hoenn",
              cause: cause || "Fainted in battle",
              diedAt: new Date().toLocaleTimeString(),
            });
            // Move out of active party into graveyard when Nuzlocke enabled
            if (this.state.runConfig.nuzlockeToggles.enabled) {
              pl.party.splice(i, 1);
            }
          }
        }
      }

      if (victims.length === 0) {
        const alreadyLogged = this.state.soulLink.graveyard.some(
          (g) => g.slotIndex === slotIndex && linkId && g.linkId === linkId
        );
        if (alreadyLogged) return;
        victims.push(`${owner.name}'s ${monUid || linkId}`);
        this.state.soulLink.graveyard.unshift({
          id: generateId("rip"),
          trainerName: owner.name,
          slotIndex: owner.slotIndex,
          species: monUid || linkId,
          nickname: monUid || linkId,
          level: 1,
          linkId: linkId || "UNLINKED",
          originMap: "Hoenn",
          cause: cause || "Fainted in battle",
          diedAt: new Date().toLocaleTimeString(),
        });
      }

      const deathAlert = `${victims.join(" & ")} perished [${linkId || "SOLO"}] — ${cause || "Fainted"}`;
      this.addLog(deathAlert, "death");
      this.emit("toast", { kind: "danger", title: "Soul-Link Broken!", message: deathAlert });
      if (!(opts && opts.skipLocalWasmNotify)) {
        global.PokemonWasmBridge._notifyWasmLinkDeath(linkId);
      }
      global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);

      if (this.isNetworkHost()) {
        this.broadcastChannel(CH_SOUL_LINK, {
          t: "soullink_sync",
          soulLink: this.state.soulLink,
          players: this.state.players,
          deathAlert,
          deadLinkId: linkId,
        });
      }
      this.saveRunSnapshotToDb();
      this.emit("players");
    }

    // --- Anywhere Quick-Trade Atomic Escrow (Channel 5) ---
    _sanitizeTradeOffer(rawOffer) {
      if (!rawOffer || typeof rawOffer !== "object") {
        return { partyIndex: -1, mon: null, items: [], money: 0 };
      }
      const pIdx = Number(rawOffer.partyIndex);
      const partyIndex = Number.isInteger(pIdx) && pIdx >= 0 && pIdx < 6 ? pIdx : -1;
      const moneyNum = Number(rawOffer.money);
      const money = Number.isFinite(moneyNum) && moneyNum > 0 ? Math.floor(moneyNum) : 0;
      const items = [];
      if (Array.isArray(rawOffer.items)) {
        for (const it of rawOffer.items.slice(0, 32)) {
          if (!it || typeof it !== "object" || !it.id) continue;
          const qty = Number(it.qty);
          if (!Number.isInteger(qty) || qty <= 0) continue;
          const itemId = resolveNumericItemId(it);
          const cleanItem = { id: String(it.id).slice(0, 48), name: String(it.name || it.id).slice(0, 48), qty };
          if (itemId > 0) cleanItem.itemId = itemId;
          items.push(cleanItem);
        }
      }
      return {
        partyIndex,
        mon: partyIndex >= 0 && rawOffer.mon ? sanitizeMon(rawOffer.mon) : null,
        items,
        money,
      };
    }

    _ensureHostEscrowMap() {
      if (!this._hostEscrowSessions) this._hostEscrowSessions = new Map();
      return this._hostEscrowSessions;
    }

    openTradeWith(targetSlotIndex) {
      const target = this.state.players[targetSlotIndex];
      const me = this.state.players[this.state.connection.mySlotIndex];
      if (!target || !target.connected || !me || targetSlotIndex === me.slotIndex) return;
      const gateReason = this.getFeatureGateReason(targetSlotIndex);
      if (gateReason) {
        this._emitLocalGateRejection("trade", gateReason);
        return;
      }

      const sessionId = generateId("trd");
      this.state.trade = {
        active: true,
        sessionId,
        partnerPeerId: target.peerId,
        partnerSlotIndex: target.slotIndex,
        partnerName: target.name,
        myOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
        partnerOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
        myLocked: false,
        partnerLocked: false,
        myConfirmed: false,
        partnerConfirmed: false,
        statusText: `Negotiating trade with ${target.name}. Add Pokémon, Bag Items, or ¥.`,
      };

      if (this.isNetworkHost()) {
        this._ensureHostEscrowMap().set(sessionId, {
          aPeer: 1,
          bPeer: target.peerId,
          aOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
          bOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
          aLocked: false,
          bLocked: false,
          aConfirmed: false,
          bConfirmed: false,
        });
      }

      const req = {
        t: "trade_request",
        sessionId,
        fromPeerId: me.peerId,
        fromSlotIndex: me.slotIndex,
        fromName: me.name,
        targetPeerId: target.peerId,
      };
      if (this.isNetworkHost()) {
        this.sendJson(CH_TRADE, target.peerId, req);
      } else {
        this.sendJsonToHost(CH_TRADE, req);
      }
      this.emit("trade");
    }

    updateMyTradeOffer(partyIndex, items, money) {
      const tr = this.state.trade;
      if (!tr.active) return;
      const me = this.state.players[this.state.connection.mySlotIndex];
      const cleanMoney = Math.max(0, Math.min(me.money, Math.floor(Number(money) || 0)));
      const idxNum = Number(partyIndex);
      const validIdx = Number.isInteger(idxNum) && idxNum >= 0 && me.party[idxNum] ? idxNum : -1;
      const mon = validIdx >= 0 ? cloneData(me.party[validIdx]) : null;
      tr.myOffer = this._sanitizeTradeOffer({
        partyIndex: validIdx,
        mon,
        items: Array.isArray(items) ? cloneData(items) : [],
        money: cleanMoney,
      });
      // Anti-scam: editing offer resets locks & confirmations
      tr.myLocked = false;
      tr.partnerLocked = false;
      tr.myConfirmed = false;
      tr.partnerConfirmed = false;
      tr.statusText = "Offer updated. Both trainers must Lock before Confirming.";

      if (this.isNetworkHost()) {
        const sess = this._ensureHostEscrowMap().get(tr.sessionId);
        if (sess) {
          if (sess.aPeer === 1) sess.aOffer = cloneData(tr.myOffer);
          else if (sess.bPeer === 1) sess.bOffer = cloneData(tr.myOffer);
          sess.aLocked = false;
          sess.bLocked = false;
          sess.aConfirmed = false;
          sess.bConfirmed = false;
        }
      }

      const msg = {
        t: "trade_offer_update",
        sessionId: tr.sessionId,
        targetPeerId: tr.partnerPeerId,
        offer: tr.myOffer,
      };
      if (this.isNetworkHost()) {
        this.sendJson(CH_TRADE, tr.partnerPeerId, msg);
      } else {
        this.sendJsonToHost(CH_TRADE, msg);
      }
      this.emit("trade");
    }

    toggleTradeLock(locked) {
      const tr = this.state.trade;
      if (!tr.active) return;
      tr.myLocked = locked !== undefined ? Boolean(locked) : !tr.myLocked;
      if (!tr.myLocked) {
        tr.myConfirmed = false;
        tr.partnerConfirmed = false;
      }
      tr.statusText = tr.myLocked
        ? tr.partnerLocked
          ? "Both locked! Click Confirm Atomic Trade."
          : "Offer locked. Waiting for partner to lock..."
        : "Offer unlocked.";

      if (this.isNetworkHost()) {
        const sess = this._ensureHostEscrowMap().get(tr.sessionId);
        if (sess) {
          if (sess.aPeer === 1) sess.aLocked = tr.myLocked;
          else if (sess.bPeer === 1) sess.bLocked = tr.myLocked;
          if (!tr.myLocked) {
            sess.aConfirmed = false;
            sess.bConfirmed = false;
          }
        }
      }

      const msg = {
        t: "trade_lock",
        sessionId: tr.sessionId,
        targetPeerId: tr.partnerPeerId,
        locked: tr.myLocked,
      };
      if (this.isNetworkHost()) {
        this.sendJson(CH_TRADE, tr.partnerPeerId, msg);
      } else {
        this.sendJsonToHost(CH_TRADE, msg);
      }
      this.emit("trade");
    }

    confirmTrade() {
      const tr = this.state.trade;
      if (!tr.active || !tr.myLocked || !tr.partnerLocked) return;
      tr.myConfirmed = true;
      tr.statusText = "Confirm sent to Host for atomic escrow validation...";

      if (this.isNetworkHost()) {
        this._hostTryCommitTrade(this.state.players[0], tr.sessionId);
      } else {
        this.sendJsonToHost(CH_TRADE, {
          t: "trade_confirm",
          sessionId: tr.sessionId,
          targetPeerId: tr.partnerPeerId,
        });
      }
      this.emit("trade");
    }

    cancelTrade() {
      const tr = this.state.trade;
      if (!tr.active) return;
      if (this.isNetworkHost()) {
        this._ensureHostEscrowMap().delete(tr.sessionId);
      }
      const msg = { t: "trade_cancel", sessionId: tr.sessionId, targetPeerId: tr.partnerPeerId };
      if (this.isNetworkHost()) {
        if (tr.partnerPeerId) this.sendJson(CH_TRADE, tr.partnerPeerId, msg);
      } else {
        this.sendJsonToHost(CH_TRADE, msg);
      }
      tr.active = false;
      tr.statusText = "Trade cancelled.";
      this.emit("trade");
    }

    _hostHandleTradeMsg(fromPeerId, senderSlot, msg) {
      if (!senderSlot || !msg || typeof msg.sessionId !== "string") return;
      const escrow = this._ensureHostEscrowMap();

      if (msg.t === "trade_request") {
        const targetPeerId = Number(msg.targetPeerId);
        const targetPlayer = this.state.players.find((p) => p.connected && p.peerId === targetPeerId);
        if (!targetPlayer || targetPeerId === fromPeerId) return;
        const gateReason = this._getHostSlotGateReason(senderSlot, targetPlayer);
        if (gateReason) {
          this._notifyHostGateRejection(fromPeerId, "trade", gateReason);
          return;
        }

        escrow.set(msg.sessionId, {
          aPeer: fromPeerId,
          bPeer: targetPeerId,
          aOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
          bOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
          aLocked: false,
          bLocked: false,
          aConfirmed: false,
          bConfirmed: false,
        });
        const cleanReq = {
          t: "trade_request",
          sessionId: msg.sessionId,
          fromPeerId,
          fromSlotIndex: senderSlot.slotIndex,
          fromName: senderSlot.name,
          targetPeerId,
        };
        this._notifyTradePeer(targetPeerId, cleanReq);
        return;
      }

      const sess = escrow.get(msg.sessionId);
      if (!sess || (fromPeerId !== sess.aPeer && fromPeerId !== sess.bPeer)) return;
      const counterpartyPeer = fromPeerId === sess.aPeer ? sess.bPeer : sess.aPeer;
      const counterpartyPlayer = this.state.players.find((p) => p.peerId === counterpartyPeer);
      if (msg.t !== "trade_cancel") {
        const gateReason = this._getHostSlotGateReason(senderSlot, counterpartyPlayer);
        if (gateReason) {
          escrow.delete(msg.sessionId);
          this._notifyHostGateRejection(fromPeerId, "trade", gateReason);
          this._notifyTradePeer(sess.aPeer, { t: "trade_failed", sessionId: msg.sessionId, reason: gateReason });
          this._notifyTradePeer(sess.bPeer, { t: "trade_failed", sessionId: msg.sessionId, reason: gateReason });
          return;
        }
      }

      if (msg.t === "trade_offer_update") {
        const cleanOffer = this._sanitizeTradeOffer(msg.offer);
        if (fromPeerId === sess.aPeer) sess.aOffer = cleanOffer;
        else sess.bOffer = cleanOffer;
        sess.aLocked = false;
        sess.bLocked = false;
        sess.aConfirmed = false;
        sess.bConfirmed = false;
        this._notifyTradePeer(counterpartyPeer, {
          t: "trade_offer_update",
          sessionId: msg.sessionId,
          offer: cleanOffer,
        });
        return;
      }

      if (msg.t === "trade_lock") {
        const locked = Boolean(msg.locked);
        if (fromPeerId === sess.aPeer) sess.aLocked = locked;
        else sess.bLocked = locked;
        if (!locked) {
          sess.aConfirmed = false;
          sess.bConfirmed = false;
        }
        this._notifyTradePeer(counterpartyPeer, {
          t: "trade_lock",
          sessionId: msg.sessionId,
          locked,
        });
        return;
      }

      if (msg.t === "trade_confirm") {
        this._hostTryCommitTrade(senderSlot, msg.sessionId);
        return;
      }

      if (msg.t === "trade_cancel") {
        escrow.delete(msg.sessionId);
        this._notifyTradePeer(counterpartyPeer, {
          t: "trade_cancel",
          sessionId: msg.sessionId,
        });
      }
    }

    _hostTryCommitTrade(senderSlot, sessionId) {
      const escrow = this._ensureHostEscrowMap();
      const sess = escrow.get(sessionId);
      if (!sess || !senderSlot || (senderSlot.peerId !== sess.aPeer && senderSlot.peerId !== sess.bPeer)) {
        if (senderSlot) {
          this._notifyTradePeer(senderSlot.peerId, {
            t: "trade_failed",
            sessionId,
            reason: "Trade session not found or expired.",
          });
        }
        return;
      }

      if (!(sess.aLocked && sess.bLocked)) {
        sess.aConfirmed = false;
        sess.bConfirmed = false;
        const failMsg = {
          t: "trade_failed",
          sessionId,
          reason: "Both trainers must lock offers before confirming.",
        };
        this._notifyTradePeer(sess.aPeer, failMsg);
        this._notifyTradePeer(sess.bPeer, failMsg);
        return;
      }

      if (senderSlot.peerId === sess.aPeer) {
        sess.aConfirmed = true;
      } else {
        sess.bConfirmed = true;
      }

      const otherPeer = senderSlot.peerId === sess.aPeer ? sess.bPeer : sess.aPeer;
      if (!(sess.aConfirmed && sess.bConfirmed)) {
        if (otherPeer === 1) {
          this.state.trade.partnerConfirmed = true;
          this.state.trade.statusText = "Partner confirmed! Click Confirm Atomic Trade to finalize.";
          this.emit("trade");
        } else {
          this.sendJson(CH_TRADE, otherPeer, { t: "trade_partner_confirmed", sessionId });
        }
        return;
      }

      // Atomic Validation & Execution using strictly Host-escrowed offers (never confirm payload)
      const pA = this.state.players.find((p) => p.peerId === sess.aPeer);
      const pB = this.state.players.find((p) => p.peerId === sess.bPeer);
      if (!pA || !pB) {
        escrow.delete(sessionId);
        return;
      }
      const gateReason = this._getHostSlotGateReason(pA, pB);
      if (gateReason) {
        escrow.delete(sessionId);
        const failMsg = { t: "trade_failed", sessionId, reason: gateReason };
        this._notifyTradePeer(sess.aPeer, failMsg);
        this._notifyTradePeer(sess.bPeer, failMsg);
        return;
      }

      const errA = this._validatePlayerTradeOffer(pA, sess.aOffer);
      const errB = this._validatePlayerTradeOffer(pB, sess.bOffer);
      let capErr = null;
      const aSendsMon = sess.aOffer.partyIndex >= 0;
      const bSendsMon = sess.bOffer.partyIndex >= 0;
      if (bSendsMon && !aSendsMon && pA.party.length >= 6) {
        capErr = `${pA.name}'s party is already full (6/6).`;
      } else if (aSendsMon && !bSendsMon && pB.party.length >= 6) {
        capErr = `${pB.name}'s party is already full (6/6).`;
      }
      if (!capErr) {
        const isMonAlive = (m) => Boolean(m && !m.isDead && (Number(m.hp) || 0) > 0);
        if (aSendsMon && isMonAlive(pA.party[sess.aOffer.partyIndex])) {
          const aliveAfterA = pA.party.filter((m, i) => i !== sess.aOffer.partyIndex && isMonAlive(m)).length;
          const bReceivesAlive = bSendsMon && isMonAlive(pB.party[sess.bOffer.partyIndex]);
          if (aliveAfterA === 0 && !bReceivesAlive) {
            capErr = `${pA.name} cannot trade away their last healthy Pokémon.`;
          }
        }
        if (!capErr && bSendsMon && isMonAlive(pB.party[sess.bOffer.partyIndex])) {
          const aliveAfterB = pB.party.filter((m, i) => i !== sess.bOffer.partyIndex && isMonAlive(m)).length;
          const aReceivesAlive = aSendsMon && isMonAlive(pA.party[sess.aOffer.partyIndex]);
          if (aliveAfterB === 0 && !aReceivesAlive) {
            capErr = `${pB.name} cannot trade away their last healthy Pokémon.`;
          }
        }
      }

      if (errA || errB || capErr) {
        sess.aLocked = false;
        sess.bLocked = false;
        sess.aConfirmed = false;
        sess.bConfirmed = false;
        const failReason = errA || errB || capErr;
        const failMsg = { t: "trade_failed", sessionId, reason: failReason };
        this._notifyTradePeer(pA.peerId, failMsg);
        this._notifyTradePeer(pB.peerId, failMsg);
        return;
      }

      // Snapshot pre-evolution offers for WASM bridge commit sync before atomic swap mutates arrays/species
      const commitOfferA = {
        ...cloneData(sess.aOffer),
        mon: aSendsMon && pA.party[sess.aOffer.partyIndex] ? cloneData(pA.party[sess.aOffer.partyIndex]) : null,
      };
      const commitOfferB = {
        ...cloneData(sess.bOffer),
        mon: bSendsMon && pB.party[sess.bOffer.partyIndex] ? cloneData(pB.party[sess.bOffer.partyIndex]) : null,
      };

      // Execute Atomic Swap
      let monA = null;
      let monB = null;
      if (aSendsMon) {
        monA = pA.party.splice(sess.aOffer.partyIndex, 1)[0];
      }
      if (bSendsMon) {
        monB = pB.party.splice(sess.bOffer.partyIndex, 1)[0];
      }

      const evoNotes = [];
      if (monA) {
        const evolved = TRADE_EVOLUTIONS[monA.species];
        if (evolved) {
          evoNotes.push(`${monA.nickname} (${monA.species} → ${evolved})`);
          if (!monA.nickname || monA.nickname.toLowerCase() === String(monA.species).toLowerCase()) {
            monA.nickname = evolved;
          }
          monA.species = evolved;
        }
        pB.party.push(monA);
      }
      if (monB) {
        const evolved = TRADE_EVOLUTIONS[monB.species];
        if (evolved) {
          evoNotes.push(`${monB.nickname} (${monB.species} → ${evolved})`);
          if (!monB.nickname || monB.nickname.toLowerCase() === String(monB.species).toLowerCase()) {
            monB.nickname = evolved;
          }
          monB.species = evolved;
        }
        pA.party.push(monB);
      }

      // Transfer Money
      const moneyA = Number(sess.aOffer.money) || 0;
      const moneyB = Number(sess.bOffer.money) || 0;
      pA.money = pA.money - moneyA + moneyB;
      pB.money = pB.money - moneyB + moneyA;

      // Transfer Bag Items
      this._transferBagItems(pA, pB, sess.aOffer.items || []);
      this._transferBagItems(pB, pA, sess.bOffer.items || []);

      const summary = `Trade complete between ${pA.name} & ${pB.name}${evoNotes.length ? " • Evolved: " + evoNotes.join(", ") : ""}!`;
      this.addLog(summary, "gold");
      this.saveRunSnapshotToDb();

      const commitMsg = {
        t: "trade_commit",
        sessionId,
        sessionIdNum: toNumericTradeSessionId(sessionId),
        slotA: pA.slotIndex,
        slotB: pB.slotIndex,
        offerA: commitOfferA,
        offerB: commitOfferB,
        players: this.state.players,
        summary,
      };
      this.broadcastControl({ t: "players_sync", players: this.state.players });
      this._notifyTradePeer(pA.peerId, commitMsg);
      this._notifyTradePeer(pB.peerId, commitMsg);
      escrow.delete(sessionId);
      this.emit("players");
    }

    _notifyTradePeer(peerId, msg) {
      if (peerId === 1) {
        this._clientHandleTradeMsg(msg);
      } else {
        this.sendJson(CH_TRADE, peerId, msg);
      }
    }

    _validatePlayerTradeOffer(player, offer) {
      if (!offer || typeof offer !== "object") return "Invalid trade offer.";
      const money = Number(offer.money);
      if (!Number.isInteger(money) || money < 0 || money > player.money) {
        return `${player.name} has insufficient Pokédollars (¥).`;
      }
      const pIdx = Number(offer.partyIndex);
      if (!Number.isInteger(pIdx) || pIdx < -1 || pIdx >= player.party.length) {
        return `${player.name}'s offered Pokémon slot is invalid.`;
      }
      if (pIdx >= 0 && !player.party[pIdx]) {
        return `${player.name}'s offered Pokémon is no longer in party.`;
      }
      const aggregated = new Map();
      for (const reqItem of offer.items || []) {
        if (!reqItem || typeof reqItem !== "object" || !reqItem.id) {
          return `${player.name} offered an invalid item.`;
        }
        const qty = Number(reqItem.qty);
        if (!Number.isInteger(qty) || qty <= 0) {
          return `${player.name} offered invalid item quantity (${reqItem.qty}).`;
        }
        const prev = aggregated.get(reqItem.id) || { name: reqItem.name || reqItem.id, qty: 0 };
        prev.qty += qty;
        aggregated.set(reqItem.id, prev);
      }
      for (const [itemId, req] of aggregated.entries()) {
        const inv = player.bagItems.find((i) => i.id === itemId);
        if (!inv || inv.qty < req.qty) {
          return `${player.name} lacks ${req.qty}x ${req.name}.`;
        }
      }
      return null;
    }

    _transferBagItems(fromPlayer, toPlayer, items) {
      for (const item of items) {
        const qty = Number(item.qty) || 0;
        if (qty <= 0) continue;
        const src = fromPlayer.bagItems.find((i) => i.id === item.id);
        if (!src || src.qty < qty) continue;
        src.qty -= qty;
        if (src.qty <= 0) {
          fromPlayer.bagItems = fromPlayer.bagItems.filter((i) => i.id !== item.id);
        }
        const dst = toPlayer.bagItems.find((i) => i.id === item.id);
        if (dst) dst.qty += qty;
        else toPlayer.bagItems.push({ id: item.id, name: item.name, qty });
      }
    }

    _clientHandleTradeMsg(msg) {
      const tr = this.state.trade;
      if (msg.t === "trade_request") {
        this.state.trade = {
          active: true,
          sessionId: msg.sessionId,
          partnerPeerId: msg.fromPeerId,
          partnerSlotIndex: msg.fromSlotIndex,
          partnerName: msg.fromName,
          myOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
          partnerOffer: { partyIndex: -1, mon: null, items: [], money: 0 },
          myLocked: false,
          partnerLocked: false,
          myConfirmed: false,
          partnerConfirmed: false,
          statusText: `${msg.fromName} opened a Quick-Trade session!`,
        };
        this.emit("toast", { kind: "gold", title: "Trade Request", message: `${msg.fromName} wants to Quick-Trade!` });
        this.emit("trade_open");
      } else if (msg.t === "trade_offer_update" && msg.sessionId === tr.sessionId) {
        tr.partnerOffer = msg.offer || { partyIndex: -1, mon: null, items: [], money: 0 };
        tr.myLocked = false;
        tr.partnerLocked = false;
        tr.myConfirmed = false;
        tr.partnerConfirmed = false;
        tr.statusText = `${tr.partnerName} updated their offer (locks reset).`;
        this.emit("trade");
      } else if (msg.t === "trade_lock" && msg.sessionId === tr.sessionId) {
        tr.partnerLocked = Boolean(msg.locked);
        if (!tr.partnerLocked) {
          tr.myConfirmed = false;
          tr.partnerConfirmed = false;
        }
        tr.statusText = tr.partnerLocked
          ? tr.myLocked
            ? "Both locked! Click Confirm Atomic Trade."
            : `${tr.partnerName} locked their offer.`
          : `${tr.partnerName} unlocked their offer.`;
        this.emit("trade");
      } else if (msg.t === "trade_partner_confirmed" && msg.sessionId === tr.sessionId) {
        tr.partnerConfirmed = true;
        tr.statusText = `${tr.partnerName} confirmed! Click Confirm Atomic Trade.`;
        this.emit("trade");
      } else if (msg.t === "trade_commit" && msg.sessionId === tr.sessionId) {
        if (Array.isArray(msg.players)) this.state.players = msg.players;
        tr.active = false;
        tr.statusText = msg.summary || "Trade completed!";
        this.emit("toast", { kind: "ok", title: "Atomic Trade Complete", message: tr.statusText });
        global.PokemonWasmBridge._applyTradeToWasm(msg);
        this.emit("trade");
        this.emit("players");
      } else if (msg.t === "trade_failed" && msg.sessionId === tr.sessionId) {
        tr.myLocked = false;
        tr.partnerLocked = false;
        tr.myConfirmed = false;
        tr.partnerConfirmed = false;
        tr.statusText = `Validation failed: ${msg.reason}`;
        this.emit("toast", { kind: "danger", title: "Trade Failed", message: msg.reason });
        this.emit("trade");
      } else if (msg.t === "trade_cancel" && msg.sessionId === tr.sessionId) {
        tr.active = false;
        tr.statusText = `${tr.partnerName} cancelled the trade.`;
        this.emit("trade");
      }
    }

    // --- Hybrid Campaign Run Save Manager (IndexedDB + JSON Export/Import) ---
    buildRunSnapshot(options) {
      let savedOk = false;
      this._insideBuildRunSnapshot = true;
      try {
        if (
          !(options && options.skipWasmSave) &&
          global.PokemonWasmBridge &&
          typeof global.PokemonWasmBridge._saveGameInWasm === "function"
        ) {
          savedOk = Boolean(global.PokemonWasmBridge._saveGameInWasm());
        }
      } finally {
        this._insideBuildRunSnapshot = false;
      }
      const myRunId = this.state.runConfig.runId;
      const myIdx = this.state.connection.mySlotIndex || 0;
      const flashSavesBySlot = cloneData(this._flashSavesBySlot || {});
      if (savedOk) {
        writeFlashSaveMeta(myRunId);
      }
      const meta = readFlashSaveMeta();
      let flashSaveB64 = "";
      const canExportFlashForRun = !meta.runId || !myRunId || meta.runId === myRunId;
      if (savedOk || canExportFlashForRun) {
        flashSaveB64 = exportWasmFlashBase64();
        if (flashSaveB64) {
          flashSavesBySlot[myIdx] = flashSaveB64;
        }
      } else {
        flashSaveB64 = flashSavesBySlot[myIdx] || "";
      }
      this._flashSavesBySlot = flashSavesBySlot;
      return {
        schema: "pokemon_emerald_mp_run_v1",
        runId: this.state.runConfig.runId,
        savedAt: new Date().toISOString(),
        runConfig: cloneData(this.state.runConfig),
        world: cloneData(this.state.world),
        soulLink: cloneData(this.state.soulLink),
        playerSlots: cloneData(this.state.players),
        flashSaveB64,
        flashSavesBySlot,
      };
    }

    async applySnapshotLocal(snapshot, markAsHost, options) {
      if (!snapshot || !snapshot.runConfig) return false;
      const fromBroadcast = Boolean(options && options.fromBroadcast);
      const prevMeta = readFlashSaveMeta();
      const targetRunId = snapshot.runConfig.runId || snapshot.runId || "";
      if (!fromBroadcast && prevMeta.runId && prevMeta.runId !== targetRunId) {
        await this.flushActiveFlashToCache();
      }
      this.state.runConfig = normalizeRunConfig(cloneData(snapshot.runConfig), makeDefaultRunConfig());
      this._gameOverToastShown = Boolean(this.state.runConfig.gameOver);
      if (snapshot.world) this.state.world = cloneData(snapshot.world);
      if (snapshot.soulLink) this.state.soulLink = cloneData(snapshot.soulLink);
      if (Array.isArray(snapshot.playerSlots)) {
        this.state.players = cloneData(snapshot.playerSlots).map((p, idx) =>
          sanitizePlayerSlot(
            {
              ...p,
              connected: markAsHost ? idx === 0 : p.connected,
            },
            idx
          )
        );
      }
      this._flashSavesBySlot = cloneData(snapshot.flashSavesBySlot || {});
      if (!fromBroadcast) {
        const myIdx = markAsHost ? 0 : (this.state.connection.mySlotIndex || 0);
        const snapSlotFlash =
          (snapshot.flashSavesBySlot && snapshot.flashSavesBySlot[myIdx]) ||
          (markAsHost ? snapshot.flashSaveB64 : "") ||
          "";
        const idbEntry = targetRunId ? await this.getCachedSlotFlashLocal(targetRunId, myIdx) : null;
        const meta = readFlashSaveMeta();
        const snapSavedMs = Date.parse(snapshot.savedAt || "") || 0;
        const snapRev = Number((this.state.players[myIdx] && this.state.players[myIdx].flashRev) || 0);
        const idbRev = idbEntry ? Number(idbEntry.flashRev) || 0 : 0;
        const keepNewerLocal =
          hasValidWasmFlashSave() &&
          meta.runId &&
          meta.runId === targetRunId &&
          (meta.flashRev > snapRev || meta.savedAtMs > snapSavedMs);
        if (keepNewerLocal) {
          const newerB64 = exportWasmFlashBase64();
          if (newerB64) {
            this._flashSavesBySlot[myIdx] = newerB64;
          }
        } else if (idbEntry && idbEntry.flashSaveB64 && (idbRev > snapRev || !snapSlotFlash)) {
          if (importWasmFlashBase64(idbEntry.flashSaveB64)) {
            this._flashSavesBySlot[myIdx] = idbEntry.flashSaveB64;
            writeFlashSaveMeta(targetRunId, idbEntry.savedAtMs || Date.now(), {
              userId: this._getEffectiveUserId(),
              slotIndex: myIdx,
              flashRev: idbRev,
            });
          }
        } else if (snapSlotFlash) {
          if (importWasmFlashBase64(snapSlotFlash)) {
            writeFlashSaveMeta(targetRunId, snapSavedMs || Date.now(), {
              userId: this._getEffectiveUserId(),
              slotIndex: myIdx,
              flashRev: snapRev,
            });
          }
        }
        this._applySpeed(this.state.runConfig.speedMultiplier);
        this._pendingRunConfigSync = { fromPeerId: 1, msg: { t: "run_config_sync" } };
        if (global.PokemonWasmBridge._dispatchWasmJson(CH_CONTROL, 1, { t: "run_config_sync" })) {
          this._pendingRunConfigSync = null;
        }
        if (
          this.state.world &&
          ((Array.isArray(this.state.world.badges) && this.state.world.badges.some(Boolean)) ||
            (this.state.world.globalFlags && Object.keys(this.state.world.globalFlags).length > 0) ||
            (this.state.world.globalVars && Object.keys(this.state.world.globalVars).length > 0))
        ) {
          const worldMsg = { t: "world_sync", badges: this.state.world.badges, world: this.state.world };
          this._pendingWorldSync = { fromPeerId: 1, msg: worldMsg };
          if (global.PokemonWasmBridge._dispatchWasmJson(CH_WORLD_SYNC, 1, worldMsg)) {
            this._pendingWorldSync = null;
          }
        }
        global.PokemonWasmBridge._loadSnapshotIntoWasm(snapshot);
      } else {
        this._applySpeed(this.state.runConfig.speedMultiplier);
      }
      global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
      this.emit("all");
      return true;
    }

    async buildSnapshotFromCloudRun(cloudRun, slotIdxToFetch) {
      if (!cloudRun || !cloudRun.runId) return null;
      const runId = cloudRun.runId;
      const worldState = cloudRun.worldState && typeof cloudRun.worldState === "object" ? cloudRun.worldState : {};
      const runCfg = normalizeRunConfig(
        {
          ...(cloudRun.runConfig || {}),
          runId,
          roomName: cloudRun.title || (cloudRun.runConfig && cloudRun.runConfig.roomName) || "Hoenn Run",
          seed: cloudRun.seed || (cloudRun.runConfig && cloudRun.runConfig.seed) || "",
          randomizerPreset:
            cloudRun.randomizerPreset ||
            (cloudRun.runConfig && cloudRun.runConfig.randomizerPreset) ||
            "balanced",
        },
        makeDefaultRunConfig()
      );
      const players = [0, 1, 2, 3].map((idx) => {
        const s = Array.isArray(cloudRun.slots)
          ? cloudRun.slots.find((item) => Number(item && item.slotIdx) === idx)
          : null;
        if (!s) return makeEmptyPlayerSlot(idx);
        return sanitizePlayerSlot(
          {
            slotIndex: idx,
            userId: s.userId || "",
            username: s.username || "",
            name: s.characterName || s.username || `Slot ${idx + 1}`,
            tutorialComplete: Boolean(s.tutorialComplete),
            party: Array.isArray(s.party) ? s.party : [],
            money: Number(s.money) || 0,
            bagItems: Array.isArray(s.bagItems) ? s.bagItems : [],
            flashRev: Number(s.flashRev) || 0,
            connected: idx === 0,
          },
          idx
        );
      });
      const flashSavesBySlot = {};
      const targetSlot = Number.isInteger(Number(slotIdxToFetch)) ? Number(slotIdxToFetch) : 0;
      const slotsToFetch = new Set([targetSlot]);
      if (targetSlot === 0 && Array.isArray(cloudRun.slots)) {
        for (const s of cloudRun.slots) {
          if (!s) continue;
          const sIdx = Number(s.slotIdx);
          if (Number.isInteger(sIdx) && sIdx >= 0 && sIdx < 4 && (s.hasFlashSave || Number(s.flashRev) > 0)) {
            slotsToFetch.add(sIdx);
          }
        }
      }
      await Promise.all(
        Array.from(slotsToFetch).map(async (sIdx) => {
          const fetched = await this.fetchSlotFlashFromBackend(runId, sIdx);
          if (fetched && fetched.flashSaveB64) {
            flashSavesBySlot[sIdx] = fetched.flashSaveB64;
            if (players[sIdx]) {
              players[sIdx].flashRev = Math.max(players[sIdx].flashRev || 0, fetched.flashRev || 0);
            }
          }
        })
      );
      return {
        schema: "pokemon_emerald_mp_run_v1",
        runId,
        savedAt: cloudRun.updatedAt ? new Date(cloudRun.updatedAt).toISOString() : new Date().toISOString(),
        runConfig: runCfg,
        world: worldState.world || makeInitialState().world,
        soulLink: worldState.soulLink || makeInitialState().soulLink,
        playerSlots: players,
        flashSaveB64: flashSavesBySlot[0] || "",
        flashSavesBySlot,
      };
    }

    async saveRunSnapshotToDb(explicitSnapshot, options) {
      const snap = explicitSnapshot || this.buildRunSnapshot(options);
      await idbPut(RUN_DB_NAME, RUN_STORE_NAME, snap.runId, snap);
      const myIdx = this.state.connection.mySlotIndex || 0;
      const mySlot = this.state.players[myIdx];
      if (snap.flashSaveB64 && snap.runId) {
        await this.cacheSlotFlashLocal(
          snap.runId,
          myIdx,
          snap.flashSaveB64,
          (mySlot && mySlot.flashRev) || 0,
          Date.now()
        );
      }
      try {
        const list = safeParseJson(localStorage.getItem(STORAGE_KEYS.runsFallback) || "[]") || [];
        const filtered = list.filter((r) => r.runId !== snap.runId);
        filtered.unshift(snap);
        localStorage.setItem(STORAGE_KEYS.runsFallback, JSON.stringify(filtered.slice(0, 8)));
      } catch (_) {}

      const silentBroadcast = Boolean(options && options.silentBroadcast);
      if (this.isNetworkHost() && !explicitSnapshot && !silentBroadcast) {
        this.broadcastControl({ t: "snapshot_sync", snapshot: snap });
      }
      if (this.isHost() && this.state.auth && this.state.auth.status === "signed_in") {
        const needsImmediateUpsert =
          this.isNetworkHost() && !(this._upsertedRunIds && this._upsertedRunIds.has(snap.runId));
        if (needsImmediateUpsert) {
          if (this._cloudUpsertTimer) {
            clearTimeout(this._cloudUpsertTimer);
            this._cloudUpsertTimer = null;
          }
          this.syncRunUpsertToBackend(snap).catch(() => {});
        } else if (this.isNetworkHost() || (this._upsertedRunIds && this._upsertedRunIds.has(snap.runId))) {
          if (this._cloudUpsertTimer) clearTimeout(this._cloudUpsertTimer);
          this._cloudUpsertTimer = setTimeout(() => {
            this._cloudUpsertTimer = null;
            this.syncRunUpsertToBackend(snap).catch(() => {});
          }, 200);
        }
      }
      this.emit("saves");
      return snap;
    }

    async listSavedRuns() {
      let runs = await idbGetAll(RUN_DB_NAME, RUN_STORE_NAME);
      if (!runs || runs.length === 0) {
        runs = safeParseJson(localStorage.getItem(STORAGE_KEYS.runsFallback) || "[]") || [];
      }
      runs.sort((a, b) => String(b.savedAt || "").localeCompare(String(a.savedAt || "")));
      return runs;
    }

    loadDemoPreviewState() {
      const slot = this.state.players[this.state.connection.mySlotIndex];
      if (!slot || slot.party.length > 0) return false;
      slot.money = 3000;
      slot.bagItems = [
        { id: "ITEM_POTION", name: "Potion", qty: 5 },
        { id: "ITEM_POKE_BALL", name: "Poké Ball", qty: 5 },
      ];
      slot.party = [
        {
          uid: "demo_starter_1",
          species: "Treecko",
          nickname: "Scepty",
          level: 12,
          hp: 34,
          maxHp: 34,
          status: "",
          ball: "pokeball",
          heldItem: "Oran Berry",
          linkId: "LINK-01",
          originMap: "Route 101",
          isDead: false,
        },
        {
          uid: "demo_r102_1",
          species: "Ralts",
          nickname: "Aria",
          level: 10,
          hp: 24,
          maxHp: 28,
          status: "",
          ball: "greatball",
          heldItem: "",
          linkId: "LINK-02",
          originMap: "Route 102",
          isDead: false,
        },
      ];
      this.state.soulLink.nextLinkNum = Math.max(this.state.soulLink.nextLinkNum || 1, 3);
      this.emit("players");
      return true;
    }

    async deleteSavedRun(runId) {
      await idbDelete(RUN_DB_NAME, RUN_STORE_NAME, runId);
      const uid = this._getEffectiveUserId();
      for (let idx = 0; idx < 4; idx++) {
        await idbDelete(FLASH_CACHE_DB_NAME, FLASH_CACHE_STORE_NAME, makeFlashCacheKey(uid, runId, idx));
        if (uid !== "guest") {
          await idbDelete(FLASH_CACHE_DB_NAME, FLASH_CACHE_STORE_NAME, makeFlashCacheKey("guest", runId, idx));
        }
      }
      try {
        const list = safeParseJson(localStorage.getItem(STORAGE_KEYS.runsFallback) || "[]") || [];
        localStorage.setItem(
          STORAGE_KEYS.runsFallback,
          JSON.stringify(list.filter((r) => r.runId !== runId))
        );
      } catch (_) {}
      this.emit("saves");
    }

    resetNewGame(options = {}) {
      const mode = options.mode === "littleroot" ? "littleroot" : "title";
      clearLocalFlashSave();
      this._flashSavesBySlot = {};
      if (this._autoSaveDbTimer) {
        clearTimeout(this._autoSaveDbTimer);
        this._autoSaveDbTimer = null;
      }
      this._pendingWasmResume = false;
      this._gameOverToastShown = false;
      if (this.state.runConfig) {
        this.state.runConfig.gameOver = false;
      }

      const myIdx = this.state.connection.mySlotIndex || 0;
      const myRunId = (this.state.runConfig && this.state.runConfig.runId) || "";
      const me = this.state.players[myIdx];
      if (me) {
        me.party = [];
        me.bagItems = [];
        me.money = 3000;
        me.inBattle = false;
        me.busy = false;
        me.tutorialComplete = false;
        me.flashRev = 0;
      }
      if (myRunId) {
        idbDelete(
          FLASH_CACHE_DB_NAME,
          FLASH_CACHE_STORE_NAME,
          makeFlashCacheKey(this._getEffectiveUserId(), myRunId, myIdx)
        ).catch(() => {});
      }
      if (!this.isClient()) {
        this.state.world = {
          badges: [false, false, false, false, false, false, false, false],
          hms: [],
          unlockedMaps: ["0:0", "0:1", "0:9", "0:16", "0:17", "0:18", "0:19"],
          defeatedTrainers: [],
          completedCutscenes: {},
          globalFlags: {},
          globalVars: {},
        };
        this.state.soulLink = {
          nextLinkNum: 1,
          routeLocks: {},
          graveyard: [],
        };
      }
      if (this.state.trade && this.state.trade.active) {
        this.cancelTrade();
      }
      global.PokemonWasmBridge._syncPeersToWasm(this.state.players);
      global.PokemonWasmBridge._syncSoulLinkToWasm(this.state);
      if (this.isNetworkHost()) {
        this.broadcastControl({ t: "players_sync", players: this.state.players });
        this.broadcastControl({ t: "run_config_sync", runConfig: this.state.runConfig });
        this.pushRoomInfo();
      } else if (this.isClient()) {
        this.sendJsonToHost(CH_CONTROL, { t: "flash_save_clear" });
        this.sendJsonToHost(CH_CONTROL, {
          t: "state_sync",
          explicitReset: true,
          party: [],
          bagItems: [],
          money: 3000,
          inBattle: false,
          busy: false,
          tutorialComplete: false,
        });
      }

      global.PokemonWasmBridge._resetNewGameInWasm(mode);
      this._pendingRunConfigSync = { fromPeerId: 1, msg: { t: "run_config_sync" } };
      if (global.PokemonWasmBridge._dispatchWasmJson(CH_CONTROL, 1, { t: "run_config_sync" })) {
        this._pendingRunConfigSync = null;
      }
      this.addLog(
        mode === "littleroot"
          ? "Flash save erased — quick-started fresh New Game in Littleroot Town."
          : "Flash save erased — restarted at Title Screen (New Game).",
        "warn"
      );
      this.emit("all");
      return true;
    }
  }

  // --- Instantiate Singleton & Expose Emscripten WASM Bridge ---
  const client = new PokemonRelayClient();
  global.PokemonNet = client;
  global.PokemonConstants = {
    FRAME_DATA,
    FRAME_PEER_CONNECTED,
    FRAME_PEER_DISCONNECTED,
    FRAME_KICK_PEER,
    FRAME_ROOM_INFO,
    FRAME_HOST_ASSIGNED,
    FRAME_REJECTED,
    CH_CONTROL,
    CH_OVERWORLD,
    CH_WORLD_SYNC,
    CH_BATTLE,
    CH_SOUL_LINK,
    CH_TRADE,
    MP_BATTLE_SOLO_2V2: 0,
    MP_BATTLE_COOP_2V2: 1,
    MP_BATTLE_PVP: 2,
    RANDOMIZER_PRESETS,
    NUZLOCKE_PRESETS,
  };
  global.PokemonAssetLoader = { loadChunkedAssetPack };

  global.PokemonWasmBridge = {
    _getWasmModule() {
      return client.wasmModule;
    },

    onWasmReady(moduleObj) {
      client.wasmModule = moduleObj;
      client.state.wasmReady = true;
      if (moduleObj && typeof moduleObj._mp_set_speed_multiplier === "function") {
        moduleObj._mp_set_speed_multiplier(client.state.runConfig.speedMultiplier);
      }
      if (moduleObj && client._gamepadInputMask) {
        this.setInputMask(client._gamepadInputMask, "gamepad");
      }
      this._syncPeersToWasm(client.state.players);
      this._syncSoulLinkToWasm(client.state);
      if (client._pendingRunConfigSync) {
        const pendingCfg = client._pendingRunConfigSync;
        client._pendingRunConfigSync = null;
        this._dispatchWasmJson(
          CH_CONTROL,
          pendingCfg.fromPeerId || 1,
          pendingCfg.msg || { t: "run_config_sync" },
          pendingCfg.rawBytes
        );
      } else {
        this._dispatchWasmJson(CH_CONTROL, 1, { t: "run_config_sync" });
      }
      if (client._pendingWorldSync) {
        const pendingWorld = client._pendingWorldSync;
        client._pendingWorldSync = null;
        this._dispatchWasmJson(CH_WORLD_SYNC, pendingWorld.fromPeerId || 1, pendingWorld.msg);
      }
      const hasAutoJoinRoom =
        typeof global.location !== "undefined" &&
        global.location.search &&
        Number(new URLSearchParams(global.location.search).get("room")) > 0;
      const scheduleSignedInAutoResume = async () => {
        if (client._validateSessionPromise) {
          await client._validateSessionPromise.catch(() => {});
        }
        if (!client.isSignedIn()) {
          client._pendingWasmResume = false;
          return;
        }
        const meta = readFlashSaveMeta();
        if (meta.userId && meta.userId !== "guest" && meta.userId !== client.state.auth.userId) {
          return;
        }
        if (
          client._pendingWasmResume ||
          (!hasAutoJoinRoom &&
            client.state.connection.status === "offline" &&
            hasValidWasmFlashSave() &&
            meta.userId === client.state.auth.userId)
        ) {
          client._pendingWasmResume = false;
          if (client._wasmResumeTimer) clearTimeout(client._wasmResumeTimer);
          client._wasmResumeTimer = setTimeout(() => {
            client._wasmResumeTimer = null;
            if (!client.isSignedIn() || client._pendingWasmResetMode || client.state.connection.status === "connecting") {
              return;
            }
            const m = client.wasmModule;
            if (m && typeof m._mp_on_snapshot_loaded === "function") {
              m._mp_on_snapshot_loaded();
            }
            this._syncSoulLinkToWasm(client.state);
          }, 120);
        }
      };
      if (client._pendingWasmResetMode) {
        const pendingReset = client._pendingWasmResetMode;
        client._pendingWasmResetMode = "";
        this._resetNewGameInWasm(pendingReset);
      } else {
        scheduleSignedInAutoResume();
      }
      client.addLog("Emscripten pokeemerald-expansion WASM core booted!", "gold");
      client.emit("wasm");
    },

    isAuthSignedIn() {
      return Boolean(client && client.isSignedIn());
    },

    _returnToTitleScreenInWasm(options) {
      const autosave = options && options.autosave ? 1 : 0;
      const m = client.wasmModule;
      if (m && typeof m._mp_return_to_title_screen === "function") {
        return Boolean(m._mp_return_to_title_screen(autosave));
      }
      if (autosave && m && typeof m._mp_save_game_now === "function") {
        return Boolean(m._mp_save_game_now());
      }
      return false;
    },

    _saveGameInWasm() {
      const m = client.wasmModule;
      if (m && typeof m._mp_save_game_now === "function") {
        return Boolean(m._mp_save_game_now());
      }
      return false;
    },

    _onResumeFallback(saveStatus) {
      const st = Number(saveStatus);
      client._resumeFallbackOccurred = true;
      if (st !== 0) {
        console.warn("[WASM] Flash resume fallback status=" + st);
      }
    },

    _onFlashSavedInWasm() {
      if (client._suppressLocalFlashSync) return;
      const myRunId = client.state.runConfig && client.state.runConfig.runId;
      const myIdx = client.state.connection.mySlotIndex || 0;
      const mySlot = client.state.players[myIdx];
      const hasProgress = Boolean(
        mySlot && ((Array.isArray(mySlot.party) && mySlot.party.length > 0) || mySlot.tutorialComplete)
      );
      if (client._ignoreBlankSaveUntilMs && Date.now() < client._ignoreBlankSaveUntilMs && !hasProgress) {
        return;
      }
      const b64 = exportWasmFlashBase64();
      const prevRev = mySlot ? Math.max(0, Number(mySlot.flashRev) || 0) : 0;
      const nextRev = prevRev + 1;
      if (mySlot) mySlot.flashRev = nextRev;
      writeFlashSaveMeta(myRunId, Date.now(), {
        userId: client._getEffectiveUserId(),
        slotIndex: myIdx,
        flashRev: nextRev,
      });
      if (b64) {
        if (!client._flashSavesBySlot) client._flashSavesBySlot = {};
        client._flashSavesBySlot[myIdx] = b64;
        if (myRunId) {
          client.cacheSlotFlashLocal(myRunId, myIdx, b64, nextRev, Date.now()).catch(() => {});
        }
        const isKnownCloudRun = Boolean(
          myRunId &&
            ((client._upsertedRunIds && client._upsertedRunIds.has(myRunId)) ||
              (client.cloudRuns &&
                [...(client.cloudRuns.hostedRuns || []), ...(client.cloudRuns.joinedRuns || [])].some(
                  (r) => r && r.runId === myRunId
                )))
        );
        const shouldSyncCloud = Boolean(
          myRunId &&
            client.state.auth &&
            client.state.auth.status === "signed_in" &&
            (client.isNetworkHost() || client.isClient() || isKnownCloudRun)
        );
        if (client.isClient()) {
          client.sendJsonToHost(CH_CONTROL, {
            t: "flash_save_sync",
            flashSaveB64: b64,
            flashRev: nextRev,
            needsHostBackup: !shouldSyncCloud,
          });
        }
        if (shouldSyncCloud) {
          client
            .syncSlotFlashToBackend(myRunId, myIdx, b64, nextRev, mySlot, { expectedRev: prevRev })
            .then((res) => {
              if (client.isClient() && (!res || (!res.ok && res.status !== 409))) {
                client.sendJsonToHost(CH_CONTROL, {
                  t: "flash_save_sync",
                  flashSaveB64: b64,
                  flashRev: nextRev,
                  needsHostBackup: true,
                });
              }
            })
            .catch(() => {
              if (client.isClient()) {
                client.sendJsonToHost(CH_CONTROL, {
                  t: "flash_save_sync",
                  flashSaveB64: b64,
                  flashRev: nextRev,
                  needsHostBackup: true,
                });
              }
            });
        }
      }
      if (client._insideBuildRunSnapshot) return;
      if (client._autoSaveDbTimer) clearTimeout(client._autoSaveDbTimer);
      client._autoSaveDbTimer = setTimeout(() => {
        client._autoSaveDbTimer = null;
        if (typeof client.saveRunSnapshotToDb === "function") {
          client.saveRunSnapshotToDb(undefined, { silentBroadcast: true, skipWasmSave: true }).catch(() => {});
        }
      }, 80);
    },

    setGamepadMask(mask) {
      return this.setInputMask(mask, "gamepad");
    },

    setKeyboardMask(mask) {
      const clean = client.isSignedIn() ? (Number(mask) || 0) & 0x03ff : 0;
      client._keyboardInputMask = clean;
      const m = client.wasmModule;
      if (m && typeof m._wasm_set_gamepad_mask !== "function" && typeof m._wasm_set_input_mask === "function") {
        const gp = client.isSignedIn() ? client._gamepadInputMask || 0 : 0;
        m._wasm_set_input_mask((clean | gp) & 0x03ff);
      }
      return clean;
    },

    setInputMask(mask, source) {
      const clean = client.isSignedIn() ? (Number(mask) || 0) & 0x03ff : 0;
      const m = client.wasmModule;
      if (source === "gamepad") {
        client._gamepadInputMask = clean;
        if (!m) return false;
        if (typeof m._wasm_set_gamepad_mask === "function") {
          m._wasm_set_gamepad_mask(clean);
          return true;
        }
        if (typeof m._wasm_set_input_mask === "function") {
          const kb = client.isSignedIn() ? client._keyboardInputMask || 0 : 0;
          m._wasm_set_input_mask((clean | kb) & 0x03ff);
          return true;
        }
        return false;
      }
      client._keyboardInputMask = clean;
      if (m && typeof m._wasm_set_input_mask === "function") {
        const gp = client.isSignedIn() ? client._gamepadInputMask || 0 : 0;
        m._wasm_set_input_mask((clean | gp) & 0x03ff);
        return true;
      }
      return false;
    },

    // Called from C EM_JS: sends packet bytes to peer (0=broadcast on host, 1=host from client)
    sendPacket(channel, targetPeerId, byteArrayOrPtr, optionalLen) {
      let bytes;
      if (typeof byteArrayOrPtr === "number" && client.wasmModule && client.wasmModule.HEAPU8) {
        bytes = client.wasmModule.HEAPU8.slice(byteArrayOrPtr, byteArrayOrPtr + Number(optionalLen || 0));
      } else {
        bytes = byteArrayOrPtr instanceof Uint8Array ? byteArrayOrPtr : new Uint8Array(byteArrayOrPtr || []);
      }
      const ch = Number(channel) || 0;
      const target = Number(targetPeerId) || 0;

      // server.js only routes peerId >= 2; Host C packets sent to MP_PEER_HOST (1) loop back locally.
      if (client.isHost() && target === 1) {
        const msg = tryDecodeJsonPayload(bytes);
        if (msg && typeof msg.t === "string") {
          client._handleHostMessage(1, ch, msg, { fromLocalWasm: true, rawBytes: bytes });
          return true;
        }
        return false;
      }

      const sent = client.sendBinary(ch, target, bytes);
      if (client.isHost() && target === 0) {
        const msg = tryDecodeJsonPayload(bytes);
        if (msg && typeof msg.t === "string") {
          client._handleLocalBroadcastJsonNotification(ch, msg);
        } else if (ch === CH_SOUL_LINK) {
          client._handleBinarySoulLinkPacket(1, bytes);
        }
      }
      return sent;
    },

    // Called from C EM_JS when local SaveBlock / Party / Map updates
    reportLocalState(jsonOrObj) {
      if (client._suppressLocalFlashSync) return;
      const data = typeof jsonOrObj === "string" ? safeParseJson(jsonOrObj) : jsonOrObj;
      if (!data) return;
      const mySlot = client.state.players[client.state.connection.mySlotIndex];
      if (!mySlot) return;
      const prevTutorial = Boolean(mySlot.tutorialComplete);
      const sanitized = { ...data };
      if (Array.isArray(sanitized.party)) {
        sanitized.party = sanitized.party.slice(0, 6).map(sanitizeMon).filter(Boolean);
      }
      if (Array.isArray(sanitized.bagItems)) {
        sanitized.bagItems = sanitizeBagItems(sanitized.bagItems);
      }
      if (typeof sanitized.mapName === "string") {
        sanitized.mapName = sanitized.mapName.slice(0, 48);
      }
      if (typeof sanitized.money === "number" && Number.isFinite(sanitized.money)) {
        sanitized.money = Math.max(0, Math.floor(sanitized.money));
      }
      if ("tutorialComplete" in sanitized) {
        sanitized.tutorialComplete = Boolean(sanitized.tutorialComplete);
      }
      const hasProgressNow = Boolean(
        (Array.isArray(sanitized.party) && sanitized.party.length > 0) || sanitized.tutorialComplete
      );
      if (client._resumeFallbackOccurred && !hasProgressNow) {
        return;
      }
      if (hasProgressNow) {
        client._resumeFallbackOccurred = false;
      }
      Object.assign(mySlot, sanitized);
      if (typeof sanitized.mapGroup === "number" && typeof sanitized.mapNum === "number") {
        client._unlockMapKey(sanitized.mapGroup, sanitized.mapNum);
      }
      if (!prevTutorial && mySlot.tutorialComplete) {
        this._syncPeersToWasm(client.state.players);
        this._syncSoulLinkToWasm(client.state);
      }
      if (client.isClient()) {
        client.sendJsonToHost(CH_CONTROL, { t: "state_sync", ...sanitized });
      } else if (client.isNetworkHost()) {
        client.broadcastControl({ t: "players_sync", players: client.state.players });
      }
      client.emit("players");
    },

    getRunConfigJson() {
      return JSON.stringify(client.state.runConfig);
    },

    getSpeedMultiplier() {
      return client.state.runConfig.speedMultiplier || 1;
    },

    getLocalSlotIndex() {
      return client.state.connection.mySlotIndex || 0;
    },

    getLastTradeCommitJson() {
      return client._lastTradeCommit ? JSON.stringify(client._lastTradeCommit) : "";
    },

    _dispatchBinaryToWasm(channel, fromPeerId, payloadBytes) {
      const m = client.wasmModule;
      if (!m || typeof m._mp_ingest_packet !== "function" || typeof m._malloc !== "function") return false;
      const ptr = m._malloc(payloadBytes.byteLength);
      if (!ptr) return false;
      try {
        m.HEAPU8.set(payloadBytes, ptr);
        m._mp_ingest_packet(channel, fromPeerId, ptr, payloadBytes.byteLength);
        return true;
      } finally {
        if (typeof m._free === "function") m._free(ptr);
      }
    },

    _dispatchWasmJson(channel, fromPeerId, msg, optionalRawBytes) {
      const m = client.wasmModule;
      if (!m || typeof m._mp_ingest_packet !== "function" || typeof m._malloc !== "function") return false;
      const bytes = buildWasmJsonBytes(msg, optionalRawBytes);
      if (!bytes || bytes.byteLength === 0) return false;
      return this._dispatchBinaryToWasm(channel, fromPeerId, bytes);
    },

    _applySpeedToWasm(speed) {
      const m = client.wasmModule;
      if (m && typeof m._mp_set_speed_multiplier === "function") {
        m._mp_set_speed_multiplier(speed);
      }
    },

    _triggerWarpInWasm(mapGroup, mapNum, x, y) {
      const m = client.wasmModule;
      if (m && typeof m._mp_trigger_warp === "function") {
        m._mp_trigger_warp(mapGroup, mapNum, x, y);
      }
    },

    _startSyncedBattleInWasm(battleMeta) {
      const m = client.wasmModule;
      if (m && typeof m._mp_start_synced_battle === "function") {
        let modeCode = 1; // MP_BATTLE_COOP_2V2
        if (battleMeta && battleMeta.mode === "pvp") {
          modeCode = 2; // MP_BATTLE_PVP
        } else if (battleMeta && battleMeta.mode === "solo_2v2") {
          modeCode = 0; // MP_BATTLE_SOLO_2V2
        }
        const rawSlot = battleMeta ? (battleMeta.partnerSlotIndex ?? battleMeta.fromSlotIndex) : undefined;
        const partnerSlot = Number.isInteger(Number(rawSlot)) && Number(rawSlot) >= 0 && Number(rawSlot) < 4
          ? Number(rawSlot)
          : 0xff;
        const partnerPeerId = Number(battleMeta && (battleMeta.partnerPeerId ?? battleMeta.fromPeerId)) || 0;
        m._mp_start_synced_battle((battleMeta && battleMeta.battleSeed) || 0, modeCode, partnerSlot, partnerPeerId);
      }
    },

    _withUtf8String(m, str, fn) {
      const text = typeof str === "string" ? str : "";
      if (!m || typeof fn !== "function") return false;
      if (!text) return fn(0);
      if (typeof m._malloc !== "function" || typeof m.stringToUTF8 !== "function") return false;
      const lenBytes =
        typeof m.lengthBytesUTF8 === "function" ? m.lengthBytesUTF8(text) + 1 : text.length * 4 + 1;
      const ptr = m._malloc(lenBytes);
      if (!ptr) return false;
      try {
        m.stringToUTF8(text, ptr, lenBytes);
        return fn(ptr);
      } finally {
        if (typeof m._free === "function") m._free(ptr);
      }
    },

    _updatePeerMetaInWasm(slotIndex, peerId, connected, pairTag, name) {
      const m = client.wasmModule;
      if (!m || typeof m._mp_update_peer_meta !== "function") return;
      const cleanName = typeof name === "string" ? name.slice(0, 15) : "";
      this._withUtf8String(m, cleanName, (namePtr) => {
        m._mp_update_peer_meta(
          Number(slotIndex) || 0,
          Number(peerId) || 0,
          connected ? 1 : 0,
          Number(pairTag) || 0,
          namePtr
        );
      });
    },

    _syncPeersToWasm(players) {
      const m = client.wasmModule;
      if (!m || typeof m._mp_update_peer_meta !== "function") return;
      const list = Array.isArray(players) ? players : client.state.players;
      for (let idx = 0; idx < 4; idx++) {
        const p = list[idx];
        if (!p) continue;
        const pairTag = p.connected ? computePartyPairTag(p.partyId) : 0;
        const peerId = p.connected ? (Number(p.peerId) || (idx + 1)) : 0;
        this._updatePeerMetaInWasm(idx, peerId, Boolean(p.connected), pairTag, p.name || "");
      }
    },

    _notifyWasmLinkDeath(linkId) {
      const m = client.wasmModule;
      if (m && typeof m._mp_kill_linked_pokemon === "function") {
        const num = parseInt(String(linkId || "").replace(/\D/g, ""), 10) || 0;
        m._mp_kill_linked_pokemon(num);
      }
    },

    _killMonByUidInWasm(otId, personality) {
      const m = client.wasmModule;
      if (!m || typeof m._mp_kill_mon_by_uid !== "function") return false;
      m._mp_kill_mon_by_uid(Number(otId) >>> 0, Number(personality) >>> 0);
      return true;
    },

    _resetNewGameInWasm(mode) {
      const m = client.wasmModule;
      if (m && typeof m._mp_reset_new_game === "function") {
        m._mp_reset_new_game(mode === "littleroot" ? 1 : 0);
        return true;
      }
      client._pendingWasmResetMode = mode === "littleroot" ? "littleroot" : "title";
      return false;
    },

    _syncSoulLinkToWasm(stateArg) {
      const st = stateArg || client.state;
      const m = client.wasmModule;
      if (!m || typeof m._mp_import_soullink_json !== "function") return false;
      const sl = (st && st.soulLink) || {};
      const myIdx = st && st.connection ? Number(st.connection.mySlotIndex) || 0 : 0;
      const groupKey = client._getLinkGroupKey(myIdx);
      const prefix = `${groupKey}:`;
      const locks = sl.routeLocks || {};
      const routes = [];
      const kills = [];
      const seenKillKeys = new Set();

      for (const [lockKey, lock] of Object.entries(locks)) {
        if (!lock || !lockKey.startsWith(prefix)) continue;
        const mapsec = parseRouteMapsec(lock.routeId);
        if (mapsec <= 0) continue;
        const linkIdNum = parseNumericLinkId(lock.linkId);
        const localUid = lock.members && lock.members[myIdx] ? lock.members[myIdx] : "";
        const isDead = Boolean(lock.dead || lock.failed);
        routes.push({
          mapsec,
          linkId: linkIdNum,
          dead: isDead,
          localCaught: Boolean(localUid),
        });
        if (isDead && localUid) {
          const uidParts = parseMonUidKeys(localUid);
          if (uidParts) {
            const k = `${uidParts.otId}:${uidParts.personality}`;
            if (!seenKillKeys.has(k)) {
              seenKillKeys.add(k);
              kills.push({
                otId: uidParts.otId,
                personality: uidParts.personality,
                linkId: linkIdNum,
                dead: true,
                mapsec,
              });
            }
          }
        }
      }

      const mySlot = st && Array.isArray(st.players) ? st.players[myIdx] : null;
      if (mySlot && Array.isArray(mySlot.party)) {
        for (const mon of mySlot.party) {
          if (!mon || !mon.isDead) continue;
          const uidParts = parseMonUidKeys(mon.uid);
          if (!uidParts) continue;
          const k = `${uidParts.otId}:${uidParts.personality}`;
          if (!seenKillKeys.has(k)) {
            seenKillKeys.add(k);
            kills.push({
              otId: uidParts.otId,
              personality: uidParts.personality,
              linkId: parseNumericLinkId(mon.linkId),
              dead: true,
              mapsec: 0,
            });
          }
        }
      }

      const payload = JSON.stringify({
        nextLinkNum: Math.max(1, Number(sl.nextLinkNum) || 1),
        routes,
        kills,
      });
      return Boolean(this._withUtf8String(m, payload, (ptr) => m._mp_import_soullink_json(ptr)));
    },

    _applyTradeToWasm(tradeCommit) {
      if (tradeCommit && typeof tradeCommit === "object") {
        client._lastTradeCommit = {
          sessionId: tradeCommit.sessionId,
          sessionIdNum: tradeCommit.sessionIdNum || toNumericTradeSessionId(tradeCommit.sessionId),
          slotA: Number(tradeCommit.slotA),
          slotB: Number(tradeCommit.slotB),
          offerA: tradeCommit.offerA || null,
          offerB: tradeCommit.offerB || null,
        };
      }
      const m = client.wasmModule;
      if (!m) return;

      if (
        tradeCommit &&
        Number.isInteger(tradeCommit.slotA) &&
        Number.isInteger(tradeCommit.slotB) &&
        tradeCommit.offerA &&
        tradeCommit.offerB
      ) {
        const baseSession =
          (Number(tradeCommit.sessionIdNum) >>> 0) || toNumericTradeSessionId(tradeCommit.sessionId);
        const itemsA = Array.isArray(tradeCommit.offerA.items) ? tradeCommit.offerA.items : [];
        const itemsB = Array.isArray(tradeCommit.offerB.items) ? tradeCommit.offerB.items : [];
        const passes = Math.max(1, itemsA.length, itemsB.length);
        for (let idx = 0; idx < passes; idx++) {
          const subSid = idx === 0 ? baseSession : (((baseSession + idx) >>> 0) || idx);
          const frame = encodeBinaryTradeCommitFrame(
            subSid,
            tradeCommit.slotA,
            tradeCommit.slotB,
            tradeCommit.offerA,
            tradeCommit.offerB,
            idx
          );
          if (frame) {
            this._dispatchBinaryToWasm(CH_TRADE, 1, frame);
          }
        }
      }

      if (typeof m._mp_on_trade_commit === "function") {
        m._mp_on_trade_commit();
      }
    },

    _loadSnapshotIntoWasm(snapshot) {
      if (client._wasmResumeTimer) {
        clearTimeout(client._wasmResumeTimer);
        client._wasmResumeTimer = null;
      }
      client._pendingWasmResetMode = "";
      const m = client.wasmModule;
      if (m && typeof m._mp_on_snapshot_loaded === "function") {
        client._pendingWasmResume = false;
        m._mp_on_snapshot_loaded();
        this._syncSoulLinkToWasm(client.state);
      } else {
        client._pendingWasmResume = true;
      }
    },

    onTeammateBusyPrompt(partnerSlot, reasonStr) {
      client.onTeammateBusyPrompt(partnerSlot, reasonStr);
    },

    onTeammateBusyResolved() {
      client.onTeammateBusyResolved();
    },

    _resolveBusyTrainerPrompt(choice) {
      const cleanChoice = Number(choice) === 1 ? 1 : 0;
      const m = client.wasmModule;
      if (m && typeof m._mp_resolve_busy_trainer_prompt === "function") {
        m._mp_resolve_busy_trainer_prompt(cleanChoice);
        return true;
      }
      return false;
    },
  };
})(typeof window !== "undefined" ? window : globalThis);
