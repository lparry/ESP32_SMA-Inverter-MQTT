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
#ifndef DEBUG_SMA
#define DEBUG_SMA 1
#endif
