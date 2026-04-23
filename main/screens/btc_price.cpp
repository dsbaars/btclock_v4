#include "screens/screens.hpp"

#include <cstdio>
#include <cstring>

#include "screens/common.hpp"

namespace btclock {

// Panel 0 = "BTC/<CCY>", panels 1..N-1 = integer price digits right-
// justified, with an optional currency-symbol glyph placed one slot
// before the first digit when the value leaves at least one blank slot.
// On overflow (price has as many digits as slots) the symbol is dropped
// rather than truncating a digit. Fractional precision tracked in lx0.12.

template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    const std::string& prev_price, const char* symbol_utf8) {
  static_assert(N >= 7, "Price layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;
  const bool full_refresh = prev_price.empty();
  const bool use_symbol = symbol_utf8 && symbol_utf8[0] != '\0';

  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "BTC",
                  currency.c_str(),
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  auto layout = [use_symbol](int32_t v, char* digits, bool* is_sym) {
    for (size_t i = 0; i < kDigitPanels; ++i) {
      digits[i] = ' ';
      is_sym[i] = false;
    }
    const int32_t vv = v < 0 ? 0 : v;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(vv));
    const size_t len = std::strlen(buf);
    if (len >= kDigitPanels) {
      for (size_t i = 0; i < kDigitPanels; ++i)
        digits[i] = buf[len - kDigitPanels + i];
      return;
    }
    const size_t pad = kDigitPanels - len;
    for (size_t i = pad; i < kDigitPanels; ++i) digits[i] = buf[i - pad];
    if (use_symbol && pad > 0) is_sym[pad - 1] = true;
  };

  const int32_t new_p = PriceInt(price);
  const int32_t old_p = full_refresh ? -1 : PriceInt(prev_price);

  char new_digits[kDigitPanels];
  char old_digits[kDigitPanels];
  bool new_sym[kDigitPanels];
  bool old_sym[kDigitPanels];
  layout(new_p, new_digits, new_sym);
  if (!full_refresh) layout(old_p, old_digits, old_sym);

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh || new_digits[i] != old_digits[i] ||
                new_sym[i] != old_sym[i];
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    if (new_sym[i]) {
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height,
                       symbol_utf8, kDigitRef, fonts.antonio(), 180.0f,
                       /*white_text=*/false);
    } else if (new_digits[i] != ' ') {
      const char one[2] = {new_digits[i], '\0'};
      // Use the digit-only ref so digits share the same baseline as
      // the currency-symbol panel and the digits on block-height /
      // Moscow-time slots. Don't widen this with punctuation: the
      // comma's descender would push `ref_box.below_baseline` up and
      // lower the baseline by 11 px on this screen only (see
      // test_host/test_screen_ref_chars for the regression).
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                       kDigitRef, fonts.antonio(), 180.0f, false);
    }
  }

  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  if (full_refresh) panels[0]->DrawFramebufferStart(fb_storage[0], kind);
  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[1 + i]->DrawFramebufferStart(fb_storage[1 + i], kind);
  }
  if (full_refresh) panels[0]->WaitForRefresh();
  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    panels[1 + i]->WaitForRefresh();
  }
}

template void RenderBtcPriceScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, const char*);
template void RenderBtcPriceScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&, const char*);

}  // namespace btclock
