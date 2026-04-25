#!/usr/bin/env node
// Standalone NIP-57 zap watcher that mirrors the firmware's REQ filter
// (kinds:[9735], #p:[recipient], since:now-15min, limit:1) so we can
// distinguish "relay never delivered" from "firmware dropped on its
// floor". Optional --device-url cross-references against /api/status.

const DEFAULT_RELAY = "wss://relay.primal.net";
const DEFAULT_PUBKEY = "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422";
const ZAP_MAX_AGE_S = 15 * 60;
const RECONNECT_CAP_MS = 30_000;
const HEARTBEAT_MS = 60_000;
const DEVICE_POLL_MS = 10_000;

// --- arg parsing -----------------------------------------------------

function parseArgs(argv) {
  const out = {};
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "-h" || a === "--help") { out.help = true; continue; }
    if (a.startsWith("--")) {
      const k = a.slice(2);
      const v = argv[i + 1];
      out[k] = v;
      i++;
    }
  }
  return out;
}

function usage() {
  console.log(`Usage: watch_zaps.mjs [options]

Options:
  --relay <wss://...>        Default: ${DEFAULT_RELAY}
  --npub <npub1...>          Recipient npub (decoded to hex)
  --pubkey <hex>             Recipient pubkey hex (64 chars)
  --since <unix-ts>          Default: now - 15 min (firmware default)
  --limit <n>                Default: 1 (firmware default)
  --device-url <http://...>  Cross-reference with device /api/status

Env fallbacks: NOSTR_RELAY, NOSTR_PUBKEY, NOSTR_NPUB, BTCLOCK_URL.`);
}

// --- bech32 npub decode (NIP-19) ------------------------------------

const BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

function bech32Decode(s) {
  s = s.toLowerCase();
  const sep = s.lastIndexOf("1");
  if (sep < 1 || sep + 7 > s.length) throw new Error("bad bech32");
  const data = [];
  for (let i = sep + 1; i < s.length; i++) {
    const c = BECH32_CHARSET.indexOf(s[i]);
    if (c === -1) throw new Error("bad bech32 char");
    data.push(c);
  }
  return { hrp: s.slice(0, sep), data: data.slice(0, -6) };
}

function fromWords(words) {
  let acc = 0, bits = 0;
  const out = [];
  for (const w of words) {
    acc = (acc << 5) | w;
    bits += 5;
    while (bits >= 8) {
      bits -= 8;
      out.push((acc >> bits) & 0xff);
    }
  }
  return Uint8Array.from(out);
}

function npubToHex(npub) {
  const { hrp, data } = bech32Decode(npub);
  if (hrp !== "npub") throw new Error(`expected npub hrp, got ${hrp}`);
  const bytes = fromWords(data);
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// --- bolt11 amount decode -------------------------------------------

function bolt11AmountSats(invoice) {
  if (!invoice || typeof invoice !== "string") return null;
  const m = invoice.toLowerCase().match(/^lnbc(\d+)([munp]?)1/);
  if (!m) return null;
  const n = BigInt(m[1]);
  const mult = { m: 100000n, u: 100n, n: 1n / 10n, p: 1n / 10000n };
  if (!m[2]) return Number(n * 100000000n);
  if (m[2] === "m") return Number(n * 100000n);
  if (m[2] === "u") return Number(n * 100n);
  if (m[2] === "n") return Number(n) / 10;
  if (m[2] === "p") return Number(n) / 10000;
  return null;
}

// --- helpers --------------------------------------------------------

function ts() { return new Date().toISOString(); }

function shortId(id) { return id ? id.slice(0, 12) : "?"; }

function pickTag(ev, name) {
  for (const t of ev.tags || []) {
    if (Array.isArray(t) && t[0] === name) return t[1];
  }
  return null;
}

function zapAmountSats(ev) {
  const amt = pickTag(ev, "amount");
  if (amt && /^\d+$/.test(amt)) return Math.floor(Number(amt) / 1000);
  const sats = bolt11AmountSats(pickTag(ev, "bolt11"));
  return sats == null ? null : Math.floor(sats);
}

// --- main -----------------------------------------------------------

const args = parseArgs(process.argv);
if (args.help) { usage(); process.exit(0); }

const relay = args.relay || process.env.NOSTR_RELAY || DEFAULT_RELAY;
let pubkey = args.pubkey || process.env.NOSTR_PUBKEY;
const npubArg = args.npub || process.env.NOSTR_NPUB;
if (!pubkey && npubArg) {
  try { pubkey = npubToHex(npubArg); }
  catch (e) {
    console.error(`[${ts()}] npub decode failed: ${e.message}`);
    process.exit(2);
  }
}
if (!pubkey) pubkey = DEFAULT_PUBKEY;
if (!/^[0-9a-f]{64}$/i.test(pubkey)) {
  console.error(`[${ts()}] pubkey must be 64 hex chars; got "${pubkey}"`);
  process.exit(2);
}
const since = args.since != null
  ? Number(args.since)
  : Math.floor(Date.now() / 1000) - ZAP_MAX_AGE_S;
const limit = args.limit != null ? Number(args.limit) : 1;
const deviceUrl = args["device-url"] || process.env.BTCLOCK_URL || null;

const subId = "zap";
const filter = { kinds: [9735], "#p": [pubkey], since, limit };
const reqFrame = JSON.stringify(["REQ", subId, filter]);

console.log(`[${ts()}] watch_zaps starting`);
console.log(`[${ts()}]   relay     = ${relay}`);
console.log(`[${ts()}]   recipient = ${pubkey}`);
console.log(`[${ts()}]   since     = ${since} (${new Date(since * 1000).toISOString()})`);
console.log(`[${ts()}]   limit     = ${limit}`);
if (deviceUrl) console.log(`[${ts()}]   device    = ${deviceUrl}`);
console.log(`[${ts()}] REQ frame   = ${reqFrame}`);

const seen = new Map();        // event id -> { sats, created_at, device_caught }
let eventCount = 0;
let backoffMs = 1_000;
let ws = null;
let stopping = false;

function logEvent(ev) {
  if (seen.has(ev.id)) return;
  const sats = zapAmountSats(ev);
  const zapper = pickTag(ev, "P");
  const recipient = pickTag(ev, "p");
  const content = (ev.content || "").replaceAll("\n", " ").slice(0, 60);
  seen.set(ev.id, { sats, created_at: ev.created_at, device_caught: false });
  eventCount++;
  console.log(
    `[${ts()}] EVENT id=${shortId(ev.id)} kind=${ev.kind} ` +
    `sats=${sats ?? "?"} p=${shortId(recipient)} P=${shortId(zapper)} ` +
    `created_at=${ev.created_at} content="${content}"`,
  );
}

function connect() {
  if (stopping) return;
  console.log(`[${ts()}] connecting to ${relay}`);
  try {
    ws = new WebSocket(relay);
  } catch (e) {
    console.error(`[${ts()}] WebSocket ctor failed: ${e.message}`);
    scheduleReconnect();
    return;
  }

  ws.addEventListener("open", () => {
    console.log(`[${ts()}] connected; sending REQ`);
    backoffMs = 1_000;
    ws.send(reqFrame);
  });

  ws.addEventListener("message", (evt) => {
    const raw = typeof evt.data === "string" ? evt.data : evt.data.toString();
    let env;
    try { env = JSON.parse(raw); }
    catch { console.error(`[${ts()}] bad JSON frame: ${raw.slice(0, 120)}`); return; }
    if (!Array.isArray(env) || env.length === 0) return;
    const t = env[0];
    if (t === "EVENT" && env[1] === subId && env[2]) {
      logEvent(env[2]);
    } else if (t === "EOSE") {
      console.log(`[${ts()}] EOSE sub=${env[1]}`);
    } else if (t === "CLOSED") {
      console.log(`[${ts()}] CLOSED sub=${env[1]} msg=${env[2] ?? ""}`);
    } else if (t === "NOTICE") {
      console.log(`[${ts()}] NOTICE ${env[1]}`);
    }
  });

  ws.addEventListener("close", (evt) => {
    console.log(`[${ts()}] disconnected code=${evt.code} reason="${evt.reason || ""}"`);
    if (!stopping) scheduleReconnect();
  });

  ws.addEventListener("error", (evt) => {
    console.error(`[${ts()}] ws error: ${evt.message || "(unknown)"}`);
  });
}

function scheduleReconnect() {
  const delay = backoffMs;
  backoffMs = Math.min(backoffMs * 2, RECONNECT_CAP_MS);
  console.log(`[${ts()}] reconnecting in ${delay} ms`);
  setTimeout(connect, delay);
}

setInterval(() => {
  if (!stopping) console.log(`[${ts()}] heartbeat: still subscribed, ${eventCount} event(s) seen`);
}, HEARTBEAT_MS).unref();

// --- device cross-reference -----------------------------------------

let lastSeenZapAmount = null;

async function pollDevice() {
  if (stopping || !deviceUrl) return;
  try {
    const res = await fetch(`${deviceUrl.replace(/\/$/, "")}/api/status`, {
      signal: AbortSignal.timeout(5_000),
    });
    if (!res.ok) {
      console.log(`[${ts()}] device poll: HTTP ${res.status}`);
      return;
    }
    const j = await res.json();
    const data = Array.isArray(j.data) ? j.data : [];
    const flat = data.join(" ");
    // Firmware renders zaps as a "ZAP" label cell + amount digits split
    // across panels; detect by the "ZAP" label sentinel in panel_texts.
    const isZapScreen = flat.includes("ZAP");
    if (isZapScreen) {
      const digits = flat.replace(/[^0-9]/g, "");
      const amt = digits ? Number(digits) : null;
      if (amt !== null && amt !== lastSeenZapAmount) {
        lastSeenZapAmount = amt;
        // Match by amount to most-recent unmatched relay event.
        let matched = null;
        for (const [id, info] of seen) {
          if (!info.device_caught && info.sats === amt) { matched = id; break; }
        }
        if (matched) {
          seen.get(matched).device_caught = true;
          console.log(`[${ts()}] device caught zap id=${shortId(matched)} sats=${amt}`);
        } else {
          console.log(`[${ts()}] device shows zap sats=${amt} (no relay match yet)`);
        }
      }
    }
  } catch (e) {
    console.log(`[${ts()}] device poll error: ${e.message}`);
  }
}

if (deviceUrl) setInterval(pollDevice, DEVICE_POLL_MS).unref();

// --- shutdown -------------------------------------------------------

function shutdown() {
  if (stopping) return;
  stopping = true;
  console.log(`[${ts()}] shutting down`);
  try { ws?.close(); } catch {}
  const total = seen.size;
  const caught = [...seen.values()].filter((v) => v.device_caught).length;
  const missed = [...seen.entries()].filter(([, v]) => !v.device_caught);
  console.log("");
  console.log(`==== summary ====`);
  console.log(`relay events:  ${total}`);
  if (deviceUrl) {
    console.log(`device caught: ${caught}`);
    console.log(`missed:        ${missed.length}`);
    for (const [id, v] of missed) {
      console.log(`  - ${id} sats=${v.sats ?? "?"} created_at=${v.created_at}`);
    }
  } else {
    console.log(`(no device-url; cross-reference skipped)`);
  }
  process.exit(0);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
process.on("unhandledRejection", (r) => {
  console.error(`[${ts()}] unhandledRejection: ${r}`);
});

connect();
