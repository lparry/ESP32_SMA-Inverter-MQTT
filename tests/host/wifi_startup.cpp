#include "ESP32_SMA_Inverter_App.h"
#include <cassert>

struct SimulatedPowerLoss {};

int main() {
  auto& mqtt = ESP32_SMA_MQTT::getInstance();

  // Existing station credentials must get a bounded connection attempt, with
  // autoreconnect left enabled and no blocking SmartConfig session.
  fake::nvs.clear();
  fake::stationSSID = "saved-network";
  fake::stationPassword = "saved-password";
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = false;
  WiFi.smartConfigBegins = 0;
  WiFi.storedBegins = 0;
  WiFi.autoReconnectEnabled = false;
  const uint64_t savedStart = fake::ticks;
  mqtt.wifiStartup();
  const uint64_t savedElapsed = fake::ticks - savedStart;
  assert(savedElapsed >= 60000 && savedElapsed < 65000);
  assert(WiFi.storedBegins == 1);
  assert(WiFi.smartConfigBegins == 0);
  assert(WiFi.autoReconnectEnabled);
  assert(fake::stationSSID == "saved-network");

  // A device with no station SSID still enters the automatic first-time flow.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  const uint64_t emptyStart = fake::ticks;
  try {
    mqtt.wifiStartup();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(fake::ticks - emptyStart < 15000);
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "new-network");
  Preferences store;
  store.begin("sma-wifi", true);
  assert(store.getBool("provisioned"));
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());

  // A completed provisioning transaction survives the following boot.
  WiFi.state = 0;
  fake::stationAssociated = false;
  WiFi.storedBegins = 0;
  WiFi.smartConfigBegins = 0;
  WiFi.done = false;
  mqtt.wifiStartup();
  assert(WiFi.storedBegins == 1);
  assert(WiFi.smartConfigBegins == 0);
  assert(fake::stationSSID == "new-network");
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());

  // SmartConfig installs the candidate in the driver's NVS before association
  // is known to have worked. Failed first-time association must clear it so a
  // later boot still takes the empty-credentials provisioning path.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  fake::wifiConfigAvailable = true;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = false;
  WiFi.smartConfigBegins = 0;
  const unsigned disconnectsBeforeFailedAssociation = fake::wifiDisconnects;
  const uint64_t failedAssociationStart = fake::ticks;
  mqtt.wifiStartup();
  const uint64_t failedAssociationElapsed = fake::ticks - failedAssociationStart;
  assert(failedAssociationElapsed >= 60000 && failedAssociationElapsed < 75000);
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::wifiDisconnects == disconnectsBeforeFailedAssociation + 1);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());

  // A successful association can still be abandoned if persisting the
  // provisioned marker fails. The rollback must disconnect this live
  // candidate before the driver accepts the empty station config.
  fake::nvs.clear();
  fake::nvsFail = false;
  fake::nvsFailWriteKey = "sma-wifi/provisioned";
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  const unsigned disconnectsBeforeMarkerFailure = fake::wifiDisconnects;
  mqtt.wifiStartup();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::wifiDisconnects == disconnectsBeforeMarkerFailure + 1);
  assert(!fake::stationAssociated);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());
  fake::nvsFailWriteKey.clear();

  // A later boot can provision again after marker-write rollback cleared the
  // associated candidate.
  WiFi.state = 0; // model the station's disconnected status on the next boot
  WiFi.connectOnSmartConfig = true;
  try {
    mqtt.wifiStartup();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(WiFi.smartConfigBegins == 2);
  assert(fake::stationSSID == "new-network");
  assert(fake::stationPassword == "new-password");
  assert(fake::stationAssociated);

  // A subsequent startup gets another bounded empty-credential wait and then
  // accepts a new SmartConfig candidate.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  try {
    mqtt.wifiStartup();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "new-network");
  assert(fake::stationPassword == "new-password");
  Preferences retryStore;
  retryStore.begin("sma-wifi", true);
  assert(retryStore.getBool("provisioned"));

  // A timeout before any candidate arrives also leaves the empty station
  // config available for a fresh provisioning attempt.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = false;
  WiFi.smartConfigProvidesCredentials = false;
  WiFi.connectOnSmartConfig = false;
  WiFi.smartConfigBegins = 0;
  const uint64_t timeoutStart = fake::ticks;
  mqtt.mySmartConfig();
  const uint64_t timeoutElapsed = fake::ticks - timeoutStart;
  assert(timeoutElapsed >= 480000 && timeoutElapsed < 485000);
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  try {
    mqtt.wifiStartup();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(WiFi.smartConfigBegins == 2);
  assert(fake::stationSSID == "new-network");

  // Failed provisioning with an existing network restores its old credentials.
  fake::nvs.clear();
  fake::stationSSID = "saved-network";
  fake::stationPassword = "saved-password";
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = false;
  WiFi.smartConfigBegins = 0;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "saved-network");
  assert(fake::stationPassword == "saved-password");

  // A failed journal write must refuse SmartConfig before it can replace the
  // still-valid saved station configuration.
  fake::nvs.clear();
  fake::nvsFail = false;
  fake::nvsFailWriteKey = "sma-wifi/sc-journal";
  fake::stationSSID = "safe-network";
  fake::stationPassword = "safe-password";
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.smartConfigBegins = 0;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 0);
  assert(fake::stationSSID == "safe-network");
  assert(fake::stationPassword == "safe-password");
  fake::nvsFailWriteKey.clear();

  // Missing prior marker is a valid false state, but an actual marker read
  // error must also stop before SmartConfig can replace the saved network.
  fake::nvs.clear();
  fake::stationSSID = "protected-network";
  fake::stationPassword = "protected-password";
  fake::stationAssociated = false;
  Preferences protectedStore;
  assert(protectedStore.begin("sma-wifi", false));
  assert(protectedStore.putBool("provisioned", true) == sizeof(bool));
  fake::nvsFailReadKey = "sma-wifi/provisioned";
  WiFi.state = 0;
  WiFi.smartConfigBegins = 0;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 0);
  assert(fake::stationSSID == "protected-network");
  assert(fake::stationPassword == "protected-password");
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());
  fake::nvsFailReadKey.clear();

  // Simulate reset after SmartConfig persisted a bad first-time candidate but
  // before association or rollback. Boot must restore confirmed-empty state
  // before deciding to start SmartConfig again.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  fake::stationSSIDHistory.clear();
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  WiFi.storedBegins = 0;
  WiFi.afterSmartConfigCredentials = [] { throw SimulatedPowerLoss{}; };
  try {
    mqtt.mySmartConfig();
    assert(false);
  } catch (const SimulatedPowerLoss&) {
  }
  assert(fake::stationSSID == "new-network");
  assert(fake::nvs.find("sma-wifi/sc-journal") != fake::nvs.end());
  assert(fake::nvs.find("sma-wifi/provisioned") == fake::nvs.end());
  WiFi.afterSmartConfigCredentials = nullptr;
  WiFi.state = 0;
  fake::stationAssociated = false;
  WiFi.done = false;
  WiFi.smartConfigProvidesCredentials = false;
  WiFi.connectOnSmartConfig = false;
  fake::nvsFailReadKey = "sma-wifi/sc-journal";
  const unsigned beginsBeforeDeferredStartup = WiFi.storedBegins;
  mqtt.wifiStartup();
  assert(WiFi.storedBegins == beginsBeforeDeferredStartup);
  assert(!WiFi.autoReconnectEnabled);
  assert(fake::stationSSID == "new-network");
  fake::nvsFailReadKey.clear();
  WiFi.smartConfigBegins = 1;
  fake::stationSSIDHistory.clear();
  fake::ticks += 5000;
  mqtt.wifiLoop(true);
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "new-network");
  fake::ticks += 5000;
  mqtt.wifiLoop(false);
  assert(WiFi.smartConfigBegins == 2);
  assert(WiFi.autoReconnectEnabled);
  assert(!fake::stationSSIDHistory.empty() && fake::stationSSIDHistory.front().empty());
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());

  // A reset during network replacement restores both the previous network
  // and its provisioned marker, and does not re-enter first-time setup.
  fake::nvs.clear();
  fake::stationSSID = "previous-network";
  fake::stationPassword = "previous-password";
  fake::stationAssociated = false;
  Preferences priorStore;
  assert(priorStore.begin("sma-wifi", false));
  assert(priorStore.putBool("provisioned", true) == sizeof(bool));
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  WiFi.afterSmartConfigCredentials = [] { throw SimulatedPowerLoss{}; };
  try {
    mqtt.mySmartConfig();
    assert(false);
  } catch (const SimulatedPowerLoss&) {
  }
  WiFi.afterSmartConfigCredentials = nullptr;
  WiFi.state = 0;
  fake::stationAssociated = false;
  WiFi.done = false;
  WiFi.storedBegins = 0;
  fake::stationSSIDHistory.clear();
  fake::nvsFailReadKey = "sma-wifi/sc-journal";
  mqtt.wifiStartup();
  assert(WiFi.storedBegins == 0 && !WiFi.autoReconnectEnabled);
  assert(fake::stationSSID == "new-network");
  fake::nvsFailReadKey.clear();
  fake::ticks += 5000;
  mqtt.wifiLoop(true);
  assert(WiFi.storedBegins == 0 && !WiFi.autoReconnectEnabled);
  fake::ticks += 5000;
  mqtt.wifiLoop(false);
  assert(fake::stationSSID == "previous-network");
  assert(fake::stationPassword == "previous-password");
  assert(WiFi.autoReconnectEnabled);
  assert(WiFi.storedBegins == 1 && WiFi.smartConfigBegins == 1);
  assert(!fake::stationSSIDHistory.empty() && fake::stationSSIDHistory.front() == "previous-network");
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());
  Preferences restoredStore;
  assert(restoredStore.begin("sma-wifi", true));
  assert(restoredStore.getBool("provisioned"));

  // If the prior-config rollback itself fails, keep the journal so a later
  // boot can retry restoring the known-empty setup.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = false;
  WiFi.smartConfigBegins = 0;
  fake::wifiConfigFailOnWrite = fake::wifiConfigWrites + 2;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "new-network");
  assert(fake::nvs.find("sma-wifi/sc-journal") != fake::nvs.end());
  fake::wifiConfigFailOnWrite = 0;
  WiFi.state = 0;
  WiFi.done = false;
  WiFi.smartConfigProvidesCredentials = false;
  WiFi.connectOnSmartConfig = false;
  fake::stationSSIDHistory.clear();
  mqtt.wifiStartup();
  assert(!fake::stationSSIDHistory.empty() && fake::stationSSIDHistory.front().empty());
  assert(fake::stationSSID.empty());
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());

  // A storage failure while clearing the journal leaves it recoverable even
  // after credentials and the marker were rolled back successfully.
  fake::nvs.clear();
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  fake::nvsFailEraseKey = "sma-wifi/sc-journal";
  mqtt.mySmartConfig();
  assert(fake::stationSSID.empty());
  assert(fake::nvs.find("sma-wifi/sc-journal") != fake::nvs.end());
  fake::nvsFailEraseKey.clear();
  WiFi.state = 0;
  fake::stationAssociated = false;
  WiFi.done = false;
  WiFi.smartConfigProvidesCredentials = false;
  WiFi.connectOnSmartConfig = false;
  mqtt.wifiStartup();
  assert(fake::stationSSID.empty());
  assert(fake::nvs.find("sma-wifi/sc-journal") == fake::nvs.end());

  // If the old station config cannot be read, do not start SmartConfig and
  // risk overwriting credentials that cannot be restored on failure.
  fake::stationSSID = "unreadable-network";
  fake::stationPassword = "unreadable-password";
  fake::stationAssociated = false;
  fake::wifiConfigAvailable = false;
  WiFi.state = 0;
  WiFi.smartConfigBegins = 0;
  const unsigned writesBeforeReadFailure = fake::wifiConfigWrites;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 0);
  assert(fake::wifiConfigWrites == writesBeforeReadFailure);
  assert(fake::stationSSID == "unreadable-network");
  assert(fake::stationPassword == "unreadable-password");

  // If the Arduino API refuses to start SmartConfig, do not enter the long
  // phone-provisioning wait. Existing credentials are restored immediately.
  fake::wifiConfigAvailable = true;
  fake::stationSSID = "saved-network";
  fake::stationPassword = "saved-password";
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = false;
  WiFi.smartConfigStartResult = false;
  WiFi.smartConfigBegins = 0;
  const unsigned stopsBeforeStartFailure = WiFi.stops;
  const uint64_t priorFailureStart = fake::ticks;
  mqtt.mySmartConfig();
  assert(fake::ticks - priorFailureStart < 10000);
  assert(WiFi.smartConfigBegins == 1);
  assert(WiFi.stops == stopsBeforeStartFailure + 1);
  assert(fake::stationSSID == "saved-network");
  assert(fake::stationPassword == "saved-password");

  // A failed start with no saved network clears any candidate state and leaves
  // provisioning available for a subsequent successful attempt.
  fake::stationSSID.clear();
  fake::stationPassword.clear();
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.smartConfigBegins = 0;
  const unsigned disconnectsBeforeStartFailure = fake::wifiDisconnects;
  const uint64_t emptyFailureStart = fake::ticks;
  mqtt.mySmartConfig();
  assert(fake::ticks - emptyFailureStart < 10000);
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::wifiDisconnects == disconnectsBeforeStartFailure + 1);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());

  WiFi.smartConfigStartResult = true;
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  WiFi.smartConfigBegins = 0;
  try {
    mqtt.mySmartConfig();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "new-network");

  auto resetEmptyBootstrap = [&] {
    fake::nvs.clear();
    fake::nvsOpenFail = false;
    fake::nvsReadFail = false;
    fake::nvsFailReadKey.clear();
    fake::nvsFailWriteKey.clear();
    fake::nvsFailEraseKey.clear();
    Preferences stateNamespace;
    assert(stateNamespace.begin("sma-wifi", false));
    const uint8_t namespaceSentinel = 1;
    assert(stateNamespace.putBytes("host-sentinel", &namespaceSentinel, sizeof(namespaceSentinel)) ==
           sizeof(namespaceSentinel));
    fake::wifiConfigAvailable = true;
    fake::wifiConfigSetAvailable = true;
    fake::wifiDisconnectAvailable = true;
    fake::stationSSID.clear();
    fake::stationPassword.clear();
    fake::stationAssociated = false;
    WiFi.state = 0;
    WiFi.done = true;
    WiFi.smartConfigProvidesCredentials = true;
    WiFi.connectOnSmartConfig = false;
    WiFi.smartConfigStartResult = true;
    WiFi.smartConfigBegins = 0;
    WiFi.storedBegins = 0;
  };

  // A wrong password restores the known-empty config and schedules a retry.
  // The retry is rollover-safe, waits through backoff, and defers throughout
  // both an outstanding Bluetooth receive and an inverter poll.
  resetEmptyBootstrap();
  const uint64_t wrap = uint64_t(UINT32_MAX) + 1;
  fake::ticks = wrap - 30000;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());
  const uint64_t failedAt = fake::ticks;
  fake::ticks = failedAt + 59000;
  mqtt.wifiLoop();
  assert(WiFi.smartConfigBegins == 1);
  fake::ticks += 2000;
  auto& app = ESP32_SMA_Inverter_App::getInstance();
  app.pollState.store(ESP32_SMA_Inverter_App::POLL_RUNNING);
  mqtt.wifiLoop();
  app.pollState.store(ESP32_SMA_Inverter_App::POLL_IDLE);
  mqtt.wifiLoop(true);
  assert(WiFi.smartConfigBegins == 1);
  app.pollState.store(ESP32_SMA_Inverter_App::POLL_IDLE);
  WiFi.connectOnSmartConfig = true;
  try {
    mqtt.wifiLoop();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(WiFi.smartConfigBegins == 2);
  assert(fake::stationSSID == "new-network");
  Preferences recoveredSetup;
  assert(recoveredSetup.begin("sma-wifi", true));
  assert(recoveredSetup.getBool("provisioned"));

  // A no-phone timeout also retries without reboot. A transient marker-read
  // error defers that retry and increases the next delay to two minutes.
  resetEmptyBootstrap();
  WiFi.done = false;
  WiFi.smartConfigProvidesCredentials = false;
  const uint64_t noPhoneStart = fake::ticks;
  mqtt.mySmartConfig();
  assert(fake::ticks - noPhoneStart >= 480000);
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());
  fake::nvsFailReadKey = "sma-wifi/provisioned";
  fake::ticks += 60000;
  mqtt.wifiLoop();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID.empty() && fake::stationPassword.empty());
  fake::nvsFailReadKey.clear();
  WiFi.done = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = true;
  fake::ticks += 119000;
  mqtt.wifiLoop();
  assert(WiFi.smartConfigBegins == 1);
  fake::ticks += 2000;
  try {
    mqtt.wifiLoop();
    assert(false);
  } catch (const fake::Restart&) {
  }
  assert(WiFi.smartConfigBegins == 2);
  assert(fake::stationSSID == "new-network");

  // Driver-read ambiguity cannot start provisioning. If another path installs
  // a network before the next retry, the retry is canceled without replacing it.
  resetEmptyBootstrap();
  WiFi.smartConfigStartResult = false;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 1);
  fake::wifiConfigAvailable = false;
  fake::ticks += 60000;
  mqtt.wifiLoop();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID.empty());
  fake::wifiConfigAvailable = true;
  fake::stationSSID = "installed-elsewhere";
  fake::stationPassword = "external-password";
  fake::ticks += 120000;
  mqtt.wifiLoop();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "installed-elsewhere");
  assert(fake::stationPassword == "external-password");

  // A failed attempt with saved credentials never schedules automatic phone
  // provisioning; the saved network is restored and left to autoreconnect.
  fake::nvs.clear();
  fake::stationSSID = "saved-network";
  fake::stationPassword = "saved-password";
  fake::stationAssociated = false;
  WiFi.state = 0;
  WiFi.done = true;
  WiFi.smartConfigStartResult = true;
  WiFi.smartConfigProvidesCredentials = true;
  WiFi.connectOnSmartConfig = false;
  WiFi.smartConfigBegins = 0;
  mqtt.mySmartConfig();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "saved-network");
  assert(fake::stationPassword == "saved-password");
  fake::ticks += 360000;
  mqtt.wifiLoop();
  assert(WiFi.smartConfigBegins == 1);
  assert(fake::stationSSID == "saved-network");
}
