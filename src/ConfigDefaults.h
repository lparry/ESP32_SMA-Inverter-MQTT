#pragma once
// Private overrides are optional. Every setting has its own fallback.
#if __has_include("config_values.h")
#include "config_values.h"
#endif
#ifndef MQTT_BROKER
#define MQTT_BROKER ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif
#ifndef MQTT_topic
#define MQTT_topic "SMA"
#endif
#ifndef SMA_PASS
#define SMA_PASS "0000"
#endif
#ifndef SMA_BTADDRESS
#define SMA_BTADDRESS "00:00:00:00:00:00"
#endif
#ifndef SCAN_RATE
#define SCAN_RATE 60
#endif
#ifndef HASS_DISCOVERY
#define HASS_DISCOVERY false
#endif
#ifndef TIMEZONE
#define TIMEZONE 0
#endif
#ifndef NTPHOSTNAME
#define NTPHOSTNAME "pool.ntp.org"
#endif
#ifndef SUNUP
#define SUNUP 6
#endif
#ifndef SUNDOWN
#define SUNDOWN 18
#endif
#ifndef NIGHTSCANRATE
#define NIGHTSCANRATE (15 * 60 * 1000)
#endif
#ifndef THISSERIAL
#define THISSERIAL 0
#endif
// Bluetooth timing. A healthy SMA reply arrives in well under a second; these
// bound how long one silent or half-awake inverter can hold the poll task.
#ifndef SMA_REPLY_TIMEOUT_MS
#define SMA_REPLY_TIMEOUT_MS 8000UL
#endif
#ifndef SMA_QUERY_TIMEOUT_MS
#define SMA_QUERY_TIMEOUT_MS 12000UL
#endif
// Whole-poll ceiling from connect to disconnect. A poll that exceeds it is
// abandoned and its partial reading discarded.
#ifndef SMA_POLL_BUDGET_MS
#define SMA_POLL_BUDGET_MS 90000UL
#endif
#ifndef SMA_POLL_TASK_STACK
#define SMA_POLL_TASK_STACK 8192
#endif
#ifndef DEBUG_SMA
#define DEBUG_SMA 1
#endif
