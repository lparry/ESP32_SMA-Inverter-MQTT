#include "gap_sdk_fake.h"

#include <array>
#include <cstring>
#include <thread>

namespace {
esp_bt_gap_cb_t activeCallback = nullptr;
esp_err_t nextRegistrationResult = ESP_OK;
unsigned registerCalls = 0;
bool dispatchDuringRegistration = false;
uint8_t registrationPeer[ESP_BD_ADDR_LEN]{};
esp_bt_status_t registrationStatus = ESP_BT_STATUS_SUCCESS;
std::thread boundaryEventThread;
}

extern "C" esp_err_t __real_esp_bt_gap_register_callback(esp_bt_gap_cb_t callback) {
  ++registerCalls;
  const esp_err_t result = nextRegistrationResult;
  nextRegistrationResult = ESP_OK;
  if (result != ESP_OK) return result;
  if (callback == nullptr) return FAKE_NULL_GAP_CALLBACK_ERROR;

  activeCallback = callback;
  if (dispatchDuringRegistration && activeCallback != nullptr) {
    dispatchDuringRegistration = false;
    const esp_bt_gap_cb_t installedCallback = activeCallback;
    std::array<uint8_t, ESP_BD_ADDR_LEN> peerCopy{};
    std::memcpy(peerCopy.data(), registrationPeer, ESP_BD_ADDR_LEN);
    const esp_bt_status_t status = registrationStatus;
    boundaryEventThread = std::thread([installedCallback, peerCopy, status]() {
      esp_bt_gap_cb_param_t param{};
      std::memcpy(param.auth_cmpl.bda, peerCopy.data(), ESP_BD_ADDR_LEN);
      param.auth_cmpl.stat = status;
      installedCallback(ESP_BT_GAP_AUTH_CMPL_EVT, &param);
    });
  }
  return result;
}

namespace fake_gap_sdk {

void reset() {
  if (boundaryEventThread.joinable()) boundaryEventThread.join();
  activeCallback = nullptr;
  nextRegistrationResult = ESP_OK;
  registerCalls = 0;
  dispatchDuringRegistration = false;
  std::memset(registrationPeer, 0, sizeof(registrationPeer));
  registrationStatus = ESP_BT_STATUS_SUCCESS;
}

void setNextRegistrationResult(esp_err_t result) { nextRegistrationResult = result; }
void dispatchAuthDuringNextRegistration(const uint8_t peer[ESP_BD_ADDR_LEN], esp_bt_status_t status) {
  std::memcpy(registrationPeer, peer, ESP_BD_ADDR_LEN);
  registrationStatus = status;
  dispatchDuringRegistration = true;
}
void waitForRegistrationDispatch() {
  if (boundaryEventThread.joinable()) boundaryEventThread.join();
}
esp_bt_gap_cb_t registeredCallback() { return activeCallback; }
unsigned registrationCalls() { return registerCalls; }

void dispatchAuth(const uint8_t peer[ESP_BD_ADDR_LEN], esp_bt_status_t status) {
  if (activeCallback == nullptr) return;
  esp_bt_gap_cb_param_t param{};
  std::memcpy(param.auth_cmpl.bda, peer, ESP_BD_ADDR_LEN);
  param.auth_cmpl.stat = status;
  activeCallback(ESP_BT_GAP_AUTH_CMPL_EVT, &param);
}

void dispatchEvent(esp_bt_gap_cb_event_t event) {
  if (activeCallback == nullptr) return;
  esp_bt_gap_cb_param_t param{};
  activeCallback(event, &param);
}

}  // namespace fake_gap_sdk
