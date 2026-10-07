import { createServer } from "node:http";
import { createHmac, randomBytes } from "node:crypto";
import { pathToFileURL } from "node:url";
import { WebSocket, WebSocketServer } from "ws";

const MAX_PLAYERS = 4;
const MAX_ROOMS = 10_000;
const MAX_PAYLOAD_BYTES = 32 * 1024;
const ROOM_CODE_PATTERN = /^[0-9A-F]{16}$/;
const PLAYER_ID_PATTERN = /^[1-9][0-9]{0,9}$/;
const SIGNAL_KINDS = new Set(["offer", "answer", "candidate"]);

function send(socket, message) {
  if (socket.readyState === WebSocket.OPEN)
    socket.send(JSON.stringify(message));
}

function validPlayerId(value) {
  return typeof value === "string" && PLAYER_ID_PATTERN.test(value)
    && Number(value) <= 0xFFFFFFFF;
}

export function createSignalingServer({
  host = "127.0.0.1",
  port = 0,
  stunUrl = process.env.COOP_STUN_URL,
  turnUrl = process.env.COOP_TURN_URL,
  turnSharedSecret = process.env.COOP_TURN_SHARED_SECRET,
  turnCredentialLifetimeSeconds = process.env.COOP_TURN_CREDENTIAL_LIFETIME_SECONDS === undefined
    ? 1800 : Number(process.env.COOP_TURN_CREDENTIAL_LIFETIME_SECONDS),
} = {}) {
  if (stunUrl !== undefined && (!/^stun:[^\s]{1,507}$/.test(stunUrl)))
    throw new Error("COOP_STUN_URL must be a STUN URL no longer than 512 characters");
  if (turnUrl !== undefined && (!/^turns?:[^\s]{1,506}$/.test(turnUrl)))
    throw new Error("COOP_TURN_URL must be a TURN URL no longer than 512 characters");
  if (turnUrl && (!turnSharedSecret || turnSharedSecret.length < 32))
    throw new Error("COOP_TURN_SHARED_SECRET must contain at least 32 characters when TURN is enabled");
  if (!Number.isInteger(turnCredentialLifetimeSeconds) || turnCredentialLifetimeSeconds < 60
    || turnCredentialLifetimeSeconds > 86_400)
    throw new Error("TURN credential lifetime must be between 60 and 86400 seconds");

  function issueIceServers(playerId) {
    const iceServers = [];
    if (stunUrl)
      iceServers.push({ urls: stunUrl });
    if (turnUrl) {
      const expires = Math.floor(Date.now() / 1000) + turnCredentialLifetimeSeconds;
      const username = `${expires}:${playerId}`;
      const credential = createHmac("sha1", turnSharedSecret).update(username).digest("base64");
      iceServers.push({ urls: turnUrl, username, credential });
    }
    return iceServers;
  }

  const rooms = new Map();
  const sessions = new Map();
  const httpServer = createServer((request, response) => {
    if (request.url !== "/healthz") {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
    response.end(JSON.stringify({ ok: true }));
  });
  const webSocketServer = new WebSocketServer({ server: httpServer, maxPayload: MAX_PAYLOAD_BYTES });
  const heartbeat = setInterval(() => {
    for (const socket of webSocketServer.clients) {
      if (socket.isAlive === false) {
        socket.terminate();
        continue;
      }
      socket.isAlive = false;
      socket.ping();
    }
  }, 30_000);
  heartbeat.unref();

  function removeClient(socket) {
    const session = sessions.get(socket);
    if (!session)
      return;
    sessions.delete(socket);
    const room = rooms.get(session.roomCode);
    if (!room)
      return;
    room.players.delete(session.playerId);
    if (room.hostId === session.playerId) {
      for (const peer of room.players.values())
        send(peer.socket, { type: "room-closed" });
      rooms.delete(session.roomCode);
      return;
    }
    const host = room.players.get(room.hostId);
    if (host)
      send(host.socket, { type: "peer-left", playerId: session.playerId });
    if (room.players.size === 0)
      rooms.delete(session.roomCode);
  }

  function assign(socket, roomCode, playerId, role) {
    const room = rooms.get(roomCode);
    if (!room || room.players.has(playerId))
      return false;
    room.players.set(playerId, { socket, role });
    sessions.set(socket, { roomCode, playerId });
    return true;
  }

  webSocketServer.on("connection", (socket) => {
    socket.isAlive = true;
    socket.on("pong", () => { socket.isAlive = true; });
    let tokens = 360;
    let lastRefill = Date.now();
    socket.on("message", (data, isBinary) => {
      if (isBinary || data.length === 0 || data.length > MAX_PAYLOAD_BYTES) {
        send(socket, { type: "error", code: "INVALID_FRAME" });
        return;
      }
      const now = Date.now();
      tokens = Math.min(360, tokens + (now - lastRefill) * 0.006);
      lastRefill = now;
      if (tokens < 1) {
        send(socket, { type: "error", code: "RATE_LIMITED" });
        return;
      }
      tokens -= 1;

      let message;
      try {
        message = JSON.parse(data.toString());
      } catch {
        send(socket, { type: "error", code: "INVALID_JSON" });
        return;
      }
      if (!message || typeof message !== "object" || Array.isArray(message)
        || typeof message.type !== "string") {
        send(socket, { type: "error", code: "INVALID_MESSAGE" });
        return;
      }

      if (message.type === "create") {
        if (sessions.has(socket) || !validPlayerId(message.playerId)) {
          send(socket, { type: "error", code: "INVALID_CREATE" });
          return;
        }
        if (rooms.size >= MAX_ROOMS) {
          send(socket, { type: "error", code: "SERVER_BUSY" });
          return;
        }
        let roomCode;
        do {
          roomCode = randomBytes(8).toString("hex").toUpperCase();
        } while (rooms.has(roomCode));
        rooms.set(roomCode, { hostId: message.playerId, players: new Map() });
        assign(socket, roomCode, message.playerId, "host");
        send(socket, { type: "created", roomCode, hostId: message.playerId, iceServers: issueIceServers(message.playerId) });
        return;
      }

      if (message.type === "join") {
        if (sessions.has(socket) || !validPlayerId(message.playerId)
          || typeof message.roomCode !== "string" || !ROOM_CODE_PATTERN.test(message.roomCode)) {
          send(socket, { type: "error", code: "INVALID_JOIN" });
          return;
        }
        const room = rooms.get(message.roomCode);
        if (!room) {
          send(socket, { type: "error", code: "ROOM_NOT_FOUND" });
          return;
        }
        if (room.players.size >= MAX_PLAYERS) {
          send(socket, { type: "error", code: "ROOM_FULL" });
          return;
        }
        if (room.players.has(message.playerId)) {
          send(socket, { type: "error", code: "DUPLICATE_PLAYER" });
          return;
        }
        assign(socket, message.roomCode, message.playerId, "guest");
        const host = room.players.get(room.hostId);
        send(socket, { type: "joined", hostId: room.hostId, peers: [room.hostId], iceServers: issueIceServers(message.playerId) });
        if (host)
          send(host.socket, { type: "peer-joined", playerId: message.playerId });
        return;
      }

      if (message.type === "signal") {
        const session = sessions.get(socket);
        if (!session || !validPlayerId(message.to) || !SIGNAL_KINDS.has(message.kind)
          || typeof message.payload !== "string" || message.payload.length === 0
          || Buffer.byteLength(message.payload, "utf8") > MAX_PAYLOAD_BYTES - 512) {
          send(socket, { type: "error", code: "INVALID_SIGNAL" });
          return;
        }
        const room = rooms.get(session.roomCode);
        const target = room?.players.get(message.to);
        if (!target || message.to === session.playerId
          || (session.playerId !== room.hostId && message.to !== room.hostId)) {
          send(socket, { type: "error", code: "INVALID_PEER" });
          return;
        }
        send(target.socket, {
          type: "signal",
          from: session.playerId,
          kind: message.kind,
          payload: message.payload,
        });
        return;
      }

      if (message.type === "leave") {
        removeClient(socket);
        send(socket, { type: "left" });
        return;
      }

      send(socket, { type: "error", code: "UNKNOWN_TYPE" });
    });
    socket.on("close", () => removeClient(socket));
    socket.on("error", () => removeClient(socket));
  });

  return {
    listen() {
      return new Promise((resolve, reject) => {
        httpServer.once("error", reject);
        httpServer.listen(port, host, () => {
          httpServer.off("error", reject);
          resolve(httpServer.address());
        });
      });
    },
    close() {
      clearInterval(heartbeat);
      for (const socket of webSocketServer.clients)
        socket.terminate();
      webSocketServer.close();
      return new Promise((resolve) => httpServer.close(() => resolve()));
    },
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const host = process.env.COOP_SIGNAL_HOST ?? "127.0.0.1";
  const port = Number(process.env.COOP_SIGNAL_PORT ?? "8765");
  if (!Number.isInteger(port) || port < 1 || port > 65535)
    throw new Error("COOP_SIGNAL_PORT must be a valid TCP port");
  const server = createSignalingServer({ host, port });
  const address = await server.listen();
  process.stdout.write(`Co-op signaling server listening on ${address.address}:${address.port}\n`);
  const stop = async () => {
    await server.close();
    process.exit(0);
  };
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}
