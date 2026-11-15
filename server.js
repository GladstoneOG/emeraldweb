const http = require("http");
const fs = require("fs");
const path = require("path");
const os = require("os");
const { WebSocketServer, WebSocket } = require("ws");

const PORT = parseInt(process.env.PORT || "8080", 10);
const PUBLIC_DIR = path.resolve(__dirname, "public");

const FRAME_DATA = 1;
const FRAME_PEER_CONNECTED = 2;
const FRAME_PEER_DISCONNECTED = 3;
const FRAME_KICK_PEER = 4;
const FRAME_ROOM_INFO = 5;
const FRAME_HOST_ASSIGNED = 6;
const FRAME_REJECTED = 7;

const MAX_CLIENTS_PER_ROOM = 3; // 1 Host (peerId=1) + up to 3 Clients (peerId=2..4)
const DEFAULT_ROOM_PORT = 27462;

// Active rooms keyed by integer room port (e.g. 27462)
const rooms = new Map();

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
      room.info = {
        ...room.info,
        ...parsed,
        game_port: room.port,
        address: "relay",
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

const server = http.createServer((req, res) => {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "*");

  if (req.method === "OPTIONS") {
    res.writeHead(204);
    res.end();
    return;
  }

  if (req.method !== "GET" && req.method !== "HEAD") {
    res.writeHead(405, { Allow: "GET, HEAD, OPTIONS" });
    res.end("Method Not Allowed");
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

  if (pathname === "/api/info") {
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
    return;
  }

  if (pathname === "/api/rooms") {
    sendJson(res, 200, { rooms: getRoomListSnapshot() });
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
    clients: new Map(),
    info: createDefaultRoomInfo(assignedPort),
    createdAt: Date.now(),
  };
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
  console.log(`[ROOM] Client peer=${peerId} joined room #${room.port} (${1 + room.clients.size}/4)`);

  const handshakePayload = Buffer.from(
    JSON.stringify({ self_peer_id: peerId, room_port: room.port }),
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
      const outFrame = makeFrame(FRAME_DATA, peerId, channel, payload);
      const isJson = payload.length > 0 && payload[0] === 0x7b;
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
  console.log("  Pokemon Web Multiplayer Relay Running!");
  console.log(`  Local URL : http://localhost:${activePort}`);
  for (const ip of lanIps) {
    console.log(`  LAN URL   : http://${ip}:${activePort}`);
  }
  console.log("============================================================");
});
