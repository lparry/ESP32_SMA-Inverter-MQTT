#pragma once
#define SMA_WIFI_CONFIG_VALUES_H
#define DEBUG_SMA 3
#define SYSLOG_HOST "127.0.0.1"
#define SUNUP 6
#define SUNDOWN 18
#define NIGHTSCANRATE 900000
#define WIFI_SSID "test-network"
#define WIFI_PASSWORD "test-password"
#define MQTT_BROKER "broker"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""
#define MQTT_topic "SMA"
#define SMA_PASS "0000"
#define SMA_BTADDRESS "00:80:25:00:00:00"
#define SCAN_RATE 60
#define HASS_DISCOVERY true
#define TIMEZONE 0
#define NTPHOSTNAME "pool.ntp.org"
#define THISSERIAL 123

// The legacy deadline regressions were written against 20 s reply and 30 s
// query windows. Firmware defaults are shorter; testConfigurableTimeouts
// exercises those values at runtime.
#define SMA_REPLY_TIMEOUT_MS 20000UL
#define SMA_QUERY_TIMEOUT_MS 30000UL
