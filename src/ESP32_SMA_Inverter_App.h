#pragma once 
#ifndef ESP32_SMA_INVERTER_APP_H
#define ESP32_SMA_INVERTER_APP_H

#include <Arduino.h>
#include <Esp.h>
#include <WiFiClient.h>
#include "BoundedWebServer.h"
#include <PubSubClient.h>
#include <atomic>

#include <BluetoothSerial.h>

#include "ESP32_SMA_MQTT.h"
#include "ESP32_SMA_Inverter_App.h"
#include "ConfigDefaults.h"


#include <logging.hpp>
#include <ets-appender.hpp>
#include <udp-appender.hpp>

#include "SMA_Utils.h"
#include "SMA_Inverter.h"
#include "ESP32Loggable.h"

//#include "ConfigDefaults.h"


// Uncomment to logoff the inverter after each connection
// Helps with connection reliabiolity on some inverters
// #define LOGOFF true


//*** debug ****************
// 0=no Debug; 
// 1=values only; 
// 2=values and info and P-buffer
// 3=values and info and T+R+P-buffer
#ifndef DEBUG_SMA
#define DEBUG_SMA 1
#endif



// One inverter transaction, from Bluetooth connect to disconnect. The main
// loop builds the job; the poll task (or, on host builds, the caller) runs it.
struct PollJob {
    bool unpairFirst = false;
    bool clockSync = false;
    uint32_t clockSyncDeadline = 0;
    uint32_t clockSyncGeneration = 0;
    int32_t utcOffsetSeconds = 0;
    bool nightTime = false;
    uint32_t intervalMs = 0;
};

struct PollResult {
    bool connected = false;
    bool readOk = false;
    int rc = 0;
    bool reconnectRequested = false;
    bool unpairOk = false;
    bool clockSyncAttempted = false;
    String clockSyncStatus;
    bool budgetExhausted = false;
    uint16_t replyTimeouts = 0;
    uint32_t durationMs = 0;
    uint32_t finishedMillis = 0;
    InverterData reading = {};
    DisplayData display = {};
};

// Main-loop view of polling health, published with the ESP diagnostics.
struct PollStats {
    uint32_t polls = 0;
    uint32_t failures = 0;
    uint32_t lastDurationMs = 0;
    uint32_t maxDurationMs = 0;
    uint16_t lastReplyTimeouts = 0;
    const char *lastOutcome = "none";
};

struct AppConfig {
    String mqttBroker;
    uint16_t mqttPort;
    String mqttUser;
    String mqttPasswd;
    String mqttTopic;
    String smaInvPass;
    String smaBTAddress;
    int scanRate;
    bool hassDisc;
    String ntphostname;
    float timezone;
    uint32_t thisSerial;
};


class ESP32_SMA_Inverter_App : public ESP32Loggable {

    public:

        // Static method to get the instance of the class.
        static ESP32_SMA_Inverter_App& getInstance() {
            // This guarantees that the instance is created only once.
            static ESP32_SMA_Inverter_App instance;
            return instance;
        }

        // Delete the copy constructor and the assignment operator to prevent cloning.
        ESP32_SMA_Inverter_App(const ESP32_SMA_Inverter_App&) = delete;
        ESP32_SMA_Inverter_App& operator=(const ESP32_SMA_Inverter_App&) = delete;



    void appSetup();
    void appLoop();
 //   void wifiStartup();
    void logBuild();


    static int smartConfig;

    static BoundedWebServer webServer;
    static WiFiClient espClient;
    static PubSubClient client;

    AppConfig appConfig;

    //Prototypes
     bool loadConfiguration();
     bool saveConfiguration();
     void printFile();
     void configSetup();
     void rmfiles();
     void requestClockSync();
     void requestDiscovery() { firstTime = true; nextDiscoveryAttempt = millis(); }
     String getClockSyncStatus() const { return clockSyncStatus; }
     bool isPolling() const { return pollState.load(std::memory_order_acquire) != POLL_IDLE; }
     bool configurationStorageReady() const { return configurationStorageAvailable; }
     const DisplayData& lastDisplayData() const {
       return hasSuccessfulRead ? pendingDisplay : lastAttemptDisplay;
     }
     const PollStats& pollStats() const { return stats; }
     bool pollTaskRunning() const { return pollTaskAvailable; }

    protected:
      //extern BluetoothSerial serialBT;
        bool nightTime = false;
        bool firstTime = true;
        bool dayNight = false;
        uint32_t nextDiscoveryAttempt = 0;
        uint32_t discoveredSerial = 0;

    private: 
        ESP32_SMA_Inverter_App() :  ESP32Loggable("ESP32_SMA_Inverter_App") {
            logger().setLevel(esp32m::Debug);

            appConfig = AppConfig();
            strcpy(smaInvPass, "0000");
            /*for (int i=0;i<6;i++) {
                smaBTAddress[i]='0';
            }*/
        };

        ~ESP32_SMA_Inverter_App() {}

        char smaInvPass[13];  // 12 protocol characters plus C-string terminator
        uint8_t smaBTAddress[6]; // SMA bluetooth address
        bool bluetoothAddressValid = false;
        //uint8_t  espBTAddress[6]; // is retrieved from BT packet

        uint32_t nextTime = 0;
        bool bluetoothReady = false;
        bool bluetoothInitRetryScheduled = false;
        uint32_t nextBluetoothInitAttempt = 0;
        uint32_t bluetoothInitRetryMs = 1000;
        int lastAdjustedScanRate = 0;
        uint32_t lastSuccessfulReadMillis = 0;
        bool hasSuccessfulRead = false;
        int failCount = 0;
        bool clockSyncRequested = false;
        uint32_t clockSyncRequestDeadline = 0;
        String clockSyncStatus = "Never requested";
        char serialCommand[16] = {};
        uint8_t serialCommandLength = 0;
        bool serialCommandOverflow = false;
        void handleSerialCommands();
        void initializeBluetoothIfDue();
        // Poll hand-off. IDLE: main loop owns smaInverter and invData/dispData.
        // RUNNING: the poll task owns them. DONE: main loop owns them again
        // and must consume pollResult before the next job.
        enum : uint8_t { POLL_IDLE = 0, POLL_RUNNING = 1, POLL_DONE = 2 };
        std::atomic<uint8_t> pollState{POLL_IDLE};
        PollJob activeJob;
        PollResult pollResult;
        PollStats stats;
        DisplayData lastAttemptDisplay = {};
        InverterData pollRollback = {};
        DisplayData pollDisplayRollback = {};
        bool pollTaskAvailable = false;
        bool pollRequestedWhileBusy = false;
        bool unpairRequested = false;
        bool lastRelayClosed = false;
        uint32_t clockSyncGeneration = 0;
        void startPollWorker();
        void dispatchPoll(const PollJob& job);
        void runPollJob();
        void processPollResult();
        void requestPollNow();
        static void pollTaskEntry(void *arg);
        bool readingPending = false;
        InverterData pendingReading = {};
        DisplayData pendingDisplay = {};
        uint32_t pendingReadingAcquiredMillis = 0;
        uint32_t pendingReadingMaxAgeMillis = 0;
        uint32_t nextPublishAttempt = 0;
        bool serialSavePending = false;
        uint32_t nextSerialSaveAttempt = 0;
        bool configurationStorageAvailable = true;

        const char *pendingRecoveryFile = nullptr;
        const String confFile = "/config.txt"; //extern const char *confFile = "/config.txt";  

};








#endif
