#!/usr/bin/env node
// Capture the EPD framebuffer from /api/preview/ws as PNG(s).
//
// The firmware publishes a 1-bpp snapshot of every panel after each
// render into /api/preview/ws (binary frames, layout matches
// components/webserver/include/preview_packet.hpp). This tool joins
// the WebSocket, sends "start", collects one frame per panel, and
// writes PNG files — one per panel plus a horizontally-stitched
// `combined.png` that mirrors the physical 8-panel strip on the
// 2.13" boards.
//
// Output replaces the need to point a camera at the device and worry
// about frontlight / glare / focus. The on-device LiveStatus snapshot
// is the same data the renderer just painted to the EPD, so a missing
// digit (e.g. earnings parser regression) shows up here identically.
//
// Requires Node 20+ (built-in WebSocket + zlib). No npm install.

import { createConnection } from "node:net";
import { Buffer } from "node:buffer";
import { deflateSync, inflateSync } from "node:zlib";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

// --- arg parsing -----------------------------------------------------

const DEFAULT_HOST = "btclock-9d5530.local";
const DEFAULT_OUT = "preview-capture";
const DEFAULT_TIMEOUT_S = 15;

function parseArgs(argv) {
  const out = {
    host: DEFAULT_HOST,
    out: DEFAULT_OUT,
    timeout: DEFAULT_TIMEOUT_S,
    user: null,
    pass: null,
    panels: 0,
  };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "-h" || a === "--help") {
      out.help = true;
      continue;
    }
    if (a === "--host") out.host = argv[++i];
    else if (a === "--out") out.out = argv[++i];
    else if (a === "--timeout") out.timeout = Number(argv[++i]);
    else if (a === "--panels") out.panels = Number(argv[++i]);
    else if (a === "--user") out.user = argv[++i];
    else if (a === "--pass") out.pass = argv[++i];
  }
  return out;
}

function usage() {
  console.log(`Usage: capture.mjs [options]

Options:
  --host <hostname>     Device hostname or IP (default: ${DEFAULT_HOST}).
                        Prefer mDNS hostnames over raw IPs (project rule).
  --out <dir>           Output directory (default: ${DEFAULT_OUT}).
  --timeout <seconds>   Give up if no frames arrive (default: ${DEFAULT_TIMEOUT_S}).
  --panels <n>          Exit once N distinct panel indices have been
                        captured. Default 0 = derive from the first
                        frame's seen-set after the timeout.
  --user <name>         HTTP Basic auth username (when httpAuthEnabled).
  --pass <password>     HTTP Basic auth password.

Output:
  <out>/panel-NN.png    One PNG per panel index.
  <out>/combined.png    Horizontal strip of all panels in index order.
  <out>/frame-meta.json Header fields per panel (width, stride, etc.).

Notes:
  Connects without TLS — the WebUI server is plain HTTP. If the firmware
  was built with CONFIG_HTTPD_WS_SUPPORT disabled, /api/preview/ws is a
  no-op and this tool will hit the read timeout with zero frames.`);
}

// --- websocket client (minimal RFC 6455, text + binary, no extensions) ---

class WsClient {
  constructor({ host, port, path, auth }) {
    this.host = host;
    this.port = port;
    this.path = path;
    this.auth = auth;
    this.sock = null;
    this.onbinary = null;
    this.ontext = null;
    this.onclose = null;
    this.buf = Buffer.alloc(0);
    this.handshakeDone = false;
  }

  connect() {
    return new Promise((resolve, reject) => {
      const sock = createConnection({ host: this.host, port: this.port }, () => {
        const key = Buffer.from(crypto.getRandomValues(new Uint8Array(16))).toString(
          "base64"
        );
        const headers = [
          `GET ${this.path} HTTP/1.1`,
          `Host: ${this.host}:${this.port}`,
          "Upgrade: websocket",
          "Connection: Upgrade",
          `Sec-WebSocket-Key: ${key}`,
          "Sec-WebSocket-Version: 13",
        ];
        if (this.auth) {
          headers.push(`Authorization: Basic ${this.auth}`);
        }
        headers.push("", "");
        sock.write(headers.join("\r\n"));
      });
      sock.on("error", (err) => reject(err));
      sock.on("close", () => {
        if (this.onclose) this.onclose();
      });
      sock.on("data", (chunk) => {
        this.buf = Buffer.concat([this.buf, chunk]);
        if (!this.handshakeDone) {
          const end = this.buf.indexOf("\r\n\r\n");
          if (end < 0) return;
          const headerText = this.buf.slice(0, end).toString("utf8");
          this.buf = this.buf.slice(end + 4);
          if (!/^HTTP\/1\.1 101/.test(headerText)) {
            reject(new Error(`handshake failed:\n${headerText}`));
            sock.destroy();
            return;
          }
          this.handshakeDone = true;
          this.sock = sock;
          resolve();
        }
        this._drainFrames();
      });
    });
  }

  _drainFrames() {
    while (this.buf.length >= 2) {
      const b0 = this.buf[0];
      const b1 = this.buf[1];
      const fin = (b0 & 0x80) !== 0;
      const opcode = b0 & 0x0f;
      const masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f;
      let offset = 2;
      if (len === 126) {
        if (this.buf.length < offset + 2) return;
        len = this.buf.readUInt16BE(offset);
        offset += 2;
      } else if (len === 127) {
        if (this.buf.length < offset + 8) return;
        const hi = this.buf.readUInt32BE(offset);
        const lo = this.buf.readUInt32BE(offset + 4);
        len = hi * 2 ** 32 + lo;
        offset += 8;
      }
      let mask;
      if (masked) {
        if (this.buf.length < offset + 4) return;
        mask = this.buf.slice(offset, offset + 4);
        offset += 4;
      }
      if (this.buf.length < offset + len) return;
      let payload = this.buf.slice(offset, offset + len);
      if (masked) {
        payload = Buffer.from(payload);
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      }
      this.buf = this.buf.slice(offset + len);
      if (!fin) {
        // Fragmented frame — firmware sends preview frames as single
        // FIN packets so we don't need reassembly.
        continue;
      }
      if (opcode === 0x8) {
        // close
        this.sock.end();
        return;
      } else if (opcode === 0x9) {
        // ping → pong (no app payload mirroring needed for the firmware)
        this._sendFrame(0xa, Buffer.alloc(0));
      } else if (opcode === 0x1) {
        if (this.ontext) this.ontext(payload.toString("utf8"));
      } else if (opcode === 0x2) {
        if (this.onbinary) this.onbinary(payload);
      }
    }
  }

  _sendFrame(opcode, payload) {
    // RFC 6455 requires client→server frames to be masked.
    const len = payload.length;
    let header;
    if (len < 126) {
      header = Buffer.alloc(2);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | len;
    } else if (len < 0x10000) {
      header = Buffer.alloc(4);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 126;
      header.writeUInt16BE(len, 2);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 127;
      header.writeUInt32BE(0, 2);
      header.writeUInt32BE(len, 6);
    }
    const mask = Buffer.from(crypto.getRandomValues(new Uint8Array(4)));
    const masked = Buffer.from(payload);
    for (let i = 0; i < masked.length; i++) masked[i] ^= mask[i & 3];
    this.sock.write(Buffer.concat([header, mask, masked]));
  }

  sendText(s) {
    this._sendFrame(0x1, Buffer.from(s, "utf8"));
  }

  close() {
    if (this.sock) this.sock.end();
  }
}

// --- preview frame decoder ------------------------------------------

function decodeHeader(buf) {
  if (buf.length < 34) throw new Error(`frame too short: ${buf.length}`);
  const magic = buf.slice(0, 4).toString("ascii");
  if (magic !== "BTFB") throw new Error(`bad magic: ${magic}`);
  return {
    version: buf[4],
    kind: buf[5],
    compression: buf[6],
    bitDepth: buf[7],
    panel: buf[8],
    width: buf.readUInt16LE(10),
    height: buf.readUInt16LE(12),
    stride: buf.readUInt16LE(14),
    rotationDeg: buf.readUInt16LE(16),
    frameId: buf.readUInt32LE(18),
    timestampMs: buf.readUInt32LE(22),
    payloadSize: buf.readUInt32LE(26),
    rawSize: buf.readUInt32LE(30),
  };
}

function decodePayload(buf, hdr) {
  const compressed = buf.slice(34, 34 + hdr.payloadSize);
  if (hdr.compression === 0) return Buffer.from(compressed);
  if (hdr.compression === 1) return inflateSync(compressed);
  throw new Error(`unsupported compression: ${hdr.compression}`);
}

// 1-bpp MSB-first → 8-bit grayscale. EPD convention: bit=0 means ink
// (black, 0x00) and bit=1 means background (white, 0xFF), matching
// the on-device LiveStatus encoder.
function unpack1bppToGray(packed, width, height, stride) {
  const out = Buffer.alloc(width * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const byte = packed[y * stride + (x >> 3)];
      const bit = (byte >> (7 - (x & 7))) & 1;
      out[y * width + x] = bit ? 0xff : 0x00;
    }
  }
  return out;
}

// Apply the per-panel rotation reported in the frame header so the
// PNG renders the same way the viewer sees the physical panel. Rev B
// boards report rotation_deg=180 for every panel; without this step
// the combined image comes out upside down.
function rotateGray(gray, width, height, rotationDeg) {
  const r = ((rotationDeg % 360) + 360) % 360;
  if (r === 0) return { buf: gray, width, height };
  if (r === 180) {
    const total = width * height;
    const out = Buffer.alloc(total);
    for (let i = 0; i < total; i++) out[i] = gray[total - 1 - i];
    return { buf: out, width, height };
  }
  if (r === 90 || r === 270) {
    const out = Buffer.alloc(width * height);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const src = gray[y * width + x];
        const nx = r === 90 ? height - 1 - y : y;
        const ny = r === 90 ? x : width - 1 - x;
        out[ny * height + nx] = src;
      }
    }
    return { buf: out, width: height, height: width };
  }
  throw new Error(`unsupported rotation: ${rotationDeg}`);
}

// --- minimal PNG encoder (grayscale, 8-bit) -------------------------

function crc32(buf) {
  // PNG uses IEEE CRC32; Node's zlib doesn't expose it, but the hash
  // module's algorithm sets match.  Simple table-driven impl is enough
  // for sub-megabyte panels.
  let crc = ~0;
  for (let i = 0; i < buf.length; i++) {
    crc ^= buf[i];
    for (let k = 0; k < 8; k++) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return ~crc >>> 0;
}

function pngChunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, "ascii");
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

function encodePng(gray, width, height) {
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 0; // color type: grayscale
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  // Each scanline is prefixed with a filter byte (0 = None).
  const scanlines = Buffer.alloc((width + 1) * height);
  for (let y = 0; y < height; y++) {
    scanlines[y * (width + 1)] = 0;
    gray.copy(scanlines, y * (width + 1) + 1, y * width, (y + 1) * width);
  }
  const idat = deflateSync(scanlines, { level: 9 });
  return Buffer.concat([
    sig,
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", idat),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
}

// --- main ------------------------------------------------------------

async function main() {
  const args = parseArgs(process.argv);
  if (args.help) {
    usage();
    return;
  }

  if (!existsSync(args.out)) mkdirSync(args.out, { recursive: true });

  const auth =
    args.user != null
      ? Buffer.from(`${args.user}:${args.pass ?? ""}`).toString("base64")
      : null;
  const client = new WsClient({
    host: args.host,
    port: 80,
    path: "/api/preview/ws",
    auth,
  });

  const seen = new Map(); // panel index → {hdr, gray}
  let firstFrameAt = 0;

  client.ontext = (msg) => {
    // Server acks with {"streaming":true|false} — useful only for
    // logging.
    process.stderr.write(`[text] ${msg}\n`);
  };
  client.onbinary = (buf) => {
    try {
      const hdr = decodeHeader(buf);
      const payload = decodePayload(buf, hdr);
      const expected = hdr.stride * hdr.height;
      if (payload.length < expected) {
        process.stderr.write(
          `[warn] panel ${hdr.panel}: payload ${payload.length} < expected ${expected}\n`
        );
        return;
      }
      const raw = unpack1bppToGray(payload, hdr.width, hdr.height, hdr.stride);
      const rot = rotateGray(raw, hdr.width, hdr.height, hdr.rotationDeg);
      seen.set(hdr.panel, {
        hdr,
        gray: rot.buf,
        width: rot.width,
        height: rot.height,
      });
      if (firstFrameAt === 0) firstFrameAt = Date.now();
      process.stderr.write(
        `[panel ${hdr.panel}] ${hdr.width}x${hdr.height} stride=${hdr.stride} ` +
          `frame=${hdr.frameId} comp=${hdr.compression}\n`
      );
    } catch (err) {
      process.stderr.write(`[error] decode: ${err.message}\n`);
    }
  };

  await client.connect();
  client.sendText("start");

  await new Promise((resolve) => {
    const tStart = Date.now();
    const intervalMs = 200;
    const t = setInterval(() => {
      const now = Date.now();
      const overallTimeout = now - tStart > args.timeout * 1000;
      const enoughPanels = args.panels > 0 && seen.size >= args.panels;
      // If we got a first frame, give the rest 1.5s to arrive (panels
      // are pushed one-by-one). Without this we'd stop at panel 0.
      const settleAfterFirst =
        firstFrameAt > 0 && now - firstFrameAt > 1500 && !enoughPanels;
      if (enoughPanels || settleAfterFirst || overallTimeout) {
        clearInterval(t);
        resolve();
      }
    }, intervalMs);
  });

  client.sendText("stop");
  client.close();

  if (seen.size === 0) {
    process.stderr.write("[fatal] no preview frames received\n");
    process.exit(2);
  }

  // Per-panel rotation_deg is applied during decode; the composite
  // strip itself goes in panel-index order. If a future variant
  // physically wires panel 0 on the right, this list would need to
  // reverse — but on Rev B today panel 0 is the leftmost cell.
  const indices = [...seen.keys()].sort((a, b) => a - b);
  const meta = [];
  for (const i of indices) {
    const { hdr, gray, width, height } = seen.get(i);
    const png = encodePng(gray, width, height);
    const fname = join(args.out, `panel-${String(i).padStart(2, "0")}.png`);
    writeFileSync(fname, png);
    meta.push({
      panel: i,
      width,
      height,
      sourceWidth: hdr.width,
      sourceHeight: hdr.height,
      stride: hdr.stride,
      rotationDeg: hdr.rotationDeg,
      frameId: hdr.frameId,
      timestampMs: hdr.timestampMs,
      compression: hdr.compression,
      file: fname,
    });
  }

  // Composite: horizontal strip in display order (rightmost panel
  // index first). Height = tallest panel; shorter ones get padded
  // with white (background).
  const heights = indices.map((i) => seen.get(i).height);
  const widths = indices.map((i) => seen.get(i).width);
  const totalW = widths.reduce((a, b) => a + b, 0);
  const totalH = Math.max(...heights);
  const composite = Buffer.alloc(totalW * totalH, 0xff);
  let xOff = 0;
  for (const i of indices) {
    const { gray, width, height } = seen.get(i);
    for (let y = 0; y < height; y++) {
      gray.copy(composite, y * totalW + xOff, y * width, (y + 1) * width);
    }
    xOff += width;
  }
  writeFileSync(join(args.out, "combined.png"), encodePng(composite, totalW, totalH));
  writeFileSync(
    join(args.out, "frame-meta.json"),
    JSON.stringify({ host: args.host, panels: meta }, null, 2)
  );

  process.stderr.write(
    `Captured ${indices.length} panel(s) → ${args.out}/combined.png\n`
  );
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
