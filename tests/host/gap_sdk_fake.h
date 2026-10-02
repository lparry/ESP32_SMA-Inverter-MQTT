#pragma once

#include <esp_gap_bt_api.h>

constexpr esp_err_t FAKE_NULL_GAP_CALLBACK_ERROR = -8;

namespace fake_gap_sdk {

void reset();
void setNextRegistrationResult(esp_err_t result);
void dispatchAuthDuringNextRegistration(const uint8_t peer[ESP_BD_ADDR_LEN], esp_bt_status_t status);
void waitForRegistrationDispatch();
esp_bt_gap_cb_t registeredCallback();
unsigned registrationCalls();
void dispatchAuth(const uint8_t peer[ESP_BD_ADDR_LEN], esp_bt_status_t status);
void dispatchEvent(esp_bt_gap_cb_event_t event);

}  // namespace fake_gap_sdk
