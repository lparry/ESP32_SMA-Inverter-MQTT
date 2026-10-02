#include "BluetoothAuthObserver.h"

#include <cstring>

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

extern "C" esp_err_t __real_esp_bt_gap_register_callback(esp_bt_gap_cb_t callback);

namespace {

#ifdef ARDUINO_ARCH_ESP32
portMUX_TYPE authObserverMux = portMUX_INITIALIZER_UNLOCKED;
class AuthObserverLock {
 public:
  AuthObserverLock() { portENTER_CRITICAL(&authObserverMux); }
  ~AuthObserverLock() { portEXIT_CRITICAL(&authObserverMux); }
};
#else
std::mutex authObserverMutex;
class AuthObserverLock {
 public:
  AuthObserverLock() : lock(authObserverMutex) {}

 private:
  std::lock_guard<std::mutex> lock;
};
#endif

esp_bt_gap_cb_t coreGapCallback = nullptr;
bool attemptActive = false;
bool targetAuthenticationFailed = false;
uint8_t targetAddress[ESP_BD_ADDR_LEN]{};

void observedGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  esp_bt_gap_cb_t callback;
  if (event == ESP_BT_GAP_AUTH_CMPL_EVT && param != nullptr) {
    {
      AuthObserverLock lock;
      bool isTarget = attemptActive;
      for (size_t i = 0; isTarget && i < ESP_BD_ADDR_LEN; ++i) {
        isTarget = targetAddress[i] == param->auth_cmpl.bda[i];
      }
      if (isTarget && param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
        // Latch only target failures. A later success from another peer cannot
        // erase the evidence used by this connection attempt.
        targetAuthenticationFailed = true;
      }
      callback = coreGapCallback;
    }
  } else {
    AuthObserverLock lock;
    callback = coreGapCallback;
  }

  // Never call the framework callback while holding the observer lock: it
  // owns discovery, PIN, and other GAP event processing.
  if (callback != nullptr && callback != observedGapCallback) callback(event, param);
}

}  // namespace

void BluetoothAuthObserver::beginAttempt(const uint8_t peerAddress[ESP_BD_ADDR_LEN]) {
  AuthObserverLock lock;
  attemptActive = false;
  targetAuthenticationFailed = false;
  if (peerAddress != nullptr) {
    std::memcpy(targetAddress, peerAddress, ESP_BD_ADDR_LEN);
    attemptActive = true;
  }
}

bool BluetoothAuthObserver::finishAttempt() {
  AuthObserverLock lock;
  attemptActive = false;
  const bool failed = targetAuthenticationFailed;
  targetAuthenticationFailed = false;
  return failed;
}

extern "C" esp_err_t __wrap_esp_bt_gap_register_callback(esp_bt_gap_cb_t callback) {
  if (callback == observedGapCallback) return ESP_ERR_INVALID_ARG;

  // The pinned SDK registration call synchronously installs its callback and
  // returns without invoking it. Keep the core handler update under the same
  // short lock as event dispatch so an event at installation cannot be lost.
  // It returns errors before installation, leaving the previous handler intact.
  AuthObserverLock lock;
  const esp_err_t result = __real_esp_bt_gap_register_callback(
      callback == nullptr ? nullptr : observedGapCallback);
  if (result == ESP_OK) coreGapCallback = callback;
  return result;
}
