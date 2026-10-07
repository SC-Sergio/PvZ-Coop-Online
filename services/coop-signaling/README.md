# Co-op signaling service

This small Node service brokers WebRTC offer, answer, and ICE-candidate messages. Gameplay remains host-authoritative and does not run on this service. Room membership is capped at four player IDs. A 16-character random hexadecimal room code is a bearer invite; keep it private.

The service does not relay gameplay packets. WebRTC DataChannels carry those packets directly when ICE succeeds and through the configured TURN server when direct paths fail. It also does not authenticate player accounts or persist rooms. Room state is in memory and disappears when the service restarts.

## Run locally

```powershell
npm ci
npm test
$env:COOP_SIGNAL_HOST = "127.0.0.1"
$env:COOP_SIGNAL_PORT = "8765"
npm start
```

The health endpoint is `http://127.0.0.1:8765/healthz`; WebSocket clients connect to `ws://127.0.0.1:8765`. A public deployment must terminate TLS and expose `wss://` through a reverse proxy, protect the service with connection/rate limits, and keep only short-lived room signaling in memory. Do not put TURN passwords or cloud credentials in this repository.

## Wire messages

Clients send JSON text frames. The service accepts `create`, `join`, `signal`, and `leave`. After a room is created, the host receives `created` with its invite code. Guests send `join`; each guest receives the host ID and only establishes a peer connection to that host. The host receives `peer-joined`. `signal` messages contain a target peer ID, one of `offer`, `answer`, or `candidate`, and the SDP/candidate string; the service binds the source ID to the joined WebSocket and forwards it only to that room peer. Guest-to-guest signaling is rejected. Binary frames, malformed IDs, unknown peers, unknown signal kinds, and payloads over 32 KiB are rejected or ignored.

This signaling service is a development component used by the game's optional `COOP_ENABLE_WEBRTC` lobby mode. No public signaling or TURN service is deployed, so its local tests do not establish Internet multiplayer availability.
