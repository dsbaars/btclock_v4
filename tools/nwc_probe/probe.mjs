#!/usr/bin/env node
// Connects to a NWC wallet's relay and prints the advertised encryption
// variants from its NIP-47 info event (kind 13194). Use this before
// pairing to predict which crypto path BTClock's NwcClient will pick:
// nip44_v2 → ChaCha20, otherwise → NIP-04 / AES-256-CBC.

const uri = process.argv[2];
if (!uri) {
  console.error('usage: probe.mjs <nostr+walletconnect://...>');
  process.exit(2);
}

const url = new URL(uri);
const walletPub = url.host || url.pathname.replace(/^\/+/, '');
const relays = url.searchParams.getAll('relay');
if (!walletPub || relays.length === 0) {
  console.error('parse error: need wallet pubkey + at least one relay');
  process.exit(2);
}

console.log(`wallet pubkey: ${walletPub}`);
console.log(`relay:         ${relays[0]}`);

const ws = new WebSocket(relays[0]);
const sub = `probe-${Date.now()}`;
const deadline = setTimeout(() => {
  console.error('timeout: no info event within 10s');
  process.exit(1);
}, 10_000);

ws.addEventListener('open', () => {
  ws.send(JSON.stringify([
    'REQ', sub,
    { kinds: [13194], authors: [walletPub], limit: 1 },
  ]));
});

ws.addEventListener('message', (msg) => {
  let frame;
  try { frame = JSON.parse(msg.data); } catch { return; }
  if (frame[0] === 'EVENT' && frame[1] === sub) {
    const ev = frame[2];
    const encTag = ev.tags.find((t) => t[0] === 'encryption');
    const notifTag = ev.tags.find((t) => t[0] === 'notifications');
    const advertised = encTag ? encTag.slice(1).join(' ').trim() : null;
    const supportsNip44 = advertised && advertised.split(/\s+/).includes('nip44_v2');

    console.log('');
    console.log(`capabilities (content): ${ev.content}`);
    console.log(`encryption tag:         ${advertised ?? '(absent → NIP-04 only per spec)'}`);
    console.log(`notifications tag:      ${notifTag ? notifTag.slice(1).join(' ') : '(absent)'}`);
    console.log('');
    console.log(`BTClock NwcClient picks:`);
    console.log(`  ${supportsNip44 ? 'NIP-44 v2 → ChaCha20 (NOT exercising HW AES path)'
                                    : 'NIP-04   → AES-256-CBC via HW AES peripheral'}`);
    clearTimeout(deadline);
    ws.close();
    process.exit(0);
  }
  if (frame[0] === 'EOSE' && frame[1] === sub) {
    console.error('EOSE: relay returned no info event for this wallet');
    clearTimeout(deadline);
    ws.close();
    process.exit(1);
  }
});

ws.addEventListener('error', (e) => {
  console.error(`ws error: ${e.message ?? e}`);
  process.exit(1);
});
