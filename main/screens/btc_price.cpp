#include "screens/screens.hpp"

#include <cstdio>
#include <cstring>

#include "screens/common.hpp"

namespace btclock {

// Panel 0 = "BTC/<CCY>", panels 1..N-1 = integer price digits right-
// justified. Fractional part rounded away — 1 USD resolution is plenty
// for the large-format display. Sub-dollar precision is tracked in
// beads lx0.12.

template <size_t N>
void RenderBtcPriceScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    const std::string& prev_price) {
  static_assert(N >= 7, "Price layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;
  const bool full_refresh = prev_price.empty();

  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height, "BTC",
                  currency.c_str(),
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  const int32_t new_p = PriceInt(price);
  const int32_t old_p = full_refresh ? -1 : PriceInt(prev_price);

  auto fmt = [](int32_t v, char* digits) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d",
                  static_cast<int>(v < 0 ? 0 : v));
    const size_t len = std::strlen(buf);
    if (len >= kDigitPanels) {
      for (size_t i = 0; i < kDigitPanels; ++i)
        digits[i] = buf[len - kDigitPanels + i];
    } else {
      const size_t pad = kDigitPanels - len;
      for (size_t i = 0; i < kDigitPanels; ++i)
        digits[i] = (i < pad) ? ' ' : buf[i - pad];
    }
  };

  char new_digits[kDigitPanels];
  char old_digits[kDigitPanels];
  fmt(new_p, new_digits);
  if (!full_refresh) fmt(old_p, old_digits);

  std::array<bool, kDigitPanels> update{};
  for (size_t i = 0; i < kDigitPanels; ++i) {
    update[i] = full_refresh || new_digits[i] != old_digits[i];
  }

  for (size_t i = 0; i < kDigitPanels; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, 1 + i);
    ClearFb(lfb, /*white=*/true);
    if (new_digits[i] != ' ') {
      const char one[2] = {new_digits[i], '\0'};
      DrawTextCentered(lfb, lfb.native_width, lfb.native_height, one,
                       kDigitAndPuncRef, fonts.antonio(), 180.0f, false);
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
    const std::string&);
template void RenderBtcPriceScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    const std::string&);

}  // namespace btclock
