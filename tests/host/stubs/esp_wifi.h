#pragma once
#include <Arduino.h>

enum wifi_interface_t { WIFI_IF_STA = 0 };
struct wifi_sta_config_t {
 uint8_t ssid[32];
 uint8_t password[64];
};
union wifi_config_t { wifi_sta_config_t sta; };

namespace fake {
inline std::string stationSSID;
inline std::string stationPassword;
inline bool wifiConfigAvailable=true;
inline bool wifiConfigSetAvailable=true;
inline bool wifiDisconnectAvailable=true;
inline bool stationAssociated=false;
inline unsigned wifiConfigReads=0;
inline unsigned wifiConfigWrites=0;
inline unsigned wifiDisconnects=0;
inline unsigned wifiConfigFailOnWrite=0;
inline std::vector<std::string> stationSSIDHistory;
}

inline int esp_wifi_get_config(wifi_interface_t, wifi_config_t *config) {
 ++fake::wifiConfigReads;
 if (!fake::wifiConfigAvailable || !config) return 1;
 memset(config, 0, sizeof(*config));
 memcpy(config->sta.ssid, fake::stationSSID.data(), std::min(fake::stationSSID.size(), sizeof(config->sta.ssid)));
 memcpy(config->sta.password, fake::stationPassword.data(), std::min(fake::stationPassword.size(), sizeof(config->sta.password)));
 return 0;
}

inline int esp_wifi_set_config(wifi_interface_t, wifi_config_t *config) {
 ++fake::wifiConfigWrites;
 // Model ESP-IDF's refusal to replace station config while connected. The
 // production rollback must disconnect the accepted SmartConfig candidate
 // before installing the confirmed-empty config.
 if (!fake::wifiConfigSetAvailable || !config || fake::stationAssociated ||
     fake::wifiConfigWrites==fake::wifiConfigFailOnWrite) return 1;
 size_t ssidLength=0;
 while (ssidLength < sizeof(config->sta.ssid) && config->sta.ssid[ssidLength]) ++ssidLength;
 size_t passwordLength=0;
 while (passwordLength < sizeof(config->sta.password) && config->sta.password[passwordLength]) ++passwordLength;
 fake::stationSSID.assign(reinterpret_cast<const char*>(config->sta.ssid), ssidLength);
 fake::stationPassword.assign(reinterpret_cast<const char*>(config->sta.password), passwordLength);
 fake::stationSSIDHistory.push_back(fake::stationSSID);
 return 0;
}

inline int esp_wifi_disconnect() {
 ++fake::wifiDisconnects;
 if (!fake::wifiDisconnectAvailable) return 1;
 fake::stationAssociated=false;
 return 0;
}
