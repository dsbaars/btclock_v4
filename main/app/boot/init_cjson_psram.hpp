#pragma once

namespace btclock {

// Register cJSON malloc/free hooks that route every cJSON node into
// PSRAM via heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT). Must
// be called before any cJSON_Parse / cJSON_CreateObject runs (i.e.
// very early in app_main, before settings/network/control_api boot).
void InitCjsonPsram();

}  // namespace btclock
