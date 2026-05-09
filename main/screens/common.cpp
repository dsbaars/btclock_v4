#include "screens/common.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace btclock {

namespace {

// Ref-char set for label text — uppercase + digits so punctuation-free
// labels share the same baseline regardless of which letters they
// contain. Matches the literal passed at every `DrawSplitText` call
// site in the data screens before the DRY refactor.
constexpr const char* kLabelRef = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Historical pixel-height baselines, hoisted here so the per-screen
// renderers can't drift independently. A screen that genuinely needs a
// different metric should introduce a new PaintSlot::Kind rather than
// plumb an override through — the family of kinds is small enough that
// one-kind-per-size is clearer than one parameterised kind.
constexpr float kLabelPx = 54.0f;
// Default digit pixel height. Lives here only as the seed value for the
// runtime-mutable g_digit_px below; ScreenManager::Render overwrites it
// each frame with the user's `digitFontPx` setting via SetGlobalDigitPx.
constexpr float kDigitPxDefault = 180.0f;
float g_digit_px = kDigitPxDefault;
constexpr float kSmallGroupPx = 90.0f;
// The Satoshi Symbol font fills its em-box (ink width ≈ em-width), so
// rendering it at the digit pixel-height would leave much less visual
// margin around the glyph than Antonio digits get. 130 keeps the glyph
// vertically matched to digit ink while shrinking horizontal ink so
// symmetric panel centering yields margins close to digit panels.
constexpr float kSatsGlyphPx = 130.0f;
// Unit-row text is sized like a label so the trailing "sat/vB"-style
// panel reads as a caption under the main digits.
constexpr float kUnitPx = 54.0f;

// Split "TOP/BOTTOM" on the first '/'. Handles "TOP" (no slash) by
// returning ("TOP", ""); the caller can then treat an empty bottom as
// "draw as single-line label" if needed. `PaintSlotIntoFb` instead
// always calls DrawSplitText, which renders an empty bottom string as
// blank — visually equivalent on the typical "TOP/BOTTOM" inputs.
void SplitOnSlash(const std::string& s, std::string& top, std::string& bottom) {
  const auto pos = s.find('/');
  if (pos == std::string::npos) {
    top = s;
    bottom.clear();
    return;
  }
  top = s.substr(0, pos);
  bottom = s.substr(pos + 1);
}

// Paint a 1-bpp MSB-first bitmap centred on the landscape framebuffer.
// Matches the semantics of the old firmware's `display.drawInvertedBitmap`
// (and the PaintInvertedBitmap / PaintBitaxeLogo helpers that lived in
// mining_pool.cpp / bitaxe.cpp before the DRY refactor): a 0 bit in the
// source = ink (black), a 1 bit = background (no draw). Caller is
// expected to ClearFb() first — PaintDataScreen's paint phase does this
// before calling PaintSlotIntoFb.
//
// Out-of-bounds pixels are silently clipped — bitmaps larger than the
// panel would be drawn partially, which is correct for a letterboxed
// layout and avoids a defensive assert on device.
void PaintInvertedBitmap(LandscapeFb& lfb, const std::uint8_t* bitmap,
                         int bmp_w, int bmp_h) {
  const int panel_w = LogicalWidth(lfb);
  const int panel_h = LogicalHeight(lfb);
  const int x_off = (panel_w - bmp_w) / 2;
  const int y_off = (panel_h - bmp_h) / 2;
  const int stride = (bmp_w + 7) / 8;
  for (int py = 0; py < bmp_h; ++py) {
    const std::uint8_t* row = bitmap + py * stride;
    for (int px = 0; px < bmp_w; ++px) {
      const std::uint8_t byte = row[px >> 3];
      const std::uint8_t bit = static_cast<std::uint8_t>(1U << (7 - (px & 7)));
      if ((byte & bit) == 0) {
        SetPixelLandscape(lfb, x_off + px, y_off + py, /*white=*/false);
      }
    }
  }
}

}  // namespace

void SetGlobalDigitPx(float px) {
  // Clamp to the schema bounds so a stale NVS slot or out-of-band caller
  // can't push a value that overflows the panel or paints a 0 px digit.
  // Mirrors the explicit 80..220 range in settings/schema.hpp.
  if (!(px >= 80.0f)) px = 80.0f;
  if (px > 220.0f) px = 220.0f;
  g_digit_px = px;
}

float GetGlobalDigitPx() {
  return g_digit_px;
}

void PaintSlotIntoFb(LandscapeFb& lfb, const AppFonts& fonts,
                     const PaintSlot& slot, bool vertical_desc) {
  // Label slots honour `vertical_desc` by rotating 90° CCW relative to
  // the caller's orientation (ClearFb used the caller's orientation too —
  // that's safe because white-fill is rotation-agnostic). For panels
  // already at k180 (REV_B solder orientation) the effective native-
  // relative rotation becomes k90Cw: 180° then rotate another 90° CCW
  // lands at 90° CW from native, which is what v3's splitText
  // verticalDesc branch produced via setRotation(1).
  //
  // Swapping the rotation also swaps logical width/height — after the
  // flip the label sees a 250×122 region instead of 122×250, so the
  // text's long axis follows the panel's physical long axis.
  const bool rotate_label =
      vertical_desc &&
      (slot.kind == PaintSlot::kLabel || slot.kind == PaintSlot::kLabelSplit);
  if (rotate_label) lfb.rotation = Rotation::k90Cw;
  const int w = LogicalWidth(lfb);
  const int h = LogicalHeight(lfb);
  // Per-kind defaults for ref-chars and pixel-height are picked below;
  // the slot's `ref_override` / `pixel_height_override` replace those
  // defaults when non-default (used by fee_rate's focused "sat/vB" ref
  // and the bitaxe tails' 160 px font).
  const auto px = [&](float default_px) {
    return slot.pixel_height_override > 0.0f ? slot.pixel_height_override
                                             : default_px;
  };
  switch (slot.kind) {
    case PaintSlot::kBlank:
      // Caller has already ClearFb'd; nothing more to paint.
      return;
    case PaintSlot::kLabelSplit: {
      std::string top, bottom;
      SplitOnSlash(slot.text, top, bottom);
      const char* ref = slot.ref_override ? slot.ref_override : kLabelRef;
      // Keep label-fit percentage tied to physical panel width even when
      // vertical_desc rotates the logical axes for label slots.
      const int split_fit_w = rotate_label ? lfb.native_width : w;
      DrawSplitText(lfb, w, h, top.c_str(), bottom.c_str(), ref, fonts.label(),
                    px(kLabelPx), /*white_text=*/false, split_fit_w);
      return;
    }
    case PaintSlot::kLabel:
      if (slot.text.empty()) return;
      DrawTextCentered(lfb, w, h, slot.text.c_str(),
                       slot.ref_override ? slot.ref_override : kLabelRef,
                       fonts.label(), px(kLabelPx), /*white_text=*/false);
      return;
    case PaintSlot::kDigit:
      // Blanks are legitimate (leading-pad cells); skip paint so the
      // cleared framebuffer is the whole story.
      if (slot.text.empty() || slot.text == " ") return;
      DrawTextCentered(lfb, w, h, slot.text.c_str(),
                       slot.ref_override ? slot.ref_override : kDigitRef,
                       fonts.digit(), px(g_digit_px), /*white_text=*/false);
      return;
    case PaintSlot::kSmallGroup:
      if (slot.text.empty() || slot.text == " ") return;
      DrawTextCentered(lfb, w, h, slot.text.c_str(),
                       slot.ref_override ? slot.ref_override : kDigitRef,
                       fonts.small_chars(), px(kSmallGroupPx),
                       /*white_text=*/false);
      return;
    case PaintSlot::kSatsGlyph:
      if (slot.text.empty()) return;
      // Scale the sats glyph proportionally to the live digit pixel height
      // so it stays visually weight-matched as `digitFontPx` moves. The
      // historical 130 px baseline pairs with the historical 180 px digit;
      // ratio that against g_digit_px so the glyph grows/shrinks 1:1 with
      // the digits next to it.
      DrawTextCentered(lfb, w, h, slot.text.c_str(), slot.text.c_str(),
                       fonts.sats_glyph(),
                       px(kSatsGlyphPx * (g_digit_px / kDigitPxDefault)),
                       /*white_text=*/false);
      return;
    case PaintSlot::kCurrencyGlyph: {
      if (slot.text.empty()) return;
      // Single-codepoint glyphs ($, €, £, ¥) render at the digit
      // pixel-height so the symbol weight-matches the price digits next
      // to it. ISO-code fallbacks (e.g. "ZAR", "BRL", "INR" — codes that
      // /api/v2/currencies includes but the firmware has no dedicated
      // glyph for) would overflow the panel at digit size; switch to the
      // label font + label pixel-height instead, same shape and size as
      // the Nostr-zap "ZAP" cell so the multi-letter code reads as a
      // caption rather than a clipped digit.
      std::size_t cp_count = 0;
      for (char c : slot.text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++cp_count;
      }
      if (cp_count > 1) {
        DrawTextCentered(lfb, w, h, slot.text.c_str(),
                         slot.ref_override ? slot.ref_override : kLabelRef,
                         fonts.label(), kLabelPx, /*white_text=*/false);
      } else {
        DrawTextCentered(lfb, w, h, slot.text.c_str(),
                         slot.ref_override ? slot.ref_override : kDigitRef,
                         fonts.digit(), px(g_digit_px), /*white_text=*/false);
      }
      return;
    }
    case PaintSlot::kUnitSplit: {
      std::string top, bottom;
      SplitOnSlash(slot.text, top, bottom);
      const char* ref = slot.ref_override ? slot.ref_override : kLabelRef;
      DrawSplitText(lfb, w, h, top.c_str(), bottom.c_str(), ref, fonts.unit(),
                    px(kUnitPx), /*white_text=*/false);
      return;
    }
    case PaintSlot::kIconBitmap:
      // 1-bpp MSB-first bitmap centred on the panel. Pool logos and the
      // bitaxe logo both vend their assets in this format — see
      // screens/assets/pool_logos and screens/assets/bitaxe_logo. A
      // null bitmap or zero extent is a no-op (the ClearFb in the
      // caller leaves the panel white).
      if (!slot.bitmap || slot.bmp_w <= 0 || slot.bmp_h <= 0) return;
      PaintInvertedBitmap(lfb, slot.bitmap, slot.bmp_w, slot.bmp_h);
      return;
    case PaintSlot::kMdiIcon:
      // MDI codepoint via the icon font role. DrawCodepointCentered
      // sizes off the glyph's own bbox — kDigitRef would compute a
      // zero ref box for PUA codepoints. 130 px matches kSatsGlyph so
      // an icon paired with the sats glyph visually weight-matches.
      if (slot.mdi_codepoint == 0) return;
      DrawCodepointCentered(lfb, w, h, slot.mdi_codepoint, fonts.icon(),
                            px(kSatsGlyphPx), /*white_text=*/false);
      return;
  }
}

void FormatDigits(uint32_t h, char* digits, size_t slots) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(h));
  const size_t len = std::strlen(buf);
  const char* src = buf;
  size_t pad = 0;
  if (len > slots) {
    src = buf + (len - slots);
  } else {
    pad = slots - len;
  }
  for (size_t i = 0; i < slots; ++i) {
    digits[i] = (i < pad) ? ' ' : src[i - pad];
  }
}

int32_t SatsPerUnit(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p <= 0.0 || endp == price_str.c_str()) return -1;
  const double sats = 1e8 / p;
  if (sats > 4e9) return -1;
  return static_cast<int32_t>(sats + 0.5);
}

int32_t PriceInt(const std::string& price_str) {
  if (price_str.empty()) return -1;
  char* endp = nullptr;
  const double p = std::strtod(price_str.c_str(), &endp);
  if (p < 0.0 || endp == price_str.c_str()) return -1;
  if (p > 2e9) return -1;
  return static_cast<int32_t>(p + 0.5);
}

std::string CurrencySymbolUtf8(const std::string& ccy) {
  if (ccy == "USD") return "$";
  if (ccy == "EUR") return "\xE2\x82\xAC";  // U+20AC
  if (ccy == "GBP") return "\xC2\xA3";      // U+00A3
  if (ccy == "JPY") return "\xC2\xA5";      // U+00A5
  // CAD / AUD share the dollar glyph. CHF has no single-char Unicode
  // symbol — and so do every code that reaches the fallback below.
  // Returning the ISO code itself keeps the currency-glyph slot non-
  // empty so the renderer paints *something* there; the digit-font
  // fits 3 chars in the slot at the medium price size.
  if (ccy == "CAD") return "$";
  if (ccy == "AUD") return "$";
  if (ccy.empty()) return "";
  return ccy;
}

DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol) {
  DigitLayout l;
  if (sats < 0) return l;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(sats));
  const size_t len = std::strlen(buf);
  const size_t slots = 6;
  if (len >= slots) {
    const size_t start = len - slots;
    for (size_t i = 0; i < slots; ++i) l.digits[i] = buf[start + i];
    return l;
  }
  const size_t pad = slots - len;
  for (size_t i = 0; i < slots; ++i) {
    l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
  }
  if (use_symbol && pad > 0) {
    l.is_sats[pad - 1] = true;
    l.digits[pad - 1] = ' ';
  }
  return l;
}

}  // namespace btclock
