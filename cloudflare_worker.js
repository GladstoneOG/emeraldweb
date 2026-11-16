// ============================================================================
// Pokemon Web Multiplayer Relay — Cloudflare Worker + Durable Object (SQLite)
// Deploy with: npx wrangler deploy
// ============================================================================

const FRAME_DATA = 1;
const FRAME_PEER_CONNECTED = 2;
const FRAME_PEER_DISCONNECTED = 3;
const FRAME_KICK_PEER = 4;
const FRAME_ROOM_INFO = 5;
const FRAME_HOST_ASSIGNED = 6;
const FRAME_REJECTED = 7;

const CH_CONTROL = 0;

const MAX_CLIENTS_PER_ROOM = 3; // 1 Host (peerId=1) + up to 3 Clients (peerId=2..4)
const DEFAULT_ROOM_PORT = 27462;
const HOST_GRACE_MS = 6000;
const SESSION_TTL_MS = 30 * 24 * 60 * 60 * 1000;
const PBKDF2_ITERATIONS = 100000;
const MAX_JSON_BODY_BYTES = 1024 * 1024;
const MAX_FLASH_RAW_BYTES = 512 * 1024;

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, PUT, DELETE, HEAD, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization, X-Session-Token",
  "Cache-Control": "no-store",
};

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

function makeFrame(type, peerId, channel, payloadBytes) {
  const payload = payloadBytes ? new Uint8Array(payloadBytes) : new Uint8Array(0);
  const out = new Uint8Array(4 + payload.byteLength);
  out[0] = type & 0xff;
  out[1] = (peerId >> 8) & 0xff;
  out[2] = peerId & 0xff;
  out[3] = channel & 0xff;
  if (payload.byteLength > 0) {
    out.set(payload, 4);
  }
  return out;
}

function toUint8Array(data) {
  if (data instanceof Uint8Array) return data;
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  return new Uint8Array(data);
}

function parseRoomPort(raw) {
  const n = parseInt(raw || String(DEFAULT_ROOM_PORT), 10);
  return Number.isFinite(n) && n >= 1024 && n <= 65535 ? n : DEFAULT_ROOM_PORT;
}

function bytesToHex(bytes) {
  let out = "";
  for (let i = 0; i < bytes.length; i++) {
    out += bytes[i].toString(16).padStart(2, "0");
  }
  return out;
}

function hexToBytes(hex) {
  const clean = String(hex || "");
  if (clean.length % 2 !== 0) return new Uint8Array(0);
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

function randomHex(byteLen) {
  const buf = new Uint8Array(byteLen);
  crypto.getRandomValues(buf);
  return bytesToHex(buf);
}

function constantTimeEqualHex(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length || a.length === 0) {
    return false;
  }
  let diff = 0;
  for (let i = 0; i < a.length; i++) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

async function hashPassword(password, saltHex) {
  const keyMaterial = await crypto.subtle.importKey(
    "raw",
    textEncoder.encode(password),
    "PBKDF2",
    false,
    ["deriveBits"]
  );
  const derived = await crypto.subtle.deriveBits(
    {
      name: "PBKDF2",
      hash: "SHA-256",
      salt: hexToBytes(saltHex),
      iterations: PBKDF2_ITERATIONS,
    },
    keyMaterial,
    256
  );
  return bytesToHex(new Uint8Array(derived));
}

function uint8ToBase64(bytes) {
  let binary = "";
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

function base64ToUint8(b64) {
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

async function readStreamBytes(stream) {
  const reader = stream.getReader();
  const chunks = [];
  let total = 0;
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    total += value.byteLength;
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const c of chunks) {
    out.set(c, offset);
    offset += c.byteLength;
  }
  return out;
}

async function transformStreamBytes(bytes, stream) {
  const readPromise = readStreamBytes(stream.readable);
  const writer = stream.writable.getWriter();
  const writePromise = (async () => {
    await writer.write(bytes);
    await writer.close();
  })();
  const [, result] = await Promise.all([writePromise, readPromise]);
  return result;
}

async function gzipBytes(bytes) {
  return transformStreamBytes(bytes, new CompressionStream("gzip"));
}

async function gunzipBytes(bytes) {
  return transformStreamBytes(bytes, new DecompressionStream("gzip"));
}

async function normalizeIncomingFlashForStorage(flashSaveB64, flashEncoding, compressedFlag) {
  const rawStr = typeof flashSaveB64 === "string" ? flashSaveB64.trim() : "";
  if (!rawStr) return { error: "flashSaveB64 is required" };

  let isGzip =
    flashEncoding === "gzip" ||
    compressedFlag === true ||
    rawStr.startsWith("gz:");
  const cleanB64 = rawStr.startsWith("gz:") ? rawStr.slice(3).trim() : rawStr;
  if (!cleanB64) return { error: "flashSaveB64 is empty" };

  let decoded;
  try {
    decoded = base64ToUint8(cleanB64);
  } catch (_) {
    return { error: "Invalid base64 in flashSaveB64" };
  }

  if (decoded.byteLength > MAX_FLASH_RAW_BYTES) {
    return { error: "Flash save exceeds maximum allowed size" };
  }

  if (isGzip) {
    if (decoded.byteLength < 2 || decoded[0] !== 0x1f || decoded[1] !== 0x8b) {
      return { error: "Invalid gzip header in flashSaveB64" };
    }
    return { stored: `gz:${cleanB64}` };
  }

  // Compress uncompressed Flash payload so SQLite rows stay compact (~15-30KB vs ~175KB).
  try {
    const gz = await gzipBytes(decoded);
    return { stored: `gz:${uint8ToBase64(gz)}` };
  } catch (_) {
    return { stored: cleanB64 };
  }
}

async function formatStoredFlashForResponse(storedValue, requestedEncoding) {
  const raw = typeof storedValue === "string" ? storedValue : "";
  if (!raw) {
    return { flashSaveB64: "", flashEncoding: requestedEncoding === "gzip" ? "gzip" : "raw" };
  }
  const isStoredGz = raw.startsWith("gz:");
  const payloadB64 = isStoredGz ? raw.slice(3) : raw;

  if (requestedEncoding === "gzip") {
    if (isStoredGz) {
      return { flashSaveB64: payloadB64, flashEncoding: "gzip" };
    }
    const gz = await gzipBytes(base64ToUint8(payloadB64));
    return { flashSaveB64: uint8ToBase64(gz), flashEncoding: "gzip" };
  }

  if (isStoredGz) {
    const decompressed = await gunzipBytes(base64ToUint8(payloadB64));
    return { flashSaveB64: uint8ToBase64(decompressed), flashEncoding: "raw" };
  }
  return { flashSaveB64: payloadB64, flashEncoding: "raw" };
}

function safeJsonParse(raw, fallback) {
  if (typeof raw !== "string" || !raw) return fallback;
  try {
    return JSON.parse(raw);
  } catch (_) {
    return fallback;
  }
}

function normalizeUsername(raw) {
  const trimmed = String(raw || "").trim().replace(/\s+/g, " ");
  if (trimmed.length < 2 || trimmed.length > 24) {
    return { error: "Username must be between 2 and 24 characters." };
  }
  if (!/^[A-Za-z0-9_.\- ]+$/.test(trimmed)) {
    return { error: "Username may only contain letters, numbers, spaces, _, -, and ." };
  }
  return { username: trimmed, usernameLower: trimmed.toLowerCase() };
}

function validatePassword(raw) {
  if (typeof raw !== "string" || raw.length < 6 || raw.length > 128) {
    return { error: "Password must be between 6 and 128 characters." };
  }
  return { password: raw };
}

function normalizeRunId(raw) {
  const clean = String(raw || "").trim();
  if (!clean || clean.length > 80 || !/^[A-Za-z0-9._:-]+$/.test(clean)) {
    return null;
  }
  return clean;
}

function parseSlotIdx(raw) {
  const n = Number(raw);
  if (!Number.isInteger(n) || n < 0 || n > 3) return null;
  return n;
}

function extractBearerToken(request) {
  const auth = request.headers.get("Authorization") || "";
  if (/^Bearer\s+/i.test(auth)) {
    return auth.replace(/^Bearer\s+/i, "").trim();
  }
  const alt = request.headers.get("X-Session-Token") || "";
  return alt.trim();
}

async function readJsonBody(request) {
  const contentLen = Number(request.headers.get("Content-Length") || "0");
  if (Number.isFinite(contentLen) && contentLen > MAX_JSON_BODY_BYTES) {
    return { errorStatus: 413, errorMessage: "Request payload too large" };
  }
  let text;
  try {
    text = await request.text();
  } catch (_) {
    return { errorStatus: 400, errorMessage: "Failed to read request body" };
  }
  if (textEncoder.encode(text).byteLength > MAX_JSON_BODY_BYTES) {
    return { errorStatus: 413, errorMessage: "Request payload too large" };
  }
  if (!text.trim()) return { data: {} };
  try {
    const parsed = JSON.parse(text);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
      return { errorStatus: 400, errorMessage: "JSON body must be an object" };
    }
    return { data: parsed };
  } catch (_) {
    return { errorStatus: 400, errorMessage: "Invalid JSON body" };
  }
}

function jsonResponse(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      ...CORS_HEADERS,
      "Content-Type": "application/json; charset=utf-8",
    },
  });
}

function createDefaultRoomInfo(port) {
  return {
    protocol: "pokemon_emerald_web",
    protocol_version: 1,
    game_version: "pokeemerald-expansion",
    host_name: "Trainer",
    host_user_id: "",
    room_name: `Hoenn Room (#${port})`,
    game_port: port,
    address: "relay",
    member_count: 1,
    member_limit: 4,
    state: "lobby",
    seed: "",
    nuzlocke_preset: "standard_soullink",
    randomizer_preset: "balanced",
    soul_link_mode: "pairs",
    speed_multiplier: 1,
    run_id: "",
    badge_count: 0,
    death_count: 0,
    caught_routes_count: 0,
    game_over: false,
    joinable: true,
  };
}

function formatRoomSnapshot(port, room) {
  const baseInfo = room.info || {};
  const hostName = String(baseInfo.host_name || "Trainer").slice(0, 32);
  const roomName = String(baseInfo.room_name || `${hostName}'s Room (#${port})`).slice(0, 64);
  const memberCount = 1 + room.clients.size;
  const memberLimit = Math.min(4, Math.max(1, Number(baseInfo.member_limit) || 4));
  const rawJoinable = baseInfo.joinable !== undefined ? Boolean(baseInfo.joinable) : true;
  const speedNum = Number(baseInfo.speed_multiplier);
  const speedMultiplier = Number.isFinite(speedNum) && speedNum > 0 ? speedNum : 1;
  const badgeCount = Math.max(0, Math.min(8, Math.floor(Number(baseInfo.badge_count) || 0)));
  const deathCount = Math.max(0, Math.floor(Number(baseInfo.death_count) || 0));
  const caughtRoutesCount = Math.max(0, Math.floor(Number(baseInfo.caught_routes_count) || 0));
  const gameOver = Boolean(baseInfo.game_over);

  return {
    protocol: "pokemon_emerald_web",
    protocol_version: 1,
    game_version: String(baseInfo.game_version || "pokeemerald-expansion"),
    host_name: hostName,
    host_user_id: String(baseInfo.host_user_id || ""),
    room_name: roomName,
    game_port: port,
    address: "relay",
    member_count: memberCount,
    member_limit: memberLimit,
    state: String(baseInfo.state || "lobby"),
    seed: baseInfo.seed !== undefined && baseInfo.seed !== null ? String(baseInfo.seed) : "",
    nuzlocke_preset: String(baseInfo.nuzlocke_preset || "standard_soullink"),
    randomizer_preset: String(baseInfo.randomizer_preset || "balanced"),
    soul_link_mode: String(baseInfo.soul_link_mode || "pairs"),
    speed_multiplier: speedMultiplier,
    run_id: String(baseInfo.run_id || ""),
    badge_count: badgeCount,
    death_count: deathCount,
    caught_routes_count: caughtRoutesCount,
    game_over: gameOver,
    joinable: !gameOver && rawJoinable && memberCount < memberLimit,
  };
}

/** Validate whether a joining client can enter `room`, returning null if allowed. */
function getClientJoinRejection(room) {
  if (!room || !room.hostWs || !room.hostWs._isOpen) {
    return {
      closeCode: 4004,
      closeReason: "Room not found",
      payload: "Room not found or host offline",
    };
  }
  if (room.info && Boolean(room.info.game_over)) {
    return {
      closeCode: 4005,
      closeReason: "Run Game Over",
      payload: JSON.stringify({
        t: "error",
        code: "RUN_GAME_OVER",
        message: "This Soul-Link Nuzlocke run has ended (Game Over).",
      }),
    };
  }
  if (room.clients.size >= MAX_CLIENTS_PER_ROOM) {
    return {
      closeCode: 4003,
      closeReason: "Room full",
      payload: "Room is full (4/4)",
    };
  }
  return null;
}

function formatSlotMeta(row) {
  if (!row) return null;
  return {
    runId: row.run_id,
    slotIdx: Number(row.slot_idx),
    userId: row.user_id || "",
    username: row.username || "",
    characterName: row.character_name || "",
    tutorialComplete: Boolean(row.tutorial_complete),
    party: safeJsonParse(row.party_json, []),
    money: Number(row.money) || 0,
    bagItems: safeJsonParse(row.bag_json, []),
    flashRev: Number(row.flash_rev) || 0,
    hasFlashSave: Boolean(row.flash_save_b64 && String(row.flash_save_b64).length > 0),
    updatedAt: Number(row.updated_at) || 0,
  };
}

export class PokemonRelayHub {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.rooms = new Map();
    this.sql = state.storage.sql;
    this._schemaReady = false;
    this.ensureSchema();
  }

  ensureSchema() {
    if (this._schemaReady) return;
    this.sql.exec(`
      CREATE TABLE IF NOT EXISTS accounts (
        user_id TEXT PRIMARY KEY,
        username TEXT UNIQUE NOT NULL,
        username_lower TEXT UNIQUE NOT NULL,
        password_hash TEXT NOT NULL,
        password_salt TEXT NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS sessions (
        token TEXT PRIMARY KEY,
        user_id TEXT NOT NULL,
        expires_at INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_sessions_user_id ON sessions(user_id);
      CREATE INDEX IF NOT EXISTS idx_sessions_expires_at ON sessions(expires_at);
      CREATE TABLE IF NOT EXISTS runs (
        run_id TEXT PRIMARY KEY,
        host_user_id TEXT NOT NULL,
        title TEXT NOT NULL,
        seed TEXT NOT NULL,
        randomizer_preset TEXT NOT NULL,
        run_config_json TEXT NOT NULL,
        world_state_json TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_runs_host_updated ON runs(host_user_id, updated_at DESC);
      CREATE TABLE IF NOT EXISTS run_slots (
        run_id TEXT NOT NULL,
        slot_idx INTEGER NOT NULL,
        user_id TEXT,
        character_name TEXT NOT NULL DEFAULT '',
        tutorial_complete INTEGER NOT NULL DEFAULT 0,
        party_json TEXT NOT NULL DEFAULT '[]',
        money INTEGER NOT NULL DEFAULT 0,
        bag_json TEXT NOT NULL DEFAULT '[]',
        flash_save_b64 TEXT NOT NULL DEFAULT '',
        flash_rev INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (run_id, slot_idx)
      );
      CREATE INDEX IF NOT EXISTS idx_run_slots_user_id ON run_slots(user_id);
    `);
    this._schemaReady = true;
  }

  sqlOne(query, ...params) {
    const rows = this.sql.exec(query, ...params).toArray();
    return rows.length > 0 ? rows[0] : null;
  }

  sqlAll(query, ...params) {
    return this.sql.exec(query, ...params).toArray();
  }

  runInTransaction(fn) {
    if (typeof this.state.storage.transactionSync === "function") {
      return this.state.storage.transactionSync(fn);
    }
    return fn();
  }

  verifySessionToken(token) {
    if (typeof token !== "string") return null;
    const clean = token.trim();
    if (!clean) return null;
    const now = Date.now();
    const row = this.sqlOne(
      `SELECT s.token, s.expires_at, a.user_id, a.username
       FROM sessions s
       JOIN accounts a ON a.user_id = s.user_id
       WHERE s.token = ?`,
      clean
    );
    if (!row) return null;
    if (Number(row.expires_at) <= now) {
      this.sql.exec(`DELETE FROM sessions WHERE token = ?`, clean);
      return null;
    }
    return {
      userId: row.user_id,
      username: row.username,
      expiresAt: Number(row.expires_at),
    };
  }

  createSessionForUser(userId) {
    const now = Date.now();
    const token = randomHex(32);
    const expiresAt = now + SESSION_TTL_MS;
    this.sql.exec(`DELETE FROM sessions WHERE expires_at <= ?`, now);
    this.sql.exec(
      `INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?)`,
      token,
      userId,
      expiresAt
    );
    return { token, expiresAt };
  }

  findActiveRoomPortForRun(runId) {
    if (!runId) return null;
    for (const [port, room] of this.rooms.entries()) {
      if (!room.hostWs || !room.hostWs._isOpen) continue;
      if (room.info && room.info.run_id === runId) {
        return port;
      }
    }
    return null;
  }

  isUserOnlineInRunRoom(runId, userId, slotIdx) {
    if (!runId || !userId) return false;
    for (const room of this.rooms.values()) {
      if (!room.hostWs || !room.hostWs._isOpen) continue;
      if (!room.info || room.info.run_id !== runId) continue;
      if (slotIdx === 0 && room.hostAuth && room.hostAuth.userId === userId) {
        return true;
      }
      if (room.clientAuth) {
        for (const [peerId, auth] of room.clientAuth.entries()) {
          const clientWs = room.clients.get(peerId);
          if (clientWs && clientWs._isOpen && auth && auth.userId === userId) {
            return true;
          }
        }
      }
    }
    return false;
  }

  getRunSlotsSummary(runId) {
    const rows = this.sqlAll(
      `SELECT rs.run_id, rs.slot_idx, rs.user_id, rs.character_name, rs.tutorial_complete,
              rs.party_json, rs.money, rs.bag_json, rs.flash_rev, rs.updated_at,
              CASE WHEN length(rs.flash_save_b64) > 0 THEN 1 ELSE 0 END AS has_flash,
              a.username
       FROM run_slots rs
       LEFT JOIN accounts a ON a.user_id = rs.user_id
       WHERE rs.run_id = ?
       ORDER BY rs.slot_idx ASC`,
      runId
    );
    return rows.map((row) => ({
      runId: row.run_id,
      slotIdx: Number(row.slot_idx),
      userId: row.user_id || "",
      username: row.username || "",
      characterName: row.character_name || "",
      tutorialComplete: Boolean(row.tutorial_complete),
      party: safeJsonParse(row.party_json, []),
      money: Number(row.money) || 0,
      bagItems: safeJsonParse(row.bag_json, []),
      flashRev: Number(row.flash_rev) || 0,
      hasFlashSave: Boolean(row.has_flash),
      updatedAt: Number(row.updated_at) || 0,
    }));
  }

  formatRunSummary(runRow, viewerUserId) {
    const slots = this.getRunSlotsSummary(runRow.run_id);
    const mySlot = slots.find((s) => s.userId && s.userId === viewerUserId);
    const mySlotIdx = mySlot
      ? mySlot.slotIdx
      : runRow.host_user_id === viewerUserId
        ? 0
        : null;
    return {
      runId: runRow.run_id,
      hostUserId: runRow.host_user_id,
      hostUsername: runRow.host_username || "",
      title: runRow.title,
      seed: runRow.seed,
      randomizerPreset: runRow.randomizer_preset,
      runConfig: safeJsonParse(runRow.run_config_json, {}),
      worldState: safeJsonParse(runRow.world_state_json, {}),
      updatedAt: Number(runRow.updated_at),
      createdAt: Number(runRow.created_at),
      activeRoomPort: this.findActiveRoomPortForRun(runRow.run_id),
      mySlotIdx,
      slots,
    };
  }

  requireAuth(request) {
    const token = extractBearerToken(request);
    if (!token) return null;
    return this.verifySessionToken(token);
  }

  allocateRoomPort(requestedPort) {
    let p = parseRoomPort(requestedPort);
    for (let i = 0; i < 65536; i++) {
      if (!this.rooms.has(p)) return p;
      p++;
      if (p > 65535) p = 27000;
    }
    return p;
  }

  allocateClientPeerId(room) {
    for (let id = 2; id <= MAX_CLIENTS_PER_ROOM + 1; id++) {
      if (!room.clients.has(id)) return id;
    }
    return null;
  }

  getRoomListSnapshot() {
    const list = [];
    for (const [port, room] of this.rooms.entries()) {
      if (!room.hostWs || !room.hostWs._isOpen) continue;
      list.push(formatRoomSnapshot(port, room));
    }
    return list;
  }

  mergeRoomInfo(room, rawJson) {
    try {
      const parsed = JSON.parse(rawJson);
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        if (typeof parsed.sessionToken === "string" && parsed.sessionToken.trim()) {
          const verified = this.verifySessionToken(parsed.sessionToken);
          if (verified) room.hostAuth = verified;
        }
        delete parsed.sessionToken;
        delete parsed.host_user_id;
        delete parsed.host_username;
        room.info = {
          ...room.info,
          ...parsed,
          game_port: room.port,
          address: "relay",
          host_user_id: room.hostAuth ? room.hostAuth.userId : "",
        };
      }
    } catch (_) {}
  }

  async handleApiRequest(request, url) {
    const pathname = url.pathname;
    const method = request.method.toUpperCase();

    if (pathname === "/api/auth/register" && method === "POST") {
      const parsed = await readJsonBody(request);
      if (parsed.errorStatus) return jsonResponse({ ok: false, error: "BAD_REQUEST", message: parsed.errorMessage }, parsed.errorStatus);

      const uNorm = normalizeUsername(parsed.data.username);
      if (uNorm.error) return jsonResponse({ ok: false, error: "INVALID_USERNAME", message: uNorm.error }, 400);
      const pVal = validatePassword(parsed.data.password);
      if (pVal.error) return jsonResponse({ ok: false, error: "INVALID_PASSWORD", message: pVal.error }, 400);

      const existing = this.sqlOne(
        `SELECT user_id FROM accounts WHERE username_lower = ?`,
        uNorm.usernameLower
      );
      if (existing) {
        return jsonResponse({ ok: false, error: "USERNAME_TAKEN", message: "Username is already taken." }, 409);
      }

      const userId = `usr_${randomHex(12)}`;
      const salt = randomHex(16);
      const passwordHash = await hashPassword(pVal.password, salt);
      const now = Date.now();

      try {
        this.sql.exec(
          `INSERT INTO accounts (user_id, username, username_lower, password_hash, password_salt, created_at)
           VALUES (?, ?, ?, ?, ?, ?)`,
          userId,
          uNorm.username,
          uNorm.usernameLower,
          passwordHash,
          salt,
          now
        );
      } catch (_) {
        return jsonResponse({ ok: false, error: "USERNAME_TAKEN", message: "Username is already taken." }, 409);
      }

      const session = this.createSessionForUser(userId);
      return jsonResponse(
        { ok: true, userId, username: uNorm.username, sessionToken: session.token, expiresAt: session.expiresAt },
        201
      );
    }

    if (pathname === "/api/auth/login" && method === "POST") {
      const parsed = await readJsonBody(request);
      if (parsed.errorStatus) return jsonResponse({ ok: false, error: "BAD_REQUEST", message: parsed.errorMessage }, parsed.errorStatus);

      const rawUser = String(parsed.data.username || "").trim().replace(/\s+/g, " ");
      const rawPass = parsed.data.password;
      if (!rawUser || typeof rawPass !== "string" || !rawPass) {
        return jsonResponse({ ok: false, error: "INVALID_CREDENTIALS", message: "Username and password are required." }, 400);
      }

      const account = this.sqlOne(
        `SELECT user_id, username, password_hash, password_salt FROM accounts WHERE username_lower = ?`,
        rawUser.toLowerCase()
      );
      if (!account) {
        return jsonResponse({ ok: false, error: "INVALID_CREDENTIALS", message: "Invalid username or password." }, 401);
      }

      const derived = await hashPassword(rawPass, account.password_salt);
      if (!constantTimeEqualHex(derived, account.password_hash)) {
        return jsonResponse({ ok: false, error: "INVALID_CREDENTIALS", message: "Invalid username or password." }, 401);
      }

      const session = this.createSessionForUser(account.user_id);
      return jsonResponse({
        ok: true,
        userId: account.user_id,
        username: account.username,
        sessionToken: session.token,
        expiresAt: session.expiresAt,
      });
    }

    if (pathname === "/api/auth/me" && method === "GET") {
      const auth = this.requireAuth(request);
      if (!auth) {
        return jsonResponse({ ok: false, error: "UNAUTHORIZED", message: "Invalid or expired session." }, 401);
      }
      return jsonResponse({ ok: true, userId: auth.userId, username: auth.username, expiresAt: auth.expiresAt });
    }

    if (pathname === "/api/auth/logout" && method === "POST") {
      let token = extractBearerToken(request);
      if (!token) {
        const parsed = await readJsonBody(request);
        if (!parsed.errorStatus && typeof parsed.data.sessionToken === "string") {
          token = parsed.data.sessionToken.trim();
        }
      }
      if (token) {
        this.sql.exec(`DELETE FROM sessions WHERE token = ?`, token);
      }
      return jsonResponse({ ok: true });
    }

    if (pathname === "/api/runs" && method === "GET") {
      const auth = this.requireAuth(request);
      if (!auth) {
        return jsonResponse({ ok: false, error: "UNAUTHORIZED", message: "Sign in required." }, 401);
      }

      const hostedRows = this.sqlAll(
        `SELECT r.*, a.username AS host_username
         FROM runs r
         LEFT JOIN accounts a ON a.user_id = r.host_user_id
         WHERE r.host_user_id = ?
         ORDER BY r.updated_at DESC`,
        auth.userId
      );
      const joinedRows = this.sqlAll(
        `SELECT DISTINCT r.*, a.username AS host_username
         FROM runs r
         JOIN run_slots rs ON rs.run_id = r.run_id
         LEFT JOIN accounts a ON a.user_id = r.host_user_id
         WHERE rs.user_id = ? AND r.host_user_id != ?
         ORDER BY r.updated_at DESC`,
        auth.userId,
        auth.userId
      );

      return jsonResponse({
        ok: true,
        hostedRuns: hostedRows.map((r) => this.formatRunSummary(r, auth.userId)),
        joinedRuns: joinedRows.map((r) => this.formatRunSummary(r, auth.userId)),
      });
    }

    if (pathname === "/api/runs/upsert" && method === "POST") {
      const auth = this.requireAuth(request);
      if (!auth) {
        return jsonResponse({ ok: false, error: "UNAUTHORIZED", message: "Sign in required." }, 401);
      }

      const parsed = await readJsonBody(request);
      if (parsed.errorStatus) return jsonResponse({ ok: false, error: "BAD_REQUEST", message: parsed.errorMessage }, parsed.errorStatus);

      const data = parsed.data;
      const runId = normalizeRunId(data.runId);
      if (!runId) {
        return jsonResponse({ ok: false, error: "INVALID_RUN_ID", message: "Valid runId is required." }, 400);
      }

      const existingRun = this.sqlOne(`SELECT * FROM runs WHERE run_id = ?`, runId);
      if (existingRun && existingRun.host_user_id !== auth.userId) {
        return jsonResponse({ ok: false, error: "FORBIDDEN", message: "Only the run host can update run metadata." }, 403);
      }

      const now = Date.now();
      const existingConfig = safeJsonParse(existingRun?.run_config_json, {});
      const incomingConfig = data.runConfig && typeof data.runConfig === "object" && !Array.isArray(data.runConfig)
        ? data.runConfig
        : existingConfig;
      const existingWorld = safeJsonParse(existingRun?.world_state_json, {});
      let incomingWorld = existingWorld;
      if (data.worldState && typeof data.worldState === "object" && !Array.isArray(data.worldState)) {
        incomingWorld = data.worldState;
      } else if (data.world && typeof data.world === "object" && !Array.isArray(data.world)) {
        incomingWorld = {
          ...data.world,
          ...(data.soulLink && typeof data.soulLink === "object" ? { soulLink: data.soulLink } : {}),
        };
      }

      const title = String(data.title || incomingConfig.roomName || existingRun?.title || "Hoenn Soul-Link").slice(0, 64);
      const seed = String(data.seed ?? incomingConfig.seed ?? existingRun?.seed ?? "").slice(0, 31);
      const randomizerPreset = String(
        data.randomizerPreset ?? incomingConfig.randomizerPreset ?? existingRun?.randomizer_preset ?? "balanced"
      ).slice(0, 32);

      this.runInTransaction(() => {
        if (existingRun) {
          this.sql.exec(
            `UPDATE runs
             SET title = ?, seed = ?, randomizer_preset = ?, run_config_json = ?, world_state_json = ?, updated_at = ?
             WHERE run_id = ?`,
            title,
            seed,
            randomizerPreset,
            JSON.stringify(incomingConfig),
            JSON.stringify(incomingWorld),
            now,
            runId
          );
        } else {
          this.sql.exec(
            `INSERT INTO runs (run_id, host_user_id, title, seed, randomizer_preset, run_config_json, world_state_json, updated_at, created_at)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
            runId,
            auth.userId,
            title,
            seed,
            randomizerPreset,
            JSON.stringify(incomingConfig),
            JSON.stringify(incomingWorld),
            now,
            now
          );
        }

        const slotUpdates = [];
        if (data.hostSlot && typeof data.hostSlot === "object") {
          slotUpdates.push({ slotIdx: 0, ...data.hostSlot, userId: auth.userId });
        }
        if (Array.isArray(data.slots)) {
          for (const s of data.slots) {
            if (!s || typeof s !== "object") continue;
            const sIdx = parseSlotIdx(s.slotIdx ?? s.slotIndex);
            if (sIdx === null) continue;
            if (sIdx === 0 && data.hostSlot) continue;
            slotUpdates.push({ ...s, slotIdx: sIdx });
          }
        }

        for (const su of slotUpdates) {
          const slotIdx = parseSlotIdx(su.slotIdx);
          if (slotIdx === null) continue;
          const existingSlot = this.sqlOne(
            `SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`,
            runId,
            slotIdx
          );
          const existingParty = safeJsonParse(existingSlot?.party_json, []);
          const existingTutorial = Boolean(existingSlot?.tutorial_complete);
          const existingRev = Number(existingSlot?.flash_rev) || 0;
          const hasExistingProgress =
            existingRev > 0 && (existingTutorial || (Array.isArray(existingParty) && existingParty.length > 0));

          const incomingParty = Array.isArray(su.party) ? su.party.slice(0, 6) : existingParty;
          const incomingTutorial =
            typeof su.tutorialComplete === "boolean" ? su.tutorialComplete : existingTutorial;

          // Do not create empty placeholder rows for unoccupied peer slots.
          if (!existingSlot && slotIdx > 0 && !su.userId && incomingParty.length === 0 && !incomingTutorial) {
            continue;
          }

          // Never downgrade an established slot back to blank tutorial via metadata upsert unless forced
          const blockBlankDowngrade =
            !su.force && hasExistingProgress && incomingParty.length === 0 && !incomingTutorial;

          let nextUserId = existingSlot?.user_id || null;
          if (slotIdx === 0) {
            nextUserId = auth.userId;
          } else if (su.userId) {
            const candidateUserId = String(su.userId);
            if (!nextUserId || nextUserId === candidateUserId || su.force || !this.isUserOnlineInRunRoom(runId, nextUserId, slotIdx)) {
              nextUserId = candidateUserId;
            }
          }
          const nextCharName = String(
            su.characterName ?? su.name ?? existingSlot?.character_name ?? (slotIdx === 0 ? auth.username : "")
          ).slice(0, 24);
          const nextTutorial = blockBlankDowngrade ? (existingTutorial ? 1 : 0) : incomingTutorial ? 1 : 0;
          const nextPartyJson = blockBlankDowngrade
            ? existingSlot.party_json
            : JSON.stringify(incomingParty);
          const nextMoney = blockBlankDowngrade
            ? Number(existingSlot?.money) || 0
            : Number.isFinite(Number(su.money))
              ? Math.max(0, Math.floor(Number(su.money)))
              : Number(existingSlot?.money) || 0;
          const nextBagJson = blockBlankDowngrade
            ? existingSlot.bag_json
            : Array.isArray(su.bagItems)
              ? JSON.stringify(su.bagItems.slice(0, 200))
              : existingSlot?.bag_json || "[]";

          if (existingSlot) {
            this.sql.exec(
              `UPDATE run_slots
               SET user_id = ?, character_name = ?, tutorial_complete = ?, party_json = ?, money = ?, bag_json = ?, updated_at = ?
               WHERE run_id = ? AND slot_idx = ?`,
              nextUserId,
              nextCharName,
              nextTutorial,
              nextPartyJson,
              nextMoney,
              nextBagJson,
              now,
              runId,
              slotIdx
            );
          } else {
            this.sql.exec(
              `INSERT INTO run_slots (run_id, slot_idx, user_id, character_name, tutorial_complete, party_json, money, bag_json, flash_save_b64, flash_rev, updated_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, '', 0, ?)`,
              runId,
              slotIdx,
              nextUserId,
              nextCharName,
              nextTutorial,
              nextPartyJson,
              nextMoney,
              nextBagJson,
              now
            );
          }
        }
      });

      const savedRow = this.sqlOne(
        `SELECT r.*, a.username AS host_username
         FROM runs r
         LEFT JOIN accounts a ON a.user_id = r.host_user_id
         WHERE r.run_id = ?`,
        runId
      );
      return jsonResponse({ ok: true, run: this.formatRunSummary(savedRow, auth.userId) });
    }

    const flashMatch = /^\/api\/runs\/([^/]+)\/slot\/([0-3])\/flash$/.exec(pathname);
    if (flashMatch) {
      const auth = this.requireAuth(request);
      if (!auth) {
        return jsonResponse({ ok: false, error: "UNAUTHORIZED", message: "Sign in required." }, 401);
      }
      const runId = normalizeRunId(decodeURIComponent(flashMatch[1]));
      const slotIdx = parseSlotIdx(flashMatch[2]);
      if (!runId || slotIdx === null) {
        return jsonResponse({ ok: false, error: "BAD_REQUEST", message: "Invalid runId or slotIdx." }, 400);
      }

      let runRow = this.sqlOne(`SELECT * FROM runs WHERE run_id = ?`, runId);

      if (method === "GET") {
        if (!runRow) {
          return jsonResponse({ ok: false, error: "RUN_NOT_FOUND", message: "Run not found." }, 404);
        }
        const slotRow = this.sqlOne(
          `SELECT rs.*, a.username
           FROM run_slots rs
           LEFT JOIN accounts a ON a.user_id = rs.user_id
           WHERE rs.run_id = ? AND rs.slot_idx = ?`,
          runId,
          slotIdx
        );
        const isHost = runRow.host_user_id === auth.userId;
        if (!slotRow) {
          const ownsAnySlot = Boolean(
            this.sqlOne(`SELECT 1 AS ok FROM run_slots WHERE run_id = ? AND user_id = ? LIMIT 1`, runId, auth.userId)
          );
          const isOnlinePeer = this.isUserOnlineInRunRoom(runId, auth.userId, slotIdx);
          if (!isHost && !ownsAnySlot && !isOnlinePeer) {
            return jsonResponse({ ok: false, error: "FORBIDDEN", message: "You do not have access to this slot." }, 403);
          }
          const requestedEnc = url.searchParams.get("encoding") === "gzip" ? "gzip" : "raw";
          return jsonResponse({
            ok: true,
            flashSaveB64: "",
            flashEncoding: requestedEnc,
            flashRev: 0,
            slotMeta: null,
          });
        }
        const isOwner = Boolean(slotRow.user_id && slotRow.user_id === auth.userId);
        if (!isHost && !isOwner) {
          const isUnclaimedEmpty =
            !slotRow.user_id &&
            Number(slotRow.flash_rev || 0) === 0 &&
            !slotRow.flash_save_b64 &&
            this.isUserOnlineInRunRoom(runId, auth.userId, slotIdx);
          if (isUnclaimedEmpty) {
            const requestedEnc = url.searchParams.get("encoding") === "gzip" ? "gzip" : "raw";
            return jsonResponse({
              ok: true,
              flashSaveB64: "",
              flashEncoding: requestedEnc,
              flashRev: 0,
              slotMeta: formatSlotMeta(slotRow),
            });
          }
          return jsonResponse({ ok: false, error: "FORBIDDEN", message: "You do not have access to this slot." }, 403);
        }

        const requestedEnc = url.searchParams.get("encoding") === "gzip" ? "gzip" : "raw";
        const formatted = await formatStoredFlashForResponse(slotRow.flash_save_b64, requestedEnc);
        return jsonResponse({
          ok: true,
          flashSaveB64: formatted.flashSaveB64,
          flashEncoding: formatted.flashEncoding,
          flashRev: Number(slotRow.flash_rev) || 0,
          slotMeta: formatSlotMeta(slotRow),
        });
      }

      if (method === "POST") {
        const parsed = await readJsonBody(request);
        if (parsed.errorStatus) return jsonResponse({ ok: false, error: "BAD_REQUEST", message: parsed.errorMessage }, parsed.errorStatus);
        const body = parsed.data;
        const force = Boolean(body.force);

        if (!runRow) {
          if (slotIdx !== 0) {
            return jsonResponse({ ok: false, error: "RUN_NOT_FOUND", message: "Run not found." }, 404);
          }
          // Auto-create run row when host saves slot 0 before debounced /api/runs/upsert completes.
          const nowInit = Date.now();
          const runCfgObj =
            body.runConfig && typeof body.runConfig === "object" && !Array.isArray(body.runConfig)
              ? body.runConfig
              : {};
          const worldObj =
            body.worldState && typeof body.worldState === "object" && !Array.isArray(body.worldState)
              ? body.worldState
              : {};
          const initTitle = String(body.title || runCfgObj.roomName || "Hoenn Soul-Link").slice(0, 64);
          const initSeed = String(body.seed ?? runCfgObj.seed ?? "").slice(0, 31);
          const initPreset = String(body.randomizerPreset ?? runCfgObj.randomizerPreset ?? "balanced").slice(0, 32);
          this.sql.exec(
            `INSERT INTO runs (run_id, host_user_id, title, seed, randomizer_preset, run_config_json, world_state_json, updated_at, created_at)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
            runId,
            auth.userId,
            initTitle,
            initSeed,
            initPreset,
            JSON.stringify(runCfgObj),
            JSON.stringify(worldObj),
            nowInit,
            nowInit
          );
          runRow = this.sqlOne(`SELECT * FROM runs WHERE run_id = ?`, runId);
        }

        const existingSlot = this.sqlOne(
          `SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`,
          runId,
          slotIdx
        );
        const isHost = runRow.host_user_id === auth.userId;
        const isOwner = Boolean(existingSlot && existingSlot.user_id === auth.userId);
        const isOnlineUnclaimed = Boolean(
          (!existingSlot || !existingSlot.user_id) && this.isUserOnlineInRunRoom(runId, auth.userId, slotIdx)
        );
        if (!isHost && !isOwner && !isOnlineUnclaimed) {
          return jsonResponse(
            { ok: false, error: "FORBIDDEN", message: "Claim this slot or host the run before saving Flash data." },
            403
          );
        }

        let storedFlash = "";
        if (typeof body.flashSaveB64 === "string" && body.flashSaveB64.trim() !== "") {
          const normalized = await normalizeIncomingFlashForStorage(
            body.flashSaveB64,
            body.flashEncoding || body.encoding,
            body.compressed
          );
          if (normalized.error) {
            return jsonResponse({ ok: false, error: "INVALID_FLASH_PAYLOAD", message: normalized.error }, 400);
          }
          storedFlash = normalized.stored;
        } else if (!force) {
          return jsonResponse({ ok: false, error: "INVALID_FLASH_PAYLOAD", message: "flashSaveB64 is required." }, 400);
        }

        const txResult = this.runInTransaction(() => {
          const freshSlot = this.sqlOne(
            `SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`,
            runId,
            slotIdx
          );
          const currentRev = freshSlot ? Number(freshSlot.flash_rev) || 0 : 0;
          const expectedRevRaw = body.expectedRev ?? body.baseRev;
          if (!force && expectedRevRaw !== undefined && expectedRevRaw !== null) {
            const expectedRev = Number(expectedRevRaw);
            if (!Number.isFinite(expectedRev) || expectedRev < currentRev) {
              return {
                conflict: true,
                error: "STALE_FLASH_REV",
                message: "Server has a newer Flash save revision.",
                currentRev,
              };
            }
          }
          if (!force && typeof body.flashRev === "number" && body.flashRev > 0 && body.flashRev < currentRev) {
            return {
              conflict: true,
              error: "STALE_FLASH_REV",
              message: "Incoming flashRev is older than server revision.",
              currentRev,
            };
          }

          const existingParty = safeJsonParse(freshSlot?.party_json, []);
          const existingTutorial = Boolean(freshSlot?.tutorial_complete);
          const hasExistingProgress =
            currentRev > 0 && (existingTutorial || (Array.isArray(existingParty) && existingParty.length > 0));

          const incomingParty = Array.isArray(body.party) ? body.party.slice(0, 6) : existingParty;
          const incomingTutorial =
            typeof body.tutorialComplete === "boolean" ? body.tutorialComplete : existingTutorial;

          if (!force && hasExistingProgress && incomingParty.length === 0 && !incomingTutorial) {
            return {
              conflict: true,
              error: "BLANK_OVERWRITE_BLOCKED",
              message: "Refusing to overwrite saved progress with empty tutorial state.",
              currentRev,
            };
          }

          const nextRev = currentRev + 1;
          const now = Date.now();
          let nextUserId = freshSlot?.user_id || null;
          if (!nextUserId) {
            if (typeof body.userId === "string" && body.userId.trim() && isHost) {
              nextUserId = body.userId.trim();
            } else if (!isHost || slotIdx === 0) {
              nextUserId = auth.userId;
            }
          }
          const nextCharName = String(
            body.characterName ?? freshSlot?.character_name ?? (nextUserId === auth.userId ? auth.username : "")
          ).slice(0, 24);
          const nextMoney = Number.isFinite(Number(body.money))
            ? Math.max(0, Math.floor(Number(body.money)))
            : Number(freshSlot?.money) || 0;
          const nextBagJson = Array.isArray(body.bagItems)
            ? JSON.stringify(body.bagItems.slice(0, 200))
            : freshSlot?.bag_json || "[]";

          if (freshSlot) {
            this.sql.exec(
              `UPDATE run_slots
               SET user_id = ?, character_name = ?, tutorial_complete = ?, party_json = ?,
                   money = ?, bag_json = ?, flash_save_b64 = ?, flash_rev = ?, updated_at = ?
               WHERE run_id = ? AND slot_idx = ?`,
              nextUserId,
              nextCharName,
              incomingTutorial ? 1 : 0,
              JSON.stringify(incomingParty),
              nextMoney,
              nextBagJson,
              storedFlash,
              nextRev,
              now,
              runId,
              slotIdx
            );
          } else {
            this.sql.exec(
              `INSERT INTO run_slots (run_id, slot_idx, user_id, character_name, tutorial_complete, party_json, money, bag_json, flash_save_b64, flash_rev, updated_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
              runId,
              slotIdx,
              nextUserId,
              nextCharName,
              incomingTutorial ? 1 : 0,
              JSON.stringify(incomingParty),
              nextMoney,
              nextBagJson,
              storedFlash,
              nextRev,
              now
            );
          }

          this.sql.exec(`UPDATE runs SET updated_at = ? WHERE run_id = ?`, now, runId);

          const updatedSlot = this.sqlOne(
            `SELECT rs.*, a.username
             FROM run_slots rs
             LEFT JOIN accounts a ON a.user_id = rs.user_id
             WHERE rs.run_id = ? AND rs.slot_idx = ?`,
            runId,
            slotIdx
          );
          return { conflict: false, nextRev, updatedAt: now, slotMeta: formatSlotMeta(updatedSlot) };
        });

        if (txResult.conflict) {
          return jsonResponse(
            {
              ok: false,
              stale: true,
              error: txResult.error,
              message: txResult.message,
              currentRev: txResult.currentRev,
              flashRev: txResult.currentRev,
            },
            409
          );
        }

        return jsonResponse({
          ok: true,
          flashRev: txResult.nextRev,
          updatedAt: txResult.updatedAt,
          slotMeta: txResult.slotMeta,
        });
      }

      return jsonResponse({ ok: false, error: "METHOD_NOT_ALLOWED", message: "Method not allowed" }, 405);
    }

    const claimMatch = /^\/api\/runs\/([^/]+)\/slot\/([0-3])\/claim$/.exec(pathname);
    if (claimMatch && method === "POST") {
      const auth = this.requireAuth(request);
      if (!auth) {
        return jsonResponse({ ok: false, error: "UNAUTHORIZED", message: "Sign in required." }, 401);
      }
      const runId = normalizeRunId(decodeURIComponent(claimMatch[1]));
      const slotIdx = parseSlotIdx(claimMatch[2]);
      if (!runId || slotIdx === null) {
        return jsonResponse({ ok: false, error: "BAD_REQUEST", message: "Invalid runId or slotIdx." }, 400);
      }

      const runRow = this.sqlOne(`SELECT * FROM runs WHERE run_id = ?`, runId);
      if (!runRow) {
        return jsonResponse({ ok: false, error: "RUN_NOT_FOUND", message: "Run not found." }, 404);
      }
      if (slotIdx === 0 && runRow.host_user_id !== auth.userId) {
        return jsonResponse({ ok: false, error: "FORBIDDEN", message: "Slot 0 belongs to the run host." }, 403);
      }

      const parsed = await readJsonBody(request);
      if (parsed.errorStatus) return jsonResponse({ ok: false, error: "BAD_REQUEST", message: parsed.errorMessage }, parsed.errorStatus);

      const existingSlot = this.sqlOne(
        `SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`,
        runId,
        slotIdx
      );
      if (existingSlot && existingSlot.user_id && existingSlot.user_id !== auth.userId) {
        if (this.isUserOnlineInRunRoom(runId, existingSlot.user_id, slotIdx)) {
          return jsonResponse(
            { ok: false, error: "SLOT_ONLINE", message: "That trainer slot is currently online." },
            409
          );
        }
      }

      const now = Date.now();
      const charName = String(
        parsed.data.characterName ?? existingSlot?.character_name ?? auth.username
      ).slice(0, 24);

      if (existingSlot) {
        this.sql.exec(
          `UPDATE run_slots SET user_id = ?, character_name = ?, updated_at = ? WHERE run_id = ? AND slot_idx = ?`,
          auth.userId,
          charName,
          now,
          runId,
          slotIdx
        );
      } else {
        this.sql.exec(
          `INSERT INTO run_slots (run_id, slot_idx, user_id, character_name, tutorial_complete, party_json, money, bag_json, flash_save_b64, flash_rev, updated_at)
           VALUES (?, ?, ?, ?, 0, '[]', 0, '[]', '', 0, ?)`,
          runId,
          slotIdx,
          auth.userId,
          charName,
          now
        );
      }
      this.sql.exec(`UPDATE runs SET updated_at = ? WHERE run_id = ?`, now, runId);

      const updated = this.sqlOne(
        `SELECT rs.*, a.username
         FROM run_slots rs
         LEFT JOIN accounts a ON a.user_id = rs.user_id
         WHERE rs.run_id = ? AND rs.slot_idx = ?`,
        runId,
        slotIdx
      );
      return jsonResponse({ ok: true, slot: formatSlotMeta(updated) });
    }

    const runDeleteMatch = /^\/api\/runs\/([^/]+)$/.exec(pathname);
    if (runDeleteMatch && method === "DELETE") {
      const auth = this.requireAuth(request);
      if (!auth) {
        return jsonResponse({ ok: false, error: "UNAUTHORIZED", message: "Sign in required." }, 401);
      }
      const runId = normalizeRunId(decodeURIComponent(runDeleteMatch[1]));
      if (!runId) {
        return jsonResponse({ ok: false, error: "BAD_REQUEST", message: "Invalid runId." }, 400);
      }
      const runRow = this.sqlOne(`SELECT * FROM runs WHERE run_id = ?`, runId);
      if (!runRow) {
        return jsonResponse({ ok: false, error: "RUN_NOT_FOUND", message: "Run not found." }, 404);
      }
      if (runRow.host_user_id !== auth.userId) {
        return jsonResponse({ ok: false, error: "FORBIDDEN", message: "Only the host can delete this run." }, 403);
      }

      this.runInTransaction(() => {
        this.sql.exec(`DELETE FROM run_slots WHERE run_id = ?`, runId);
        this.sql.exec(`DELETE FROM runs WHERE run_id = ?`, runId);
      });
      return jsonResponse({ ok: true, runId });
    }

    return null;
  }

  async fetch(request) {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }
    const url = new URL(request.url);

    if (request.headers.get("Upgrade")?.toLowerCase() === "websocket") {
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      server.accept();
      server._isOpen = true;
      server.addEventListener("close", () => {
        server._isOpen = false;
      });
      server.addEventListener("error", () => {
        server._isOpen = false;
      });
      this.handleSocket(server, url);
      return new Response(null, { status: 101, webSocket: client });
    }

    if (url.pathname === "/api/info") {
      const rooms = this.getRoomListSnapshot();
      return jsonResponse({
        ok: true,
        service: "Pokemon Web Multiplayer Relay (Cloudflare DO)",
        port: 443,
        lanIps: [],
        roomCount: rooms.length,
        rooms,
      });
    }

    if (url.pathname === "/api/rooms") {
      return jsonResponse({ rooms: this.getRoomListSnapshot() });
    }

    const apiResponse = await this.handleApiRequest(request, url);
    if (apiResponse) return apiResponse;

    if (url.pathname === "/" || url.pathname === "/health") {
      if (request.method === "HEAD") {
        return new Response(null, { status: 200, headers: CORS_HEADERS });
      }
      return jsonResponse({
        ok: true,
        service: "Pokemon Web Multiplayer Relay (Cloudflare DO)",
        roomCount: this.rooms.size,
      });
    }

    return new Response(request.method === "HEAD" ? null : "Not found", {
      status: 404,
      headers: CORS_HEADERS,
    });
  }

  handleSocket(ws, url) {
    const defaultRole = url.searchParams.has("room") ? "client" : "discover";
    const role = (url.searchParams.get("role") || defaultRole).toLowerCase();
    const requestedRoom = parseRoomPort(url.searchParams.get("room"));
    const queryToken = url.searchParams.get("sessionToken") || url.searchParams.get("token") || "";
    ws._auth = queryToken ? this.verifySessionToken(queryToken) : null;

    if (role === "discover") {
      const sendRooms = () => {
        if (!ws._isOpen) return;
        try {
          ws.send(JSON.stringify({ rooms: this.getRoomListSnapshot() }));
        } catch (_) {}
      };
      sendRooms();
      ws.addEventListener("message", () => sendRooms());
      return;
    }

    if (role === "host") {
      this.handleHostSocket(ws, requestedRoom);
      return;
    }

    if (role === "client") {
      this.handleClientSocket(ws, requestedRoom);
      return;
    }

    try {
      ws.send(makeFrame(FRAME_REJECTED, 0, 0, textEncoder.encode("Unknown role")));
      ws.close(4002, "Unknown role");
    } catch (_) {}
  }

  handleHostSocket(ws, requestedRoom) {
    let room = this.rooms.get(requestedRoom);
    let roomPort = requestedRoom;
    if (room && room.hostWs && room.hostWs._isOpen) {
      roomPort = this.allocateRoomPort(requestedRoom);
      room = null;
    }

    let resumed = false;
    if (room && room.cleanupTimer) {
      clearTimeout(room.cleanupTimer);
      room.cleanupTimer = null;
      room.hostWs = ws;
      room.hostAuth = ws._auth || room.hostAuth || null;
      if (room.hostAuth) {
        room.info.host_user_id = room.hostAuth.userId;
      }
      resumed = true;
    } else {
      room = {
        port: roomPort,
        hostWs: ws,
        hostAuth: ws._auth || null,
        clients: new Map(),
        clientAuth: new Map(),
        info: createDefaultRoomInfo(roomPort),
        cleanupTimer: null,
      };
      if (room.hostAuth) {
        room.info.host_user_id = room.hostAuth.userId;
      }
      this.rooms.set(roomPort, room);
    }

    try {
      ws.send(makeFrame(FRAME_HOST_ASSIGNED, roomPort, 0));
      if (resumed) {
        for (const peerId of room.clients.keys()) {
          ws.send(makeFrame(FRAME_PEER_CONNECTED, peerId, 0));
        }
      }
    } catch (_) {}

    ws.addEventListener("message", (ev) => {
      if (typeof ev.data === "string") {
        this.mergeRoomInfo(room, ev.data);
        return;
      }
      const buf = toUint8Array(ev.data);
      if (buf.byteLength < 4) return;
      const frameType = buf[0];
      const targetPeer = (buf[1] << 8) | buf[2];
      const channel = buf[3];
      const payload = buf.subarray(4);

      if (frameType === FRAME_ROOM_INFO) {
        this.mergeRoomInfo(room, textDecoder.decode(payload));
        return;
      }

      if (frameType === FRAME_KICK_PEER) {
        const clientWs = room.clients.get(targetPeer);
        if (clientWs) {
          room.clients.delete(targetPeer);
          room.clientAuth.delete(targetPeer);
          try {
            clientWs.send(makeFrame(FRAME_REJECTED, 1, 0, textEncoder.encode("Kicked by host")));
            clientWs.close(4001, "Kicked by host");
            if (room.hostWs && room.hostWs._isOpen) {
              room.hostWs.send(makeFrame(FRAME_PEER_DISCONNECTED, targetPeer, 0));
            }
          } catch (_) {}
        }
        return;
      }

      if (frameType === FRAME_DATA) {
        const outFrame = makeFrame(FRAME_DATA, 1, channel, payload);
        if (targetPeer === 0) {
          for (const clientWs of room.clients.values()) {
            if (clientWs._isOpen) {
              try {
                clientWs.send(outFrame);
              } catch (_) {}
            }
          }
        } else {
          const clientWs = room.clients.get(targetPeer);
          if (clientWs && clientWs._isOpen) {
            try {
              clientWs.send(outFrame);
            } catch (_) {}
          }
        }
      }
    });

    ws.addEventListener("close", () => {
      if (room.hostWs !== ws) return;
      room.cleanupTimer = setTimeout(() => {
        for (const clientWs of room.clients.values()) {
          try {
            if (clientWs._isOpen) {
              clientWs.send(makeFrame(FRAME_PEER_DISCONNECTED, 1, 0));
            }
            clientWs.close(4000, "Host closed room");
          } catch (_) {}
        }
        room.clients.clear();
        room.clientAuth.clear();
        if (this.rooms.get(roomPort) === room) {
          this.rooms.delete(roomPort);
        }
      }, HOST_GRACE_MS);
    });
  }

  rejectClientSocket(ws, rejection) {
    try {
      ws.send(makeFrame(FRAME_REJECTED, 0, 0, textEncoder.encode(rejection.payload)));
      ws.close(rejection.closeCode, rejection.closeReason);
    } catch (_) {}
  }

  rewriteControlIdentityPayload(ws, room, peerId, payload) {
    const parsed = safeJsonParse(textDecoder.decode(payload), null);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
      return payload;
    }
    const t = parsed.t;
    if (t !== "hello" && t !== "claim_slot" && t !== "auth_identify") {
      return payload;
    }

    let verifiedAuth = ws._auth || null;
    let authError = "";
    if (typeof parsed.sessionToken === "string" && parsed.sessionToken.trim()) {
      const checked = this.verifySessionToken(parsed.sessionToken);
      if (checked) {
        verifiedAuth = checked;
        ws._auth = checked;
        room.clientAuth.set(peerId, checked);
      } else {
        verifiedAuth = null;
        authError = "INVALID_SESSION";
      }
    }

    // Strip any client-supplied token or spoofed identity fields before forwarding to Host.
    delete parsed.sessionToken;
    delete parsed.authUserId;
    delete parsed.authUsername;
    delete parsed.authVerified;
    delete parsed.authError;

    parsed.authUserId = verifiedAuth ? verifiedAuth.userId : "";
    parsed.authUsername = verifiedAuth ? verifiedAuth.username : "";
    parsed.authVerified = Boolean(verifiedAuth);
    if (authError) parsed.authError = authError;

    return textEncoder.encode(JSON.stringify(parsed));
  }

  handleClientSocket(ws, requestedRoom) {
    const room = this.rooms.get(requestedRoom);
    const rejection = getClientJoinRejection(room);
    if (rejection) {
      this.rejectClientSocket(ws, rejection);
      return;
    }

    const peerId = this.allocateClientPeerId(room);
    if (peerId === null) {
      this.rejectClientSocket(ws, {
        closeCode: 4003,
        closeReason: "Room full",
        payload: "Room is full (4/4)",
      });
      return;
    }

    room.clients.set(peerId, ws);
    if (ws._auth) {
      room.clientAuth.set(peerId, ws._auth);
    }

    try {
      const handshakePayload = textEncoder.encode(
        JSON.stringify({
          self_peer_id: peerId,
          room_port: room.port,
          auth_user_id: ws._auth ? ws._auth.userId : "",
          auth_username: ws._auth ? ws._auth.username : "",
        })
      );
      ws.send(makeFrame(FRAME_PEER_CONNECTED, 1, 0, handshakePayload));
      room.hostWs.send(makeFrame(FRAME_PEER_CONNECTED, peerId, 0));
    } catch (_) {}

    ws.addEventListener("message", (ev) => {
      if (typeof ev.data === "string") return;
      const buf = toUint8Array(ev.data);
      if (buf.byteLength < 4) return;
      const frameType = buf[0];
      const targetPeer = (buf[1] << 8) | buf[2];
      const channel = buf[3];
      const payload = buf.subarray(4);

      if (frameType === FRAME_DATA) {
        const isJson = payload.byteLength > 0 && payload[0] === 0x7b;
        const finalPayload =
          channel === CH_CONTROL && isJson
            ? this.rewriteControlIdentityPayload(ws, room, peerId, payload)
            : payload;
        const outFrame = makeFrame(FRAME_DATA, peerId, channel, finalPayload);

        if (isJson || targetPeer === 1) {
          if (room.hostWs && room.hostWs._isOpen) {
            try {
              room.hostWs.send(outFrame);
            } catch (_) {}
          }
        } else if (targetPeer === 0) {
          if (room.hostWs && room.hostWs._isOpen) {
            try {
              room.hostWs.send(outFrame);
            } catch (_) {}
          }
          for (const [otherPeerId, otherWs] of room.clients.entries()) {
            if (otherPeerId !== peerId && otherWs && otherWs._isOpen) {
              try {
                otherWs.send(outFrame);
              } catch (_) {}
            }
          }
        } else if (targetPeer >= 2 && targetPeer !== peerId) {
          const targetWs = room.clients.get(targetPeer);
          if (targetWs && targetWs._isOpen) {
            try {
              targetWs.send(outFrame);
            } catch (_) {}
          }
        }
      }
    });

    ws.addEventListener("close", () => {
      if (room.clients.get(peerId) === ws) {
        room.clients.delete(peerId);
        room.clientAuth.delete(peerId);
        if (room.hostWs && room.hostWs._isOpen) {
          try {
            room.hostWs.send(makeFrame(FRAME_PEER_DISCONNECTED, peerId, 0));
          } catch (_) {}
        }
      }
    });
  }
}

export default {
  async fetch(request, env) {
    const id = env.RELAY_HUB.idFromName("global-pokemon-hub");
    const stub = env.RELAY_HUB.get(id);
    return stub.fetch(request);
  },
};
