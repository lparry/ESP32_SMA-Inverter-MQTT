#pragma once

#include <stdint.h>
#include <esp_gap_bt_api.h>

namespace BluetoothAuthObserver {

// Bracket one outgoing inverter connection attempt. The GAP callback may run
// on the Bluetooth task while the Arduino loop is waiting in connect().
void beginAttempt(const uint8_t peerAddress[ESP_BD_ADDR_LEN]);
bool finishAttempt();

}  // namespace BluetoothAuthObserver

// GNU ld --wrap entry point used by the firmware build. Host tests invoke this
// symbol directly, since Apple's linker does not implement --wrap.
extern "C" esp_err_t __wrap_esp_bt_gap_register_callback(esp_bt_gap_cb_t callback);
