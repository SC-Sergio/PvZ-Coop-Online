import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { after, before, test } from "node:test";
import { WebSocket } from "ws";
import { createSignalingServer, MAX_SOCKET_BUFFERED_BYTES, sendBounded } from "./server.js";

let server;
let endpoint;
const messageQueues = new WeakMap();

before(async () => {
  server = createSignalingServer({ host: "127.0.0.1", port: 0 });
  const address = await server.listen();
  endpoint = `ws://127.0.0.1:${address.port}`;
});

after(async () => {
  await server.close();
});

function connect(url = endpoint) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url);
    socket.once("open", () => {
      messageQueues.set(socket, []);
      socket.on("message", (data) => {
        messageQueues.get(socket)?.push(JSON.parse(data.toString()));
        deliverQueuedMessages(socket);
      });
      resolve(socket);
    });
    socket.once("error", reject);
  });
}

function nextMessage(socket, timeoutMs = 3000) {
  const queued = messageQueues.get(socket);
  if (queued?.length)
    return Promise.resolve(queued.shift());
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      waiters.delete(onMessage);
      reject(new Error("Timed out waiting for signaling message"));
    }, timeoutMs);
    const onMessage = (message) => {
      clearTimeout(timeout);
      waiters.delete(onMessage);
      resolve(message);
    };
    let waiters = socketWaiters.get(socket);
    if (!waiters) {
      waiters = new Set();
      socketWaiters.set(socket, waiters);
    }
    waiters.add(onMessage);
  });
}

const socketWaiters = new WeakMap();

function deliverQueuedMessages(socket) {
  const queued = messageQueues.get(socket);
  const waiters = socketWaiters.get(socket);
  if (!queued || !waiters)
    return;
  while (queued.length && waiters.size)
    [...waiters][0](queued.shift());
}

function send(socket, message) {
  socket.send(JSON.stringify(message));
}

test("bounds signaling output buffers and closes slow consumers", () => {
  const message = { type: "signal", payload: "bounded" };
  const payloadBytes = Buffer.byteLength(JSON.stringify(message), "utf8");
  let acceptedPayload;
  const healthySocket = {
    readyState: WebSocket.OPEN,
    bufferedAmount: MAX_SOCKET_BUFFERED_BYTES - payloadBytes,
    send(payload) { acceptedPayload = payload; },
    close() { assert.fail("healthy socket must remain open"); },
  };
  assert.equal(sendBounded(healthySocket, message), true);
  assert.equal(acceptedPayload, JSON.stringify(message));

  let attemptedSend = false;
  let closeCall;
  const slowSocket = {
    readyState: WebSocket.OPEN,
    bufferedAmount: MAX_SOCKET_BUFFERED_BYTES - payloadBytes + 1,
    send() { attemptedSend = true; },
    close(code, reason) { closeCall = { code, reason }; },
  };
  assert.equal(sendBounded(slowSocket, message), false);
  assert.equal(attemptedSend, false);
  assert.deepEqual(closeCall, { code: 1013, reason: "slow consumer" });
});

async function waitForType(socket, type) {
  for (let i = 0; i < 8; ++i) {
    const message = await nextMessage(socket);
    if (message.type === type)
      return message;
    if (message.type === "error")
      throw new Error(`Unexpected signaling error: ${message.code}`);
  }
  throw new Error(`Did not receive ${type}`);
}

test("creates private rooms and routes bounded signaling only through the host", async (t) => {
  const host = await connect();
  const guest = await connect();
  const guestTwo = await connect();
  const guestThree = await connect();
  const overflow = await connect();
  t.after(() => {
    for (const socket of [host, guest, guestTwo, guestThree, overflow])
      socket.terminate();
  });

  send(host, { type: "create", playerId: "1" });
  const created = await waitForType(host, "created");
  assert.match(created.roomCode, /^[0-9A-F]{16}$/);
  assert.equal(created.hostId, "1");
  assert.match(created.resumeToken, /^[A-Za-z0-9_-]{43}$/);

  send(guest, { type: "join", playerId: "2", roomCode: created.roomCode });
  const joined = await waitForType(guest, "joined");
  assert.deepEqual(joined.peers, ["1"]);
  assert.match(joined.resumeToken, /^[A-Za-z0-9_-]{43}$/);
  assert.deepEqual(await waitForType(host, "peer-joined"), { type: "peer-joined", playerId: "2" });

  send(guestTwo, { type: "join", playerId: "3", roomCode: created.roomCode });
  await waitForType(guestTwo, "joined");
  assert.deepEqual(await waitForType(host, "peer-joined"), { type: "peer-joined", playerId: "3" });
  send(guestThree, { type: "join", playerId: "4", roomCode: created.roomCode });
  await waitForType(guestThree, "joined");
  assert.deepEqual(await waitForType(host, "peer-joined"), { type: "peer-joined", playerId: "4" });

  send(overflow, { type: "join", playerId: "5", roomCode: created.roomCode });
  assert.deepEqual(await waitForType(overflow, "error"), { type: "error", code: "ROOM_FULL" });

  const offer = "v=0\r\no=peer 1 1 IN IP4 0.0.0.0";
  send(host, { type: "signal", to: "2", kind: "offer", payload: offer });
  assert.deepEqual(await waitForType(guest, "signal"), {
    type: "signal", from: "1", kind: "offer", payload: offer,
  });

  send(host, { type: "signal", to: "2", kind: "offer", payload: "x".repeat(32_600) });
  assert.deepEqual(await waitForType(host, "error"), { type: "error", code: "INVALID_SIGNAL" },
    "the signaling relay rejects descriptions that exceed its bounded forwarding envelope");

  send(guest, { type: "signal", to: "3", kind: "candidate", payload: "candidate:1" });
  assert.deepEqual(await waitForType(guest, "error"), { type: "error", code: "INVALID_PEER" });
  send(host, { type: "signal", to: "99", kind: "candidate", payload: "candidate:1" });
  assert.deepEqual(await waitForType(host, "error"), { type: "error", code: "INVALID_PEER" });

  const peerClosed = [guest, guestTwo, guestThree].map((socket) =>
    new Promise((resolve) => socket.once("close", (code) => resolve(code))));
  send(host, { type: "leave" });
  await waitForType(host, "left");
  for (const socket of [guest, guestTwo, guestThree])
    assert.deepEqual(await waitForType(socket, "room-closed"), { type: "room-closed" });
  assert.deepEqual(await Promise.all(peerClosed), [1000, 1000, 1000],
    "host leave closes peer sockets so abandoned clients do not consume server capacity");
});

test("closes guest sockets when the host connection is lost", async (t) => {
  const host = await connect();
  const guest = await connect();
  t.after(() => {
    host.terminate();
    guest.terminate();
  });

  send(host, { type: "create", playerId: "11" });
  const created = await waitForType(host, "created");
  send(guest, { type: "join", playerId: "12", roomCode: created.roomCode });
  await waitForType(guest, "joined");
  await waitForType(host, "peer-joined");

  const guestClosed = new Promise((resolve) => guest.once("close", (code, reason) => resolve({
    code,
    reason: reason.toString(),
  })));
  host.terminate();
  assert.deepEqual(await waitForType(guest, "room-closed"), { type: "room-closed" });
  assert.deepEqual(await guestClosed, { code: 1000, reason: "room closed" });
});

test("reserves a disconnected player slot and permits only token-authenticated rejoin", async (t) => {
  const host = await connect();
  const guest = await connect();
  const attacker = await connect();
  const rejoinedSocket = await connect();
  t.after(() => {
    for (const socket of [host, guest, attacker, rejoinedSocket])
      socket.terminate();
  });

  send(host, { type: "create", playerId: "301" });
  const created = await waitForType(host, "created");
  send(guest, { type: "join", playerId: "302", roomCode: created.roomCode });
  const joined = await waitForType(guest, "joined");
  await waitForType(host, "peer-joined");

  const guestClosed = new Promise((resolve) => guest.once("close", resolve));
  guest.close();
  await guestClosed;
  assert.deepEqual(await waitForType(host, "peer-left"), { type: "peer-left", playerId: "302" });

  send(attacker, { type: "rejoin", playerId: "302", roomCode: created.roomCode, resumeToken: "A".repeat(43) });
  assert.deepEqual(await waitForType(attacker, "error"), { type: "error", code: "REJOIN_REJECTED" });
  send(rejoinedSocket, { type: "rejoin", playerId: "302", roomCode: created.roomCode, resumeToken: joined.resumeToken });
  const resumed = await waitForType(rejoinedSocket, "rejoined");
  assert.equal(resumed.hostId, "301");
  assert.deepEqual(resumed.peers, ["301"]);
  assert.match(resumed.resumeToken, /^[A-Za-z0-9_-]{43}$/);
  assert.notEqual(resumed.resumeToken, joined.resumeToken);
  assert.deepEqual(await waitForType(host, "peer-joined"), { type: "peer-joined", playerId: "302" });

  send(attacker, { type: "rejoin", playerId: "302", roomCode: created.roomCode, resumeToken: joined.resumeToken });
  assert.deepEqual(await waitForType(attacker, "error"), { type: "error", code: "REJOIN_REJECTED" });
});

test("rejects invalid room IDs, peer IDs, and signal payloads", async (t) => {
  const socket = await connect();
  t.after(() => socket.terminate());

  send(socket, { type: "join", playerId: "0", roomCode: "NOPE" });
  assert.deepEqual(await waitForType(socket, "error"), { type: "error", code: "INVALID_JOIN" });

  send(socket, { type: "create", playerId: "4294967296" });
  assert.deepEqual(await waitForType(socket, "error"), { type: "error", code: "INVALID_CREATE" });
});

test("issues per-player short-lived TURN credentials without disclosing the shared secret", async (t) => {
  const sharedSecret = "only-a-test-secret-value-longer-than-32-characters";
  assert.throws(() => createSignalingServer({
    turnUrl: "turn:turn.example.test:3478?transport=udp",
    turnSharedSecret: "short",
  }), /at least 32 characters/);
  const turnServer = createSignalingServer({
    host: "127.0.0.1",
    port: 0,
    stunUrl: "stun:stun.example.test:3478",
    turnUrl: "turn:turn.example.test:3478?transport=udp",
    turnSharedSecret: sharedSecret,
    turnCredentialLifetimeSeconds: 300,
  });
  const address = await turnServer.listen();
  const turnEndpoint = `ws://127.0.0.1:${address.port}`;
  const host = await connect(turnEndpoint);
  const guest = await connect(turnEndpoint);
  t.after(async () => {
    host.terminate();
    guest.terminate();
    await turnServer.close();
  });

  send(host, { type: "create", playerId: "101" });
  const created = await waitForType(host, "created");
  send(guest, { type: "join", playerId: "202", roomCode: created.roomCode });
  const joined = await waitForType(guest, "joined");
  const hostTurn = created.iceServers.find((server) => server.urls.startsWith("turn:"));
  const guestTurn = joined.iceServers.find((server) => server.urls.startsWith("turn:"));
  assert.ok(hostTurn);
  assert.ok(guestTurn);
  assert.match(hostTurn.username, /^\d+:101$/);
  assert.match(guestTurn.username, /^\d+:202$/);
  assert.notEqual(hostTurn.username, guestTurn.username);
  assert.equal(hostTurn.urls, "turn:turn.example.test:3478?transport=udp");
  const expectedPassword = createHmac("sha1", sharedSecret).update(hostTurn.username).digest("base64");
  assert.equal(hostTurn.credential, expectedPassword);
  assert.ok(Number(hostTurn.username.split(":")[0]) <= Math.floor(Date.now() / 1000) + 300);
  assert.ok(!JSON.stringify([created, joined]).includes(sharedSecret));
  assert.ok(created.iceServers.some((server) => server.urls.startsWith("stun:")));
});

test("exposes a no-store health endpoint", async () => {
	const address = new URL(endpoint);
	const response = await fetch(`http://${address.host}/healthz`);
	assert.equal(response.status, 200);
	assert.equal(response.headers.get("cache-control"), "no-store");
	assert.deepEqual(await response.json(), { ok: true });
});

test("bounds active WebSocket connections and rejects invalid limits", async (t) => {
  assert.throws(() => createSignalingServer({ maxConnections: 0 }), /between 1 and 16384/);
  assert.throws(() => createSignalingServer({ maxConnections: 1.5 }), /between 1 and 16384/);
  const limitedServer = createSignalingServer({ host: "127.0.0.1", port: 0, maxConnections: 1 });
  const address = await limitedServer.listen();
  const limitedEndpoint = `ws://127.0.0.1:${address.port}`;
  const first = await connect(limitedEndpoint);
  const second = await connect(limitedEndpoint);
  t.after(async () => {
    first.terminate();
    second.terminate();
    await limitedServer.close();
  });

  assert.deepEqual(await waitForType(second, "error"), { type: "error", code: "SERVER_BUSY" });
  const close = await new Promise((resolve) => second.once("close", (code, reason) => resolve({
    code,
    reason: reason.toString(),
  })));
  assert.equal(close.code, 1013);
  assert.equal(close.reason, "server busy");
});
