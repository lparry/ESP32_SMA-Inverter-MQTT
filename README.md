# ESP32_SMA-Inverter-MQTT
Arduino Project to read SMA Inverter data via ESP32 bluetooth and post to MQTT for consumption by Home Assistant.

It is tested with my 2x SB3000TL-20 and 1x SB1600TL-10 with a plugin SMA bluetooth module.
Please let me know when you have tested the software on other SMA Inverters.

The starting point for this project was the code posted by "ESP32_SMA-Inverter-MQTT" by  and "SBFspot" and "ESP32_to_SMA" on github.
Forked from the great work of Darryl Bond and of Lupo135.
Many thanks for the work on these projects!


### COMPILE:
A working VSCode installation must be installed with the PlatformIO plugin installed

### SETUP:
To first configure:
  - Find the Bluetooth address of your Inverter ( Connect to SMAxxx with smart phone and note the displayed device bluetooth address)
  - Plug the device in, If possible connect the Serial Monitor to check on output
  - Use ESP Touch App found in Apple Store, or Google Play to configure the IP Address, Note the IP Address given
  - Browse to the IP address.
  - Fill out the form with the Bluetooth address and configured inverter password
  - Enter the MQTT broker details
  - The topic preamble defaults to "SMA" 
  - If using Home Assistant, enable auto discovery
  - Save the settings
  - Device will reboot and should display successful connects to the inverter and mqtt server on the serial monitor
  - In Home Assistant add an entities card and search for entities starting with SMA-XXXXXXXXXXX

### Home Assistant
The device performs Home Assistant auto-discovery via MQTT.  
The inverter is added as a device, while all the inverter parameters are defined in Home Assistant entities. The entity label begins with the topic preamble and inverter serial.  
  - Device name: SMA-21005XXXXX
  - Entity label: sma_21005XXXXX_grid_relay_status

The ESP32 also publishes its own diagnostics every 60 seconds, even while the inverter is asleep.
Its state topic is `sma/solar/SMA-XXXXXXXX/esp/state` and contains `IP`, `WiFiRSSI`
(dBm), `Uptime` (seconds), and `FreeHeap` (bytes).
With Home Assistant discovery enabled, these appear as diagnostic sensors on the inverter
device, including its ESP32 IP address and a link to the web UI. Sensor states expire after
180 seconds if the ESP stops publishing. MQTT state messages are not retained. The status
page is readable without credentials; changing configuration or setting the inverter clock
requires the configured MQTT username and password when a username is configured. With a blank
MQTT username, those pages remain open.


### Bluetooth recovery over USB

At 115200 baud, send `unpair` followed by a newline after switching firmware or
when Bluetooth authentication fails. This removes only the configured inverter's
saved bond, confirms that it is absent, and retries the connection immediately,
including at night. The Bluetooth pairing PIN remains `0000`.

Send `poll` followed by a newline to request an inverter read without waiting for
the normal night interval. Neither command sets the inverter clock.

When Bluetooth reports an authentication failure from the configured inverter
during an outgoing connection, the firmware performs one automatic
unpair-and-retry attempt per boot. Authentication events from other Bluetooth
peers do not affect the inverter's saved bond.
Ordinary connection/discovery failures do not repeatedly clear the bond.

A settings-filesystem mount failure preserves its contents and runs with the
compiled defaults. If a new device needs its filesystem initialized, or you
have deliberately chosen to discard damaged settings, send `format-config`
followed by a newline over USB. This erases the settings filesystem and saves
the current configuration. Startup never formats it automatically.

Builds with `WIFI_SSID` configured enable automatic Wi-Fi reconnect at startup
and wait up to 60 seconds for the access point. A delayed connection no longer
automatically enters phone provisioning and blocks Bluetooth initialization.
The application continues if that wait expires. Builds without compiled Wi-Fi
credentials keep the initial ESP Touch provisioning flow. If setup fails or
times out and rollback confirms there are still no saved credentials, the device
retries ESP Touch after one minute, then backs off to a maximum of five minutes.
Each retry rechecks the station config and storage state, and waits until no
Bluetooth receive or inverter poll is active.

Hardware evidence is recorded in the [night-time verification](docs/nighttime-verification-2026-09-30.md)
and [producing-inverter comparison](docs/daytime-comparison-2026-10-01.md).

### NOTES:

### TODO:
  - Read month and year History
  - refactor code a bit more for more clarity
  - should be possible to merge/fork with the sbfspot project, or at least re-use more code from it without needing to port.
  - should also be possible to directly integrate into Home Assistant through esphome without the going through mqtt. Also this allows OTA update and (re)configuration through yaml files. see my first attempt here: https://github.com/keerekeerweere/esphome_smabluetooth
