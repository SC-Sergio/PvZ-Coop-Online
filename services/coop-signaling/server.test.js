import assert from "node:assert/strict";
import { after, before, test } from "node:test";
import { WebSocket } from "ws";
import { createSignalingServer } from "./server.js";

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

function connect() {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(endpoint);
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

  send(guest, { type: "join", playerId: "2", roomCode: created.roomCode });
  const joined = await waitForType(guest, "joined");
  assert.deepEqual(joined.peers, ["1"]);
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

  send(guest, { type: "signal", to: "3", kind: "candidate", payload: "candidate:1" });
  assert.deepEqual(await waitForType(guest, "error"), { type: "error", code: "INVALID_PEER" });
  send(host, { type: "signal", to: "99", kind: "candidate", payload: "candidate:1" });
  assert.deepEqual(await waitForType(host, "error"), { type: "error", code: "INVALID_PEER" });

  send(host, { type: "leave" });
  await waitForType(host, "left");
  for (const socket of [guest, guestTwo, guestThree])
    assert.deepEqual(await waitForType(socket, "room-closed"), { type: "room-closed" });
});

test("rejects invalid room IDs, peer IDs, and signal payloads", async (t) => {
  const socket = await connect();
  t.after(() => socket.terminate());

  send(socket, { type: "join", playerId: "0", roomCode: "NOPE" });
  assert.deepEqual(await waitForType(socket, "error"), { type: "error", code: "INVALID_JOIN" });

  send(socket, { type: "create", playerId: "4294967296" });
  assert.deepEqual(await waitForType(socket, "error"), { type: "error", code: "INVALID_CREATE" });
});

test("exposes a no-store health endpoint", async () => {
	const address = new URL(endpoint);
	const response = await fetch(`http://${address.host}/healthz`);
	assert.equal(response.status, 200);
	assert.equal(response.headers.get("cache-control"), "no-store");
	assert.deepEqual(await response.json(), { ok: true });
});
