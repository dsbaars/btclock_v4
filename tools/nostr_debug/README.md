# nostr_debug / watch_zaps.mjs

Standalone NIP-57 zap watcher that subscribes to a relay with the same
REQ filter the BTClock firmware uses (`kinds:[9735]`, `#p:[recipient]`,
`since:now-15min`, `limit:1` per `components/nostr/src/zap_listener.cpp`).
Use it to tell apart "the relay never delivered the zap" from "the
firmware silently dropped it" — optionally cross-reference against a
running device's `/api/status`.

## Examples

```sh
node watch_zaps.mjs --pubkey b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422
node watch_zaps.mjs --relay wss://relay.damus.io --npub npub1k5f8gzx0xds3p8ygqjsu83q6n7vwqju8phzdhfdu5jsfsc6hzzygsxh3ad
node watch_zaps.mjs --pubkey <hex> --device-url http://btclock-rev-b.local
```

## Requirements

Node 20+ (uses the built-in global `WebSocket`). On older Node versions,
`npm i ws` and replace the implicit `WebSocket` with
`import WebSocket from "ws"` at the top of `watch_zaps.mjs`.

No npm install steps needed on Node 20+.
