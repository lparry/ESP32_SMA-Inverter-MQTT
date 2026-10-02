/* MIT License

Copyright (c) 2022 Lupo135
Copyright (c) 2023 darrylb123

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include "ESP32_SMA_Inverter_App.h"
#include <cstdio>
#include <cmath>
#if defined(ARDUINO_ARCH_ESP32)
#include <unistd.h>
#endif



ESP32_SMA_Inverter_App& smaInverterApp = ESP32_SMA_Inverter_App::getInstance();
ESP32_SMA_Inverter& smaInverter = ESP32_SMA_Inverter::getInstance();
ESP32_SMA_MQTT& mqttInstanceForApp = ESP32_SMA_MQTT::getInstance();

WiFiClient ESP32_SMA_Inverter_App::espClient = WiFiClient();
PubSubClient ESP32_SMA_Inverter_App::client = PubSubClient(espClient);
BoundedWebServer ESP32_SMA_Inverter_App::webServer(80);

int ESP32_SMA_Inverter_App::smartConfig = 0;

static bool writeTemporaryConfiguration(const String& payload) {
#if defined(ARDUINO_ARCH_ESP32)
  // Arduino File::flush/close hide fflush, fsync and fclose failures. Use the
  // registered LittleFS VFS directly so a failed sync cannot commit settings.
  FILE *file = fopen("/littlefs/config.tmp", "wb");
  if (!file) return false;
  bool complete = fwrite(payload.c_str(), 1, payload.length(), file) == payload.length();
  if (fflush(file) != 0) complete = false;
  if (fsync(fileno(file)) != 0) complete = false;
  if (fclose(file) != 0) complete = false;
#else
  // Host hardware fakes use the File interface. Readback is shared below.
  File file = LittleFS.open("/config.tmp", "w");
  if (!file) return false;
  bool complete = file.write(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length()) == payload.length();
  file.flush();
  file.close();
#endif
  if (!complete) return false;
  File verified = LittleFS.open("/config.tmp", "r");
  bool matches = verified && verified.size() == payload.length();
  for (size_t i = 0; matches && i < payload.length(); ++i) {
    matches = verified.read() == static_cast<uint8_t>(payload[i]);
  }
  verified.close();
  return matches;
}

static String formatLocalEpoch(int32_t epoch) {
  if (epoch <= 0) return String("unknown");
  time_t value = (time_t)epoch;
  struct tm localValue;
  localtime_r(&value, &localValue);
  char formatted[32];
  strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &localValue);
  return String(formatted);
}



static float configuredTimezoneFallback() {
  const float configured = static_cast<float>(TIMEZONE);
  return std::isfinite(configured) && configured >= -12.0f && configured <= 14.0f
      ? configured
      : 0.0f;
}

static float loadTimezone(JsonVariantConst value) {
  const float fallback = configuredTimezoneFallback();
  // The file format historically stores whole-hour offsets as JSON integers,
  // while the settings page also accepts fractional-hour offsets.
  if (!value.is<int>() && !value.is<uint32_t>() && !value.is<float>()) return fallback;

  const float configured = value.as<float>();
  return std::isfinite(configured) && configured >= -12.0f && configured <= 14.0f
      ? configured
      : fallback;
}

void setup() { 

  Logging::setLevel(esp32m::Info);
  Logging::addAppender(&ETSAppender::instance());
#ifdef SYSLOG_HOST
  static UDPAppender udpappender(SYSLOG_HOST);
  udpappender.setMode(UDPAppender::Format::Syslog);
  Logging::addAppender(&udpappender);
#endif
  Serial.println("added appenders");
  smaInverterApp.logBuild();
  smaInverterApp.appSetup();
}

void ESP32_SMA_Inverter_App::logBuild() {
  logW("v1 Build 2w d (%s) t (%s) "  ,__DATE__ , __TIME__) ; 
}

void ESP32_SMA_Inverter_App::appSetup() { 
  Serial.begin(115200); 
  delay(1000);
  configSetup();
  mqttInstanceForApp.wifiStartup();
  
  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  DisplayData& dispData = ESP32_SMA_Inverter::getInstance().dispData;  


  if ( !smartConfig) {
    // Convert the MAC address string to binary
    sscanf(appConfig.smaBTAddress.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", 
            &smaBTAddress[0], &smaBTAddress[1], &smaBTAddress[2], &smaBTAddress[3], &smaBTAddress[4], &smaBTAddress[5]);
    // Zero the array, all unused butes must be 0
    for(int i = 0; i < sizeof(smaInvPass);i++)
       smaInvPass[i] ='\0';
    strlcpy(smaInvPass , appConfig.smaInvPass.c_str(), sizeof(smaInvPass));

    invData.SUSyID = 0x7d;
    invData.Serial = 0;
    nextTime = millis();
    // reverse inverter BT address
    for(uint8_t i=0; i<6; i++) invData.BTAddress[i] = smaBTAddress[5-i];
    logD("invData.BTAddress: %02X:%02X:%02X:%02X:%02X:%02X\n",
                invData.BTAddress[5], invData.BTAddress[4], invData.BTAddress[3],
                invData.BTAddress[2], invData.BTAddress[1], invData.BTAddress[0]);
    // *** Start BT
    smaInverter.begin("ESP32toSMA", true); // "true" creates this device as a BT Master.
  }
  // *** Start WIFI and WebServer

} 

  // **** Loop ************
void loop() { 
  smaInverterApp.appLoop();
}

void ESP32_SMA_Inverter_App::appLoop() { 
  int adjustedScanRate;
  struct tm timeinfo;
  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  DisplayData& dispData = ESP32_SMA_Inverter::getInstance().dispData;
  bool ntpWorking = getLocalTime(&timeinfo);

  if (clockSyncRequested && (int32_t)(millis() - clockSyncRequestDeadline) >= 0) {
    clockSyncRequested = false;
    clockSyncStatus = "Request expired before an inverter connection was available";
  }

// Check if the Sun is up or the grid relay is closed
  if ((ntpWorking && (timeinfo.tm_hour >= SUNUP) && (timeinfo.tm_hour < SUNDOWN)) || (invData.GridRelay == 51)){
    nightTime = false;
    adjustedScanRate = constrain(appConfig.scanRate, 10, 3600) * 1000;
  } else {
    nightTime = true;
    adjustedScanRate = NIGHTSCANRATE;
  }
  // connect or reconnect after connection lost 
  bool freshData = false;
  if (!smartConfig && ((int32_t)(millis() - nextTime) >= 0) && (!smaInverter.isBtConnected())) {
    nextTime = millis() + adjustedScanRate;
    if(nightTime)
      logW("Night time - 15min scans\n");
    smaInverter.setPcktID(1);//pcktID = 1;
    
    // **** Connect SMA **********
    logW("Connecting SMA inverter: \n");
    if (smaInverter.connect(smaBTAddress)) {
      //btConnected = true;
      
      // **** Initialize SMA *******
      logW("BT connected \n");
      E_RC rc = smaInverter.initialiseSMAConnection();
      logI("SMA %d \n",rc);
      if (rc == E_OK) {
        smaInverter.getBT_SignalStrength();
      }

#ifdef LOGOFF
      // not sure the purpose but SBfSpot code logs off before logging on and this has proved very reliable for me: mrtoy-me 
      smaInverter.logoffSMAInverter();
#endif
      // **** logon SMA ************
      logW("*** logonSMAInverter\n");
      if (rc == E_OK) {
        rc = smaInverter.logonSMAInverter(smaInvPass, USERGROUP);
        logI("Logon return code %d\n",rc);
      }
      if (rc == E_OK && clockSyncRequested) {
        // Clear before attempting: every button press permits exactly one write attempt.
        clockSyncRequested = false;
        int32_t beforeTime = 0;
        int32_t afterTime = 0;
        int32_t utcOffsetSeconds = (int32_t)lroundf(appConfig.timezone * 3600.0f);
        E_RC clockRc = smaInverter.syncPlantTime(utcOffsetSeconds, &beforeTime, &afterTime);
        if (clockRc == E_OK) {
          clockSyncStatus = "Verified: " + formatLocalEpoch(beforeTime) + " -> " + formatLocalEpoch(afterTime);
        } else {
          clockSyncStatus = "Clock sync failed with code " + String((int)clockRc) + "; no automatic retry";
          logW("Clock sync failed (%d)", clockRc);
        }
      }
      if (rc == E_OK) {
        InverterData previousInvData = invData;
        DisplayData previousDispData = dispData;
        rc = smaInverter.ReadCurrentData();
        if (rc == E_OK) {
          freshData = true;
        } else {
          invData = previousInvData;
          dispData = previousDispData;
          logW("Discarding incomplete inverter read (%d)", rc);
        }
      } else {
        logW("Skipping inverter read after setup/login failure (%d)", rc);
      }
#ifdef LOGOFF    
      //logoff before disconnecting
      smaInverter.logoffSMAInverter();
#endif
      
      smaInverter.disconnect(); //moved btConnected to inverter class
//       mqttInstanceForApp.publishData();
      failCount=0;
    } else { 
      // Inverter shuts down at night so no bluetooth. Don't bother rebooting, just keep trying 
      if (!nightTime) {
        mqttInstanceForApp.logViaMQTT("Bluetooth failed to connect");
        failCount++;
        if( failCount > 5 ) {
          logW("Failed to connect 5 times: Reboot\n");
          ESP.restart();
        }
      }
    } 
  }
  
  if (invData.Serial != 0 && appConfig.thisSerial != invData.Serial) {
    appConfig.thisSerial = invData.Serial;
    ESP32_SMA_Inverter_App::getInstance().saveConfiguration();
  }
  // Discovery carries the sensor expiry. Update it when the polling mode changes,
  // even if the inverter has gone to sleep and no new reading can be published.
  const uint32_t currentSerial = invData.Serial != 0 ? invData.Serial : appConfig.thisSerial;
  if (appConfig.hassDisc && appConfig.mqttBroker.length() > 0 &&
      currentSerial != 0 &&
      (firstTime || nightTime != dayNight || currentSerial != discoveredSerial) &&
      (int32_t)(millis() - nextDiscoveryAttempt) >= 0) {
    const int expiry = nightTime
        ? max(2700, (NIGHTSCANRATE / 1000) * 3)
        : max(300, constrain(appConfig.scanRate, 10, 3600) * 3 + 60);
    nextDiscoveryAttempt = millis() + 60000UL;
    if (mqttInstanceForApp.hassAutoDiscover(expiry)) {
      if (firstTime) mqttInstanceForApp.logViaMQTT("First boot");
      else mqttInstanceForApp.logViaMQTT(nightTime ? "Night Time" : "Day Time");
      firstTime = false;
      dayNight = nightTime;
      discoveredSerial = currentSerial;
    }
  }
  // Publish the fresh, non-retained reading after Home Assistant has its discovery config.
  if (freshData) {
    mqttInstanceForApp.publishData();
  }
  // DEBUG1_PRINT(".");
  mqttInstanceForApp.wifiLoop();

    
  delay(100);
}





void ESP32_SMA_Inverter_App::requestClockSync() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    clockSyncRequested = false;
    clockSyncStatus = "Rejected: ESP NTP time is not valid";
    return;
  }
  clockSyncRequested = true;
  clockSyncRequestDeadline = millis() + 5UL * 60UL * 1000UL;
  nextTime = millis();
  clockSyncStatus = "Queued for the next inverter connection; expires in five minutes";
}


// Loads the configuration from a file
bool ESP32_SMA_Inverter_App::loadConfiguration() {
  pendingRecoveryFile = nullptr;
  StaticJsonDocument<2048> doc;
  bool recovered = true;
  // The backup is the last committed configuration; a temporary file is last resort.
  for (const char *path : {"/config.txt", "/config.bak", "/config.tmp"}) {
    File file = LittleFS.open(path, "r");
    doc.clear();
    bool valid = file && !deserializeJson(doc, file) && doc.is<JsonObject>() &&
        doc["mqttTopic"].is<const char *>() && doc["smaBTAddress"].is<const char *>();
    file.close();
    if (!valid) { doc.clear(); continue; }
    if (strcmp(path, "/config.txt") != 0) {
      LittleFS.remove("/config.txt");
      recovered = LittleFS.rename(path, "/config.txt");
      if (!recovered) {
        pendingRecoveryFile = path;
        log_e("Could not restore configuration; keeping recovery file");
      }
    }
    break;
  }

  // Copy values from the JsonDocument to the Config         
  std::vector<std::string> keyNames = {"mqttBroker", "mqttPort", "mqttUser","mqttPasswd", "mqttTopic","smaInvPass", "smaBTAddress", "scanRate", "hassDisc","thisserial"};
  for (uint i=0;i<keyNames.size();i++) {
    std::string k = keyNames[i];
    log_w("loaded key: %s", k.c_str());
  }

    appConfig.mqttBroker =  doc["mqttBroker"] | MQTT_BROKER;
    appConfig.mqttPort = doc["mqttPort"] | MQTT_PORT ;
    appConfig.mqttUser = doc["mqttUser"] | MQTT_USER;
    appConfig.mqttPasswd = doc["mqttPasswd"] | MQTT_PASS;
    appConfig.mqttTopic = doc["mqttTopic"] | MQTT_topic;
    if (!validMqttPrefix(appConfig.mqttTopic)) appConfig.mqttTopic = "SMA";
    appConfig.smaInvPass = doc["smaInvPass"] | SMA_PASS;
    appConfig.smaBTAddress = doc["smaBTAddress"] | SMA_BTADDRESS;
    appConfig.scanRate = doc["scanRate"] | SCAN_RATE ;
    appConfig.hassDisc = doc["hassDisc"] | HASS_DISCOVERY ;
    appConfig.timezone = loadTimezone(doc["timezone"]);
    appConfig.ntphostname = doc["ntphostname"] | NTPHOSTNAME;
    const auto storedSerial = doc["thisserial"];
    const uint32_t defaultSerial = static_cast<uint32_t>(THISSERIAL);
    appConfig.thisSerial = storedSerial.is<uint32_t>()
        ? storedSerial.as<uint32_t>() : defaultSerial;


  
  // Close the file (Curiously, File's destructor doesn't close the file)
  return recovered;
}



// Saves the configuration to a file
bool ESP32_SMA_Inverter_App::saveConfiguration() {
  if (!configurationStorageAvailable) return false;
  if (pendingRecoveryFile != nullptr) {
    if (!LittleFS.rename(pendingRecoveryFile, "/config.txt")) return false;
    pendingRecoveryFile = nullptr;
  }
  // Do not delete the only committed file after an interrupted replacement.
  if (!LittleFS.exists("/config.txt") && LittleFS.exists("/config.bak") &&
      !LittleFS.rename("/config.bak", "/config.txt")) return false;
  const char *tempConfig = "/config.tmp";
  LittleFS.remove(tempConfig);
  log_i("creating temporary configuration file");

  // Allocate a temporary JsonDocument
  // Don't forget to change the capacity to match your requirements.
  // Use arduinojson.org/assistant to compute the capacity.
  StaticJsonDocument<2048> doc;

  // Set the values in the document
  doc["mqttBroker"] = appConfig.mqttBroker;
  doc["mqttPort"] = appConfig.mqttPort;
  doc["mqttUser"] = appConfig.mqttUser;
  doc["mqttPasswd"] = appConfig.mqttPasswd;
  doc["mqttTopic"] = appConfig.mqttTopic; 
  doc["smaInvPass"] = appConfig.smaInvPass;
  doc["smaBTAddress"] = appConfig.smaBTAddress;
  doc["scanRate"] = appConfig.scanRate;
  doc["hassDisc"] = appConfig.hassDisc;
  doc["timezone"] = appConfig.timezone;
  doc["ntphostname"] = appConfig.ntphostname;
  doc["thisserial"] = appConfig.thisSerial;

  std::vector<std::string> keyNames = {"mqttBroker", "mqttPort", "mqttUser","mqttPasswd", "mqttTopic","smaInvPass", "smaBTAddress", "scanRate", "hassDisc", "timezone", "ntphostname","thisserial"};
  for (uint i=0;i<keyNames.size();i++) {
    std::string k = keyNames[i];
    log_w("saving key: %s", k.c_str());
  }
 
  // Reject allocation failures and partial writes before touching the good file.
  if (doc.overflowed()) {
    LittleFS.remove(tempConfig);
    return false;
  }
  const size_t expected = measureJson(doc);
  String payload;
  if (!payload.reserve(expected) || serializeJson(doc, payload) != expected ||
      !writeTemporaryConfiguration(payload)) {
    log_e("Configuration write or sync failed");
    LittleFS.remove(tempConfig);
    return false;
  }
  const char *backupConfig = "/config.bak";
  if (LittleFS.exists(backupConfig) && !LittleFS.remove(backupConfig)) return false;
  bool hadConfig = LittleFS.exists("/config.txt");
  if (hadConfig && !LittleFS.rename("/config.txt", backupConfig)) {
    log_e("Failed to preserve existing configuration");
    LittleFS.remove(tempConfig);
    return false;
  }
  if (!LittleFS.rename(tempConfig, "/config.txt")) {
    log_e("Failed to install configuration file");
    if (hadConfig) LittleFS.rename(backupConfig, "/config.txt");
    return false;
  }
  // Keep the last committed configuration available for startup recovery.
  log_d("close file");
  return true;
}

// Prints the content of a file to the Serial
void ESP32_SMA_Inverter_App::printFile() {
  // Open file for reading
  File file = LittleFS.open("/config.txt","r");
  if (!file) {
    log_e("Failed to read file");
    return;
  }

  log_w("Configuration file present (%u bytes); secrets not printed", (unsigned int)file.size());

  // Close the file
  file.close();
}

void ESP32_SMA_Inverter_App::configSetup() {
  
  configurationStorageAvailable = LittleFS.begin(false);
  if (!configurationStorageAvailable) {
    log_e("LittleFS mount failed; preserving flash and using compiled defaults");
  } else{
    log_w("little fs mount sucess");
  }

  // Should load default config if run for the first time
  log_w("Loading configuration...");
  if (!loadConfiguration() || !configurationStorageAvailable) return;

  // Create configuration file
  log_w("Saving configuration...");
  saveConfiguration( );

  // Dump config file
  log_w("Print config file...");
  printFile();
}

void ESP32_SMA_Inverter_App::rmfiles(){
  if (LittleFS.remove("/config.txt")) {
    log_w("%s removed", "/config.txt");
  } else {
    log_e("%s removal failed", "/config.txt");
  }
}
