#pragma once

#include <stdint.h>

using esp_err_t = int32_t;
using esp_bd_addr_t = uint8_t[6];
using esp_bt_status_t = uint8_t;

#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG -2
#endif
#define ESP_BD_ADDR_LEN 6
constexpr esp_bt_status_t ESP_BT_STATUS_SUCCESS = 0;

enum esp_bt_gap_cb_event_t : uint8_t {
  ESP_BT_GAP_DISC_STATE_CHANGED_EVT = 1,
  ESP_BT_GAP_AUTH_CMPL_EVT = 4,
};

struct esp_bt_gap_cb_param_t {
  struct {
    esp_bd_addr_t bda;
    esp_bt_status_t stat;
    uint8_t device_name[249];
  } auth_cmpl;
};

using esp_bt_gap_cb_t = void (*)(esp_bt_gap_cb_event_t, esp_bt_gap_cb_param_t *);
