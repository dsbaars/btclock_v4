// Host-test stub for esp_log.h. Real header lives in ESP-IDF and pulls
// in FreeRTOS + the rest of the platform — none of which we want when
// running the pure-logic test suite on a developer laptop or in CI.
//
// All macros become no-ops. Tests that care about log content stay on
// device builds (or assert via captured outputs through purpose-built
// shims). Tests that just need the .cpp under test to compile get
// what they need.

#pragma once

#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, ...) do { (void)(tag); } while (0)
