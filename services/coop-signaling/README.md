# Co-op signaling service

This small Node service brokers WebRTC offer, answer, and ICE-candidate messages. Gameplay remains host-authoritative and does not run on this service. Room membership is capped at four player IDs. A 16-character random hexadecimal room code is a bearer invite; keep it private.

The service does not relay gameplay packets. WebRTC DataChannels carry those packets directly when ICE succeeds and through the configured TURN server when direct paths fail. It does not authenticate player accounts or persist rooms. Room state is in memory and disappears when the service restarts. A disconnected guest's player ID and room slot remain reserved until the room closes; the guest can reclaim it only with its random resume token. The service stores only a hash of that token.

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

The health endpoint is `http://127.0.0.1:8765/healthz`; WebSocket clients connect to `ws://127.0.0.1:8765`. A public deployment must terminate TLS and expose `wss://` through a reverse proxy, keep the configured global connection cap and per-socket message rate limit enabled, and keep only short-lived room signaling in memory. Never commit the TURN shared secret or other credentials.

## Linux VPS deployment template

The repository includes a Docker Compose deployment for a Linux VPS with a public IPv4 address and a DNS name that points to it. It runs the signaling service, Caddy for automatic HTTPS/WSS certificates, and coturn for UDP TURN relay. This is a template; no server, DNS name, TLS certificate, or credentials are provisioned by the project.

1. Install Docker Engine with the Compose plugin on the VPS. Create an A record for the chosen domain pointing to the VPS IPv4 and allow DNS to propagate.
2. Copy this service directory to the server. Copy `.env.example` to `.env`, set `COOP_SIGNAL_DOMAIN` and `TURN_EXTERNAL_IP`, and replace `COOP_TURN_SHARED_SECRET` with a random value of at least 32 characters from a secret manager. Keep `.env` private; it is ignored by Git.
3. Permit inbound TCP 80 and 443 for Caddy, UDP 3478 for TURN, and UDP 49160–49200 for TURN relay traffic. The compose file uses host networking for coturn so the configured public address and relay ports match the VPS firewall.
4. Run `docker compose up --build -d` in this directory. Verify `https://<domain>/healthz`, then configure the game's Internet lobby with `wss://<domain>`.
5. Test from two unrelated networks and check both direct ICE and TURN relay paths. A healthy signaling endpoint does not prove TURN reachability. Monitor bandwidth and rotate the secret if it is exposed.

The coturn command deliberately disables TLS/DTLS and uses authenticated UDP TURN only; WSS protects signaling, while WebRTC encrypts DataChannels. Some restrictive networks block UDP. Add a separately tested TURN/TCP or TURN/TLS endpoint before claiming those networks are supported. Caddy and the service should be kept patched; the template currently pins major service images, so operators should choose and regularly update reviewed image digests for production.

## Wire messages

Clients send JSON text frames. The service accepts `create`, `join`, `rejoin`, `signal`, and `leave`. After room creation or join, the client receives its resume token over the signaling socket. Guests send `join`; each guest receives the host ID and only establishes a peer connection to that host. A returning guest sends `rejoin` with the room code, same player ID, and resume token; the host receives `peer-joined` and negotiates a fresh DataChannel. Successful rejoin rotates the token and invalidates the previous one. The host receives `peer-joined` for new and returning guests. `signal` messages contain a target peer ID, one of `offer`, `answer`, or `candidate`, and the SDP/candidate string; the service binds the source ID to the joined WebSocket and forwards it only to that room peer. Guest-to-guest signaling is rejected. Binary frames, malformed IDs, unknown peers, unknown signal kinds, and payloads over 32 KiB are rejected or ignored.

This signaling service is a development component used by the game's optional `COOP_ENABLE_WEBRTC` lobby mode. No public signaling or TURN service is deployed, so its local tests do not establish Internet multiplayer availability.
