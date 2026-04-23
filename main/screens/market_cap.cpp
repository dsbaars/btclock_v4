#include "screens/screens.hpp"

#include "screens/common.hpp"

namespace btclock {

// cap = integer(price_ccy) * SupplyAtBlock(height), right-justified
// across the digit panels. Matches the old firmware's non-bigChars
// mode (parseMarketCap, lib/btclock/data_handler.cpp): a full digit
// string, no suffix scaling. Currency follows the currently-selected
// rotation currency because isCurrencySpecific(SCREEN_MARKET_CAP) is
// true in the old firmware — we don't hard-code USD.

template <size_t N>
void RenderMarketCapScreen(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
    const std::string& currency, const std::string& price,
    uint32_t block_height, const std::string& prev_price,
    uint32_t prev_height) {
  static_assert(N >= 7, "market-cap layout needs at least 7 panels");
  constexpr size_t kDigitPanels = N - 1;

  const bool full_refresh = prev_price.empty() || prev_height == 0;

  if (full_refresh) {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    DrawSplitText(lfb, lfb.native_width, lfb.native_height,
                  currency.c_str(), "MCAP",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                  fonts.oswald_bold(), 54.0f, /*white_text=*/false);
  }

  const int32_t new_price = PriceInt(price);
  const int32_t old_price = full_refresh ? -1 : PriceInt(prev_price);
  const uint64_t now_cap =
      new_price < 0
          ? 0
          : MarketCap(static_cast<uint32_t>(new_price), block_height);
  const uint64_t prev_cap =
      full_refresh || old_price < 0
          ? 0
          : MarketCap(static_cast<uint32_t>(old_price), prev_height);

  char new_digits[kDigitPanels];
  char old_digits[kDigitPanels];
  FormatDigits64(now_cap, new_digits, kDigitPanels);
  if (!full_refresh) FormatDigits64(prev_cap, old_digits, kDigitPanels);

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
                       kDigitRef, fonts.antonio(), 180.0f,
                       /*white_text=*/false);
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

template void RenderMarketCapScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    uint32_t, const std::string&, uint32_t);
template void RenderMarketCapScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&,
    uint32_t, const std::string&, uint32_t);

}  // namespace btclock
