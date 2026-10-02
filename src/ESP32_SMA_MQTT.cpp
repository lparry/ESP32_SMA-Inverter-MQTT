/* MIT License

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
// Wifi Functions choose between Station or SoftAP

#include "ESP32_SMA_MQTT.h"
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <cstdarg>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>

class HtmlPage {
public:
  explicit HtmlPage(size_t capacity) : capacity(capacity), buffer(static_cast<char*>(malloc(capacity))) {
    if (buffer) buffer[0] = '\0';
  }
  ~HtmlPage() { free(buffer); }
  bool allocated() const { return buffer != nullptr; }
  bool complete() const { return buffer && valid; }
  const char *data() const { return buffer; }
  void append(const char *value) {
    if (!complete()) return;
    const size_t length = strlen(value);
    if (length >= capacity - used) { valid = false; return; }
    memcpy(buffer + used, value, length + 1);
    used += length;
  }
  __attribute__((format(printf, 2, 3))) void format(const char *pattern, ...) {
    if (!complete()) return;
    va_list args;
    va_start(args, pattern);
    const int length = vsnprintf(buffer + used, capacity - used, pattern, args);
    va_end(args);
    if (length < 0 || static_cast<size_t>(length) >= capacity - used) { valid = false; return; }
    used += length;
  }
private:
  size_t capacity;
  char *buffer;
  size_t used = 0;
  bool valid = true;
};


//ESP32_SMA_Inverter_App_Config& appConfigInstance = ESP32_SMA_Inverter_App_Config::getInstance();
ESP32_SMA_MQTT& mqttInstance = ESP32_SMA_MQTT::getInstance();

static String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '\"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += value[i]; break;
    }
  }
  return escaped;
}

static bool parseIntegerSetting(const String& value, long long& parsed) {
  const char *start = value.c_str();
  if (!start || *start == '\0') return false;
  char *end = nullptr;
  errno = 0;
  const long long result = strtoll(start, &end, 10);
  if (end == start || *end != '\0' || errno == ERANGE) return false;
  parsed = result;
  return true;
}

static bool parseFiniteFloatSetting(const String& value, float& parsed) {
  const char *start = value.c_str();
  if (!start || *start == '\0') return false;
  char *end = nullptr;
  errno = 0;
  const float result = strtof(start, &end);
  if (end == start || *end != '\0' || errno == ERANGE || !std::isfinite(result)) return false;
  parsed = result;
  return true;
}

static bool validHostname(const String& value) {
  if (value.isEmpty() || value.length() > 128) return false;
  size_t label = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '.') {
      if (label == 0 || value[i - 1] == '-') return false;
      label = 0;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') ||
          (label == 0 && c == '-') || ++label > 63) return false;
    }
  }
  return value[value.length() - 1] != '-';
}

static bool isBasicWhitespace(char value) {
  return value == ' ' || value == '\t';
}

static char basicBase64Digit(uint8_t value) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  return alphabet[value & 0x3f];
}

static uint8_t basicCredentialByte(const String& user, const String& password,
                                   size_t userLength, size_t offset) {
  if (offset < userLength) return static_cast<uint8_t>(user[offset]);
  if (offset == userLength) return static_cast<uint8_t>(':');
  return static_cast<uint8_t>(password[offset - userLength - 1]);
}

// Compare the canonical Base64 form without constructing or decoding a
// credential buffer. BoundedWebServer caps the complete request headers at
// 4096 bytes, so both this copy and the comparison work are bounded.
static bool basicAuthorizationMatches(const String& authorization,
                                      const String& user,
                                      const String& password) {
  static constexpr size_t maxAuthorizationLength = 4096;
  const size_t headerLength = authorization.length();
  if (headerLength == 0 || headerLength > maxAuthorizationLength) return false;

  const char *header = authorization.c_str();
  size_t begin = 0;
  size_t end = headerLength;
  // Accept outer HTTP whitespace and the case-insensitive Basic scheme;
  // Base64 payload bytes below remain case-sensitive.
  while (begin < end && isBasicWhitespace(header[begin])) ++begin;
  while (end > begin && isBasicWhitespace(header[end - 1])) --end;
  if (end - begin <= 5) return false;

  static const char scheme[] = "basic";
  for (size_t i = 0; i < sizeof(scheme) - 1; ++i) {
    char value = header[begin + i];
    if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
    if (value != scheme[i]) return false;
  }
  size_t tokenStart = begin + sizeof(scheme) - 1;
  if (!isBasicWhitespace(header[tokenStart])) return false;
  while (tokenStart < end && isBasicWhitespace(header[tokenStart])) ++tokenStart;
  if (tokenStart == end) return false;

  const size_t userLength = user.length();
  const size_t passwordLength = password.length();
  const size_t maxSize = std::numeric_limits<size_t>::max();
  if (passwordLength == maxSize || userLength > maxSize - passwordLength - 1) return false;
  const size_t credentialLength = userLength + 1 + passwordLength;
  const size_t groups = credentialLength / 3 + (credentialLength % 3 != 0);
  if (groups > maxSize / 4) return false;
  const size_t encodedLength = groups * 4;
  const size_t tokenLength = end - tokenStart;
  if (encodedLength > maxAuthorizationLength || tokenLength != encodedLength) return false;

  // Length is public. For matching-length values, compare every Base64 byte
  // and require canonical padding; Base64 data itself remains case-sensitive.
  uint8_t difference = 0;
  size_t output = 0;
  for (size_t input = 0; input < credentialLength; input += 3) {
    const bool hasSecond = input + 1 < credentialLength;
    const bool hasThird = input + 2 < credentialLength;
    const uint8_t first = basicCredentialByte(user, password, userLength, input);
    const uint8_t second = hasSecond
        ? basicCredentialByte(user, password, userLength, input + 1) : 0;
    const uint8_t third = hasThird
        ? basicCredentialByte(user, password, userLength, input + 2) : 0;
    const char encoded[4] = {
      basicBase64Digit(first >> 2),
      basicBase64Digit(static_cast<uint8_t>((first << 4) | (second >> 4))),
      hasSecond ? basicBase64Digit(static_cast<uint8_t>((second << 2) | (third >> 6))) : '=',
      hasThird ? basicBase64Digit(third) : '='
    };
    for (size_t i = 0; i < sizeof(encoded); ++i)
      difference |= static_cast<uint8_t>(encoded[i] ^ header[tokenStart + output++]);
  }
  return difference == 0;
}

static bool requireWebAuthentication() {
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
  // An empty MQTT username intentionally keeps first-time web setup accessible.
  if (config.mqttUser.length() == 0) return true;
  const String authorization = ESP32_SMA_Inverter_App::webServer.header("Authorization");
  if (basicAuthorizationMatches(authorization, config.mqttUser, config.mqttPasswd)) return true;
  ESP32_SMA_Inverter_App::webServer.requestAuthentication();
  return false;
}


//link to singleton methods
extern void E_formPage() {
  ESP32_SMA_MQTT::getInstance().formPage();
}

extern void E_connectAP() {
  ESP32_SMA_MQTT::getInstance().connectAP();
}

extern void E_showSmartConfigConfirmation() {
  ESP32_SMA_MQTT::getInstance().showSmartConfigConfirmation();
}

extern void E_handleForm() {
  ESP32_SMA_MQTT::getInstance().handleForm();
}

extern void E_handleSetClock() {
  ESP32_SMA_MQTT::getInstance().handleSetClock();
}

void ESP32_SMA_MQTT::wifiTime() {
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;

  long  gmtOffset_sec = config.timezone * 3600;   // offset seconds, this depends on your time zone (3600 is GMT +1)
  int   daylightOffset_sec = 0;  // daylight saving offset seconds
  logD("Setting time via %s, gmt: %ld, dst: %d ", config.ntphostname.c_str(), gmtOffset_sec, daylightOffset_sec);


  configTime(gmtOffset_sec, daylightOffset_sec, config.ntphostname.c_str());

  String t = getTime();
  logI("Time %s" , t.c_str());

}


String ESP32_SMA_MQTT::getTime() {
  struct tm timeinfo;
  for (int i=0;i<5;i++) {
    logD(".time.");
    if(getLocalTime(&timeinfo)) {
      char charTime[64];
      strftime(charTime, sizeof(charTime), "%A, %B %d %Y %H:%M:%S", &timeinfo);
      logD("now : %s ", charTime);
      return String(charTime);
    }
    delay(500);
  }
  logE("failed to obtain time");
  return String("");
}

void ESP32_SMA_MQTT::wifiStartup(){
  // Build Hostname
  logD("wifiStartup()");
  char sapString[20]="";
  snprintf(sapString, 20, "SMA-%08lX", (unsigned long)(uint32_t)ESP.getEfuseMac());
  mqttInstance.sapString = String(sapString);
  char token[17];
  snprintf(token, sizeof(token), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
  clockSyncToken = String(token);
  snprintf(token, sizeof(token), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
  smartConfigToken = String(token);
  snprintf(token, sizeof(token), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
  settingsToken = String(token);
  logD("%s", mqttInstance.sapString.c_str());

  // Attempt to connect to the AP stored on board, if not, start in SoftAP mode
  WiFi.mode(WIFI_STA);
  ESP32_SMA_Inverter_App::client.setBufferSize(1024);
  ESP32_SMA_Inverter_App::client.setKeepAlive(120);
  ESP32_SMA_Inverter_App::client.setSocketTimeout(5);
  delay(2000);

  logD("setHostname");
  WiFi.hostname(mqttInstance.sapString);

#ifdef WIFI_SSID
  //overriding ssid and hostname
  logD("wifi begin with ssid(%s) and password (.......)", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int w = 0; w <= 10 && WiFi.status() != WL_CONNECTED; w++) {
    delay(500);
    logD(".wifi.");
  }
#else
  WiFi.begin();
#endif

  //delay(2000);
  logD("Using config");

  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
  logD("mqtt topic: %s", config.mqttTopic.c_str());
  if (config.mqttTopic == "")
    config.mqttTopic = mqttInstance.sapString;

  Preferences energyStore;
  if (energyStore.begin("sma-mqtt", true)) {
    uint32_t savedSerial = energyStore.getUInt("serial", 0);
    if (savedSerial != 0 && savedSerial == config.thisSerial) {
      lastAcceptedETotalWh = energyStore.getULong64("etotal", 0);
      lastPersistedETotalWh = lastAcceptedETotalWh;
    }
    energyStore.end();
  }
  int i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    // Launch smartconfig to reconfigure wifi
    logD(">");
    if (i > 3)
      mySmartConfig();
    i++;
    delay(1000);
  }
  // Success connecting
  ESP32_SMA_Inverter_App::smartConfig = 0;
  String hostName = mqttInstance.sapString;
  logW("hostname %s", hostName.c_str());
  logW("IP Address: %s", ((String)WiFi.localIP().toString()).c_str());
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);


  wifiTime();


  ESP32_SMA_Inverter_App::webServer.on("/", E_formPage);
  ESP32_SMA_Inverter_App::webServer.on("/smartconfig", HTTP_GET, E_showSmartConfigConfirmation);
  ESP32_SMA_Inverter_App::webServer.on("/smartconfig", HTTP_POST, E_connectAP);
  ESP32_SMA_Inverter_App::webServer.on("/postform/", E_handleForm);
  ESP32_SMA_Inverter_App::webServer.on("/setclock/", HTTP_POST, E_handleSetClock);
  ESP32_SMA_Inverter_App::webServer.begin();

  logI("Web Server Running: ");

}

// Configure wifi using ESP Smartconfig app on phone
void ESP32_SMA_MQTT::mySmartConfig() {
  logD("smartConfig");
  // Wipe current credentials
  // WiFi.disconnect(true); // deletes the wifi credentials

  WiFi.mode(WIFI_STA);
  delay(2000);
  WiFi.begin();
  WiFi.beginSmartConfig();

  //Wait for SmartConfig packet from mobile
  logI("Waiting for SmartESP32_SMA_Inverter_App_Config::config");
  // If no SmartConfig message arrives after about 8 minutes, reboot and try again.
  int count = 0;
  while (!WiFi.smartConfigDone()) {
    delay(2000);
    logD(".");
    if (count++ > 250 ) ESP.restart();
  }
  logD("");
  logI("SmartConfig received.");

  //Wait for WiFi to connect to AP
  logW("Waiting for WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    logD(".");
  }
  logW("IP Address: %s", ((String)WiFi.localIP().toString()).c_str());
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  logW("Restarting in 5 seconds");
  delay(5000);
  ESP.restart();
}

void ESP32_SMA_MQTT::showSmartConfigConfirmation() {
  if (!requireWebAuthentication()) return;
  if (!requireWebAuthentication()) return;
  String page =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Change ESP32 Wi-Fi network</title></head><body>"
    "<h1>Change ESP32 Wi-Fi network</h1>"
    "<p>This starts ESPTouch setup so a phone can send new Wi-Fi credentials to the ESP32. "
    "Use it only if you want to move the ESP32 to another Wi-Fi network.</p>"
    "<p>You need the ESPTouch app on a phone connected to the new network. "
    "Once started, inverter polling and MQTT updates pause, and this page may stop responding. "
    "The ESP32 restarts after receiving the new settings or after about 8 minutes without them.</p>"
    "<form method=\"post\" action=\"/smartconfig\">"
    "<input type=\"hidden\" name=\"token\" value=\"" + smartConfigToken + "\">"
    "<button type=\"submit\">Start Wi-Fi setup</button></form>"
    "<p><a href=\"/\">Cancel and return to inverter page</a></p></body></html>";
  ESP32_SMA_Inverter_App::webServer.sendHeader("Cache-Control", "no-store");
  ESP32_SMA_Inverter_App::webServer.send(200, "text/html", page);
}

// Use ESP SmartConfig to connect to Wi-Fi only after the confirmation form is submitted.
void ESP32_SMA_MQTT::connectAP(){
  if (!requireWebAuthentication()) return;
  if (!requireWebAuthentication()) return;
  if (!ESP32_SMA_Inverter_App::webServer.hasArg("token") ||
      ESP32_SMA_Inverter_App::webServer.arg("token") != smartConfigToken) {
    ESP32_SMA_Inverter_App::webServer.send(403, "text/plain", "Invalid or expired request token");
    return;
  }
  ESP32_SMA_Inverter_App::webServer.send(200, "text/plain",
      "Wi-Fi setup started. Open the ESPTouch app on a phone connected to the new Wi-Fi network. "
      "Inverter and MQTT updates are paused until the ESP32 restarts.");
  delay(2000);
  mySmartConfig();

}


void ESP32_SMA_MQTT::wifiLoop(bool receiveWait){
  // Attempt to reconnect to Wifi if disconnected
  unsigned long currentMillis = millis();
  // if WiFi is down, try reconnecting
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - mqttInstance.previousMillis >= mqttInstance.interval)) {
    logW("Reconnecting to WiFi...\n");
    WiFi.disconnect();
    WiFi.reconnect();
    mqttInstance.previousMillis = currentMillis;
  }

  if (WiFi.status() == WL_CONNECTED) {
    publishEspStatus();
    if (ESP32_SMA_Inverter_App::client.connected()) {
      ESP32_SMA_Inverter_App::client.loop();
    }
  }

  ESP32_SMA_Inverter_App::webServer.handleClient();
}

void ESP32_SMA_MQTT::formPage () {
  HtmlPage page(10000);
  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  const DisplayData& dispData = ESP32_SMA_Inverter_App::getInstance().lastDisplayData();
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
  char fulltopic[100];

  if (!page.allocated()) {
    ESP32_SMA_Inverter_App::webServer.send(503, "text/plain", "Out of memory");
    return;
  }
  logW("Connect formpage\n");
  page.append("<!DOCTYPE html><html><head>\
                      <title>SMA Inverter</title></head><body>\
                      <style>\
                      table {\
  border-collapse: collapse;\
  width: 100%;\
  font-size: 30px;\
}\
\
table, th, td {\
  border: 1px solid black;\
}\
</style>\
<H1> SMA Bluetooth Configuration</H1>\
<form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"/postform/\">");


  page.format( "<input type=\"hidden\" name=\"token\" value=\"%s\">", settingsToken.c_str());
  page.append( "<TABLE><TR><TH>Configuration</TH><TH>Setting</TH></TR>\n");
  page.format("<TR><TD>Inverter Bluetooth Address (Format AA:BB:CC:DD:EE:FF) : </TD><TD> <input maxlength=\"17\" type=\"text\" name=\"btaddress\" value=\"%s\"></TD><TR>\n\n",htmlEscape(config.smaBTAddress).c_str());
  page.format("<TR><TD>SMA Inverter Password:</TD><TD><input maxlength=\"12\" type=\"password\" name=\"smapw\" placeholder=\"unchanged\"></TD><TR>\n\n");
  page.format("<TR><TD>MQTT Broker Hostname or IP Address :</TD><TD> <input maxlength=\"128\" type=\"text\" name=\"mqttBroker\" value=\"%s\"></TD><TR>\n\n",htmlEscape(config.mqttBroker).c_str());
  page.format("<TR><TD>MQTT Broker Port : </TD><TD><input type=\"text\" name=\"mqttPort\" value=\"%d\"></TD><TR>\n\n",config.mqttPort);
  page.format("<TR><TD>MQTT Broker User :</TD><TD> <input maxlength=\"128\" type=\"text\" name=\"mqttUser\" value=\"%s\"></TD><TR>\n\n",htmlEscape(config.mqttUser).c_str());
  page.format("<TR><TD>MQTT Broker Password :</TD><TD> <input maxlength=\"128\" type=\"password\" name=\"mqttPasswd\" placeholder=\"unchanged\"></TD><TR>\n\n");
  page.format("<TR><TD>MQTT Topic Preamble:</TD><TD> <input maxlength=\"32\" type=\"text\" name=\"mqttTopic\" value=\"%s\"></TD><TR>\n\n",htmlEscape(config.mqttTopic).c_str());
  page.format("<TR><TD>Inverter scan rate:</TD><TD> <input type=\"text\" name=\"scanRate\" value=\"%d\"></TD><TR>\n\n",config.scanRate);
  page.format("<TR><TD>Timezone (in hours from UTC):</TD><TD> <input type=\"text\" name=\"timezone\" value=\"%4.2f\"></TD><TR>\n\n",config.timezone);
  page.format("<TR><TD>NTP host:</TD><TD> <input type=\"text\" name=\"ntphostname\" value=\"%s\"></TD><TR>\n\n",htmlEscape(config.ntphostname).c_str());

  if (config.hassDisc) {
    page.append( "<TR><TD>Home Assistant Auto Discovery:</TD><TD> <input type=\"checkbox\" name=\"hassDisc\" checked ></TD><TR>\n");
    snprintf(fulltopic,sizeof(fulltopic),"sma/solar/%s-%lu/state",config.mqttTopic.c_str(),(unsigned long)config.thisSerial);
  } else {
    page.append( "<TR><TD>Home Assistant Auto Discovery:</TD><TD> <input type=\"checkbox\" name=\"hassDisc\"></TD><TR>\n");
    snprintf(fulltopic,sizeof(fulltopic),"%s-%lu/state",config.mqttTopic.c_str(),(unsigned long)config.thisSerial);
  }
  page.append( "</TABLE>");
  page.append( "<input type=\"submit\" value=\"Submit\"></form><BR> <A href=\"/smartconfig\">Change ESP32 Wi-Fi network (review before starting)</A><BR>");

  String clockStatus = htmlEscape(ESP32_SMA_Inverter_App::getInstance().getClockSyncStatus());
  String espTime = htmlEscape(getTime());
  page.format(
    "<H2>Inverter clock</H2><P>ESP time: %s (configured UTC offset: %.2f hours)</P>"
    "<P>Last clock action: %s</P>"
    "<form method=\"post\" action=\"/setclock/\" onsubmit=\"return confirm('Set the inverter clock from the displayed ESP time now?');\">"
    "<input type=\"hidden\" name=\"token\" value=\"%s\">"
    "<input type=\"submit\" value=\"Set inverter clock from NTP\"></form><BR>",
    espTime.c_str(), config.timezone, clockStatus.c_str(), clockSyncToken.c_str());


  page.append( "<TABLE><TR><TH>Last Scan</TH><TH>Data</TH>\n");


  page.format(
"<tr><td>MQTT Topic</td><td>%s</td></tr>\n\
 <tr><td>BT Signal Strength</td><td>%4.1f %%</td></tr>\n\
  <tr><td>Uac</td><td>A: %15.1f ,B: %15.1f ,C: %15.1f V</td></tr>\n\
 <tr><td>Iac</td><td>A: %15.1f ,B: %15.1f ,C: %15.1f A</td></tr>\n\
 <tr><td>Pac</td><td>%15.0f W</td></tr>\n\
 <tr><td>Udc</td><td>String 1: %15.1f V, String 2: %15.1f V</td></tr>\n\
 <tr><td>Idc</td><td>String 1: %15.1f A, String 2: %15.1f A</td></tr>\n\
 <tr><td>Wdc</td><td>String 1: %15.1f kW, String 2: %15.1f kW</td></tr>\n"
 , fulltopic
 , dispData.BTSigStrength
 , dispData.Uac[0], dispData.Uac[1], dispData.Uac[2]
 , dispData.Iac[0], dispData.Iac[1], dispData.Iac[2]
 , dispData.Pac
 , dispData.Udc[0], dispData.Udc[1]
 , dispData.Idc[0], dispData.Idc[1]
 , dispData.Udc[0] * dispData.Idc[0] / 1000 , dispData.Udc[1] * dispData.Idc[1] / 1000);



  page.format(
"<tr><td>Frequency</td><td>%5.2f Hz</td></tr>\n\
 <tr><td>E-Today</td><td>%15.1f kWh</td></tr>\n\
 <tr><td>E-Total</td><td>%15.1f kWh</td></tr>\n "
 , dispData.Freq
 , dispData.EToday
 , dispData.ETotal);
  page.append("</TABLE></body></html>\n");
  ESP32_SMA_Inverter_App::webServer.sendHeader("Cache-Control", "no-store");
  if (!page.complete()) {
    ESP32_SMA_Inverter_App::webServer.send(503, "text/plain", "Configuration is too large to display");
    return;
  }
  ESP32_SMA_Inverter_App::webServer.send(200, "text/html", page.data());
}

void ESP32_SMA_MQTT::handleSetClock() {
  if (!requireWebAuthentication()) return;
  if (ESP32_SMA_Inverter_App::getInstance().isPolling()) {
    ESP32_SMA_Inverter_App::webServer.send(503, "text/plain", "Inverter read in progress; retry the clock request shortly");
    return;
  }
  if (!ESP32_SMA_Inverter_App::webServer.hasArg("token") ||
      ESP32_SMA_Inverter_App::webServer.arg("token") != clockSyncToken) {
    ESP32_SMA_Inverter_App::webServer.send(403, "text/plain", "Invalid or expired request token");
    return;
  }

  ESP32_SMA_Inverter_App::getInstance().requestClockSync();
  String response = ESP32_SMA_Inverter_App::getInstance().getClockSyncStatus();
  response += "\n\nReturn to the main page to check the verified result.";
  ESP32_SMA_Inverter_App::webServer.send(202, "text/plain", response);
}

// Function to extract the configuration
void ESP32_SMA_MQTT::handleForm() {
  if (!requireWebAuthentication()) return;
  if (ESP32_SMA_Inverter_App::getInstance().isPolling()) {
    ESP32_SMA_Inverter_App::webServer.send(503, "text/plain", "Inverter read in progress; retry saving shortly");
    return;
  }
  AppConfig& saved = ESP32_SMA_Inverter_App::getInstance().appConfig;
  AppConfig config = saved;

  logW("Connect handleForm\n");
  if (ESP32_SMA_Inverter_App::webServer.method() != HTTP_POST) {
    ESP32_SMA_Inverter_App::webServer.send(405, "text/plain", "Method Not Allowed");
  } else {
    if (settingsToken.isEmpty() || !ESP32_SMA_Inverter_App::webServer.hasArg("token") ||
        ESP32_SMA_Inverter_App::webServer.arg("token") != settingsToken) {
      ESP32_SMA_Inverter_App::webServer.send(403, "text/plain", "Invalid or expired request token");
      return;
    }
    logW("POST form was:");
    const int count = ESP32_SMA_Inverter_App::webServer.args();
    if (count > 24) {
      ESP32_SMA_Inverter_App::webServer.send(413, "text/plain", "Too many settings");
      return;
    }
    config.hassDisc = false;
    for (int i = 0; i < count; ++i) {
      String name = ESP32_SMA_Inverter_App::webServer.argName(i);
      String v = ESP32_SMA_Inverter_App::webServer.arg(i);
      if (name != "mqttPasswd" && name != "smapw") v.trim();
      logW("Received setting: %s", name.c_str());
      if (name == "mqttBroker") {
        config.mqttBroker = v.substring(0, 128);
      } else if (name == "mqttPort") {
        long long port;
        if (!parseIntegerSetting(v, port) || port < 1 || port > 65535) {
          ESP32_SMA_Inverter_App::webServer.send(400, "text/plain", "Invalid MQTT port");
          return;
        }
        config.mqttPort = static_cast<uint16_t>(port);
      } else if (name == "mqttUser") {
        config.mqttUser = v.substring(0, 128);
      } else if (name == "mqttPasswd") {
        if (v.length() > 0) config.mqttPasswd = v.substring(0, 128);
      } else if (name == "mqttTopic") {
        if (!validMqttPrefix(v)) {
          ESP32_SMA_Inverter_App::webServer.send(400, "text/plain", "Topic prefix must use 1-32 letters, digits, underscores or hyphens");
          return;
        }
        config.mqttTopic = v;
      } else if (name == "btaddress") {
        uint8_t octets[6];
        if (!parseSmaBluetoothAddress(v, octets)) {
          ESP32_SMA_Inverter_App::webServer.send(400, "text/plain", "Invalid Bluetooth address");
          return;
        }
        config.smaBTAddress = v;
      } else if (name == "smapw") {
        if (v.length() > 0 && v.length() <= 12) config.smaInvPass = v;
      } else if (name == "scanRate") {
        long long scanRate;
        if (!parseIntegerSetting(v, scanRate) || scanRate < INT_MIN || scanRate > INT_MAX) {
          ESP32_SMA_Inverter_App::webServer.send(400, "text/plain", "Invalid inverter scan rate");
          return;
        }
        config.scanRate = static_cast<int>(constrain(scanRate, 10LL, 3600LL));
      } else if (name == "hassDisc") {
        logW("%s\n",v.c_str());
        config.hassDisc = true;
      } else if (name == "timezone") {
        float timezone;
        if (!parseFiniteFloatSetting(v, timezone)) {
          ESP32_SMA_Inverter_App::webServer.send(400, "text/plain", "Invalid timezone");
          return;
        }
        config.timezone = constrain(timezone, -12.0f, 14.0f);
      } else if (name == "ntphostname") {
        if (!validHostname(v)) {
          ESP32_SMA_Inverter_App::webServer.send(400, "text/plain", "Invalid NTP hostname");
          return;
        }
        config.ntphostname = v;
      }

    }
    AppConfig previous = saved;
    saved = config;
    if (!ESP32_SMA_Inverter_App::getInstance().saveConfiguration()) {
      saved = previous;
      ESP32_SMA_Inverter_App::webServer.send(500, "text/plain", "Could not save settings; please retry");
      return;
    }
    ESP32_SMA_Inverter_App::webServer.send(200, "text/plain", "Settings saved; restarting");
    delay(3000);
    ESP.restart();
  }
}

bool ESP32_SMA_MQTT::brokerConnect() {
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
  if(config.mqttBroker.length() < 1 ){
    return false;
  }
  logW("Connecting to MQTT Broker");

  ESP32_SMA_Inverter_App::client.setServer(config.mqttBroker.c_str(), config.mqttPort);

  // client.setCallback(callback);
  if (!ESP32_SMA_Inverter_App::client.connected()) {
      espDiscoveryPublished = false;
      logW("The client %s connects to the mqtt broker %s ", mqttInstance.sapString.c_str(), config.mqttBroker.c_str());
      // If there is a user account
      if(config.mqttUser.length() > 0){
        logW(" with user/password\n");
        if (!ESP32_SMA_Inverter_App::client.connect(mqttInstance.sapString.c_str(),config.mqttUser.c_str(),config.mqttPasswd.c_str())) {
          log_e("mqtt connect failed with state %i",ESP32_SMA_Inverter_App::client.state());
        }
      } else {
        logW(" without user/password ");
        if (!ESP32_SMA_Inverter_App::client.connect(mqttInstance.sapString.c_str())) {
          log_e("mqtt connect failed with state %i", ESP32_SMA_Inverter_App::client.state());
        }
      }
      if (ESP32_SMA_Inverter_App::client.connected()) {
        ESP32_SMA_Inverter_App::client.subscribe("homeassistant/status");
        ESP32_SMA_Inverter_App::getInstance().requestDiscovery();
      }
    }
  return ESP32_SMA_Inverter_App::client.connected();
}

bool ESP32_SMA_MQTT::saveDiscoveryIdentity(const String& identity) {
  Preferences store;
  if (!store.begin("sma-discovery", false)) return false;
  const size_t length = identity.length() + 1;
  const bool saved = store.putBytes("identity", identity.c_str(), length) == length;
  store.end();
  return saved;
}

bool ESP32_SMA_MQTT::loadDiscoveryIdentity() {
  if (!ESP32_SMA_Inverter_App::getInstance().configurationStorageReady()) return false;
  if (discoveryIdentityLoaded) return true;

  // Preferences::getBytesLength() returns zero for both NOT_FOUND and real
  // NVS errors. Only a genuinely absent key is safe to migrate from config.
  nvs_handle_t store;
  const esp_err_t openResult = nvs_open("sma-discovery", NVS_READWRITE, &store);
  if (openResult != ESP_OK) {
    logW("Could not open discovery identity storage; preserving the saved identity");
    return false;
  }

  char identity[44] = {};
  size_t length = 0;
  const esp_err_t lengthResult = nvs_get_blob(store, "identity", nullptr, &length);
  const bool identityMissing = lengthResult == ESP_ERR_NVS_NOT_FOUND;
  if (!identityMissing && lengthResult != ESP_OK) {
    logW("Could not read discovery identity length (NVS error %d); preserving the saved identity",
         static_cast<int>(lengthResult));
    nvs_close(store);
    return false;
  }
  if (!identityMissing) {
    if (length == 0 || length > sizeof(identity)) {
      logW("Invalid discovery identity length %u; preserving the saved identity",
           static_cast<unsigned>(length));
      nvs_close(store);
      return false;
    }
    size_t actualLength = length;
    const esp_err_t readResult = nvs_get_blob(store, "identity", identity, &actualLength);
    if (readResult != ESP_OK) {
      logW("Could not read discovery identity (NVS error %d); preserving the saved identity",
           static_cast<int>(readResult));
      nvs_close(store);
      return false;
    }
    if (actualLength != length || identity[length - 1] != '\0' || strlen(identity) + 1 != length) {
      logW("Invalid discovery identity contents; preserving the saved identity");
      nvs_close(store);
      return false;
    }
  }
  nvs_close(store);

  if (identity[0]) {
    const char *separator = strrchr(identity, '-');
    if (!separator || !validMqttPrefix(String(identity).substring(0, separator - identity))) {
      logW("Invalid discovery identity prefix; preserving the saved identity");
      return false;
    }
    const char *digits = separator + 1;
    if (!*digits) {
      logW("Invalid discovery identity serial; preserving the saved identity");
      return false;
    }
    for (const char *p = digits; *p; ++p) if (*p < '0' || *p > '9') {
      logW("Invalid discovery identity serial; preserving the saved identity");
      return false;
    }
    const unsigned long long serial = strtoull(digits, nullptr, 10);
    if (serial == 0 || serial > UINT32_MAX) {
      logW("Invalid discovery identity serial; preserving the saved identity");
      return false;
    }
  }
  discoveryIdentity = identity;
  if (identityMissing) {
    // Older firmware did not record this identity. Capture the loaded settings
    // before a web save can change their topic prefix or discovery flag.
    const AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
    if (config.thisSerial != 0 && validMqttPrefix(config.mqttTopic)) {
      discoveryIdentity = config.mqttTopic + "-" + String(config.thisSerial);
    }
    if (!saveDiscoveryIdentity(discoveryIdentity)) return false;
  }
  discoveryIdentityLoaded = true;
  return true;
}

bool ESP32_SMA_MQTT::removeDiscovery(const String& identity) {
  // Includes both inverter sensors and diagnostics that share its device.
  const char *ids[] = {"Pac", "Iac0", "Iac1", "Iac2", "Uac0", "Uac1", "Uac2", "Freq",
      "Wdc0", "Wdc1", "Udc0", "Udc1", "Idc0", "Idc1", "EToday", "ETotal", "InvTemp",
      "DevStatus", "GridRelay", "BTStrength", "esp_ip", "esp_wifi_rssi", "esp_uptime", "esp_free_heap", "esp_time"};
  if (!identity.isEmpty()) {
    for (const char *id : ids) {
      char topic[128];
      snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", identity.c_str(), id);
      if (!ESP32_SMA_Inverter_App::client.publish(topic, "", true)) return false;
    }
  }
  for (const char *id : {"esp_ip", "esp_wifi_rssi", "esp_uptime", "esp_free_heap", "esp_time"}) {
    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", sapString.c_str(), id);
    if (!ESP32_SMA_Inverter_App::client.publish(topic, "", true)) return false;
  }
  return true;
}

bool ESP32_SMA_MQTT::prepareDiscovery(const AppConfig& config) {
  // A mount failure can substitute defaults for a previously enabled flag.
  // Preserve retained entities until the committed settings can be recovered.
  if (!ESP32_SMA_Inverter_App::getInstance().configurationStorageReady()) return false;
  const uint32_t serial = ESP32_SMA_Inverter::invData.Serial != 0
      ? ESP32_SMA_Inverter::invData.Serial : config.thisSerial;
  const String desired = config.hassDisc && serial != 0
      ? config.mqttTopic + "-" + String(serial) : String("");
  if (discoveryIdentityLoaded && discoveryIdentity == desired) {
    discoveryMigrationAttempted = false;
    return true;
  }
  const uint32_t now = millis();
  if (discoveryMigrationAttempted &&
      static_cast<uint32_t>(now - lastDiscoveryMigrationAttemptMillis) < 5000UL) return false;
  lastDiscoveryMigrationAttemptMillis = now;
  discoveryMigrationAttempted = true;
  if (!loadDiscoveryIdentity()) return false;
  if (discoveryIdentity == desired) {
    discoveryMigrationAttempted = false;
    return true;
  }
  if (!brokerConnect() || !removeDiscovery(discoveryIdentity) || !saveDiscoveryIdentity(desired)) return false;
  // Record the target before its first retained publish, so partial discovery
  // remains removable after a reboot. Never forget an uncleared old identity.
  discoveryIdentity = desired;
  discoveryMigrationAttempted = false;
  espDiscoveryPublished = false;
  espDiscoveredSerial = 0;
  ESP32_SMA_Inverter_App::getInstance().requestDiscovery();
  return true;
}

// ESP status is published independently of inverter polling, including at night.
bool ESP32_SMA_MQTT::publishEspDiscovery(const char *stateTopic) {
  if (!prepareDiscovery(ESP32_SMA_Inverter_App::getInstance().appConfig)) return false;
  struct Sensor {
    const char *id;
    const char *name;
    const char *field;
    const char *deviceClass;
    const char *unit;
  };
  const Sensor sensors[] = {
    {"ip", "IP address", "IP", nullptr, nullptr},
    {"wifi_rssi", "Wi-Fi signal", "WiFiRSSI", "signal_strength", "dBm"},
    {"uptime", "Uptime", "Uptime", "duration", "s"},
    {"free_heap", "Free heap", "FreeHeap", "data_size", "B"},
  };
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  const uint32_t serial = invData.Serial != 0 ? invData.Serial : config.thisSerial;
  if (serial == 0) return false;
  const String inverterId = config.mqttTopic + "-" + String(serial);
  // Clear the retired clock sensor from both discovery layouts.
  char oldTimeTopic[128];
  char inverterTimeTopic[128];
  snprintf(oldTimeTopic, sizeof(oldTimeTopic),
           "homeassistant/sensor/%s/esp_time/config", sapString.c_str());
  snprintf(inverterTimeTopic, sizeof(inverterTimeTopic),
           "homeassistant/sensor/%s/esp_time/config", inverterId.c_str());
  if (!ESP32_SMA_Inverter_App::client.publish(oldTimeTopic, "", true) ||
      !ESP32_SMA_Inverter_App::client.publish(inverterTimeTopic, "", true)) return false;
  // Retained configs from older firmware keep the separate ESP32 device alive.
  // Remove them before publishing the same entities under the inverter device.
  for (const Sensor& sensor : sensors) {
    char oldTopic[128];
    snprintf(oldTopic, sizeof(oldTopic),
             "homeassistant/sensor/%s/esp_%s/config", sapString.c_str(), sensor.id);
    if (!ESP32_SMA_Inverter_App::client.publish(oldTopic, "", true)) return false;
  }
  bool success = true;
  for (const Sensor& sensor : sensors) {
    char discoveryTopic[128];
    snprintf(discoveryTopic, sizeof(discoveryTopic),
             "homeassistant/sensor/%s/esp_%s/config", inverterId.c_str(), sensor.id);
    DynamicJsonDocument discovery(1536);
    discovery["name"] = sensor.name;
    discovery["state_topic"] = stateTopic;
    discovery["value_template"] = String("{{ value_json.") + sensor.field + " }}";
    discovery["unique_id"] = sapString + "-esp-" + sensor.id;
    discovery["entity_category"] = "diagnostic";
    discovery["expire_after"] = 180;
    if (sensor.deviceClass) discovery["device_class"] = sensor.deviceClass;
    if (sensor.unit) discovery["unit_of_measurement"] = sensor.unit;
    JsonObject device = discovery.createNestedObject("device");
    device.createNestedArray("identifiers").add(inverterId);
    device["name"] = inverterId;
    device["manufacturer"] = "SMA";
    device["configuration_url"] = String("http://") + WiFi.localIP().toString() + "/";
    char payload[768];
    size_t length = measureJson(discovery);
    if (discovery.overflowed() || length >= sizeof(payload)) {
      success = false;
      continue;
    }
    serializeJson(discovery, payload, sizeof(payload));
    if (!ESP32_SMA_Inverter_App::client.publish(discoveryTopic, payload, true)) {
      success = false;
    }
  }
  return success;
}

bool ESP32_SMA_MQTT::publishEspStatus(bool serviceDiscovery) {
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;
  if (config.mqttBroker.isEmpty() || WiFi.status() != WL_CONNECTED) return false;
  if (!serviceDiscovery && !ESP32_SMA_Inverter_App::client.connected()) return false;

  const unsigned long nowMillis = millis();
  if (lastEspStatusMillis != 0 && nowMillis - lastEspStatusMillis < 60000UL) return true;
  lastEspStatusMillis = nowMillis;
  if (serviceDiscovery && !brokerConnect()) return false;

  char stateTopic[96];
  snprintf(stateTopic, sizeof(stateTopic), "sma/solar/%s/esp/state", sapString.c_str());
  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  const uint32_t serial = invData.Serial != 0 ? invData.Serial : config.thisSerial;
  if (serviceDiscovery && config.hassDisc && serial != 0 &&
      (!espDiscoveryPublished || espDiscoveredSerial != serial)) {
    espDiscoveryPublished = publishEspDiscovery(stateTopic);
    if (espDiscoveryPublished) espDiscoveredSerial = serial;
  }

  StaticJsonDocument<256> status;
  status["IP"] = WiFi.localIP().toString();
  status["WiFiRSSI"] = WiFi.RSSI();
  status["Uptime"] = (uint64_t)(esp_timer_get_time() / 1000000LL);
  status["FreeHeap"] = ESP.getFreeHeap();
  char payload[256];
  size_t length = serializeJson(status, payload, sizeof(payload));
  return length > 0 && ESP32_SMA_Inverter_App::client.publish(
      stateTopic, reinterpret_cast<const uint8_t*>(payload), length, false);
}

// Returns true if nighttime
void ESP32_SMA_MQTT::loadEnergyBaseline(uint32_t serial) {
  energySerial = serial;
  lastAcceptedETotalWh = lastPersistedETotalWh = 0;
  energyBaselineLoaded = false;
  energyBaselinePersisted = false;
  if (serial == 0) return;

  // NVS reports NOT_FOUND separately from real read errors; Preferences' size
  // helpers collapse both into zero, which could mistake a damaged fence for
  // a new device and permit a lifetime-energy decrease after reboot.
  nvs_handle_t store;
  const esp_err_t openResult = nvs_open("sma-mqtt", NVS_READWRITE, &store);
  // Opening read-write creates an absent namespace on a new installation.
  if (openResult != ESP_OK) {
    logW("Could not open lifetime-energy storage; ETotal stays unavailable until retry");
    return;
  }

  uint8_t record[12];
  bool baselineFound = false;
  auto readBlob = [&](const char *key, uint8_t *value, size_t expectedLength, bool &found) {
    size_t length = 0;
    const esp_err_t result = nvs_get_blob(store, key, nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
      found = false;
      return true;
    }
    if (result != ESP_OK || length != expectedLength) return false;
    if (nvs_get_blob(store, key, value, &length) != ESP_OK || length != expectedLength) return false;
    found = true;
    return true;
  };
  bool blobFound = false;
  if (!readBlob("energy-v2", record, sizeof(record), blobFound)) {
    logW("Could not read lifetime-energy fence; ETotal stays unavailable until retry");
    nvs_close(store);
    return;
  }
  if (blobFound) {
    if (get_u32(record) == serial) {
      lastAcceptedETotalWh = lastPersistedETotalWh = get_u64(record + 4);
      energyBaselinePersisted = true;
      baselineFound = true;
    }
  }

  // Migrate both prior storage formats. Their value remains the lower bound;
  // the first eligible reading writes an exact v2 fence before publication.
  if (!baselineFound) {
    if (!readBlob("energy-v1", record, sizeof(record), blobFound)) {
      logW("Could not read legacy lifetime-energy fence; ETotal stays unavailable until retry");
      nvs_close(store);
      return;
    }
    if (blobFound) {
      if (get_u32(record) == serial) {
        lastAcceptedETotalWh = lastPersistedETotalWh = get_u64(record + 4);
        baselineFound = true;
      }
    }
    if (!baselineFound) {
      uint32_t legacySerial = 0;
      uint64_t legacyEnergy = 0;
      const esp_err_t serialResult = nvs_get_u32(store, "serial", &legacySerial);
      const esp_err_t energyResult = nvs_get_u64(store, "etotal", &legacyEnergy);
      const bool hasLegacySerial = serialResult == ESP_OK;
      const bool hasLegacyEnergy = energyResult == ESP_OK;
      const bool serialMissing = serialResult == ESP_ERR_NVS_NOT_FOUND;
      const bool energyMissing = energyResult == ESP_ERR_NVS_NOT_FOUND;
      if ((!serialMissing && !hasLegacySerial) || (!energyMissing && !hasLegacyEnergy)) {
        logW("Could not read legacy lifetime-energy fence; ETotal stays unavailable until retry");
        nvs_close(store);
        return;
      }
      if (hasLegacySerial || hasLegacyEnergy) {
        if (!hasLegacySerial || !hasLegacyEnergy) {
          logW("Could not read legacy lifetime-energy fence; ETotal stays unavailable until retry");
          nvs_close(store);
          return;
        }
        if (legacySerial == serial) {
          lastAcceptedETotalWh = lastPersistedETotalWh = legacyEnergy;
          baselineFound = true;
        }
      }
    }
  }
  energyBaselineLoaded = true;
  nvs_close(store);
}

bool ESP32_SMA_MQTT::persistEnergyBaseline(uint32_t serial, uint64_t energyWh) {
  if (!energyBaselineLoaded || serial == 0 || serial != energySerial) return false;
  nvs_handle_t store;
  if (nvs_open("sma-mqtt", NVS_READWRITE, &store) != ESP_OK) {
    logW("Could not open lifetime-energy storage; ETotal stays unavailable until retry");
    return false;
  }
  uint8_t record[12];
  for (size_t k = 0; k < 4; ++k) record[k] = serial >> (8 * k);
  for (size_t k = 0; k < 8; ++k) record[k + 4] = energyWh >> (8 * k);
  // v2 stores the exact high-water mark before it can enter an MQTT payload.
  bool stored = nvs_set_blob(store, "energy-v2", record, sizeof(record)) == ESP_OK;
  if (stored) stored = nvs_commit(store) == ESP_OK;
  nvs_close(store);
  if (!stored) {
    logW("Lifetime-energy fence write failed; ETotal stays unavailable until retry");
    return false;
  }
  lastAcceptedETotalWh = lastPersistedETotalWh = energyWh;
  energyBaselinePersisted = true;
  return true;
}

bool ESP32_SMA_MQTT::publishData(const InverterData *reading, const DisplayData *display){
  const InverterData& invData = reading ? *reading : ESP32_SMA_Inverter::getInstance().invData;
  const DisplayData& dispData = display ? *display : ESP32_SMA_Inverter::getInstance().dispData;
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;

  if(config.mqttBroker.length() < 1 ){
    return false;
  }

  if (energySerial != invData.Serial || !energyBaselineLoaded) loadEnergyBaseline(invData.Serial);
  const bool acceptedEnergy = energyBaselineLoaded && invData.Serial != 0 && invData.ETotalValid &&
      (lastAcceptedETotalWh == 0 || invData.ETotal >= lastAcceptedETotalWh);
  if (!acceptedEnergy) logW("Omitting unavailable or decreased lifetime energy counter");

  if (brokerConnect()){
    bool publishEnergy = acceptedEnergy;
    if (publishEnergy && (!energyBaselinePersisted || invData.ETotal > lastPersistedETotalWh)) {
      // Persist before setting ETotal below. If NVS is unavailable, the rest of
      // the live payload still publishes with only its lifetime total omitted.
      publishEnergy = persistEnergyBaseline(invData.Serial, invData.ETotal);
    }
    char theData[2000];
    DynamicJsonDocument data(1536);
    data["Serial"] = invData.Serial;
    auto number = [](JsonVariant target, double value) {
      if (std::isfinite(value)) target.set(value); else target.set(nullptr);
    };
    number(data["BTStrength"].to<JsonVariant>(), dispData.BTSigStrength);
    number(data["Pac"].to<JsonVariant>(), dispData.Pac);
    number(data["Freq"].to<JsonVariant>(), dispData.Freq);
    number(data["InvTemp"].to<JsonVariant>(), dispData.InvTemp);
    const float *arrays[] = {dispData.Uac, dispData.Iac, dispData.Udc, dispData.Idc};
    const char *names[] = {"Uac", "Iac", "Udc", "Idc"};
    for (size_t a = 0; a < 4; ++a) {
      JsonArray values = data.createNestedArray(names[a]);
      for (size_t k = 0; k < (a < 2 ? 3U : 2U); ++k) number(values.add(), arrays[a][k]);
    }
    JsonArray power = data.createNestedArray("Wdc");
    for (size_t k = 0; k < 2; ++k) number(power.add(), double(dispData.Udc[k]) * dispData.Idc[k]);
    if (invData.ETodayValid) data["EToday"] = double(invData.EToday) / 1000.0;
    else data["EToday"] = nullptr;
    if (publishEnergy) data["ETotal"] = double(invData.ETotal) / 1000.0;
    else data["ETotal"] = nullptr;
    if (invData.DevStatus != 0xFFFFFD) data["DevStatus"] = getInverterCode(invData.DevStatus);
    else data["DevStatus"] = nullptr;
    if (invData.GridRelay != 0xFFFFFD) data["GridRelay"] = getInverterCode(invData.GridRelay);
    else data["GridRelay"] = nullptr;
    if (data.overflowed() || measureJson(data) >= sizeof(theData)) return false;
    serializeJson(data, theData, sizeof(theData));

    // strcat(theData,"}");
    char topic[100];
    if (config.hassDisc)
      snprintf(topic,sizeof(topic), "sma/solar/%s-%lu/state",config.mqttTopic.c_str(), (unsigned long)invData.Serial);
    else
      snprintf(topic,sizeof(topic), "%s-%lu/state",config.mqttTopic.c_str(), (unsigned long)invData.Serial);
    logI("%s = %s",topic, theData);
    int len = strlen(theData);
    if (ESP32_SMA_Inverter_App::client.publish(topic, reinterpret_cast<const uint8_t*>(theData), len, false)) {
      logI("Published\n");
      return true;
    } else {
      logW("Failed Publish\n");
    }
  }
  return false;
}

void ESP32_SMA_MQTT::logViaMQTT(const char *logStr){
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;

  char tmp[1000];
  if(config.mqttBroker.length() < 1 ){
    return;
  }
  snprintf(tmp,sizeof(tmp),"{ \"Log\": \"%s\" }",logStr);
  brokerConnect();

  if (ESP32_SMA_Inverter_App::client.connected()){

    // strcat(theData,"}");
    char topic[100];
    snprintf(topic,sizeof(topic), "sma/solar/%s-%lu/log",config.mqttTopic.c_str(), (unsigned long)config.thisSerial);
    logI("%s = ", topic);
    logI(" %s\n",tmp);
    int len = strlen(tmp);
    if (ESP32_SMA_Inverter_App::client.publish(topic, reinterpret_cast<const uint8_t*>(tmp), len, false))
      logI("Published\n");
    else
      logW("Failed Publish\n");
  }

}


// Set up the topics in home assistant
bool ESP32_SMA_MQTT::hassAutoDiscover(int timeout){

  InverterData& invData = ESP32_SMA_Inverter::getInstance().invData;
  DisplayData& dispData = ESP32_SMA_Inverter::getInstance().dispData;
  AppConfig& config = ESP32_SMA_Inverter_App::getInstance().appConfig;

  char msg[768];
  char topic[50];
  if (!prepareDiscovery(config) || !brokerConnect()) return false;
  discoveryPublishOK = true;
  if (!validMqttPrefix(config.mqttTopic)) return false;

  const uint32_t serial = invData.Serial != 0 ? invData.Serial : config.thisSerial;
  if (serial == 0) return false;
  snprintf(topic,sizeof(topic), "%s-%lu", config.mqttTopic.c_str(), (unsigned long)serial);
  const size_t msg_size = sizeof(msg);

    sendHassAuto(msg, msg_size, timeout, topic, "power", "measurement", "true", "AC Power", "W", "Pac", "Pac");
    sendHassAuto(msg, msg_size, timeout, topic, "current", "measurement", "true", "A Phase Current", "A", "Iac[0]", "Iac0");
    sendHassAuto(msg, msg_size, timeout, topic, "current", "measurement", "true", "B Phase Current", "A", "Iac[1]", "Iac1");
    sendHassAuto(msg, msg_size, timeout, topic, "current", "measurement", "true", "C Phase Current", "A", "Iac[2]", "Iac2");
    sendHassAuto(msg, msg_size, timeout, topic, "voltage", "measurement", "true", "A Phase Voltage", "V", "Uac[0]", "Uac0");
    sendHassAuto(msg, msg_size, timeout, topic, "voltage", "measurement", "true", "B Phase Voltage", "V", "Uac[1]", "Uac1");
    sendHassAuto(msg, msg_size, timeout, topic, "voltage", "measurement", "true", "C Phase Voltage", "V", "Uac[2]", "Uac2");
    sendHassAuto(msg, msg_size, timeout, topic, "frequency", "measurement", "true", "AC Frequency", "Hz", "Freq", "Freq");
    sendHassAuto(msg, msg_size, timeout, topic, "power", "measurement", "true", "DC Power (String 1)", "W", "Wdc[0]", "Wdc0");
    sendHassAuto(msg, msg_size, timeout, topic, "power", "measurement", "true", "DC Power (String 2)", "W", "Wdc[1]", "Wdc1");
    sendHassAuto(msg, msg_size, timeout, topic, "voltage", "measurement", "true", "DC Voltage (String 1)", "V", "Udc[0]", "Udc0");
    sendHassAuto(msg, msg_size, timeout, topic, "voltage", "measurement", "true", "DC Voltage (String 2)", "V", "Udc[1]", "Udc1");
    sendHassAuto(msg, msg_size, timeout, topic, "current", "measurement", "true", "DC Current (String 1)", "A", "Idc[0]", "Idc0");
    sendHassAuto(msg, msg_size, timeout, topic, "current", "measurement", "true", "DC Current (String 2)", "A", "Idc[1]", "Idc1");

    sendHassAuto(msg, msg_size, timeout, topic, "energy", "total_increasing", "true", "kWh Today", "kWh", "EToday", "EToday");
    sendHassAuto(msg, msg_size, timeout, topic, "energy", "total_increasing", "true", "kWh Total", "kWh", "ETotal", "ETotal");

    sendHassAuto(msg, msg_size, timeout, topic, "temperature", "measurement", "true", "Inverter Temperature", "°C", "InvTemp", "InvTemp");
    sendHassAutoNoClassNoUnit(msg, msg_size, timeout, topic, "Device Status", "DevStatus", "DevStatus");
    sendHassAutoNoClassNoUnit(msg, msg_size, timeout, topic, "Grid Relay Status", "GridRelay", "GridRelay");
    sendHassAutoNoClass(msg, msg_size, timeout, topic, "Bluetooth", "%", "BTStrength", "BTStrength");

    return discoveryPublishOK && ESP32_SMA_Inverter_App::client.connected();
}

void ESP32_SMA_MQTT::sendHassDiscovery(char *msg, size_t size, int timeout, const char *topic,
                                     const char *name, const char *unit, const char *sensor,
                                     const char *id, const char *deviceClass, const char *stateClass, bool force) {
  DynamicJsonDocument doc(2048);
  doc["name"] = name;
  const String stateTopic = String("sma/solar/") + topic + "/state";
  doc["state_topic"] = stateTopic;
  doc["expire_after"] = timeout;
  doc["value_template"] = String("{{ value_json.") + sensor + " }}";
  doc["unique_id"] = String(topic) + "-" + id;
  if (unit) doc["unit_of_measurement"] = unit;
  if (deviceClass) doc["device_class"] = deviceClass;
  if (stateClass) doc["state_class"] = stateClass;
  if (force) doc["force_update"] = true;
  doc["availability_topic"] = stateTopic;
  doc["availability_template"] = String("{{ 'online' if value_json.") + sensor +
      " is defined and value_json." + sensor + " is not none else 'offline' }}";
  JsonObject device = doc.createNestedObject("device");
  device.createNestedArray("identifiers").add(topic);
  device["name"] = topic;
  device["manufacturer"] = "SMA";
  if (doc.overflowed() || measureJson(doc) >= size) { discoveryPublishOK = false; return; }
  serializeJson(doc, msg, size);
  discoveryPublishOK &= sendLongMQTT(topic, id, msg);
}

void ESP32_SMA_MQTT::sendHassAutoNoClassNoUnit(char *msg, size_t size, int timeout, const char *topic, const char *name, const char *sensor, const char *id) {
  sendHassDiscovery(msg, size, timeout, topic, name, nullptr, sensor, id, nullptr, nullptr, false);
}

void ESP32_SMA_MQTT::sendHassAutoNoClass(char *msg, size_t size, int timeout, const char *topic, const char *name, const char *unit, const char *sensor, const char *id) {
  sendHassDiscovery(msg, size, timeout, topic, name, unit, sensor, id, nullptr, nullptr, false);
}

void ESP32_SMA_MQTT::sendHassAuto(char *msg, size_t size, int timeout, const char *topic, const char *deviceClass, const char *stateClass, const char *force,
                              const char *name, const char *unit, const char *sensor, const char *id) {
  sendHassDiscovery(msg, size, timeout, topic, name, unit, sensor, id, deviceClass, stateClass, strcmp(force, "true") == 0);
}

bool ESP32_SMA_MQTT::sendLongMQTT(const char *topic, const char *postscript, const char *msg){
  int len = strlen(msg);
  char tmpstr[100];
  snprintf(tmpstr,sizeof(tmpstr),"homeassistant/sensor/%s/%s/config",topic,postscript);
  logI("%s:  %s... ",tmpstr,msg);
   if (ESP32_SMA_Inverter_App::client.publish(tmpstr, reinterpret_cast<const uint8_t*>(msg), len, true)) {
      logI("Published\n");
      delay(50);
      return true;
    } else {
      logW("Failed Publish\n");
    }
    return false;
}
