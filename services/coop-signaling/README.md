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

To use ICE services, configure them only on the signaling server:

```powershell
$env:COOP_STUN_URL = "stun:stun.example.net:3478"
$env:COOP_TURN_URL = "turn:turn.example.net:3478?transport=udp"
$env:COOP_TURN_SHARED_SECRET = "<at-least-32-random-characters-from-your-secret-store>"
$env:COOP_TURN_CREDENTIAL_LIFETIME_SECONDS = "1800"
```

The service returns Coturn REST-style, per-player credentials with an expiry to each room member over the signaling connection. The shared secret stays server-side. Omit TURN settings for direct-connect-only development; restrictive NAT/firewall pairs may then fail. Protect the WSS endpoint and secret in deployment, and rotate the shared secret if exposed.

The health endpoint is `http://127.0.0.1:8765/healthz`; WebSocket clients connect to `ws://127.0.0.1:8765`. A public deployment must terminate TLS and expose `wss://` through a reverse proxy, protect the service with connection/rate limits, and keep only short-lived room signaling in memory. Never commit the TURN shared secret or other credentials.

## Wire messages

Clients send JSON text frames. The service accepts `create`, `join`, `signal`, and `leave`. After a room is created, the host receives `created` with its invite code. Guests send `join`; each guest receives the host ID and only establishes a peer connection to that host. The host receives `peer-joined`. `signal` messages contain a target peer ID, one of `offer`, `answer`, or `candidate`, and the SDP/candidate string; the service binds the source ID to the joined WebSocket and forwards it only to that room peer. Guest-to-guest signaling is rejected. Binary frames, malformed IDs, unknown peers, unknown signal kinds, and payloads over 32 KiB are rejected or ignored.

This signaling service is a development component used by the game's optional `COOP_ENABLE_WEBRTC` lobby mode. No public signaling or TURN service is deployed, so its local tests do not establish Internet multiplayer availability.
