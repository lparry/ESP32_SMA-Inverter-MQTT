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

static uint32_t measurementExpirySeconds(bool nightTime, int scanRate) {
  return nightTime
      ? static_cast<uint32_t>(max(2700, (NIGHTSCANRATE / 1000) * 3))
      : static_cast<uint32_t>(max(300, constrain(scanRate, 10, 3600) * 3 + 60));
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


  if (!smartConfig) {
    uint8_t parsedAddress[6];
    if (!parseSmaBluetoothAddress(appConfig.smaBTAddress, parsedAddress)) {
      bluetoothAddressValid = false;
      logE("Invalid inverter Bluetooth address; Bluetooth polling is disabled");
      return;
    }
    for (uint8_t i = 0; i < 6; ++i) smaBTAddress[i] = parsedAddress[i];
    bluetoothAddressValid = true;

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
    startPollWorker();
    initializeBluetoothIfDue(); // "true" creates this device as a BT Master.
    logW("USB commands: unpair (remove inverter bond and retry), poll (read now)");
  }
  // *** Start WIFI and WebServer

} 

  // **** Loop ************
void loop() { 
  smaInverterApp.appLoop();
}

void ESP32_SMA_Inverter_App::appLoop() { 
  // MQTT, Wi-Fi and HTTP are serviced here on every pass. With the poll task
  // running, Bluetooth waits happen on that task instead of this loop.
  mqttInstanceForApp.wifiLoop();
  handleSerialCommands();
  initializeBluetoothIfDue();
  processPollResult();
  int adjustedScanRate;
  struct tm timeinfo;
  bool ntpWorking = getLocalTime(&timeinfo, 0);
  const bool pollIdle = !isPolling();

  if (clockSyncRequested && (int32_t)(millis() - clockSyncRequestDeadline) >= 0) {
    clockSyncRequested = false;
    clockSyncStatus = "Request expired before an inverter connection was available";
  }

  // invData belongs to the poll task while a poll is running. Use the last
  // relay state observed while idle until the poll hands the data back.
  if (pollIdle) lastRelayClosed = ESP32_SMA_Inverter::invData.GridRelay == 51;
  const uint32_t relayFreshnessMs = max(120, constrain(appConfig.scanRate, 10, 3600) * 2) * 1000UL;
  const bool freshClosedRelay = hasSuccessfulRead && lastRelayClosed &&
      (uint32_t)(millis() - lastSuccessfulReadMillis) < relayFreshnessMs;
// Check if the Sun is up or a recent reading reports the grid relay closed
  if ((ntpWorking && (timeinfo.tm_hour >= SUNUP) && (timeinfo.tm_hour < SUNDOWN)) || freshClosedRelay){
    nightTime = false;
    adjustedScanRate = constrain(appConfig.scanRate, 10, 3600) * 1000;
  } else {
    nightTime = true;
    adjustedScanRate = NIGHTSCANRATE;
  }
  if (lastAdjustedScanRate > adjustedScanRate) {
    const uint32_t fasterDeadline = millis() + (uint32_t)adjustedScanRate;
    if ((int32_t)(nextTime - fasterDeadline) > 0) nextTime = fasterDeadline;
  }
  lastAdjustedScanRate = adjustedScanRate;
  // Start a poll when one is due and the previous one has been consumed.
  if (!smartConfig && bluetoothReady && bluetoothAddressValid && pollIdle &&
      ((int32_t)(millis() - nextTime) >= 0) && (!smaInverter.isBtConnected())) {
    nextTime = millis() + adjustedScanRate;
    if(nightTime)
      logW("Night time - 15min scans\n");
    PollJob job;
    job.nightTime = nightTime;
    job.intervalMs = (uint32_t)adjustedScanRate;
    job.unpairFirst = unpairRequested;
    unpairRequested = false;
    job.clockSync = clockSyncRequested;
    job.clockSyncDeadline = clockSyncRequestDeadline;
    job.clockSyncGeneration = clockSyncGeneration;
    job.utcOffsetSeconds = (int32_t)lroundf(appConfig.timezone * 3600.0f);
    dispatchPoll(job);
    // Host builds and the no-task fallback finish the poll synchronously.
    processPollResult();
  }
  
  if (!isPolling()) {
    const uint32_t polledSerial = ESP32_SMA_Inverter::invData.Serial;
    if (polledSerial != 0 && appConfig.thisSerial != polledSerial) {
      appConfig.thisSerial = polledSerial;
      serialSavePending = true;
      nextSerialSaveAttempt = millis();
    }
  }
  if (serialSavePending && (int32_t)(millis() - nextSerialSaveAttempt) >= 0) {
    nextSerialSaveAttempt = millis() + 30000UL;
    if (saveConfiguration()) serialSavePending = false;
    else logW("Inverter serial save failed; will retry");
  }
  // Discovery carries the sensor expiry. Update it when the polling mode changes,
  // even if the inverter has gone to sleep and no new reading can be published.
  const uint32_t currentSerial = appConfig.thisSerial != 0 ? appConfig.thisSerial
      : (hasSuccessfulRead ? pendingReading.Serial : 0);
  if (appConfig.hassDisc && appConfig.mqttBroker.length() > 0 &&
      currentSerial != 0 &&
      (firstTime || nightTime != dayNight || currentSerial != discoveredSerial) &&
      (int32_t)(millis() - nextDiscoveryAttempt) >= 0) {
    const int expiry = measurementExpirySeconds(nightTime, appConfig.scanRate);
    nextDiscoveryAttempt = millis() + 60000UL;
    if (mqttInstanceForApp.hassAutoDiscover(expiry)) {
      if (firstTime) mqttInstanceForApp.logViaMQTT("First boot");
      else mqttInstanceForApp.logViaMQTT(nightTime ? "Night Time" : "Day Time");
      firstTime = false;
      dayNight = nightTime;
      discoveredSerial = currentSerial;
    }
  }
  // Retry the most recent complete reading until it is published or superseded.
  // Discovery is retained by the broker, so once this inverter's entities have
  // been announced a later re-announcement must not hold readings back.
  const bool discoveryReady = !appConfig.hassDisc ||
      (discoveredSerial != 0 && discoveredSerial == pendingReading.Serial);
  if (readingPending &&
      (uint32_t)(millis() - pendingReadingAcquiredMillis) >= pendingReadingMaxAgeMillis) {
    readingPending = false;
    logW("Discarding stale queued inverter reading");
  }
  if (readingPending && discoveryReady && WiFi.status() == WL_CONNECTED &&
      (int32_t)(millis() - nextPublishAttempt) >= 0) {
    nextPublishAttempt = millis() + 5000UL;
    if (mqttInstanceForApp.publishData(&pendingReading, &pendingDisplay)) readingPending = false;
  }
  // DEBUG1_PRINT(".");
  mqttInstanceForApp.wifiLoop();

    
  delay(pollTaskAvailable ? 20 : 100);
}

void ESP32_SMA_Inverter_App::requestPollNow() {
  // A request made while a poll runs is honoured as soon as that poll ends,
  // instead of being overwritten by the end-of-poll schedule.
  if (isPolling()) pollRequestedWhileBusy = true;
  nextTime = millis();
}

#if defined(ARDUINO_ARCH_ESP32)
static TaskHandle_t pollTaskHandle = nullptr;
#endif

void ESP32_SMA_Inverter_App::pollTaskEntry(void *arg) {
#if defined(ARDUINO_ARCH_ESP32)
  ESP32_SMA_Inverter_App *app = static_cast<ESP32_SMA_Inverter_App*>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (app->pollState.load(std::memory_order_acquire) == POLL_RUNNING) app->runPollJob();
  }
#else
  (void)arg;
#endif
}

void ESP32_SMA_Inverter_App::startPollWorker() {
  // Opt-in only. On hardware, running Bluetooth connects on a separate task
  // while the main loop services Wi-Fi made almost every SPP connect fail
  // (ESP_SPP_DISCOVERY_COMP_EVT status 1): 0/5 polls with the task versus
  // 4/5 inline on the same build, position and evening (2026-10-05). Inline
  // polling with the shorter reply timeouts keeps polls to a few seconds.
#if defined(ARDUINO_ARCH_ESP32) && defined(SMA_POLL_TASK)
  if (pollTaskAvailable) return;
  // Same stack size as the Arduino loop task the poll previously ran on.
  if (xTaskCreatePinnedToCore(pollTaskEntry, "sma-poll", SMA_POLL_TASK_STACK, this, 1,
                              &pollTaskHandle, ARDUINO_RUNNING_CORE) == pdPASS) {
    pollTaskAvailable = true;
    logI("Inverter poll task started");
  } else {
    pollTaskHandle = nullptr;
    logE("Could not start the inverter poll task; polling inline");
  }
#endif
}

void ESP32_SMA_Inverter_App::dispatchPoll(const PollJob& job) {
  if (pollState.load(std::memory_order_acquire) != POLL_IDLE) return;
  activeJob = job;
  pollResult = PollResult();
  pollState.store(POLL_RUNNING, std::memory_order_release);
#if defined(ARDUINO_ARCH_ESP32)
  if (pollTaskAvailable) {
    xTaskNotifyGive(pollTaskHandle);
    return;
  }
#endif
  runPollJob();
}

// Runs on the poll task. It may touch smaInverter, invData/dispData,
// activeJob and pollResult only; everything else belongs to the main loop.
void ESP32_SMA_Inverter_App::runPollJob() {
  const PollJob& job = activeJob;
  PollResult& result = pollResult;
  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  DisplayData& dispData = ESP32_SMA_Inverter::getInstance().dispData;
  const uint32_t started = millis();
  smaInverter.beginPollBudget(SMA_POLL_BUDGET_MS);
  E_RC rc = E_NODATA;

  if (job.unpairFirst) {
    logW("Removing configured inverter bond before connecting");
    result.unpairOk = smaInverter.unpair(smaBTAddress);
  }
  smaInverter.setPcktID(1);//pcktID = 1;

  // **** Connect SMA **********
  logW("Connecting SMA inverter: \n");
  if (smaInverter.connect(smaBTAddress)) {
    result.connected = true;

    // **** Initialize SMA *******
    logW("BT connected \n");
    rc = smaInverter.initialiseSMAConnection();
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
    if (rc == E_OK && job.clockSync) {
      // Every button press permits exactly one write attempt.
      result.clockSyncAttempted = true;
      int32_t beforeTime = 0;
      int32_t afterTime = 0;
      E_RC clockRc = smaInverter.syncPlantTime(job.utcOffsetSeconds, &beforeTime, &afterTime,
                                               &job.clockSyncDeadline);
      if (clockRc == E_OK) {
        result.clockSyncStatus = "Verified: " + formatLocalEpoch(beforeTime) + " -> " + formatLocalEpoch(afterTime);
      } else if (clockRc == E_EXPIRED) {
        result.clockSyncStatus = "Request expired before the clock write; inverter clock unchanged";
      } else {
        result.clockSyncStatus = "Clock sync failed with code " + String((int)clockRc) + "; no automatic retry";
        logW("Clock sync failed (%d)", clockRc);
      }
    }
    if (rc == E_OK) {
      pollRollback = invData;
      pollDisplayRollback = dispData;
      rc = smaInverter.ReadCurrentData();
      if (rc == E_OK) {
        result.readOk = true;
        result.reading = invData;
        result.display = dispData;
      } else {
        invData = pollRollback;
        dispData = pollDisplayRollback;
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
  } else {
    result.reconnectRequested = smaInverter.takeReconnectRequest();
  }
  result.rc = rc;
  result.budgetExhausted = smaInverter.pollBudgetWasExhausted();
  result.replyTimeouts = smaInverter.replyTimeoutCount();
  smaInverter.endPollBudget();
  if (!result.readOk) result.display = dispData;
  result.finishedMillis = millis();
  result.durationMs = result.finishedMillis - started;
  logI("Inverter poll finished in %lu ms (%u reply timeouts)",
       static_cast<unsigned long>(result.durationMs), static_cast<unsigned>(result.replyTimeouts));
  pollState.store(POLL_DONE, std::memory_order_release);
}

// Main loop: consume a finished poll and schedule the next one from its end,
// so the inverter and radio get a full scan interval of rest between polls.
void ESP32_SMA_Inverter_App::processPollResult() {
  if (pollState.load(std::memory_order_acquire) != POLL_DONE) return;
  const PollJob& job = activeJob;
  const PollResult& result = pollResult;

  ++stats.polls;
  stats.lastDurationMs = result.durationMs;
  if (result.durationMs > stats.maxDurationMs) stats.maxDurationMs = result.durationMs;
  stats.lastReplyTimeouts = result.replyTimeouts;
  if (!result.connected) stats.lastOutcome = "no connection";
  else if (result.readOk) stats.lastOutcome = "ok";
  else if (result.budgetExhausted) stats.lastOutcome = "poll budget exhausted";
  else if (result.replyTimeouts) stats.lastOutcome = "reply timeout";
  else stats.lastOutcome = "read failed";
  if (!result.readOk) ++stats.failures;
  lastAttemptDisplay = result.display;

  if (result.clockSyncAttempted) {
    clockSyncStatus = result.clockSyncStatus;
    // A newer press made during the poll stays queued for the next poll.
    if (job.clockSyncGeneration == clockSyncGeneration) clockSyncRequested = false;
  }

  if (result.connected) {
    if (result.readOk) {
      pendingReading = result.reading;
      pendingDisplay = result.display;
      pendingReadingAcquiredMillis = result.finishedMillis;
      pendingReadingMaxAgeMillis = measurementExpirySeconds(job.nightTime, appConfig.scanRate) * 1000UL;
      readingPending = true;
      nextPublishAttempt = millis();
      hasSuccessfulRead = true;
      lastSuccessfulReadMillis = result.finishedMillis;
    }
    failCount=0;
  } else if (!job.nightTime) {
    // Inverter shuts down at night so no bluetooth. Don't bother rebooting, just keep trying
    mqttInstanceForApp.logViaMQTT("Bluetooth failed to connect");
    failCount++;
    if( failCount > 5 ) {
      logW("Failed to connect 5 times: Reboot\n");
      mqttInstanceForApp.logViaMQTT("Rebooting after 5 failed Bluetooth connections");
      ESP.restart();
    }
  }

  const uint32_t interval = lastAdjustedScanRate > 0 ? (uint32_t)lastAdjustedScanRate : job.intervalMs;
  if (result.reconnectRequested) nextTime = millis() + 1000UL;
  else if (pollRequestedWhileBusy) nextTime = millis();
  else nextTime = millis() + interval;
  pollRequestedWhileBusy = false;
  pollState.store(POLL_IDLE, std::memory_order_release);
}

void ESP32_SMA_Inverter_App::handleSerialCommands() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      serialCommand[serialCommandLength] = '\0';
      if (!serialCommandOverflow && !smartConfig) {
        if (strcmp(serialCommand, "unpair") == 0) {
          if (!bluetoothAddressValid) logE("Cannot unpair: inverter Bluetooth address is invalid");
          else if (!bluetoothReady) logE("Cannot unpair: Bluetooth is still initializing");
          else {
            logW("USB unpair command: removing configured inverter bond before the next poll");
            unpairRequested = true;
            requestPollNow();
          }
        } else if (strcmp(serialCommand, "poll") == 0) {
          if (bluetoothAddressValid) {
            logW("%s", bluetoothReady ? "USB poll command: requesting inverter read"
                                      : "USB poll command: queued until Bluetooth initializes");
            requestPollNow();
          } else logE("Cannot poll: inverter Bluetooth address is invalid");
        } else if (strcmp(serialCommand, "format-config") == 0) {
          logW("USB format-config command: erasing the settings filesystem");
          configurationStorageAvailable = LittleFS.format() && LittleFS.begin(false);
          if (configurationStorageAvailable) pendingRecoveryFile = nullptr;
          if (configurationStorageAvailable && saveConfiguration()) {
            logW("Settings filesystem initialized with the current configuration");
          } else logE("Settings filesystem initialization failed");
        }
      }
      serialCommandLength = 0;
      serialCommandOverflow = false;
    } else if (serialCommandLength < sizeof(serialCommand) - 1) {
      serialCommand[serialCommandLength++] = ch;
    } else {
      serialCommandOverflow = true;
    }
  }
}

void ESP32_SMA_Inverter_App::initializeBluetoothIfDue() {
  if (smartConfig || !bluetoothAddressValid || bluetoothReady) return;
  if (bluetoothInitRetryScheduled &&
      (int32_t)(millis() - nextBluetoothInitAttempt) < 0) return;

  const uint32_t retryDelay = bluetoothInitRetryMs;
  if (!smaInverter.begin("ESP32toSMA", true)) {
    // A failed core begin can leave controller/SPP resources partially
    // initialized. ESP32_SMA_Inverter::begin tears those down before retrying.
    smaInverter.setServiceCallback(nullptr);
    nextBluetoothInitAttempt = millis() + retryDelay;
    bluetoothInitRetryScheduled = true;
    bluetoothInitRetryMs = retryDelay >= 30000UL ? 60000UL : retryDelay * 2;
    logW("Bluetooth initialization failed; retrying in %lu ms",
         static_cast<unsigned long>(retryDelay));
    return;
  }

  bluetoothReady = true;
  bluetoothInitRetryScheduled = false;
  bluetoothInitRetryMs = 1000UL;
  // Inline polling (host builds, or no poll task) must keep the network
  // serviced from inside Bluetooth waits. The poll task needs no callback:
  // the main loop keeps running beside it.
  if (!pollTaskAvailable) smaInverter.setServiceCallback([]() { mqttInstanceForApp.wifiLoop(true); });
  logI("Bluetooth initialized");
}

void ESP32_SMA_Inverter_App::requestClockSync() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    clockSyncRequested = false;
    clockSyncStatus = "Rejected: ESP NTP time is not valid";
    return;
  }
  clockSyncRequested = true;
  ++clockSyncGeneration;
  clockSyncRequestDeadline = millis() + 5UL * 60UL * 1000UL;
  requestPollNow();
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
