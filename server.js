const http = require("http");
const fs = require("fs");
const path = require("path");
const os = require("os");
const zlib = require("zlib");
const nodeCrypto = require("crypto");
const { WebSocketServer, WebSocket } = require("ws");

const PORT = parseInt(process.env.PORT || "8080", 10);
const PUBLIC_DIR = path.resolve(__dirname, "public");
const LOCAL_DATA_DIR = path.resolve(process.env.LOCAL_DATA_DIR || path.join(__dirname, ".local_data"));

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
const SESSION_TTL_MS = 30 * 24 * 60 * 60 * 1000;
const PBKDF2_ITERATIONS = 100000;
const MAX_JSON_BODY_BYTES = 1024 * 1024;
const MAX_FLASH_RAW_BYTES = 512 * 1024;

const webCrypto = globalThis.crypto && globalThis.crypto.subtle ? globalThis.crypto : nodeCrypto.webcrypto;
const textEncoder = new TextEncoder();

// Active rooms keyed by integer room port (e.g. 27462)
const rooms = new Map();

function randomHex(byteLen) {
  return nodeCrypto.randomBytes(byteLen).toString("hex");
}

function hexToBytes(hex) {
  return new Uint8Array(Buffer.from(String(hex || ""), "hex"));
}

function constantTimeEqualHex(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length || a.length === 0) {
    return false;
  }
  const bufA = Buffer.from(a, "utf8");
  const bufB = Buffer.from(b, "utf8");
  return bufA.length === bufB.length && nodeCrypto.timingSafeEqual(bufA, bufB);
}

async function hashPassword(password, saltHex) {
  const keyMaterial = await webCrypto.subtle.importKey(
    "raw",
    textEncoder.encode(password),
    "PBKDF2",
    false,
    ["deriveBits"]
  );
  const derived = await webCrypto.subtle.deriveBits(
    {
      name: "PBKDF2",
      hash: "SHA-256",
      salt: hexToBytes(saltHex),
      iterations: PBKDF2_ITERATIONS,
    },
    keyMaterial,
    256
  );
  return Buffer.from(derived).toString("hex");
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

function isStrictBase64(b64) {
  const clean = b64.replace(/\s+/g, "");
  return clean.length > 0 && clean.length % 4 === 0 && /^[A-Za-z0-9+/]+={0,2}$/.test(clean);
}

function normalizeIncomingFlashForStorage(flashSaveB64, flashEncoding, compressedFlag) {
  const rawStr = typeof flashSaveB64 === "string" ? flashSaveB64.trim() : "";
  if (!rawStr) return { error: "flashSaveB64 is required" };

  const isGzip =
    flashEncoding === "gzip" ||
    compressedFlag === true ||
    rawStr.startsWith("gz:");
  const cleanB64 = (rawStr.startsWith("gz:") ? rawStr.slice(3) : rawStr).replace(/\s+/g, "");
  if (!isStrictBase64(cleanB64)) {
    return { error: "Invalid base64 in flashSaveB64" };
  }

  const decoded = Buffer.from(cleanB64, "base64");
  if (decoded.length === 0) return { error: "flashSaveB64 is empty" };
  if (decoded.length > MAX_FLASH_RAW_BYTES) {
    return { error: "Flash save exceeds maximum allowed size" };
  }

  if (isGzip) {
    if (decoded.length < 2 || decoded[0] !== 0x1f || decoded[1] !== 0x8b) {
      return { error: "Invalid gzip header in flashSaveB64" };
    }
    return { stored: `gz:${cleanB64}` };
  }

  try {
    const gz = zlib.gzipSync(decoded);
    return { stored: `gz:${gz.toString("base64")}` };
  } catch (_) {
    return { stored: cleanB64 };
  }
}

function formatStoredFlashForResponse(storedValue, requestedEncoding) {
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
    const gz = zlib.gzipSync(Buffer.from(payloadB64, "base64"));
    return { flashSaveB64: gz.toString("base64"), flashEncoding: "gzip" };
  }

  if (isStoredGz) {
    const decompressed = zlib.gunzipSync(Buffer.from(payloadB64, "base64"));
    return { flashSaveB64: decompressed.toString("base64"), flashEncoding: "raw" };
  }
  return { flashSaveB64: payloadB64, flashEncoding: "raw" };
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

function findActiveRoomPortForRun(runId) {
  if (!runId) return null;
  for (const [port, room] of rooms.entries()) {
    if (!room.hostWs || room.hostWs.readyState !== WebSocket.OPEN) continue;
    if (room.info && room.info.run_id === runId) {
      return port;
    }
  }
  return null;
}

function isUserOnlineInRunRoom(runId, userId, slotIdx) {
  if (!runId || !userId) return false;
  for (const room of rooms.values()) {
    if (!room.hostWs || room.hostWs.readyState !== WebSocket.OPEN) continue;
    if (!room.info || room.info.run_id !== runId) continue;
    if (slotIdx === 0 && room.hostAuth && room.hostAuth.userId === userId) {
      return true;
    }
    if (room.clientAuth) {
      for (const [peerId, auth] of room.clientAuth.entries()) {
        const clientWs = room.clients.get(peerId);
        if (clientWs && clientWs.readyState === WebSocket.OPEN && auth && auth.userId === userId) {
          return true;
        }
      }
    }
  }
  return false;
}

/**
 * Storage adapter backed by Node's built-in `node:sqlite` (with JSON file fallback on older Node).
 */
function createLocalStore(dataDir) {
  fs.mkdirSync(dataDir, { recursive: true });
  let sqliteMod = null;
  try {
    sqliteMod = require("node:sqlite");
  } catch (_) {}

  if (sqliteMod && typeof sqliteMod.DatabaseSync === "function") {
    const dbPath = path.join(dataDir, "emerald_relay.sqlite");
    const db = new sqliteMod.DatabaseSync(dbPath);
    db.exec(`
      PRAGMA journal_mode = WAL;
      PRAGMA busy_timeout = 5000;
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

    const runInTransaction = (fn) => {
      db.exec("BEGIN IMMEDIATE");
      try {
        const res = fn();
        db.exec("COMMIT");
        return res;
      } catch (err) {
        try {
          db.exec("ROLLBACK");
        } catch (_) {}
        throw err;
      }
    };

    const getRunSlotsSummary = (runId) => {
      const rows = db
        .prepare(
          `SELECT rs.run_id, rs.slot_idx, rs.user_id, rs.character_name, rs.tutorial_complete,
                  rs.party_json, rs.money, rs.bag_json, rs.flash_rev, rs.updated_at,
                  CASE WHEN length(rs.flash_save_b64) > 0 THEN 1 ELSE 0 END AS has_flash,
                  a.username
           FROM run_slots rs
           LEFT JOIN accounts a ON a.user_id = rs.user_id
           WHERE rs.run_id = ?
           ORDER BY rs.slot_idx ASC`
        )
        .all(runId);
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
    };

    const formatRunSummary = (runRow, viewerUserId) => {
      const slots = getRunSlotsSummary(runRow.run_id);
      const mySlot = slots.find((s) => s.userId && s.userId === viewerUserId);
      const mySlotIdx = mySlot ? mySlot.slotIdx : runRow.host_user_id === viewerUserId ? 0 : null;
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
        activeRoomPort: findActiveRoomPortForRun(runRow.run_id),
        mySlotIdx,
        slots,
      };
    };

    return {
      backend: "node:sqlite",
      getAccountByUsernameLower(usernameLower) {
        return db.prepare(`SELECT * FROM accounts WHERE username_lower = ?`).get(usernameLower) || null;
      },
      createAccount({ userId, username, usernameLower, passwordHash, passwordSalt, createdAt }) {
        db.prepare(
          `INSERT INTO accounts (user_id, username, username_lower, password_hash, password_salt, created_at)
           VALUES (?, ?, ?, ?, ?, ?)`
        ).run(userId, username, usernameLower, passwordHash, passwordSalt, createdAt);
      },
      createSession(userId) {
        const now = Date.now();
        const token = randomHex(32);
        const expiresAt = now + SESSION_TTL_MS;
        db.prepare(`DELETE FROM sessions WHERE expires_at <= ?`).run(now);
        db.prepare(`INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?)`).run(
          token,
          userId,
          expiresAt
        );
        return { token, expiresAt };
      },
      verifySessionToken(token) {
        if (typeof token !== "string") return null;
        const clean = token.trim();
        if (!clean) return null;
        const now = Date.now();
        const row = db
          .prepare(
            `SELECT s.token, s.expires_at, a.user_id, a.username
             FROM sessions s
             JOIN accounts a ON a.user_id = s.user_id
             WHERE s.token = ?`
          )
          .get(clean);
        if (!row) return null;
        if (Number(row.expires_at) <= now) {
          db.prepare(`DELETE FROM sessions WHERE token = ?`).run(clean);
          return null;
        }
        return {
          userId: row.user_id,
          username: row.username,
          expiresAt: Number(row.expires_at),
        };
      },
      deleteSession(token) {
        if (!token) return;
        db.prepare(`DELETE FROM sessions WHERE token = ?`).run(token);
      },
      listRunsForUser(userId) {
        const hostedRows = db
          .prepare(
            `SELECT r.*, a.username AS host_username
             FROM runs r
             LEFT JOIN accounts a ON a.user_id = r.host_user_id
             WHERE r.host_user_id = ?
             ORDER BY r.updated_at DESC`
          )
          .all(userId);
        const joinedRows = db
          .prepare(
            `SELECT DISTINCT r.*, a.username AS host_username
             FROM runs r
             JOIN run_slots rs ON rs.run_id = r.run_id
             LEFT JOIN accounts a ON a.user_id = r.host_user_id
             WHERE rs.user_id = ? AND r.host_user_id != ?
             ORDER BY r.updated_at DESC`
          )
          .all(userId, userId);
        return {
          hostedRuns: hostedRows.map((r) => formatRunSummary(r, userId)),
          joinedRuns: joinedRows.map((r) => formatRunSummary(r, userId)),
        };
      },
      getRun(runId) {
        return db.prepare(`SELECT * FROM runs WHERE run_id = ?`).get(runId) || null;
      },
      upsertRun({ runId, auth, data }) {
        const existingRun = db.prepare(`SELECT * FROM runs WHERE run_id = ?`).get(runId) || null;
        if (existingRun && existingRun.host_user_id !== auth.userId) {
          return { forbidden: true };
        }
        const now = Date.now();
        const existingConfig = safeJsonParse(existingRun?.run_config_json, {});
        const incomingConfig =
          data.runConfig && typeof data.runConfig === "object" && !Array.isArray(data.runConfig)
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

        runInTransaction(() => {
          if (existingRun) {
            db.prepare(
              `UPDATE runs
               SET title = ?, seed = ?, randomizer_preset = ?, run_config_json = ?, world_state_json = ?, updated_at = ?
               WHERE run_id = ?`
            ).run(title, seed, randomizerPreset, JSON.stringify(incomingConfig), JSON.stringify(incomingWorld), now, runId);
          } else {
            db.prepare(
              `INSERT INTO runs (run_id, host_user_id, title, seed, randomizer_preset, run_config_json, world_state_json, updated_at, created_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
            ).run(
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
            const existingSlot =
              db.prepare(`SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`).get(runId, slotIdx) || null;
            const existingParty = safeJsonParse(existingSlot?.party_json, []);
            const existingTutorial = Boolean(existingSlot?.tutorial_complete);
            const existingRev = Number(existingSlot?.flash_rev) || 0;
            const hasExistingProgress =
              existingRev > 0 && (existingTutorial || (Array.isArray(existingParty) && existingParty.length > 0));

            const incomingParty = Array.isArray(su.party) ? su.party.slice(0, 6) : existingParty;
            const incomingTutorial =
              typeof su.tutorialComplete === "boolean" ? su.tutorialComplete : existingTutorial;
            const blockBlankDowngrade =
              !su.force && hasExistingProgress && incomingParty.length === 0 && !incomingTutorial;

            const nextUserId =
              slotIdx === 0
                ? auth.userId
                : su.userId
                  ? String(su.userId)
                  : existingSlot?.user_id || null;
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
              db.prepare(
                `UPDATE run_slots
                 SET user_id = ?, character_name = ?, tutorial_complete = ?, party_json = ?, money = ?, bag_json = ?, updated_at = ?
                 WHERE run_id = ? AND slot_idx = ?`
              ).run(nextUserId, nextCharName, nextTutorial, nextPartyJson, nextMoney, nextBagJson, now, runId, slotIdx);
            } else {
              db.prepare(
                `INSERT INTO run_slots (run_id, slot_idx, user_id, character_name, tutorial_complete, party_json, money, bag_json, flash_save_b64, flash_rev, updated_at)
                 VALUES (?, ?, ?, ?, ?, ?, ?, ?, '', 0, ?)`
              ).run(runId, slotIdx, nextUserId, nextCharName, nextTutorial, nextPartyJson, nextMoney, nextBagJson, now);
            }
          }
        });

        const savedRow = db
          .prepare(
            `SELECT r.*, a.username AS host_username
             FROM runs r
             LEFT JOIN accounts a ON a.user_id = r.host_user_id
             WHERE r.run_id = ?`
          )
          .get(runId);
        return { run: formatRunSummary(savedRow, auth.userId) };
      },
      getSlotWithUser(runId, slotIdx) {
        return (
          db
            .prepare(
              `SELECT rs.*, a.username
               FROM run_slots rs
               LEFT JOIN accounts a ON a.user_id = rs.user_id
               WHERE rs.run_id = ? AND rs.slot_idx = ?`
            )
            .get(runId, slotIdx) || null
        );
      },
      saveSlotFlash({ runId, slotIdx, auth, isHost, body, storedFlash, force }) {
        return runInTransaction(() => {
          const freshSlot =
            db.prepare(`SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`).get(runId, slotIdx) || null;
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
            db.prepare(
              `UPDATE run_slots
               SET user_id = ?, character_name = ?, tutorial_complete = ?, party_json = ?,
                   money = ?, bag_json = ?, flash_save_b64 = ?, flash_rev = ?, updated_at = ?
               WHERE run_id = ? AND slot_idx = ?`
            ).run(
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
            db.prepare(
              `INSERT INTO run_slots (run_id, slot_idx, user_id, character_name, tutorial_complete, party_json, money, bag_json, flash_save_b64, flash_rev, updated_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
            ).run(
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

          db.prepare(`UPDATE runs SET updated_at = ? WHERE run_id = ?`).run(now, runId);
          const updatedSlot = db
            .prepare(
              `SELECT rs.*, a.username
               FROM run_slots rs
               LEFT JOIN accounts a ON a.user_id = rs.user_id
               WHERE rs.run_id = ? AND rs.slot_idx = ?`
            )
            .get(runId, slotIdx);
          return { conflict: false, nextRev, updatedAt: now, slotMeta: formatSlotMeta(updatedSlot) };
        });
      },
      claimSlot({ runId, slotIdx, userId, characterName }) {
        const now = Date.now();
        const existingSlot =
          db.prepare(`SELECT * FROM run_slots WHERE run_id = ? AND slot_idx = ?`).get(runId, slotIdx) || null;
        if (existingSlot) {
          db.prepare(
            `UPDATE run_slots SET user_id = ?, character_name = ?, updated_at = ? WHERE run_id = ? AND slot_idx = ?`
          ).run(userId, characterName, now, runId, slotIdx);
        } else {
          db.prepare(
            `INSERT INTO run_slots (run_id, slot_idx, user_id, character_name, tutorial_complete, party_json, money, bag_json, flash_save_b64, flash_rev, updated_at)
             VALUES (?, ?, ?, ?, 0, '[]', 0, '[]', '', 0, ?)`
          ).run(runId, slotIdx, userId, characterName, now);
        }
        db.prepare(`UPDATE runs SET updated_at = ? WHERE run_id = ?`).run(now, runId);
        return this.getSlotWithUser(runId, slotIdx);
      },
      deleteRun(runId) {
        runInTransaction(() => {
          db.prepare(`DELETE FROM run_slots WHERE run_id = ?`).run(runId);
          db.prepare(`DELETE FROM runs WHERE run_id = ?`).run(runId);
        });
      },
    };
  }

  // Fallback JSON store in .local_data/ for older Node (<22.5) without node:sqlite
  const jsonPath = path.join(dataDir, "emerald_relay_store.json");
  let state = { accounts: [], sessions: [], runs: [], run_slots: [] };
  if (fs.existsSync(jsonPath)) {
    state = { ...state, ...(safeJsonParse(fs.readFileSync(jsonPath, "utf8"), {}) || {}) };
  }
  const persist = () => {
    const tmp = `${jsonPath}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify(state), "utf8");
    fs.renameSync(tmp, jsonPath);
  };
  const findUsername = (userId) => {
    const acc = state.accounts.find((a) => a.user_id === userId);
    return acc ? acc.username : "";
  };
  const formatRunSummaryJson = (runRow, viewerUserId) => {
    const slots = state.run_slots
      .filter((s) => s.run_id === runRow.run_id)
      .sort((a, b) => a.slot_idx - b.slot_idx)
      .map((s) => ({
        runId: s.run_id,
        slotIdx: Number(s.slot_idx),
        userId: s.user_id || "",
        username: s.user_id ? findUsername(s.user_id) : "",
        characterName: s.character_name || "",
        tutorialComplete: Boolean(s.tutorial_complete),
        party: safeJsonParse(s.party_json, []),
        money: Number(s.money) || 0,
        bagItems: safeJsonParse(s.bag_json, []),
        flashRev: Number(s.flash_rev) || 0,
        hasFlashSave: Boolean(s.flash_save_b64 && String(s.flash_save_b64).length > 0),
        updatedAt: Number(s.updated_at) || 0,
      }));
    const mySlot = slots.find((s) => s.userId && s.userId === viewerUserId);
    return {
      runId: runRow.run_id,
      hostUserId: runRow.host_user_id,
      hostUsername: findUsername(runRow.host_user_id),
      title: runRow.title,
      seed: runRow.seed,
      randomizerPreset: runRow.randomizer_preset,
      runConfig: safeJsonParse(runRow.run_config_json, {}),
      worldState: safeJsonParse(runRow.world_state_json, {}),
      updatedAt: Number(runRow.updated_at),
      createdAt: Number(runRow.created_at),
      activeRoomPort: findActiveRoomPortForRun(runRow.run_id),
      mySlotIdx: mySlot ? mySlot.slotIdx : runRow.host_user_id === viewerUserId ? 0 : null,
      slots,
    };
  };

  return {
    backend: "json-file",
    getAccountByUsernameLower(usernameLower) {
      return state.accounts.find((a) => a.username_lower === usernameLower) || null;
    },
    createAccount(acc) {
      state.accounts.push({
        user_id: acc.userId,
        username: acc.username,
        username_lower: acc.usernameLower,
        password_hash: acc.passwordHash,
        password_salt: acc.passwordSalt,
        created_at: acc.createdAt,
      });
      persist();
    },
    createSession(userId) {
      const now = Date.now();
      const token = randomHex(32);
      const expiresAt = now + SESSION_TTL_MS;
      state.sessions = state.sessions.filter((s) => s.expires_at > now);
      state.sessions.push({ token, user_id: userId, expires_at: expiresAt });
      persist();
      return { token, expiresAt };
    },
    verifySessionToken(token) {
      if (typeof token !== "string") return null;
      const clean = token.trim();
      if (!clean) return null;
      const now = Date.now();
      const sess = state.sessions.find((s) => s.token === clean);
      if (!sess) return null;
      if (sess.expires_at <= now) {
        state.sessions = state.sessions.filter((s) => s.token !== clean);
        persist();
        return null;
      }
      const acc = state.accounts.find((a) => a.user_id === sess.user_id);
      if (!acc) return null;
      return { userId: acc.user_id, username: acc.username, expiresAt: sess.expires_at };
    },
    deleteSession(token) {
      if (!token) return;
      state.sessions = state.sessions.filter((s) => s.token !== token);
      persist();
    },
    listRunsForUser(userId) {
      const hosted = state.runs
        .filter((r) => r.host_user_id === userId)
        .sort((a, b) => b.updated_at - a.updated_at);
      const joinedIds = new Set(
        state.run_slots.filter((s) => s.user_id === userId).map((s) => s.run_id)
      );
      const joined = state.runs
        .filter((r) => r.host_user_id !== userId && joinedIds.has(r.run_id))
        .sort((a, b) => b.updated_at - a.updated_at);
      return {
        hostedRuns: hosted.map((r) => formatRunSummaryJson(r, userId)),
        joinedRuns: joined.map((r) => formatRunSummaryJson(r, userId)),
      };
    },
    getRun(runId) {
      return state.runs.find((r) => r.run_id === runId) || null;
    },
    upsertRun({ runId, auth, data }) {
      let existingRun = state.runs.find((r) => r.run_id === runId) || null;
      if (existingRun && existingRun.host_user_id !== auth.userId) return { forbidden: true };
      const now = Date.now();
      const existingConfig = safeJsonParse(existingRun?.run_config_json, {});
      const incomingConfig =
        data.runConfig && typeof data.runConfig === "object" && !Array.isArray(data.runConfig)
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

      if (existingRun) {
        existingRun.title = title;
        existingRun.seed = seed;
        existingRun.randomizer_preset = randomizerPreset;
        existingRun.run_config_json = JSON.stringify(incomingConfig);
        existingRun.world_state_json = JSON.stringify(incomingWorld);
        existingRun.updated_at = now;
      } else {
        existingRun = {
          run_id: runId,
          host_user_id: auth.userId,
          title,
          seed,
          randomizer_preset: randomizerPreset,
          run_config_json: JSON.stringify(incomingConfig),
          world_state_json: JSON.stringify(incomingWorld),
          updated_at: now,
          created_at: now,
        };
        state.runs.push(existingRun);
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
        let existingSlot = state.run_slots.find((s) => s.run_id === runId && s.slot_idx === slotIdx) || null;
        const existingParty = safeJsonParse(existingSlot?.party_json, []);
        const existingTutorial = Boolean(existingSlot?.tutorial_complete);
        const existingRev = Number(existingSlot?.flash_rev) || 0;
        const hasExistingProgress =
          existingRev > 0 && (existingTutorial || (Array.isArray(existingParty) && existingParty.length > 0));
        const incomingParty = Array.isArray(su.party) ? su.party.slice(0, 6) : existingParty;
        const incomingTutorial = typeof su.tutorialComplete === "boolean" ? su.tutorialComplete : existingTutorial;
        const blockBlankDowngrade = !su.force && hasExistingProgress && incomingParty.length === 0 && !incomingTutorial;

        const nextUserId = slotIdx === 0 ? auth.userId : su.userId ? String(su.userId) : existingSlot?.user_id || null;
        const nextCharName = String(
          su.characterName ?? su.name ?? existingSlot?.character_name ?? (slotIdx === 0 ? auth.username : "")
        ).slice(0, 24);
        const nextTutorial = blockBlankDowngrade ? (existingTutorial ? 1 : 0) : incomingTutorial ? 1 : 0;
        const nextPartyJson = blockBlankDowngrade ? existingSlot.party_json : JSON.stringify(incomingParty);
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
          Object.assign(existingSlot, {
            user_id: nextUserId,
            character_name: nextCharName,
            tutorial_complete: nextTutorial,
            party_json: nextPartyJson,
            money: nextMoney,
            bag_json: nextBagJson,
            updated_at: now,
          });
        } else {
          state.run_slots.push({
            run_id: runId,
            slot_idx: slotIdx,
            user_id: nextUserId,
            character_name: nextCharName,
            tutorial_complete: nextTutorial,
            party_json: nextPartyJson,
            money: nextMoney,
            bag_json: nextBagJson,
            flash_save_b64: "",
            flash_rev: 0,
            updated_at: now,
          });
        }
      }
      persist();
      return { run: formatRunSummaryJson(existingRun, auth.userId) };
    },
    getSlotWithUser(runId, slotIdx) {
      const slot = state.run_slots.find((s) => s.run_id === runId && s.slot_idx === slotIdx);
      if (!slot) return null;
      return { ...slot, username: slot.user_id ? findUsername(slot.user_id) : "" };
    },
    saveSlotFlash({ runId, slotIdx, auth, isHost, body, storedFlash, force }) {
      let freshSlot = state.run_slots.find((s) => s.run_id === runId && s.slot_idx === slotIdx) || null;
      const currentRev = freshSlot ? Number(freshSlot.flash_rev) || 0 : 0;
      const expectedRevRaw = body.expectedRev ?? body.baseRev;
      if (!force && expectedRevRaw !== undefined && expectedRevRaw !== null) {
        const expectedRev = Number(expectedRevRaw);
        if (!Number.isFinite(expectedRev) || expectedRev < currentRev) {
          return { conflict: true, error: "STALE_FLASH_REV", message: "Server has a newer Flash save revision.", currentRev };
        }
      }
      if (!force && typeof body.flashRev === "number" && body.flashRev > 0 && body.flashRev < currentRev) {
        return { conflict: true, error: "STALE_FLASH_REV", message: "Incoming flashRev is older than server revision.", currentRev };
      }
      const existingParty = safeJsonParse(freshSlot?.party_json, []);
      const existingTutorial = Boolean(freshSlot?.tutorial_complete);
      const hasExistingProgress =
        currentRev > 0 && (existingTutorial || (Array.isArray(existingParty) && existingParty.length > 0));
      const incomingParty = Array.isArray(body.party) ? body.party.slice(0, 6) : existingParty;
      const incomingTutorial = typeof body.tutorialComplete === "boolean" ? body.tutorialComplete : existingTutorial;
      if (!force && hasExistingProgress && incomingParty.length === 0 && !incomingTutorial) {
        return { conflict: true, error: "BLANK_OVERWRITE_BLOCKED", message: "Refusing to overwrite saved progress with empty tutorial state.", currentRev };
      }

      const nextRev = currentRev + 1;
      const now = Date.now();
      let nextUserId = freshSlot?.user_id || null;
      if (!nextUserId) {
        if (typeof body.userId === "string" && body.userId.trim() && isHost) nextUserId = body.userId.trim();
        else if (!isHost || slotIdx === 0) nextUserId = auth.userId;
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
        Object.assign(freshSlot, {
          user_id: nextUserId,
          character_name: nextCharName,
          tutorial_complete: incomingTutorial ? 1 : 0,
          party_json: JSON.stringify(incomingParty),
          money: nextMoney,
          bag_json: nextBagJson,
          flash_save_b64: storedFlash,
          flash_rev: nextRev,
          updated_at: now,
        });
      } else {
        freshSlot = {
          run_id: runId,
          slot_idx: slotIdx,
          user_id: nextUserId,
          character_name: nextCharName,
          tutorial_complete: incomingTutorial ? 1 : 0,
          party_json: JSON.stringify(incomingParty),
          money: nextMoney,
          bag_json: nextBagJson,
          flash_save_b64: storedFlash,
          flash_rev: nextRev,
          updated_at: now,
        };
        state.run_slots.push(freshSlot);
      }
      const runRow = state.runs.find((r) => r.run_id === runId);
      if (runRow) runRow.updated_at = now;
      persist();
      return {
        conflict: false,
        nextRev,
        updatedAt: now,
        slotMeta: formatSlotMeta(this.getSlotWithUser(runId, slotIdx)),
      };
    },
    claimSlot({ runId, slotIdx, userId, characterName }) {
      const now = Date.now();
      let existingSlot = state.run_slots.find((s) => s.run_id === runId && s.slot_idx === slotIdx) || null;
      if (existingSlot) {
        existingSlot.user_id = userId;
        existingSlot.character_name = characterName;
        existingSlot.updated_at = now;
      } else {
        state.run_slots.push({
          run_id: runId,
          slot_idx: slotIdx,
          user_id: userId,
          character_name: characterName,
          tutorial_complete: 0,
          party_json: "[]",
          money: 0,
          bag_json: "[]",
          flash_save_b64: "",
          flash_rev: 0,
          updated_at: now,
        });
      }
      const runRow = state.runs.find((r) => r.run_id === runId);
      if (runRow) runRow.updated_at = now;
      persist();
      return this.getSlotWithUser(runId, slotIdx);
    },
    deleteRun(runId) {
      state.run_slots = state.run_slots.filter((s) => s.run_id !== runId);
      state.runs = state.runs.filter((r) => r.run_id !== runId);
      persist();
    },
  };
}

const store = createLocalStore(LOCAL_DATA_DIR);

function getLanAddresses() {
  const interfaces = os.networkInterfaces();
  const results = [];
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name] || []) {
      if (iface.family === "IPv4" && !iface.internal && !iface.address.startsWith("169.254.")) {
        results.push(iface.address);
      }
    }
  }
  results.sort((a, b) => {
    const score = (ip) => (ip.startsWith("192.168.1.") ? 0 : ip.startsWith("192.168.") ? 1 : ip.startsWith("10.") ? 2 : 3);
    return score(a) - score(b);
  });
  return results;
}

function parseRoomPort(raw) {
  const n = parseInt(raw || String(DEFAULT_ROOM_PORT), 10);
  return Number.isFinite(n) && n >= 1024 && n <= 65535 ? n : DEFAULT_ROOM_PORT;
}

function allocateRoomPort(requestedPort) {
  let p = parseRoomPort(requestedPort);
  for (let i = 0; i < 65536; i++) {
    if (!rooms.has(p)) return p;
    p++;
    if (p > 65535) p = 27000;
  }
  return p;
}

function allocateClientPeerId(room) {
  for (let id = 2; id <= MAX_CLIENTS_PER_ROOM + 1; id++) {
    if (!room.clients.has(id)) return id;
  }
  return null;
}

function makeFrame(type, peerId, channel, payloadBuf) {
  const payload = payloadBuf ? Buffer.from(payloadBuf) : Buffer.alloc(0);
  const out = Buffer.allocUnsafe(4 + payload.length);
  out[0] = type & 0xff;
  out[1] = (peerId >> 8) & 0xff;
  out[2] = peerId & 0xff;
  out[3] = channel & 0xff;
  if (payload.length > 0) {
    payload.copy(out, 4);
  }
  return out;
}

function safeSend(ws, data, binary = true) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false;
  try {
    ws.send(data, { binary });
    return true;
  } catch (_) {
    return false;
  }
}

function safeClose(ws, code, reason) {
  if (!ws) return;
  try {
    ws.close(code, reason);
  } catch (_) {
    try {
      ws.terminate();
    } catch (_) {}
  }
}

function parseRequestUrl(rawUrl) {
  try {
    return new URL(rawUrl || "/", "http://localhost");
  } catch (_) {
    return null;
  }
}

function pipeFileStream(stream, res) {
  stream.on("error", () => {
    if (!res.headersSent) {
      res.writeHead(500);
      res.end("Internal Server Error");
    } else {
      res.destroy();
    }
  });
  res.on("close", () => {
    if (!stream.destroyed) stream.destroy();
  });
  stream.pipe(res);
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

function getRoomListSnapshot() {
  const list = [];
  for (const [port, room] of rooms.entries()) {
    if (room.hostWs.readyState !== WebSocket.OPEN) continue;
    list.push(formatRoomSnapshot(port, room));
  }
  return list;
}

function mergeRoomInfo(room, rawJson) {
  try {
    const parsed = JSON.parse(rawJson);
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
      if (typeof parsed.sessionToken === "string" && parsed.sessionToken.trim()) {
        const verified = store.verifySessionToken(parsed.sessionToken);
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

const MIME_TYPES = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".mjs": "application/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".map": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".webp": "image/webp",
  ".ico": "image/x-icon",
  ".woff": "font/woff",
  ".woff2": "font/woff2",
  ".ttf": "font/ttf",
  ".bin": "application/octet-stream",
  ".data": "application/octet-stream",
  ".pck": "application/octet-stream",
};

function isChunkedOrBinaryAsset(ext) {
  return (
    ext === ".wasm" ||
    ext === ".bin" ||
    ext === ".data" ||
    ext === ".pck" ||
    /^\.part\d+$/i.test(ext)
  );
}

function getMimeType(filePath) {
  const ext = path.extname(filePath).toLowerCase();
  if (/^\.part\d+$/i.test(ext)) {
    return "application/octet-stream";
  }
  return MIME_TYPES[ext] || "application/octet-stream";
}

function sendJson(res, statusCode, bodyObj) {
  const payload = JSON.stringify(bodyObj);
  res.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
  });
  res.end(payload);
}

function extractBearerToken(req) {
  const auth = String(req.headers["authorization"] || "");
  if (/^Bearer\s+/i.test(auth)) {
    return auth.replace(/^Bearer\s+/i, "").trim();
  }
  const alt = String(req.headers["x-session-token"] || "");
  return alt.trim();
}

function requireAuth(req) {
  const token = extractBearerToken(req);
  if (!token) return null;
  return store.verifySessionToken(token);
}

function readJsonBody(req) {
  return new Promise((resolve) => {
    const contentLen = Number(req.headers["content-length"] || "0");
    if (Number.isFinite(contentLen) && contentLen > MAX_JSON_BODY_BYTES) {
      resolve({ errorStatus: 413, errorMessage: "Request payload too large" });
      return;
    }
    const chunks = [];
    let total = 0;
    let settled = false;
    req.on("data", (chunk) => {
      if (settled) return;
      total += chunk.length;
      if (total > MAX_JSON_BODY_BYTES) {
        settled = true;
        resolve({ errorStatus: 413, errorMessage: "Request payload too large" });
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => {
      if (settled) return;
      settled = true;
      const text = Buffer.concat(chunks).toString("utf8");
      if (!text.trim()) {
        resolve({ data: {} });
        return;
      }
      try {
        const parsed = JSON.parse(text);
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          resolve({ errorStatus: 400, errorMessage: "JSON body must be an object" });
          return;
        }
        resolve({ data: parsed });
      } catch (_) {
        resolve({ errorStatus: 400, errorMessage: "Invalid JSON body" });
      }
    });
    req.on("error", () => {
      if (settled) return;
      settled = true;
      resolve({ errorStatus: 400, errorMessage: "Failed to read request body" });
    });
  });
}

async function handleApiRoute(req, res, parsedUrl, pathname) {
  const method = (req.method || "GET").toUpperCase();

  if (pathname === "/api/info" && method === "GET") {
    const addr = server.address();
    const activePort = addr && typeof addr === "object" ? addr.port : PORT;
    sendJson(res, 200, {
      ok: true,
      service: "Pokemon Web Multiplayer Relay",
      port: activePort,
      lanIps: getLanAddresses(),
      roomCount: rooms.size,
      rooms: getRoomListSnapshot(),
    });
    return true;
  }

  if (pathname === "/api/rooms" && method === "GET") {
    sendJson(res, 200, { rooms: getRoomListSnapshot() });
    return true;
  }

  if (pathname === "/api/auth/register" && method === "POST") {
    const parsed = await readJsonBody(req);
    if (parsed.errorStatus) {
      sendJson(res, parsed.errorStatus, { ok: false, error: "BAD_REQUEST", message: parsed.errorMessage });
      return true;
    }
    const uNorm = normalizeUsername(parsed.data.username);
    if (uNorm.error) {
      sendJson(res, 400, { ok: false, error: "INVALID_USERNAME", message: uNorm.error });
      return true;
    }
    const pVal = validatePassword(parsed.data.password);
    if (pVal.error) {
      sendJson(res, 400, { ok: false, error: "INVALID_PASSWORD", message: pVal.error });
      return true;
    }
    if (store.getAccountByUsernameLower(uNorm.usernameLower)) {
      sendJson(res, 409, { ok: false, error: "USERNAME_TAKEN", message: "Username is already taken." });
      return true;
    }

    const userId = `usr_${randomHex(12)}`;
    const salt = randomHex(16);
    const passwordHash = await hashPassword(pVal.password, salt);
    try {
      store.createAccount({
        userId,
        username: uNorm.username,
        usernameLower: uNorm.usernameLower,
        passwordHash,
        passwordSalt: salt,
        createdAt: Date.now(),
      });
    } catch (_) {
      sendJson(res, 409, { ok: false, error: "USERNAME_TAKEN", message: "Username is already taken." });
      return true;
    }
    const session = store.createSession(userId);
    sendJson(res, 201, {
      ok: true,
      userId,
      username: uNorm.username,
      sessionToken: session.token,
      expiresAt: session.expiresAt,
    });
    return true;
  }

  if (pathname === "/api/auth/login" && method === "POST") {
    const parsed = await readJsonBody(req);
    if (parsed.errorStatus) {
      sendJson(res, parsed.errorStatus, { ok: false, error: "BAD_REQUEST", message: parsed.errorMessage });
      return true;
    }
    const rawUser = String(parsed.data.username || "").trim().replace(/\s+/g, " ");
    const rawPass = parsed.data.password;
    if (!rawUser || typeof rawPass !== "string" || !rawPass) {
      sendJson(res, 400, { ok: false, error: "INVALID_CREDENTIALS", message: "Username and password are required." });
      return true;
    }
    const account = store.getAccountByUsernameLower(rawUser.toLowerCase());
    if (!account) {
      sendJson(res, 401, { ok: false, error: "INVALID_CREDENTIALS", message: "Invalid username or password." });
      return true;
    }
    const derived = await hashPassword(rawPass, account.password_salt);
    if (!constantTimeEqualHex(derived, account.password_hash)) {
      sendJson(res, 401, { ok: false, error: "INVALID_CREDENTIALS", message: "Invalid username or password." });
      return true;
    }
    const session = store.createSession(account.user_id);
    sendJson(res, 200, {
      ok: true,
      userId: account.user_id,
      username: account.username,
      sessionToken: session.token,
      expiresAt: session.expiresAt,
    });
    return true;
  }

  if (pathname === "/api/auth/me" && method === "GET") {
    const auth = requireAuth(req);
    if (!auth) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED", message: "Invalid or expired session." });
      return true;
    }
    sendJson(res, 200, { ok: true, userId: auth.userId, username: auth.username, expiresAt: auth.expiresAt });
    return true;
  }

  if (pathname === "/api/auth/logout" && method === "POST") {
    let token = extractBearerToken(req);
    if (!token) {
      const parsed = await readJsonBody(req);
      if (!parsed.errorStatus && typeof parsed.data.sessionToken === "string") {
        token = parsed.data.sessionToken.trim();
      }
    }
    store.deleteSession(token);
    sendJson(res, 200, { ok: true });
    return true;
  }

  if (pathname === "/api/runs" && method === "GET") {
    const auth = requireAuth(req);
    if (!auth) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED", message: "Sign in required." });
      return true;
    }
    const grouped = store.listRunsForUser(auth.userId);
    sendJson(res, 200, { ok: true, ...grouped });
    return true;
  }

  if (pathname === "/api/runs/upsert" && method === "POST") {
    const auth = requireAuth(req);
    if (!auth) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED", message: "Sign in required." });
      return true;
    }
    const parsed = await readJsonBody(req);
    if (parsed.errorStatus) {
      sendJson(res, parsed.errorStatus, { ok: false, error: "BAD_REQUEST", message: parsed.errorMessage });
      return true;
    }
    const runId = normalizeRunId(parsed.data.runId);
    if (!runId) {
      sendJson(res, 400, { ok: false, error: "INVALID_RUN_ID", message: "Valid runId is required." });
      return true;
    }
    const result = store.upsertRun({ runId, auth, data: parsed.data });
    if (result.forbidden) {
      sendJson(res, 403, { ok: false, error: "FORBIDDEN", message: "Only the run host can update run metadata." });
      return true;
    }
    sendJson(res, 200, { ok: true, run: result.run });
    return true;
  }

  const flashMatch = /^\/api\/runs\/([^/]+)\/slot\/([0-3])\/flash$/.exec(pathname);
  if (flashMatch) {
    const auth = requireAuth(req);
    if (!auth) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED", message: "Sign in required." });
      return true;
    }
    const runId = normalizeRunId(flashMatch[1]);
    const slotIdx = parseSlotIdx(flashMatch[2]);
    if (!runId || slotIdx === null) {
      sendJson(res, 400, { ok: false, error: "BAD_REQUEST", message: "Invalid runId or slotIdx." });
      return true;
    }
    const runRow = store.getRun(runId);
    if (!runRow) {
      sendJson(res, 404, { ok: false, error: "RUN_NOT_FOUND", message: "Run not found." });
      return true;
    }

    if (method === "GET") {
      const slotRow = store.getSlotWithUser(runId, slotIdx);
      if (!slotRow) {
        sendJson(res, 404, { ok: false, error: "SLOT_NOT_FOUND", message: "No save data for this slot." });
        return true;
      }
      const isHost = runRow.host_user_id === auth.userId;
      const isOwner = slotRow.user_id && slotRow.user_id === auth.userId;
      if (!isHost && !isOwner) {
        sendJson(res, 403, { ok: false, error: "FORBIDDEN", message: "You do not have access to this slot." });
        return true;
      }
      const requestedEnc = parsedUrl.searchParams.get("encoding") === "gzip" ? "gzip" : "raw";
      const formatted = formatStoredFlashForResponse(slotRow.flash_save_b64, requestedEnc);
      sendJson(res, 200, {
        ok: true,
        flashSaveB64: formatted.flashSaveB64,
        flashEncoding: formatted.flashEncoding,
        flashRev: Number(slotRow.flash_rev) || 0,
        slotMeta: formatSlotMeta(slotRow),
      });
      return true;
    }

    if (method === "POST") {
      const parsed = await readJsonBody(req);
      if (parsed.errorStatus) {
        sendJson(res, parsed.errorStatus, { ok: false, error: "BAD_REQUEST", message: parsed.errorMessage });
        return true;
      }
      const body = parsed.data;
      const force = Boolean(body.force);
      const existingSlot = store.getSlotWithUser(runId, slotIdx);
      const isHost = runRow.host_user_id === auth.userId;
      const isOwner = existingSlot && existingSlot.user_id === auth.userId;
      if (!isHost && !isOwner) {
        sendJson(res, 403, {
          ok: false,
          error: "FORBIDDEN",
          message: "Claim this slot or host the run before saving Flash data.",
        });
        return true;
      }

      let storedFlash = "";
      if (typeof body.flashSaveB64 === "string" && body.flashSaveB64.trim() !== "") {
        const normalized = normalizeIncomingFlashForStorage(
          body.flashSaveB64,
          body.flashEncoding || body.encoding,
          body.compressed
        );
        if (normalized.error) {
          sendJson(res, 400, { ok: false, error: "INVALID_FLASH_PAYLOAD", message: normalized.error });
          return true;
        }
        storedFlash = normalized.stored;
      } else if (!force) {
        sendJson(res, 400, { ok: false, error: "INVALID_FLASH_PAYLOAD", message: "flashSaveB64 is required." });
        return true;
      }

      const txResult = store.saveSlotFlash({ runId, slotIdx, auth, isHost, body, storedFlash, force });
      if (txResult.conflict) {
        sendJson(res, 409, {
          ok: false,
          stale: true,
          error: txResult.error,
          message: txResult.message,
          currentRev: txResult.currentRev,
          flashRev: txResult.currentRev,
        });
        return true;
      }
      sendJson(res, 200, {
        ok: true,
        flashRev: txResult.nextRev,
        updatedAt: txResult.updatedAt,
        slotMeta: txResult.slotMeta,
      });
      return true;
    }

    sendJson(res, 405, { ok: false, error: "METHOD_NOT_ALLOWED", message: "Method not allowed" });
    return true;
  }

  const claimMatch = /^\/api\/runs\/([^/]+)\/slot\/([0-3])\/claim$/.exec(pathname);
  if (claimMatch && method === "POST") {
    const auth = requireAuth(req);
    if (!auth) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED", message: "Sign in required." });
      return true;
    }
    const runId = normalizeRunId(claimMatch[1]);
    const slotIdx = parseSlotIdx(claimMatch[2]);
    if (!runId || slotIdx === null) {
      sendJson(res, 400, { ok: false, error: "BAD_REQUEST", message: "Invalid runId or slotIdx." });
      return true;
    }
    const runRow = store.getRun(runId);
    if (!runRow) {
      sendJson(res, 404, { ok: false, error: "RUN_NOT_FOUND", message: "Run not found." });
      return true;
    }
    if (slotIdx === 0 && runRow.host_user_id !== auth.userId) {
      sendJson(res, 403, { ok: false, error: "FORBIDDEN", message: "Slot 0 belongs to the run host." });
      return true;
    }
    const parsed = await readJsonBody(req);
    if (parsed.errorStatus) {
      sendJson(res, parsed.errorStatus, { ok: false, error: "BAD_REQUEST", message: parsed.errorMessage });
      return true;
    }
    const existingSlot = store.getSlotWithUser(runId, slotIdx);
    if (existingSlot && existingSlot.user_id && existingSlot.user_id !== auth.userId) {
      if (isUserOnlineInRunRoom(runId, existingSlot.user_id, slotIdx)) {
        sendJson(res, 409, { ok: false, error: "SLOT_ONLINE", message: "That trainer slot is currently online." });
        return true;
      }
    }
    const charName = String(parsed.data.characterName ?? existingSlot?.character_name ?? auth.username).slice(0, 24);
    const updated = store.claimSlot({ runId, slotIdx, userId: auth.userId, characterName: charName });
    sendJson(res, 200, { ok: true, slot: formatSlotMeta(updated) });
    return true;
  }

  const runDeleteMatch = /^\/api\/runs\/([^/]+)$/.exec(pathname);
  if (runDeleteMatch && method === "DELETE") {
    const auth = requireAuth(req);
    if (!auth) {
      sendJson(res, 401, { ok: false, error: "UNAUTHORIZED", message: "Sign in required." });
      return true;
    }
    const runId = normalizeRunId(runDeleteMatch[1]);
    if (!runId) {
      sendJson(res, 400, { ok: false, error: "BAD_REQUEST", message: "Invalid runId." });
      return true;
    }
    const runRow = store.getRun(runId);
    if (!runRow) {
      sendJson(res, 404, { ok: false, error: "RUN_NOT_FOUND", message: "Run not found." });
      return true;
    }
    if (runRow.host_user_id !== auth.userId) {
      sendJson(res, 403, { ok: false, error: "FORBIDDEN", message: "Only the host can delete this run." });
      return true;
    }
    store.deleteRun(runId);
    sendJson(res, 200, { ok: true, runId });
    return true;
  }

  return false;
}

const server = http.createServer(async (req, res) => {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, HEAD, OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Session-Token");

  if (req.method === "OPTIONS") {
    res.writeHead(204);
    res.end();
    return;
  }

  const parsedUrl = parseRequestUrl(req.url);
  if (!parsedUrl) {
    res.writeHead(400);
    res.end("Bad Request");
    return;
  }
  let pathname;
  try {
    pathname = decodeURIComponent(parsedUrl.pathname);
  } catch (_) {
    res.writeHead(400);
    res.end("Bad Request");
    return;
  }
  if (pathname.includes("\0")) {
    res.writeHead(400);
    res.end("Bad Request");
    return;
  }

  if (pathname.startsWith("/api/")) {
    try {
      const handled = await handleApiRoute(req, res, parsedUrl, pathname);
      if (handled) return;
      sendJson(res, 404, { ok: false, error: "NOT_FOUND", message: "API route not found." });
      return;
    } catch (err) {
      console.error("[API ERROR]", err);
      if (!res.headersSent) {
        sendJson(res, 500, { ok: false, error: "INTERNAL_ERROR", message: "Internal server error." });
      }
      return;
    }
  }

  if (req.method !== "GET" && req.method !== "HEAD") {
    res.writeHead(405, { Allow: "GET, HEAD, OPTIONS" });
    res.end("Method Not Allowed");
    return;
  }

  const relPath = pathname === "/" ? "/index.html" : pathname;
  const safePath = path.resolve(PUBLIC_DIR, "." + path.posix.normalize("/" + relPath.replace(/\\/g, "/")));
  if (safePath !== PUBLIC_DIR && !safePath.startsWith(PUBLIC_DIR + path.sep)) {
    res.writeHead(403);
    res.end("Forbidden");
    return;
  }

  fs.stat(safePath, (err, stat) => {
    if (err || !stat.isFile()) {
      if (pathname === "/" || pathname === "/health") {
        sendJson(res, 200, {
          ok: true,
          service: "Pokemon Web Multiplayer Relay",
          roomCount: rooms.size,
        });
        return;
      }
      res.writeHead(404);
      res.end("Not found");
      return;
    }

    const ext = path.extname(safePath).toLowerCase();
    const contentType = getMimeType(safePath);
    const etag = `"${stat.size}-${Number(stat.mtimeMs).toString(16)}"`;

    res.setHeader("Content-Type", contentType);
    res.setHeader("Accept-Ranges", "bytes");
    res.setHeader("ETag", etag);
    res.setHeader(
      "Cache-Control",
      isChunkedOrBinaryAsset(ext) ? "public, max-age=3600" : "no-cache"
    );

    if (req.headers["if-none-match"] === etag) {
      res.writeHead(304);
      res.end();
      return;
    }

    const rangeHeader = req.headers.range;
    if (rangeHeader) {
      const match = /^bytes=(\d*)-(\d*)$/i.exec(rangeHeader.trim());
      if (!match || (match[1] === "" && match[2] === "") || stat.size === 0) {
        res.writeHead(416, { "Content-Range": `bytes */${stat.size}` });
        res.end();
        return;
      }

      let start;
      let end;
      if (match[1] === "") {
        const suffixLen = parseInt(match[2], 10);
        if (!Number.isFinite(suffixLen) || suffixLen <= 0) {
          res.writeHead(416, { "Content-Range": `bytes */${stat.size}` });
          res.end();
          return;
        }
        start = Math.max(0, stat.size - suffixLen);
        end = stat.size - 1;
      } else {
        start = parseInt(match[1], 10);
        end = match[2] ? Math.min(parseInt(match[2], 10), stat.size - 1) : stat.size - 1;
      }

      if (!Number.isFinite(start) || !Number.isFinite(end) || start < 0 || start >= stat.size || end < start) {
        res.writeHead(416, { "Content-Range": `bytes */${stat.size}` });
        res.end();
        return;
      }

      res.writeHead(206, {
        "Content-Range": `bytes ${start}-${end}/${stat.size}`,
        "Content-Length": end - start + 1,
      });
      if (req.method === "HEAD") {
        res.end();
        return;
      }
      pipeFileStream(fs.createReadStream(safePath, { start, end }), res);
      return;
    }

    res.writeHead(200, { "Content-Length": stat.size });
    if (req.method === "HEAD") {
      res.end();
      return;
    }
    pipeFileStream(fs.createReadStream(safePath), res);
  });
});

const wss = new WebSocketServer({ noServer: true, maxPayload: 16 * 1024 * 1024 });
wss.on("error", () => {});

server.on("clientError", (err, socket) => {
  if (err && err.code === "ECONNRESET") return;
  if (socket.writable && !socket.destroyed) {
    socket.end("HTTP/1.1 400 Bad Request\r\n\r\n");
  }
});

server.on("upgrade", (req, socket, head) => {
  socket.on("error", () => {});
  socket.setNoDelay(true);
  const parsedUrl = parseRequestUrl(req.url);
  if (!parsedUrl || parsedUrl.pathname !== "/ws") {
    socket.destroy();
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    wss.emit("connection", ws, req, parsedUrl);
  });
});

function handleHostConnection(ws, requestedRoom) {
  const assignedPort = allocateRoomPort(requestedRoom);
  const room = {
    port: assignedPort,
    hostWs: ws,
    hostAuth: ws._auth || null,
    clients: new Map(),
    clientAuth: new Map(),
    info: createDefaultRoomInfo(assignedPort),
    createdAt: Date.now(),
  };
  if (room.hostAuth) {
    room.info.host_user_id = room.hostAuth.userId;
  }
  rooms.set(assignedPort, room);
  console.log(`[ROOM] Host created room #${assignedPort} (active rooms: ${rooms.size})`);

  safeSend(ws, makeFrame(FRAME_HOST_ASSIGNED, assignedPort, 0), true);

  ws.on("message", (data, isBinary) => {
    if (!isBinary) {
      mergeRoomInfo(room, Buffer.isBuffer(data) ? data.toString("utf8") : String(data));
      return;
    }

    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (buf.length < 4) return;
    const frameType = buf[0];
    const targetPeer = (buf[1] << 8) | buf[2];
    const channel = buf[3];
    const payload = buf.subarray(4);

    if (frameType === FRAME_DATA) {
      const outFrame = makeFrame(FRAME_DATA, 1, channel, payload);
      if (targetPeer === 0) {
        for (const clientWs of room.clients.values()) {
          safeSend(clientWs, outFrame, true);
        }
      } else {
        const clientWs = room.clients.get(targetPeer);
        safeSend(clientWs, outFrame, true);
      }
    } else if (frameType === FRAME_KICK_PEER) {
      const clientWs = room.clients.get(targetPeer);
      if (clientWs) {
        room.clients.delete(targetPeer);
        room.clientAuth.delete(targetPeer);
        safeSend(clientWs, makeFrame(FRAME_REJECTED, 1, 0, Buffer.from("Kicked by host")), true);
        safeClose(clientWs, 4001, "Kicked by host");
        safeSend(room.hostWs, makeFrame(FRAME_PEER_DISCONNECTED, targetPeer, 0), true);
      }
    } else if (frameType === FRAME_ROOM_INFO) {
      mergeRoomInfo(room, payload.toString("utf8"));
    }
  });

  ws.on("close", () => {
    if (rooms.get(assignedPort) === room) {
      rooms.delete(assignedPort);
    }
    console.log(`[ROOM] Host closed room #${assignedPort} (active rooms: ${rooms.size})`);
    for (const clientWs of room.clients.values()) {
      safeSend(clientWs, makeFrame(FRAME_PEER_DISCONNECTED, 1, 0), true);
      safeClose(clientWs, 4000, "Host closed room");
    }
    room.clients.clear();
    room.clientAuth.clear();
  });
}

/** Validate whether a joining client can enter `room`, returning null if allowed. */
function getClientJoinRejection(room) {
  if (!room || room.hostWs.readyState !== WebSocket.OPEN) {
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

function rejectClientConnection(ws, rejection) {
  safeSend(ws, makeFrame(FRAME_REJECTED, 0, 0, Buffer.from(rejection.payload)), true);
  safeClose(ws, rejection.closeCode, rejection.closeReason);
}

function rewriteControlIdentityPayload(ws, room, peerId, payload) {
  const parsed = safeJsonParse(payload.toString("utf8"), null);
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
    const checked = store.verifySessionToken(parsed.sessionToken);
    if (checked) {
      verifiedAuth = checked;
      ws._auth = checked;
      room.clientAuth.set(peerId, checked);
    } else {
      verifiedAuth = null;
      authError = "INVALID_SESSION";
    }
  }

  delete parsed.sessionToken;
  delete parsed.authUserId;
  delete parsed.authUsername;
  delete parsed.authVerified;
  delete parsed.authError;

  parsed.authUserId = verifiedAuth ? verifiedAuth.userId : "";
  parsed.authUsername = verifiedAuth ? verifiedAuth.username : "";
  parsed.authVerified = Boolean(verifiedAuth);
  if (authError) parsed.authError = authError;

  return Buffer.from(JSON.stringify(parsed), "utf8");
}

function handleClientConnection(ws, requestedRoom) {
  const room = rooms.get(requestedRoom);
  const rejection = getClientJoinRejection(room);
  if (rejection) {
    rejectClientConnection(ws, rejection);
    return;
  }

  const peerId = allocateClientPeerId(room);
  if (peerId === null) {
    rejectClientConnection(ws, {
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
  console.log(`[ROOM] Client peer=${peerId} joined room #${room.port} (${1 + room.clients.size}/4)`);

  const handshakePayload = Buffer.from(
    JSON.stringify({
      self_peer_id: peerId,
      room_port: room.port,
      auth_user_id: ws._auth ? ws._auth.userId : "",
      auth_username: ws._auth ? ws._auth.username : "",
    }),
    "utf8"
  );
  safeSend(ws, makeFrame(FRAME_PEER_CONNECTED, 1, 0, handshakePayload), true);
  safeSend(room.hostWs, makeFrame(FRAME_PEER_CONNECTED, peerId, 0), true);

  ws.on("message", (data, isBinary) => {
    if (!isBinary) return;
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (buf.length < 4) return;
    const frameType = buf[0];
    const targetPeer = (buf[1] << 8) | buf[2];
    const channel = buf[3];
    const payload = buf.subarray(4);

    if (frameType === FRAME_DATA) {
      const isJson = payload.length > 0 && payload[0] === 0x7b;
      const finalPayload =
        channel === CH_CONTROL && isJson
          ? rewriteControlIdentityPayload(ws, room, peerId, payload)
          : payload;
      const outFrame = makeFrame(FRAME_DATA, peerId, channel, finalPayload);
      if (isJson || targetPeer === 1) {
        safeSend(room.hostWs, outFrame, true);
      } else if (targetPeer === 0) {
        safeSend(room.hostWs, outFrame, true);
        for (const [otherPeerId, otherWs] of room.clients.entries()) {
          if (otherPeerId !== peerId) {
            safeSend(otherWs, outFrame, true);
          }
        }
      } else if (targetPeer >= 2 && targetPeer !== peerId) {
        const targetWs = room.clients.get(targetPeer);
        safeSend(targetWs, outFrame, true);
      }
    }
  });

  ws.on("close", () => {
    if (room.clients.get(peerId) === ws) {
      room.clients.delete(peerId);
      room.clientAuth.delete(peerId);
      console.log(`[ROOM] Client peer=${peerId} left room #${room.port}`);
      safeSend(room.hostWs, makeFrame(FRAME_PEER_DISCONNECTED, peerId, 0), true);
    }
  });
}

wss.on("connection", (ws, _req, parsedUrl) => {
  ws.on("error", () => {});

  const defaultRole = parsedUrl.searchParams.has("room") ? "client" : "discover";
  const role = (parsedUrl.searchParams.get("role") || defaultRole).toLowerCase();
  const requestedRoom = parseRoomPort(parsedUrl.searchParams.get("room"));
  const queryToken = parsedUrl.searchParams.get("sessionToken") || parsedUrl.searchParams.get("token") || "";
  ws._auth = queryToken ? store.verifySessionToken(queryToken) : null;

  if (role === "discover") {
    const sendRooms = () => {
      safeSend(ws, JSON.stringify({ rooms: getRoomListSnapshot() }), false);
    };
    sendRooms();
    ws.on("message", () => sendRooms());
    return;
  }

  if (role === "host") {
    handleHostConnection(ws, requestedRoom);
    return;
  }

  if (role === "client") {
    handleClientConnection(ws, requestedRoom);
    return;
  }

  safeSend(ws, makeFrame(FRAME_REJECTED, 0, 0, Buffer.from("Unknown role")), true);
  safeClose(ws, 4002, "Unknown role");
});

function shutdown() {
  for (const client of wss.clients) {
    try {
      client.close(1001, "Server shutting down");
    } catch (_) {}
  }
  wss.close(() => {
    server.close(() => process.exit(0));
  });
  setTimeout(() => process.exit(0), 1500).unref();
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

server.listen(PORT, "0.0.0.0", () => {
  const addr = server.address();
  const activePort = addr && typeof addr === "object" ? addr.port : PORT;
  const lanIps = getLanAddresses();
  console.log("============================================================");
  console.log(`  Pokemon Web Multiplayer Relay Running! (store: ${store.backend})`);
  console.log(`  Local URL : http://localhost:${activePort}`);
  for (const ip of lanIps) {
    console.log(`  LAN URL   : http://${ip}:${activePort}`);
  }
  console.log("============================================================");
});
