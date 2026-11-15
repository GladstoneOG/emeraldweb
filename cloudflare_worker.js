// ============================================================================
// Pokemon Web Multiplayer Relay — Cloudflare Worker + Durable Object
// Deploy with: npx wrangler deploy
// ============================================================================

const FRAME_DATA = 1;
const FRAME_PEER_CONNECTED = 2;
const FRAME_PEER_DISCONNECTED = 3;
const FRAME_KICK_PEER = 4;
const FRAME_ROOM_INFO = 5;
const FRAME_HOST_ASSIGNED = 6;
const FRAME_REJECTED = 7;

const MAX_CLIENTS_PER_ROOM = 3; // 1 Host (peerId=1) + up to 3 Clients (peerId=2..4)
const DEFAULT_ROOM_PORT = 27462;
const HOST_GRACE_MS = 6000;

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
  "Access-Control-Allow-Headers": "*",
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

export class PokemonRelayHub {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.rooms = new Map();
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
        room.info = {
          ...room.info,
          ...parsed,
          game_port: room.port,
          address: "relay",
        };
      }
    } catch (_) {}
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
      return new Response(
        JSON.stringify({
          ok: true,
          service: "Pokemon Web Multiplayer Relay (Cloudflare DO)",
          port: 443,
          lanIps: [],
          roomCount: rooms.length,
          rooms,
        }),
        { status: 200, headers: { ...CORS_HEADERS, "Content-Type": "application/json; charset=utf-8" } }
      );
    }

    if (url.pathname === "/api/rooms") {
      return new Response(
        JSON.stringify({ rooms: this.getRoomListSnapshot() }),
        { status: 200, headers: { ...CORS_HEADERS, "Content-Type": "application/json; charset=utf-8" } }
      );
    }

    if (url.pathname === "/" || url.pathname === "/health") {
      return new Response(
        request.method === "HEAD"
          ? null
          : JSON.stringify({
              ok: true,
              service: "Pokemon Web Multiplayer Relay (Cloudflare DO)",
              roomCount: this.rooms.size,
            }),
        { status: 200, headers: { ...CORS_HEADERS, "Content-Type": "application/json; charset=utf-8" } }
      );
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
      resumed = true;
    } else {
      room = {
        port: roomPort,
        hostWs: ws,
        clients: new Map(),
        info: createDefaultRoomInfo(roomPort),
        cleanupTimer: null,
      };
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

    try {
      const handshakePayload = textEncoder.encode(
        JSON.stringify({ self_peer_id: peerId, room_port: room.port })
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
        const outFrame = makeFrame(FRAME_DATA, peerId, channel, payload);
        const isJson = payload.byteLength > 0 && payload[0] === 0x7b;
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
