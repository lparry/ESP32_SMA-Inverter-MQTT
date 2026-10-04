#include "ESP32_SMA_Inverter_App.h"
#include "BluetoothAuthObserver.h"
#include "gap_sdk_fake.h"
#include <cassert>
#include <cmath>
#include <iostream>
// A fixed host clock makes CRC collision and clock-write tests deterministic.
#if defined(__APPLE__)
#define SMA_TEST_TIME_NOEXCEPT
#else
#define SMA_TEST_TIME_NOEXCEPT noexcept
#endif
extern "C" time_t time(time_t *out) SMA_TEST_TIME_NOEXCEPT {
 const time_t now=1800000000;if(out)*out=now;return now;
}
struct AfterProtocolHeader { char c; uint32_t value; };
static_assert(sizeof(L1Hdr)==18, "SMA wire header must remain packed");
static_assert(alignof(AfterProtocolHeader)==alignof(uint32_t), "packing must be restored");
std::vector<uint8_t> l1Frame(const std::vector<uint8_t>&payload,uint16_t command=1,const uint8_t*source=nullptr){
 uint16_t n=18+payload.size();std::vector<uint8_t> header(18,0);
 header[0]=0x7e;header[1]=n;header[2]=n>>8;header[3]=header[0]^header[1]^header[2];
 header[16]=command;header[17]=command>>8;
 if(source)std::copy(source,source+6,header.begin()+4);
 header.insert(header.end(),payload.begin(),payload.end());return header;
}
void queueL1(BluetoothSerial&b,const std::vector<uint8_t>&payload,uint16_t command=1,const uint8_t*source=nullptr){
 auto frame=l1Frame(payload,command,source);b.inject(frame.data(),frame.size());
}
void put(std::vector<uint8_t>&v,size_t offset,uint64_t n,size_t width=4){
 for(size_t k=0;k<width;k++)v.at(offset+k)=n>>(8*k);
}
std::vector<uint8_t> response(ESP32_SMA_Inverter&i,const std::vector<uint8_t>&data,uint32_t first=1,uint32_t last=1,uint16_t status=0){
 std::vector<uint8_t> v(41+data.size());v[0]=0x7e;put(v,1,BTH_L2SIGNATURE);v[5]=9+data.size()/4;
 put(v,15,i.invData.SUSyID,2);put(v,17,i.invData.Serial);put(v,23,status,2);put(v,27,i.pcktID,2);
 put(v,33,first);put(v,37,last);std::copy(data.begin(),data.end(),v.begin()+41);return v;
}
void queueResponse(BluetoothSerial&b,std::vector<uint8_t> v,bool corrupt=false,const uint8_t*source=nullptr){
 uint16_t crc=0xffff;
 for(size_t k=1;k<v.size();k++){crc^=v[k];for(int bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0x8408:0);}
 crc^=0xffff;if(corrupt)crc^=1;v.push_back(crc);v.push_back(crc>>8);
 std::vector<uint8_t> wire{0x7e};
 for(size_t k=1;k<v.size();k++){auto c=v[k];if(c==0x7d||c==0x7e||c==0x11||c==0x12||c==0x13){wire.push_back(0x7d);wire.push_back(c^0x20);}else wire.push_back(c);}
 wire.push_back(0x7e);queueL1(b,wire,1,source?source:ESP32_SMA_Inverter::invData.BTAddress);
}
std::vector<uint8_t> loginResponse(ESP32_SMA_Inverter&i,uint16_t susyId,uint32_t serial){
 std::vector<uint8_t> data(8);put(data,0,uint32_t(time(nullptr)));
 auto v=response(i,data);put(v,15,susyId,2);put(v,17,serial);put(v,29,0xFFFD040D);return v;
}
E_RC query(const std::vector<uint8_t>&data,uint32_t first=1,uint32_t last=1,uint16_t status=0){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,response(i,data,first,last,status));b.output.clear();};
 auto rc=i.getInverterDataCfl(0x51000200,first,last);b.sent=nullptr;return rc;
}
void testSaveFailures(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();
 a.loadConfiguration();
 assert(a.saveConfiguration());
 const auto good=fake::files["/config.txt"];
 a.appConfig.mqttBroker="changed";
 for(size_t limit:{size_t(0),size_t(40)}){
  fake::writeLimit=limit;assert(!a.saveConfiguration());assert(fake::files["/config.txt"]==good);
 }
 fake::writeLimit=SIZE_MAX;
 a.appConfig.mqttBroker=std::string(3000,'x');
 assert(!a.saveConfiguration());assert(fake::files["/config.txt"]==good);
 a.appConfig.mqttBroker="broker";
 assert(a.saveConfiguration());
}
void testConfigRecovery(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();
 a.loadConfiguration();a.saveConfiguration();auto good=fake::files["/config.txt"];
 for(bool corrupt:{false,true}){
  fake::files.clear();fake::files["/config.bak"]=good;
  fake::files["/config.tmp"]="{partial";
  if(corrupt)fake::files["/config.txt"]="{";
  assert(a.loadConfiguration());assert(fake::files["/config.txt"]==good);
  assert(a.saveConfiguration());
 }
 fake::files.clear();fake::files["/config.tmp"]=good;assert(a.loadConfiguration());
 assert(fake::files["/config.txt"]==good);
 fake::files.clear();fake::files["/config.bak"]=good;fake::failRenameFrom="/config.bak";
 assert(!a.loadConfiguration());assert(!a.saveConfiguration());
 assert(fake::files["/config.bak"]==good);fake::failRenameFrom="";
 assert(a.loadConfiguration());
 fake::files.clear();fake::files["/config.tmp"]=good;fake::files["/config.bak"]="{bad";
 fake::failRenameFrom="/config.tmp";assert(!a.loadConfiguration());fake::writeLimit=0;
 assert(!a.saveConfiguration());assert(fake::files["/config.tmp"]==good);
 fake::failRenameFrom="";fake::writeLimit=SIZE_MAX;assert(a.loadConfiguration());
}
void testSerialConfigPersistenceAndValidation(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();
 auto&i=ESP32_SMA_Inverter::getInstance();
 const AppConfig originalConfig=a.appConfig;const uint32_t originalInverterSerial=i.invData.Serial;
 const uint32_t defaultSerial=static_cast<uint32_t>(THISSERIAL);
 const auto originalFiles=fake::files;const auto originalNvs=fake::nvs;
 const size_t originalWriteLimit=fake::writeLimit;const int originalNvsWrites=fake::nvsWrites;
 const bool originalMountable=fake::mountable;
 const bool originalNvsOpenFail=fake::nvsOpenFail,originalNvsReadFail=fake::nvsReadFail;
 const bool originalNvsFail=fake::nvsFail,originalNvsBlobReadFail=fake::nvsBlobDataReadFail;
 const bool originalNvsIdentityTypeMismatch=fake::nvsIdentityTypeMismatch;
 const std::string originalNvsFailReadKey=fake::nvsFailReadKey;
 const std::string originalNvsFailWriteKey=fake::nvsFailWriteKey;
 const std::string originalNvsFailEraseKey=fake::nvsFailEraseKey;
 const String originalDiscoveryIdentity=m.discoveryIdentity;
 const bool originalDiscoveryLoaded=m.discoveryIdentityLoaded;
 const uint32_t originalDiscoveryAttempt=m.lastDiscoveryMigrationAttemptMillis;
 const bool originalDiscoveryAttempted=m.discoveryMigrationAttempted;
 const bool originalStorageAvailable=a.configurationStorageAvailable;
 fake::mountable=true;a.configurationStorageAvailable=true;
 a.appConfig.mqttTopic="SMA";a.appConfig.smaBTAddress="00:80:25:00:00:00";
 for(uint32_t serial:{0x80000000u,3000000000u,UINT32_MAX}){
  fake::files.clear();a.appConfig.thisSerial=serial;
  assert(a.saveConfiguration());
  a.appConfig.thisSerial=17;
  assert(a.loadConfiguration());assert(a.appConfig.thisSerial==serial);
  a.appConfig.thisSerial=17;
  a.configSetup();
  assert(a.configurationStorageReady());assert(a.appConfig.thisSerial==serial);
  StaticJsonDocument<2048> saved;assert(!deserializeJson(saved,fake::files["/config.txt"]));
  assert(saved["thisserial"].is<uint32_t>()&&saved["thisserial"].as<uint32_t>()==serial);
 }

 // A restored serial must be available for Home Assistant identity migration
 // before Bluetooth has supplied a live inverter serial.
 a.appConfig.thisSerial=3000000000u;i.invData.Serial=0;
 fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsFail=false;
 fake::nvsBlobDataReadFail=fake::nvsIdentityTypeMismatch=false;
 fake::nvsFailReadKey=fake::nvsFailWriteKey=fake::nvsFailEraseKey="";
 m.discoveryIdentity="";m.discoveryIdentityLoaded=false;
 m.discoveryMigrationAttempted=false;
 assert(m.prepareDiscovery(a.appConfig));
 assert(m.discoveryIdentity=="SMA-3000000000");

 auto loadSerialJson=[&](const char *serialJson,uint32_t expected){
  fake::files.clear();
  fake::files["/config.txt"] = std::string("{\"mqttTopic\":\"SMA\",\"smaBTAddress\":\"00:80:25:00:00:00\"")+
      (serialJson ? std::string(",\"thisserial\":")+serialJson : std::string())+"}";
  a.appConfig.thisSerial=999;
  assert(a.loadConfiguration());assert(a.appConfig.thisSerial==expected);
 };
 loadSerialJson("42",42); // legacy integer format remains accepted
 loadSerialJson(nullptr,defaultSerial); // older files use the compiled fallback
 for(const char *invalid:{"-1","4294967296","123.0","3000000000.0","1e40"})
  loadSerialJson(invalid,defaultSerial);

 fake::files=originalFiles;fake::writeLimit=originalWriteLimit;
 fake::mountable=originalMountable;fake::nvs=originalNvs;fake::nvsWrites=originalNvsWrites;
 fake::nvsOpenFail=originalNvsOpenFail;fake::nvsReadFail=originalNvsReadFail;
 fake::nvsFail=originalNvsFail;fake::nvsBlobDataReadFail=originalNvsBlobReadFail;
 fake::nvsIdentityTypeMismatch=originalNvsIdentityTypeMismatch;
 fake::nvsFailReadKey=originalNvsFailReadKey;fake::nvsFailWriteKey=originalNvsFailWriteKey;
 fake::nvsFailEraseKey=originalNvsFailEraseKey;m.discoveryIdentity=originalDiscoveryIdentity;
 m.discoveryIdentityLoaded=originalDiscoveryLoaded;
 m.lastDiscoveryMigrationAttemptMillis=originalDiscoveryAttempt;
 m.discoveryMigrationAttempted=originalDiscoveryAttempted;
 a.appConfig=originalConfig;i.invData.Serial=originalInverterSerial;
 a.configurationStorageAvailable=originalStorageAvailable;
}
void testSettingsToken(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&w=a.webServer;
 m.settingsToken="secret";a.appConfig.mqttBroker="unchanged";
 for(const char*t:{"", "wrong"}){
  w.params={{"mqttBroker","attacker"},{"token",t}};m.handleForm();
  assert(w.code==403);assert(a.appConfig.mqttBroker=="unchanged");
 }
 w.params={{"token","secret"},{"mqttBroker","allowed"}};fake::writeLimit=0;
 m.handleForm();assert(w.code==500);fake::writeLimit=SIZE_MAX;
 m.formPage();assert(w.body.s.find("name=\"token\" value=\"secret\"")!=std::string::npos);
}

static std::string base64ForHttpTest(const std::string&input){
 static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::string encoded;
 for(size_t offset=0;offset<input.size();offset+=3){
  const size_t remaining=input.size()-offset;
  const uint8_t a=static_cast<uint8_t>(input[offset]);
  const uint8_t b=remaining>1?static_cast<uint8_t>(input[offset+1]):0;
  const uint8_t c=remaining>2?static_cast<uint8_t>(input[offset+2]):0;
  const uint32_t block=(static_cast<uint32_t>(a)<<16)|(static_cast<uint32_t>(b)<<8)|c;
  encoded+=alphabet[(block>>18)&0x3f];encoded+=alphabet[(block>>12)&0x3f];
  encoded+=remaining>1?alphabet[(block>>6)&0x3f]:'=';
  encoded+=remaining>2?alphabet[block&0x3f]:'=';
 }
 return encoded;
}

struct AppHttpResponse {
 int code=0;
 std::string body;
 std::map<std::string,std::string> headers;
 bool restarted=false;
};

static AppHttpResponse appHttpRequest(const std::string&method,const std::string&path,
                                      const std::string&body,const std::string*authorization){
 auto&server=ESP32_SMA_Inverter_App::webServer;
 server.code=0;server.body="";server.responseHeaders.clear();
 std::string request=method+" "+path+" HTTP/1.1\r\nHost: test\r\n";
 if(authorization)request+="Authorization: "+*authorization+"\r\n";
 if(method=="POST"){
  request+="Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+
      std::to_string(body.size())+"\r\n\r\n"+body;
 }else request+="\r\n";
 WiFiClient client(request);server._server.pending.push_back(client);
 bool restarted=false;
 try{
  for(unsigned calls=0;client.socket->open&&calls<64;++calls)server.handleClient();
 }catch(const fake::Restart&){restarted=true;server.close();}
 if(client.socket->open)server.close();
 return {server.code,server.body.s,server.responseHeaders,restarted};
}

static void assertBasicChallenge(const AppHttpResponse&response){
 assert(response.code==401);
 auto challenge=response.headers.find("WWW-Authenticate");
 assert(challenge!=response.headers.end());
 assert(challenge->second=="Basic realm=\"Login Required\"");
}

void testWebBasicAuthentication(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();
 const AppConfig savedConfig=a.appConfig;
 const String savedSettingsToken=m.settingsToken,savedClockToken=m.clockSyncToken,savedSmartToken=m.smartConfigToken;
 const bool savedClockRequest=a.clockSyncRequested;const uint32_t savedClockDeadline=a.clockSyncRequestDeadline;
 const uint32_t savedNextTime=a.nextTime;const String savedClockStatus=a.clockSyncStatus;
 const unsigned oldAuthenticateCalls=a.webServer.authenticateCalls;const bool oldAuthenticateResult=a.webServer.auth;
 const int savedWifiState=WiFi.state;const bool savedSmartConfigStart=WiFi.smartConfigStartResult;
 const bool savedSmartConfigDone=WiFi.done;const bool savedPersistent=WiFi.persistentEnabled;
 const bool savedAutoReconnect=WiFi.autoReconnectEnabled;const unsigned savedSmartConfigBegins=WiFi.smartConfigBegins;
 const unsigned savedStops=WiFi.stops;
 const std::string savedStationSSID=fake::stationSSID,savedStationPassword=fake::stationPassword;
 const bool savedStationAssociated=fake::stationAssociated,savedWiFiConfigAvailable=fake::wifiConfigAvailable;
 const bool savedWiFiConfigSetAvailable=fake::wifiConfigSetAvailable;
 const auto savedNvs=fake::nvs;const bool savedNvsFail=fake::nvsFail,savedNvsOpenFail=fake::nvsOpenFail;
 const bool savedNvsReadFail=fake::nvsReadFail;const std::string savedFailReadKey=fake::nvsFailReadKey;
 const uint64_t savedTicks=fake::ticks;

 auto&server=a.webServer;server.begin();
 a.appConfig.mqttUser="u";a.appConfig.mqttPasswd="p";
 m.settingsToken="settings-csrf";m.clockSyncToken="clock-csrf";m.smartConfigToken="smart-csrf";
 server.auth=false; // Valid headers must pass without the unsafe core authenticate() overload.
 assert(base64ForHttpTest("u:p")=="dTpw");
 const std::string shortAuth="Basic "+base64ForHttpTest("u:p");
 auto missing=appHttpRequest("GET","/smartconfig","",nullptr);assertBasicChallenge(missing);
 const std::string wrongAuth="Basic "+base64ForHttpTest("u:wrong");
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",&wrongAuth));
 const std::string wrongScheme="Bearer "+base64ForHttpTest("u:p");
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",&wrongScheme));
 const std::string malformed="Basic ???";
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",&malformed));
 assert(appHttpRequest("GET","/smartconfig","",&shortAuth).code==200);
 const std::string trimmedMixedCase="  bAsIc    "+base64ForHttpTest("u:p")+" \t";
 assert(appHttpRequest("GET","/smartconfig","",&trimmedMixedCase).code==200);
 std::string changedCase=shortAuth;changedCase[6]='D';
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",&changedCase));

 // The settings/status page remains public by design; authenticated writes
 // still require both Basic credentials and their independent CSRF token.
 assert(appHttpRequest("GET","/","",nullptr).code==200);
 const std::string settingsBody="token=settings-csrf&mqttPort=0";
 assertBasicChallenge(appHttpRequest("POST","/postform/",settingsBody,nullptr));
 assert(appHttpRequest("POST","/postform/",settingsBody,&shortAuth).code==400);
 assertBasicChallenge(appHttpRequest("POST","/setclock/","token=clock-csrf",nullptr));
 const auto clockRejected=appHttpRequest("POST","/setclock/","token=wrong",&shortAuth);
 assert(clockRejected.code==403);
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",nullptr));
 assertBasicChallenge(appHttpRequest("POST","/smartconfig","token=smart-csrf",nullptr));
 assert(appHttpRequest("POST","/smartconfig","token=wrong",&shortAuth).code==403);

 // Both maximum GUI credential lengths must survive canonical Base64 padding.
 const std::string user127(127,'u'),password128(128,'p');
 a.appConfig.mqttUser=user127;a.appConfig.mqttPasswd=password128;
 std::string auth127="Basic "+base64ForHttpTest(user127+":"+password128);
 assert(auth127.size()>7&&auth127.substr(auth127.size()-2)=="==");
 assert(appHttpRequest("GET","/smartconfig","",&auth127).code==200);
 std::string badDoublePadding=auth127;badDoublePadding.back()='A';
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",&badDoublePadding));

 const std::string user128(128,'u');
 a.appConfig.mqttUser=user128;a.appConfig.mqttPasswd=password128;
 std::string auth128="Basic "+base64ForHttpTest(user128+":"+password128);
 assert(auth128.back()=='=');
 assert(appHttpRequest("GET","/smartconfig","",&auth128).code==200);
 std::string missingPadding=auth128.substr(0,auth128.size()-1);
 assertBasicChallenge(appHttpRequest("GET","/smartconfig","",&missingPadding));

 // Valid Basic + valid CSRF reaches the clock handler. SmartConfig also
 // reaches its action path, but the fake start failure keeps the test bounded.
 a.appConfig.mqttUser="u";a.appConfig.mqttPasswd="p";
 const auto clockAccepted=appHttpRequest("POST","/setclock/","token=clock-csrf",&shortAuth);
 assert(clockAccepted.code==202);
 WiFi.state=0;fake::stationSSID.clear();fake::stationPassword.clear();fake::stationAssociated=false;
 fake::wifiConfigAvailable=fake::wifiConfigSetAvailable=true;fake::nvs.clear();
 fake::nvsFail=fake::nvsOpenFail=fake::nvsReadFail=false;fake::nvsFailReadKey.clear();
 WiFi.smartConfigStartResult=false;WiFi.done=false;
 const auto smartAccepted=appHttpRequest("POST","/smartconfig","token=smart-csrf",&shortAuth);
 assert(smartAccepted.code==200);

 // Empty MQTT username intentionally retains the existing bootstrap access.
 a.appConfig.mqttUser="";
 assert(appHttpRequest("POST","/postform/",settingsBody,nullptr).code==400);
 assert(server.authenticateCalls==oldAuthenticateCalls);

 a.appConfig=savedConfig;m.settingsToken=savedSettingsToken;
 m.clockSyncToken=savedClockToken;m.smartConfigToken=savedSmartToken;
 a.clockSyncRequested=savedClockRequest;a.clockSyncRequestDeadline=savedClockDeadline;
 a.nextTime=savedNextTime;a.clockSyncStatus=savedClockStatus;server.auth=oldAuthenticateResult;
 WiFi.state=savedWifiState;WiFi.smartConfigStartResult=savedSmartConfigStart;
 WiFi.done=savedSmartConfigDone;WiFi.persistentEnabled=savedPersistent;
 WiFi.autoReconnectEnabled=savedAutoReconnect;WiFi.smartConfigBegins=savedSmartConfigBegins;
 WiFi.stops=savedStops;fake::stationSSID=savedStationSSID;fake::stationPassword=savedStationPassword;
 fake::stationAssociated=savedStationAssociated;fake::wifiConfigAvailable=savedWiFiConfigAvailable;
 fake::wifiConfigSetAvailable=savedWiFiConfigSetAvailable;fake::nvs=savedNvs;
 fake::nvsFail=savedNvsFail;fake::nvsOpenFail=savedNvsOpenFail;fake::nvsReadFail=savedNvsReadFail;
 fake::nvsFailReadKey=savedFailReadKey;fake::ticks=savedTicks;
 server.close();
}

void testNumericSettingsValidation(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&w=a.webServer;
 const AppConfig original=a.appConfig;const auto originalTicks=fake::ticks;
 m.settingsToken="secret";
 a.appConfig.mqttBroker="before";a.appConfig.mqttPort=1883;a.appConfig.scanRate=60;
 a.appConfig.timezone=0.5f;a.appConfig.hassDisc=true;
 assert(a.saveConfiguration());
 const auto committed=fake::files["/config.txt"];
 auto rejected=[&](const char*name,const std::string&value){
  w.params={{"token","secret"},{"mqttBroker","must-not-save"},{name,value}};w.code=0;
  m.handleForm();
  assert(w.code==400);assert(a.appConfig.mqttBroker=="before");
  assert(a.appConfig.mqttPort==1883&&a.appConfig.scanRate==60&&a.appConfig.timezone==0.5f);
  assert(a.appConfig.hassDisc);assert(fake::files["/config.txt"]==committed);
 };
 for(const std::string&value:std::vector<std::string>{"","1883junk","0","65536",std::string(80,'9')}) rejected("mqttPort",value);
 for(const std::string&value:std::vector<std::string>{"","60junk","60.5","2147483648",std::string(80,'9')}) rejected("scanRate",value);
 for(const std::string&value:std::vector<std::string>{"","NaN","Infinity","-inf","5hours","1e9999","1e-9999"}) rejected("timezone",value);
 auto saved=[&](const char*port,const char*scanRate,const char*timezone){
  w.params={{"token","secret"},{"mqttPort",port},{"scanRate",scanRate},{"timezone",timezone}};w.code=0;
  try{m.handleForm();assert(false);}catch(const fake::Restart&){}
  assert(w.code==200);
 };
 saved("1","-1","-20");
 assert(a.appConfig.mqttPort==1&&a.appConfig.scanRate==10&&a.appConfig.timezone==-12.0f);
 saved("65535","3601","20.25");
 assert(a.appConfig.mqttPort==65535&&a.appConfig.scanRate==3600&&a.appConfig.timezone==14.0f);
 saved("1883","900","-4.5");
 assert(a.appConfig.mqttPort==1883&&a.appConfig.scanRate==900&&a.appConfig.timezone==-4.5f);
 StaticJsonDocument<2048> json;assert(!deserializeJson(json,fake::files["/config.txt"]));
 assert(json["mqttPort"]==1883&&json["scanRate"]==900&&json["timezone"]==-4.5f);
 a.appConfig=original;assert(a.saveConfiguration());fake::ticks=originalTicks;
}

void testTimezoneConfigurationReload(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();
 const AppConfig original=a.appConfig;const auto originalFiles=fake::files;
 const bool originalStorageReady=a.configurationStorageAvailable;
 assert(a.saveConfiguration());

 // Save/load and the normal startup load-and-save path must preserve the
 // fractional offsets accepted by the settings page.
 for(float timezone:{5.5f,9.5f,-3.5f,5.75f}){
  a.appConfig.timezone=timezone;assert(a.saveConfiguration());
  a.appConfig.timezone=0;
  assert(a.loadConfiguration());assert(a.appConfig.timezone==timezone);
  a.appConfig.timezone=0;a.configSetup();
  assert(a.appConfig.timezone==timezone);
 }

 const float compiledTimezone=static_cast<float>(TIMEZONE);
 const float safeFallback=std::isfinite(compiledTimezone) &&
         compiledTimezone>=-12.0f && compiledTimezone<=14.0f
     ? compiledTimezone : 0.0f;
 StaticJsonDocument<2048> base;
 assert(!deserializeJson(base,fake::files["/config.txt"]));
 base["timezone"]=10;
 String legacyPayload;serializeJson(base,legacyPayload);
 fake::files["/config.txt"]=legacyPayload.s;
 a.appConfig.timezone=0;
 assert(a.loadConfiguration());assert(a.appConfig.timezone==10.0f);

 // Bad optional timezone values must fall back safely without rejecting an
 // otherwise valid legacy file or disturbing its recovery backup.
 const std::string validConfig=fake::files["/config.txt"];
 const std::string backup="preserved recovery backup";
 fake::files["/config.bak"]=backup;
 auto checkFallback=[&](const char*kind){
  StaticJsonDocument<2048> doc;
  assert(!deserializeJson(doc,validConfig));
  if(std::strcmp(kind,"missing")==0) doc.remove("timezone");
  else if(std::strcmp(kind,"string")==0) doc["timezone"]="5.5";
  else if(std::strcmp(kind,"boolean")==0) doc["timezone"]=true;
  else if(std::strcmp(kind,"null")==0) doc["timezone"]=nullptr;
  else if(std::strcmp(kind,"too-low")==0) doc["timezone"]=-12.25;
  else if(std::strcmp(kind,"too-high")==0) doc["timezone"]=14.25;
  else if(std::strcmp(kind,"large-finite")==0) doc["timezone"]=1e39;
  String payload;serializeJson(doc,payload);fake::files["/config.txt"]=payload.s;
  a.appConfig.timezone=123.0f;
  assert(a.loadConfiguration());
  assert(std::isfinite(a.appConfig.timezone));
  assert(a.appConfig.timezone>=-12.0f&&a.appConfig.timezone<=14.0f);
  assert(a.appConfig.timezone==safeFallback);
  assert(fake::files["/config.bak"]==backup);
 };
 for(const char*kind:{"missing","string","boolean","null","too-low","too-high","large-finite"})
  checkFallback(kind);

 fake::files=originalFiles;a.appConfig=original;
 a.configurationStorageAvailable=originalStorageReady;
}

void testNtpInput(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&w=a.webServer;
 a.appConfig.ntphostname="\"><script>alert(1)</script>";m.formPage();
 assert(w.body.s.find("<script>")==std::string::npos);
 assert(w.body.s.find("&quot;&gt;&lt;script&gt;")!=std::string::npos);
 a.appConfig.ntphostname="pool.ntp.org";
 for(const String&host:{String("\" onfocus=alert(1)"),String(std::string(64,'a')+".org"),String("-bad.org")}){
  w.params={{"token","secret"},{"ntphostname",host}};m.handleForm();
  assert(w.code==400);assert(a.appConfig.ntphostname=="pool.ntp.org");
 }
}
void testExcessFormArguments(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&w=a.webServer;
 m.settingsToken="secret";a.appConfig.mqttBroker="before";
 w.params={{"token","secret"},{"mqttBroker","after"}};
 for(int i=0;i<300;i++)w.params.push_back({"unused","x"});
 m.handleForm();assert(w.code==413);assert(a.appConfig.mqttBroker=="before");
 w.params={{"token","secret"},{"mqttBroker","after"},{"ntphostname","invalid<"}};
 m.handleForm();assert(w.code==400);assert(a.appConfig.mqttBroker=="before");
}
void drainHttp(BoundedWebServer &server,const WiFiClient &client){
 for(int n=0;n<100&&client.socket->open;++n)server.handleClient();
}
void testBoundedHttpRequests(){
 BoundedWebServer server;server.begin();int dispatched=0;
 server.on("/",[&]{++dispatched;server.send(200,"text/plain","ok");});
 server.on("/postform/",HTTP_POST,[&]{++dispatched;server.send(200,"text/plain","ok");});
 auto postHeaders=[](const std::string &length,const std::string &extra=""){
  return "POST /postform/ HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: "+length+"\r\n"+extra+"\r\n";
 };
 auto rejected=[&](const std::string &request,int expected){
  auto before=dispatched;WiFiClient client(request);server._server.pending.push_back(client);
  drainHttp(server,client);assert(!client.socket->open);assert(server.code==expected);assert(dispatched==before);
  assert(server._currentArgs==nullptr&&server._currentArgCount==0);
 };
 rejected("GET /"+std::string(512,'x')+" HTTP/1.1\r\n\r\n",414);
 rejected("GET / HTTP/1.1\r\nX-Large: "+std::string(5000,'x'),431);
 rejected(postHeaders("4097"),413);
 rejected(postHeaders(std::string(80,'9')),413);
 rejected(postHeaders("-1"),400);
 rejected(postHeaders("1","Content-Length: 1\r\n"),400);
 rejected(postHeaders("1","Transfer-Encoding: chunked\r\n"),400);
 rejected(postHeaders("1","Expect: 100-continue\r\n"),417);
 rejected("POST /postform/ HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=missing\r\nContent-Length: 10\r\n\r\n",415);
 rejected("POST /postform/ HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",415);
 rejected("GET / HTTP/1.1\r\nContent-Length: 1\r\n\r\nx",400);
 std::string fields="a=x";for(int n=1;n<25;++n)fields+="&a=x";
 rejected(postHeaders(std::to_string(fields.size()))+fields,413);
 rejected("GET /?"+fields+" HTTP/1.1\r\n\r\n",413);
 std::string split="POST /postform/?a=x HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
 fields.erase(fields.size()-4); // 24 in body plus one in query must still fail.
 rejected(split+std::to_string(fields.size())+"\r\n\r\n"+fields,413);
 for(const std::string &body:std::vector<std::string>{"a=%", "a=%0", "a=%GG", "a=%00", std::string("a=x\0y",5)})
  rejected(postHeaders(std::to_string(body.size()))+body,400);
 std::string manyHeaders;for(int n=0;n<33;++n)manyHeaders+="X-Test: x\r\n";
 rejected("GET / HTTP/1.1\r\n"+manyHeaders+"\r\n",431);

 // A rejection must not poison the next connection or bypass normal routing.
 WiFiClient good("GET / HTTP/1.1\r\n\r\n");server._server.pending.push_back(good);drainHttp(server,good);
 assert(!good.socket->open&&server.code==200&&dispatched==1);
 // A body at the size boundary is accepted, with a bounded amount read per call.
 std::string boundary="a="+std::string(4094,'x');
 WiFiClient large(postHeaders("4096")+boundary);server._server.pending.push_back(large);
 auto size=large.socket->input.size();server.handleClient();
 assert(large.socket->open&&large.socket->input.size()==size-256);
 drainHttp(server,large);assert(!large.socket->open&&server.code==200&&dispatched==2);
}
void testBoundedHttpFormCompatibility(){
 BoundedWebServer server;server.begin();int dispatched=0;
 server.on("/postform/",HTTP_POST,[&]{
  ++dispatched;assert(server.method()==HTTP_POST);assert(server.args()==3);
  assert(server.arg("token")=="secret");assert(server.arg("mqttPasswd")=="a&b+c d");
  assert(server.arg("mqttTopic")=="SMA");assert(server.header("Authorization")=="Basic dXNlcjpwYXNz");
  assert(server._hostHeader=="192.0.2.1");server.send(200,"text/plain","ok");
 });
 std::string body="mqttPasswd=a%26b%2Bc+d&mqttTopic=SMA";
 WiFiClient client("POST /postform/?token=secret HTTP/1.1\r\nhost: 192.0.2.1\r\nauthorization: Basic dXNlcjpwYXNz\r\nContent-Type: application/x-www-form-urlencoded; charset=UTF-8\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body);
 server._server.pending.push_back(client);drainHttp(server,client);
 assert(dispatched==1&&server.code==200&&!client.socket->open);assert(server.header("Authorization").isEmpty());
 server.on("/",[&]{++dispatched;assert(server.args()==0);assert(server.header("Authorization").isEmpty());server.send(200,"text/plain","ok");});
 WiFiClient next("GET / HTTP/1.0\r\n\r\n");server._server.pending.push_back(next);drainHttp(server,next);
 assert(dispatched==2&&server.code==200&&!next.socket->open);
}
void testBoundedHttpDeadline(){
 for(bool body:{false,true}){
  BoundedWebServer server;server.begin();int dispatched=0;
  server.on("/",[&]{++dispatched;server.send(200,"text/plain","ok");});
  // Exercise the absolute deadline across millis() rollover and keep sending.
  fake::ticks=UINT32_MAX-1000ULL;
  WiFiClient client(body?"POST / HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 10\r\n\r\na":"GET / HTTP/1.1\r\nX-Slow: a");
  server._server.pending.push_back(client);server.handleClient();assert(client.socket->open);
  for(int n=0;n<4;++n){fake::ticks+=1000;client.socket->input.push_back('x');server.handleClient();assert(client.socket->open);}
  fake::ticks+=1000;client.socket->input.push_back('x');server.handleClient();
  assert(!client.socket->open&&server.code==408&&dispatched==0);
 }
}
void testMissingNtpDoesNotBlock(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();
 fake::validTime=false;fake::maxTimeWait=0;fake::ticks=1000000;a.nextTime=1100000;
 auto start=fake::ticks;bool discovery=a.appConfig.hassDisc;a.appConfig.hassDisc=false;assert(m.getTime().isEmpty());a.appLoop();m.formPage();a.appConfig.hassDisc=discovery;
 assert(fake::maxTimeWait==0);assert(fake::ticks-start<500);
 fake::validTime=true;
}
void testLateWifiStartsNtp(){
 auto&m=ESP32_SMA_MQTT::getInstance();m.ntpStarted=false;fake::ntpCalls=0;
 WiFi.state=0;m.wifiLoop();assert(fake::ntpCalls==0);
 WiFi.state=WL_CONNECTED;m.wifiLoop();assert(fake::ntpCalls==1);
 m.wifiLoop();assert(fake::ntpCalls==1);
}
void testStaleRelayExpires(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();
 fake::validTime=false;fake::ticks=2000000;a.nextTime=3000000;
 a.appConfig.scanRate=60;ESP32_SMA_Inverter::invData.GridRelay=51;
 a.hasSuccessfulRead=true;a.lastSuccessfulReadMillis=2000000;
 a.appLoop();assert(!a.nightTime);
 fake::ticks+=121000;a.appLoop();assert(a.nightTime);
 a.hasSuccessfulRead=false;fake::validTime=true;
}
void testNightToDayShortensPollDeadline(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto config=a.appConfig;const auto oldNextTime=a.nextTime;const auto oldRate=a.lastAdjustedScanRate;
 const auto oldNightTime=a.nightTime;const auto oldRead=a.hasSuccessfulRead;const auto oldReadAt=a.lastSuccessfulReadMillis;
 const auto oldSerial=i.invData.Serial;const auto oldRelay=i.invData.GridRelay;const auto oldThisSerial=a.appConfig.thisSerial;
 const auto oldValidTime=fake::validTime;const auto oldHour=fake::localHourOverride;const auto oldTicks=fake::ticks;
 a.appConfig.scanRate=10;a.appConfig.hassDisc=false;a.appConfig.mqttBroker="";a.appConfig.thisSerial=0;
 i.invData.Serial=0;i.invData.GridRelay=0;a.hasSuccessfulRead=false;a.lastAdjustedScanRate=0;
 fake::ticks=5000000;fake::validTime=false;fake::localHourOverride=-1;
 const uint32_t scheduledAt=millis();a.nextTime=scheduledAt+NIGHTSCANRATE;
 a.appLoop();assert(a.nightTime);const uint32_t nightDeadline=a.nextTime;
 assert(a.lastAdjustedScanRate==NIGHTSCANRATE);
 fake::ticks+=NIGHTSCANRATE/2;fake::validTime=true;fake::localHourOverride=6;
 a.appLoop();assert(!a.nightTime);assert(a.nextTime!=nightDeadline);
 assert((int32_t)(a.nextTime-nightDeadline)<0);
 const int32_t remaining=(int32_t)(a.nextTime-millis());assert(remaining>0&&remaining<=10000);
 a.appConfig=config;a.nextTime=oldNextTime;a.lastAdjustedScanRate=oldRate;a.nightTime=oldNightTime;
 a.hasSuccessfulRead=oldRead;a.lastSuccessfulReadMillis=oldReadAt;i.invData.Serial=oldSerial;i.invData.GridRelay=oldRelay;
 a.appConfig.thisSerial=oldThisSerial;fake::validTime=oldValidTime;fake::localHourOverride=oldHour;fake::ticks=oldTicks;
}
void testFasterModePreservesEarlierDeadlineAcrossRollover(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto config=a.appConfig;const auto oldNextTime=a.nextTime;const auto oldRate=a.lastAdjustedScanRate;
 const auto oldNightTime=a.nightTime;const auto oldRead=a.hasSuccessfulRead;const auto oldReadAt=a.lastSuccessfulReadMillis;
 const auto oldSerial=i.invData.Serial;const auto oldRelay=i.invData.GridRelay;const auto oldThisSerial=a.appConfig.thisSerial;
 const auto oldValidTime=fake::validTime;const auto oldHour=fake::localHourOverride;const auto oldTicks=fake::ticks;
 a.appConfig.scanRate=10;a.appConfig.hassDisc=false;a.appConfig.mqttBroker="";a.appConfig.thisSerial=0;
 i.invData.Serial=0;i.invData.GridRelay=0;a.hasSuccessfulRead=false;a.lastAdjustedScanRate=0;
 fake::ticks=uint64_t(UINT32_MAX)-3000;fake::validTime=false;fake::localHourOverride=-1;
 a.nextTime=millis()+NIGHTSCANRATE;a.appLoop();assert(a.nightTime);
 // Model an already queued explicit/retry deadline that falls before the new day scan.
 const uint32_t earlierDeadline=millis()+5000;a.nextTime=earlierDeadline;
 fake::validTime=true;fake::localHourOverride=6;a.appLoop();assert(!a.nightTime);
 assert(a.nextTime==earlierDeadline);
 const int32_t remaining=(int32_t)(a.nextTime-millis());assert(remaining>0&&remaining<5000);
 a.appConfig=config;a.nextTime=oldNextTime;a.lastAdjustedScanRate=oldRate;a.nightTime=oldNightTime;
 a.hasSuccessfulRead=oldRead;a.lastSuccessfulReadMillis=oldReadAt;i.invData.Serial=oldSerial;i.invData.GridRelay=oldRelay;
 a.appConfig.thisSerial=oldThisSerial;fake::validTime=oldValidTime;fake::localHourOverride=oldHour;fake::ticks=oldTicks;
}
void testBluetoothTimerRollover(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 b.input.clear();fake::ticks=UINT32_MAX-25ULL;auto start=fake::ticks;
 b.onAvailable=[&]{if(fake::ticks-start>=50 && b.input.empty())b.input.push_back(0x42);};
 assert(i.BTgetByte()==0x42);assert(!i.readTimeout);assert(fake::ticks-start>=50);
 b.onAvailable=nullptr;fake::ticks=UINT32_MAX-25ULL;start=fake::ticks;
 i.BTgetByte();assert(i.readTimeout);assert(fake::ticks-start>=20000);
}
void testSlowPacketDeadline(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 b.input.clear();fake::ticks=100;auto start=fake::ticks;auto delays=fake::delayCalls;
 std::vector<uint8_t> packet(200,0);packet[0]=0x7e;packet[1]=200;packet[3]=0x7e^200;packet[16]=1;
 size_t offset=0;uint64_t next=start+1000;
 b.onAvailable=[&]{if(fake::ticks>=next&&offset<packet.size()){b.input.push_back(packet[offset++]);next+=1000;}};
 assert(i.getPacket(i.sixff,1)==E_NODATA);assert(fake::ticks-start<20100);
 assert(fake::delayCalls>delays);assert(!i.receivingPacket);b.onAvailable=nullptr;
}
void testCallerOperationDeadlinesAndRollover(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const bool savedConnected=i.btConnected;const auto savedTicks=fake::ticks;
 const auto savedSent=b.sent;const auto savedAvailable=b.onAvailable;
 const uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x1234;i.invData.Serial=55;

 // Initialization shares one 20-second budget across its handshake stages.
 b.input.clear();b.output.clear();fake::ticks=9000000;queueL1(b,{0,4,0x70,0,1},2,target);
 const uint64_t initStarted=fake::ticks;uint64_t initReplyWindow=0;bool initReplyQueued=false;int initSends=0;
 b.sent=[&]{if(++initSends==1)initReplyWindow=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(initSends==1&&!initReplyQueued&&fake::ticks-initReplyWindow>=19800){
   queueL1(b,std::vector<uint8_t>(14),5,target);initReplyQueued=true;
  }
 };
 assert(i.initialiseSMAConnection()==E_NODATA);
 assert(initReplyQueued&&initSends==2&&fake::ticks-initStarted<20500);

 // A late unrelated login reply must not start a fresh 20-second packet wait.
 b.input.clear();b.output.clear();fake::ticks=uint64_t(UINT32_MAX)-5000;
 const uint64_t loginStarted=fake::ticks;uint64_t loginReceiveStarted=0;bool loginReplyQueued=false;
 b.sent=[&]{loginReceiveStarted=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(!loginReplyQueued&&fake::ticks-loginReceiveStarted>=19800){
   auto unrelated=loginResponse(i,0x1234,i.invData.Serial);
   put(unrelated,27,(i.pcktID+1)&0x7fff,2);queueResponse(b,unrelated);loginReplyQueued=true;
  }
 };
 assert(i.logonSMAInverter("0000",USERGROUP)==E_NODATA);
 assert(loginReplyQueued&&fake::ticks-loginStarted<20500);
 assert(!i.receivingPacket&&i.pcktBufPos==0);

 // The plant clock read has the same overall bound when its first reply is stale.
 b.input.clear();b.output.clear();fake::ticks=11000000;
 const uint64_t clockStarted=fake::ticks;uint64_t clockReceiveStarted=0;bool clockReplyQueued=false;
 auto clockReply=[&]{
  std::vector<uint8_t> data(24);put(data,0,0x00236d00);put(data,4,1800000000);
  put(data,8,1790000000);put(data,16,36000);put(data,20,7);
  auto reply=response(i,data);put(reply,29,0xf000020b);return reply;
 };
 b.sent=[&]{clockReceiveStarted=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(!clockReplyQueued&&fake::ticks-clockReceiveStarted>=19800){
   auto unrelated=clockReply();put(unrelated,27,(i.pcktID+1)&0x7fff,2);
   queueResponse(b,unrelated);clockReplyQueued=true;
  }
 };
 int32_t plantNow=0,lastSet=0,offset=0;uint32_t setCount=0;
 assert(i.readPlantTime(&plantNow,&lastSet,&offset,&setCount)==E_NODATA);
 assert(clockReplyQueued&&fake::ticks-clockStarted<20500);

 // CFL has a 30-second transaction budget even after an unrelated late reply.
 b.input.clear();b.output.clear();fake::ticks=7000000;
 const uint64_t queryStarted=fake::ticks;uint64_t queryReceiveStarted=0;bool queryReplyQueued=false;
 b.sent=[&]{queryReceiveStarted=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(!queryReplyQueued&&fake::ticks-queryReceiveStarted>=19800){
   auto unrelated=response(i,{});put(unrelated,27,(i.pcktID+1)&0x7fff,2);
   queueResponse(b,unrelated);queryReplyQueued=true;
  }
 };
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_NODATA);
 assert(queryReplyQueued&&fake::ticks-queryStarted<31000);
 assert(!i.receivingPacket&&i.pcktBufPos==0);

 // A deadline also bounds a packet assembled from multiple L1 fragments;
 // its absolute comparison remains correct when millis() wraps.
 b.input.clear();b.output.clear();fake::ticks=uint64_t(UINT32_MAX)-100;
 const uint32_t packetStarted=millis();const uint64_t packetStartTicks=fake::ticks;
 const uint32_t packetDeadline=packetStarted+160UL;
 assert(packetDeadline<packetStarted);
 const std::vector<uint8_t> continuation(80,0x44);
 const auto secondFrame=l1Frame(continuation,1);
 size_t secondOffset=0;uint64_t nextByteAt=0;bool firstFragmentQueued=false;
 b.onAvailable=[&]{
  const uint64_t elapsed=fake::ticks-packetStartTicks;
  if(!firstFragmentQueued&&elapsed>=70){
   queueL1(b,{0x7e,0x01,0x02,0x03},8);firstFragmentQueued=true;
  }
  if(firstFragmentQueued&&secondOffset==0&&elapsed>=120){
   secondOffset=20;b.inject(secondFrame.data(),secondOffset);nextByteAt=elapsed+10;
  } else if(secondOffset>0&&secondOffset<secondFrame.size()&&elapsed>=nextByteAt){
   b.inject(secondFrame.data()+secondOffset,1);++secondOffset;nextByteAt+=10;
  }
 };
 assert(i.getPacket(i.sixff,1,&packetDeadline)==E_NODATA);
 assert(firstFragmentQueued&&secondOffset>20&&secondOffset<secondFrame.size());
 assert(fake::ticks-packetStartTicks<500&&!i.receivingPacket&&i.pcktBufPos==0);

 // The next receive has no inherited deadline and can complete normally.
 b.onAvailable=nullptr;b.input.clear();queueL1(b,{0x42},1);
 assert(i.getPacket(i.sixff,1)==E_OK&&!i.receivingPacket);
 b.sent=savedSent;b.onAvailable=savedAvailable;b.input.clear();b.output.clear();
 i.invData=savedIdentity;i.pcktID=savedId;i.btConnected=savedConnected;fake::ticks=savedTicks;
}
void testFragmentEscapes(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const std::vector<uint8_t> wire={0x7e,0xff,0x03,0x60,0x65,0x7d,0x5e,0x7d,0x5d,0x7e};
 const std::vector<uint8_t> decoded={0x7e,0xff,0x03,0x60,0x65,0x7e,0x7d,0x7e};
 for(size_t split=1;split<wire.size();split++){
  b.input.clear();queueL1(b,{wire.begin(),wire.begin()+split},8);queueL1(b,{wire.begin()+split,wire.end()});
  assert(i.getPacket(i.sixff,1)==E_OK);assert(i.pcktBufPos==decoded.size());
  assert(std::equal(decoded.begin(),decoded.end(),i.pcktBuf));
 }
 b.input.clear();queueL1(b,{0x7e,0xff,0x03,0x60,0x65,0x7d});
 assert(i.getPacket(i.sixff,1)==E_INVRESP);
}
void testMalformedRecordRanges(){
 std::vector<uint8_t> record(16);put(record,0,uint32_t(MeteringTotWhOut)<<8);put(record,8,12345,8);
 assert(query(record,0,UINT32_MAX)==E_INVRESP);
 assert(query(record,2,1)==E_INVRESP);
 assert(query(record,1,2)==E_INVRESP);
 assert(query(record)==E_OK);assert(ESP32_SMA_Inverter::invData.ETotal==12345);
 auto&i=ESP32_SMA_Inverter::getInstance();i.pcktBufPos=0;assert(!i.validateChecksum());
}
void testStatusRecordBounds(){
 for(size_t n:{size_t(16),size_t(20),size_t(36)}){
  std::vector<uint8_t> record(n);put(record,0,(8U<<24)|(uint32_t(OperationHealth)<<8));
  assert(query(record)==E_INVRESP);
  assert(ESP32_SMA_Inverter::getInstance().getattribute(record.data(),n)==0xFFFFFD);
 }
 std::vector<uint8_t> record(40);put(record,0,(8U<<24)|(uint32_t(OperationHealth)<<8));
 put(record,8,(1U<<24)|51);put(record,12,0xfffffe);
 assert(query(record)==E_OK);assert(ESP32_SMA_Inverter::invData.DevStatus==51);
 put(record,0,uint32_t(OperationHealth)<<8);assert(query(record)==E_INVRESP);
}
void testSignedAndUnavailableMeasurements(){
 std::vector<uint8_t> record(40);put(record,0,(0x40U<<24)|(uint32_t(CoolsysTmpNom)<<8));
 put(record,16,uint32_t(-1234));assert(query(record)==E_OK);
 auto&i=ESP32_SMA_Inverter::getInstance();assert(i.invData.InvTemp==-1234);assert(std::abs(i.dispData.InvTemp+12.34f)<0.001);
 put(record,16,uint32_t(-1));assert(query(record)==E_OK);assert(std::abs(i.dispData.InvTemp+0.01f)<0.001);
 put(record,16,0x80000000);assert(query(record)==E_OK);assert(std::isnan(i.dispData.InvTemp));
 put(record,0,uint32_t(CoolsysTmpNom)<<8);put(record,16,UINT32_MAX);
 assert(query(record)==E_OK);assert(std::isnan(i.dispData.InvTemp));
 put(record,16,0x80000000);assert(query(record)==E_OK);assert(std::isnan(i.dispData.InvTemp));
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();
 a.appConfig.mqttBroker="broker";i.invData.ETotal=12345;i.invData.ETotalValid=true;m.lastAcceptedETotalWh=0;
 i.dispData.Pac=321; a.client.messages.clear();assert(m.publishData());
 StaticJsonDocument<2048> json;assert(!deserializeJson(json,a.client.messages.back().payload));assert(json["InvTemp"].isNull());assert(json["Pac"].as<double>()==321);
 std::vector<uint8_t> energy(16);put(energy,0,uint32_t(MeteringTotWhOut)<<8);put(energy,8,UINT64_MAX,8);
 assert(query(energy)==E_OK);assert(!i.invData.ETotalValid);
 assert(m.publishData());assert(!deserializeJson(json,a.client.messages.back().payload));assert(json["ETotal"].isNull());
}
std::vector<uint8_t> numericRecord(uint16_t lri,uint8_t channel,int32_t value){
 std::vector<uint8_t> record(40);put(record,0,(0x40U<<24)|(uint32_t(lri)<<8)|channel);put(record,16,uint32_t(value));return record;
}
void testDcChannels(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 std::vector<uint8_t> data;
 for(auto r:{numericRecord(DcMsVol,2,20000),numericRecord(DcMsVol,1,10000),numericRecord(DcMsAmp,1,1000),numericRecord(DcMsAmp,2,2000)})data.insert(data.end(),r.begin(),r.end());
 auto poll=[&]{b.input.clear();b.output.clear();b.sent=[&]{queueResponse(b,response(i,data,1,data.size()/40));b.output.clear();};auto rc=i.getInverterData(SpotDCVoltage);b.sent=nullptr;return rc;};
 assert(poll()==E_OK);assert(i.dispData.Udc[0]==100 && i.dispData.Udc[1]==200);
 assert(i.dispData.Idc[0]==1 && i.dispData.Idc[1]==2);
 data=numericRecord(DcMsVol,2,30000);auto current=numericRecord(DcMsAmp,1,4000);data.insert(data.end(),current.begin(),current.end());
 assert(poll()==E_OK);assert(std::isnan(i.dispData.Udc[0]));assert(std::isnan(i.dispData.Idc[1]));
 assert(i.dispData.Udc[1]==300 && i.dispData.Idc[0]==4);
}
std::vector<uint8_t> decodeOutput(const std::vector<uint8_t>&wire){
 std::vector<uint8_t> out;bool escape=false;
 for(size_t k=18;k<wire.size();k++){auto c=wire[k];if(escape){out.push_back(c^0x20);escape=false;}else if(c==0x7d)escape=true;else out.push_back(c);}
 return out;
}
void testUnsupportedTemperature(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;i.btConnected=true;
 auto poll=[&](bool mandatoryError){
  b.input.clear();b.output.clear();
  b.sent=[&]{auto tx=decodeOutput(b.output);uint16_t lri=get_u32(tx.data()+33)>>8;std::vector<uint8_t> data;
   uint16_t status=0;
   if(lri==CoolsysTmpNom||(mandatoryError&&lri==GridMsHz))status=E_LRINOTAVAIL;
   else if(lri==MeteringTotWhOut){data.resize(16);put(data,0,uint32_t(lri)<<8);put(data,8,50000,8);}
   else if(lri==OperationHealth||lri==OperationGriSwStt){data.resize(40);put(data,0,(8U<<24)|(uint32_t(lri)<<8));put(data,8,(1U<<24)|51);put(data,12,0xfffffe);}
   else data=numericRecord(lri,1,1234);
   queueResponse(b,response(i,data,1,1,status));b.output.clear();};
  auto rc=i.ReadCurrentData();b.sent=nullptr;return rc;
 };
 i.dispData.Uac[1]=222; i.invData.ETodayValid=true;
 assert(poll(false)==E_OK);assert(std::isnan(i.dispData.Uac[1]));assert(!i.invData.ETodayValid);assert(i.dispData.Pac==1234);assert(i.invData.ETotalValid);assert(std::isnan(i.dispData.InvTemp));
 assert(poll(true)==E_NODATA);i.btConnected=false;
}
void testEnergyFilteringDoesNotHidePower(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto serial=i.invData.Serial;fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsFail=false;
 a.appConfig.mqttBroker="broker";i.invData.Serial=77;i.dispData.Pac=321;i.invData.ETotalValid=true;m.loadEnergyBaseline(77);
 StaticJsonDocument<2048> json;
 for(uint64_t wh:{uint64_t(0),uint64_t(1234)}){
  i.invData.ETotal=wh;assert(m.publishData());assert(!deserializeJson(json,a.client.messages.back().payload));
  assert(!json["ETotal"].isNull());assert(std::abs(json["ETotal"].as<double>()-double(wh)/1000)<0.0001);
 }
 auto writes=fake::nvsWrites;i.invData.ETotal=100;assert(m.publishData());
 assert(!deserializeJson(json,a.client.messages.back().payload));assert(json["ETotal"].isNull());assert(json["Pac"]==321);
 assert(m.lastAcceptedETotalWh==1234);assert(fake::nvsWrites==writes);
 i.invData.Serial=serial;m.loadEnergyBaseline(serial);fake::nvs.clear();
}
void testReplacingInverterResetsEnergyBaseline(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 fake::nvs.clear();Preferences store;store.begin("sma-mqtt");store.putUInt("serial",123);store.putULong64("etotal",900000);
 m.loadEnergyBaseline(123);assert(m.lastAcceptedETotalWh==900000);
 a.appConfig.mqttBroker="broker";a.appConfig.thisSerial=123;i.invData.Serial=456;i.invData.ETotal=500;i.invData.ETotalValid=true;
 assert(m.publishData());StaticJsonDocument<2048> json;assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].as<double>()==0.5);assert(m.energySerial==456);assert(m.lastAcceptedETotalWh==500);
 i.invData.Serial=0;m.energySerial=0;m.lastAcceptedETotalWh=0;fake::nvs.clear();
}
void testEnergyPersistenceRetries(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsFail=false;
 i.invData.Serial=88;i.invData.ETotal=50000;i.invData.ETotalValid=true;i.dispData.Pac=321;a.appConfig.mqttBroker="broker";
 m.loadEnergyBaseline(88);fake::nvsFail=true;
 assert(m.publishData());StaticJsonDocument<2048> json;assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].isNull());assert(json["Pac"]==321);assert(!m.energyBaselinePersisted);
 assert(m.lastAcceptedETotalWh==0&&m.lastPersistedETotalWh==0);
 fake::nvsFail=false;assert(m.publishData());assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].as<double>()==50.0);assert(m.energyBaselinePersisted);assert(m.lastPersistedETotalWh==50000);
 auto writes=fake::nvsWrites;assert(m.publishData());assert(fake::nvsWrites==writes);
 m.loadEnergyBaseline(88);assert(m.lastAcceptedETotalWh==50000);
 i.invData.ETotal=50050;a.client.publishOK=false;writes=fake::nvsWrites;assert(!m.publishData());
 assert(m.lastPersistedETotalWh==50050&&m.lastAcceptedETotalWh==50050);assert(fake::nvsWrites==writes+1);
 auto savedFence=fake::nvs["sma-mqtt/energy-v2"];
 a.client.publishOK=true;assert(m.publishData());assert(fake::nvs["sma-mqtt/energy-v2"]==savedFence);
 m.loadEnergyBaseline(88);assert(m.lastAcceptedETotalWh==50050);
 i.invData.Serial=0;m.loadEnergyBaseline(0);fake::nvs.clear();
}
void testEnergyFenceSurvivesReboot(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsFail=false;a.client.publishOK=true;
 a.appConfig.mqttBroker="broker";a.appConfig.hassDisc=false;i.invData.Serial=77;i.invData.ETotalValid=true;i.dispData.Pac=321;
 m.loadEnergyBaseline(77);i.invData.ETotal=100000;assert(m.publishData());
 i.invData.ETotal=100900;assert(m.publishData());assert(m.lastPersistedETotalWh==100900);
 m.loadEnergyBaseline(77);assert(m.lastAcceptedETotalWh==100900);
 i.invData.ETotal=100500;assert(m.publishData());StaticJsonDocument<2048> json;
 assert(!deserializeJson(json,a.client.messages.back().payload));assert(json["ETotal"].isNull());assert(json["Pac"]==321);
 assert(m.lastAcceptedETotalWh==100900);
 i.invData.Serial=0;m.loadEnergyBaseline(0);fake::nvs.clear();
}
void testEnergyLegacyMigration(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 for(bool blobFormat:{false,true}){
  fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsFail=false;a.client.publishOK=true;
  Preferences old;old.begin("sma-mqtt",false);
  if(blobFormat){uint8_t record[12];for(size_t k=0;k<4;++k)record[k]=uint32_t(88)>>(8*k);for(size_t k=0;k<8;++k)record[k+4]=uint64_t(50000)>>(8*k);old.putBytes("energy-v1",record,sizeof(record));}
  else {old.putUInt("serial",88);old.putULong64("etotal",50000);}
  old.end();m.loadEnergyBaseline(88);assert(m.lastAcceptedETotalWh==50000);assert(!m.energyBaselinePersisted);
  a.appConfig.mqttBroker="broker";i.invData.Serial=88;i.invData.ETotalValid=true;i.invData.ETotal=49999;i.dispData.Pac=321;
  assert(m.publishData());StaticJsonDocument<2048> json;assert(!deserializeJson(json,a.client.messages.back().payload));
  assert(json["ETotal"].isNull()&&json["Pac"]==321);
  i.invData.ETotal=50050;assert(m.publishData());assert(m.energyBaselinePersisted);
  assert(m.lastPersistedETotalWh==50050);
  Preferences migrated;migrated.begin("sma-mqtt",true);uint8_t record[12];
  assert(migrated.getBytesLength("energy-v2")==sizeof(record));assert(migrated.getBytes("energy-v2",record,sizeof(record))==sizeof(record));
  assert(get_u32(record)==88&&get_u64(record+4)==50050);migrated.end();
 }
 i.invData.Serial=0;m.loadEnergyBaseline(0);fake::nvs.clear();
}
void testEnergyStorageReadFailures(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsFail=false;a.client.publishOK=true;
 a.appConfig.mqttBroker="broker";a.appConfig.hassDisc=false;i.invData.Serial=90;i.invData.ETotal=800;i.invData.ETotalValid=true;i.dispData.Pac=321;
 fake::nvsOpenFail=true;m.loadEnergyBaseline(90);assert(!m.energyBaselineLoaded);
 assert(m.publishData());StaticJsonDocument<2048> json;assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].isNull()&&json["Pac"]==321);assert(fake::nvs.count("sma-mqtt/energy-v2")==0);
 fake::nvsOpenFail=false;assert(m.publishData());assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].as<double>()==0.8&&m.energyBaselinePersisted);

 i.invData.ETotal=900;fake::nvsReadFail=true;m.loadEnergyBaseline(90);assert(!m.energyBaselineLoaded);
 assert(m.publishData());assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].isNull()&&json["Pac"]==321);assert(m.lastAcceptedETotalWh==0);
 fake::nvsReadFail=false;assert(m.publishData());assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["ETotal"].as<double>()==0.9&&m.lastAcceptedETotalWh==900);
 i.invData.Serial=0;m.loadEnergyBaseline(0);fake::nvs.clear();
}
bool unsafeCrc(ESP32_SMA_Inverter&i){return !i.isCrcValid(i.pcktBuf[i.pcktBufPos-3],i.pcktBuf[i.pcktBufPos-2]);}
void testLoginAndInitCrc(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;uint16_t seed=0;
 const auto savedIdentity=i.invData;
 const uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=0;
 for(;seed<1000;seed++){
  i.pcktID=seed+1;i.writePacketHeader(i.pcktBuf,1,i.sixff);i.writePacket(i.pcktBuf,14,0xa0,0x0100,0xffff,0xffffffff);
  i.write32(i.pcktBuf,0xfffd040c);i.write32(i.pcktBuf,USERGROUP);i.write32(i.pcktBuf,900);i.write32(i.pcktBuf,time(nullptr));i.write32(i.pcktBuf,0);
  uint8_t pw[12];std::fill(pw,pw+12,0x88);std::fill(pw,pw+4,uint8_t('0'+0x88));i.writeArray(i.pcktBuf,pw,12);i.writePacketTrailer(i.pcktBuf);
  if(unsafeCrc(i))break;
 }
 assert(seed<1000);i.pcktID=seed;b.output.clear();b.input.clear();
 b.sent=[&]{auto tx=decodeOutput(b.output);assert(b.output[b.output.size()-3]!=0x7d&&b.output[b.output.size()-3]!=0x7e);
  std::vector<uint8_t> data(8);put(data,0,get_u32(tx.data()+41));auto reply=response(i,data);
  put(reply,15,0x1234,2);put(reply,17,456);put(reply,29,0xFFFD040D);queueResponse(b,reply);b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);assert(i.pcktID>seed+1);
 assert(i.invData.SUSyID==0x1234&&i.invData.Serial==456);b.sent=nullptr;
 i.invData=savedIdentity;
 for(seed=0;seed<1000;seed++){
  i.pcktID=seed+1;i.writePacketHeader(i.pcktBuf,1,i.sixff);i.writePacket(i.pcktBuf,9,0xa0,0,0xffff,0xffffffff);
  i.write32(i.pcktBuf,0x00000200);i.write32(i.pcktBuf,0);i.write32(i.pcktBuf,0);i.writePacketTrailer(i.pcktBuf);
  if(unsafeCrc(i))break;
 }
 assert(seed<1000);i.pcktID=seed;b.output.clear();b.input.clear();queueL1(b,{0,4,0x70,0,1},2);int step=0;
 b.sent=[&]{if(step++==0)queueL1(b,std::vector<uint8_t>(14),5);else{
   assert(!unsafeCrc(i));std::vector<uint8_t> data(20);put(data,16,123);auto reply=response(i,data);put(reply,29,0x00000201);queueResponse(b,reply);}b.output.clear();};
 assert(i.initialiseSMAConnection()==E_OK);assert(i.pcktID>seed+1);b.sent=nullptr;
 i.invData=savedIdentity;
}
void testLoginFiltersSenderAndKnownSerial(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const uint8_t target[]={0x10,0x21,0x32,0x43,0x54,0x65};
 const uint8_t other[]={0x99,0x88,0x77,0x66,0x55,0x44};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=123456;
 i.pcktID=0x7fff;b.input.clear();b.output.clear();
 b.sent=[&]{
  queueResponse(b,loginResponse(i,0x4321,123456),false,other);
  auto wrongCommand=loginResponse(i,0x5678,123456);put(wrongCommand,29,0xFFFD010F);queueResponse(b,wrongCommand);
  queueResponse(b,loginResponse(i,0x5678,654321));
  queueResponse(b,loginResponse(i,0x1234,123456));
  b.output.clear();
 };
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);
 assert(i.pcktID==0x8000);assert(i.invData.SUSyID==0x1234&&i.invData.Serial==123456);
 assert(b.input.empty());b.sent=nullptr;i.invData=savedIdentity;i.pcktID=savedId;
}
void testLoginBoundsUnrelatedReplies(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const uint8_t target[]={0x10,0x21,0x32,0x43,0x54,0x65};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=123456;
 i.pcktID=40;b.input.clear();b.output.clear();
 size_t oneReplyBytes=0;
 b.sent=[&]{for(unsigned k=0;k<11;++k){queueResponse(b,loginResponse(i,0x1234,654321));if(k==0)oneReplyBytes=b.input.size();}b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_INVRESP);
 assert(i.invData.SUSyID==0x007d&&i.invData.Serial==123456);assert(b.input.size()==oneReplyBytes);
 b.input.clear();
 b.sent=nullptr;i.invData=savedIdentity;i.pcktID=savedId;
}
void testSenderAddressMatching(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 uint8_t targetWithFF[]={0x10,0xff,0x32,0x43,0x54,0x65};
 uint8_t nearMatch[6];std::copy(targetWithFF,targetWithFF+6,nearMatch);nearMatch[1]=0x21;
 b.input.clear();queueL1(b,{0xa1},1,nearMatch);queueL1(b,{0xb2},1,targetWithFF);
 assert(i.getPacket(targetWithFF,1)==E_OK);
 assert(i.pcktBufPos==19&&i.pcktBuf[18]==0xb2);

 uint8_t exactAddress[]={0x10,0x21,0x32,0x43,0x54,0x65};
 b.input.clear();queueL1(b,{0xc3},1,exactAddress);
 assert(i.getPacket(exactAddress,1)==E_OK);
 assert(i.pcktBufPos==19&&i.pcktBuf[18]==0xc3);

 uint8_t broadcast[]={0xff,0xff,0xff,0xff,0xff,0xff};
 const uint8_t anySender[]={0xde,0xad,0xbe,0xef,0x12,0x34};
 b.input.clear();queueL1(b,{0xd4},1,anySender);
 assert(i.getPacket(broadcast,1)==E_OK);
 assert(i.pcktBufPos==19&&i.pcktBuf[18]==0xd4);
}
void testLogoffRetriesCrcCollision(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const uint16_t savedId=i.pcktID;const bool savedWriteFails=b.writeFails;
 uint16_t seed=0;
 for(;seed<1000;++seed){
  i.pcktID=seed+1;i.writePacketHeader(i.pcktBuf,0x01,i.sixff);
  i.writePacket(i.pcktBuf,0x08,0xA0,0x0300,0xFFFF,0xFFFFFFFF);
  i.write32(i.pcktBuf,0xFFFD010E);i.write32(i.pcktBuf,0xFFFFFFFF);
  i.writePacketTrailer(i.pcktBuf);i.writePacketLength(i.pcktBuf);
  if(unsafeCrc(i))break;
 }
 assert(seed<1000);i.pcktID=seed;b.output.clear();b.sent=nullptr;
 i.logoffSMAInverter();assert(i.pcktID>seed+1);
 auto tx=decodeOutput(b.output);assert(tx.size()>=36&&tx.front()==0x7e&&tx.back()==0x7e);
 assert(tx[5]==0x08&&tx[6]==0xA0);assert(get_u16(tx.data()+7)==0xFFFF);
 assert(get_u32(tx.data()+9)==0xFFFFFFFF);assert(get_u16(tx.data()+13)==0x0300);
 assert(get_u16(tx.data()+27)==uint16_t(i.pcktID|0x8000));
 assert(get_u32(tx.data()+29)==0xFFFD010E&&get_u32(tx.data()+33)==0xFFFFFFFF);
 const uint8_t crcLow=tx[tx.size()-3],crcHigh=tx[tx.size()-2];
 assert(crcLow!=0x7d&&crcLow!=0x7e&&crcHigh!=0x7d&&crcHigh!=0x7e);
 uint16_t crc=0xffff;
 for(size_t k=1;k<tx.size()-3;++k){crc^=tx[k];for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0x8408:0);}
 crc^=0xffff;assert(get_u16(tx.data()+tx.size()-3)==crc);

 b.output.clear();b.writeFails=true;i.logoffSMAInverter();assert(b.output.empty());
 b.writeFails=savedWriteFails;i.pcktID=savedId;
}
void testDiscoveryCapacity(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();
 a.appConfig.mqttTopic=std::string(32,'x');a.appConfig.thisSerial=UINT32_MAX;a.appConfig.mqttBroker="broker";
 auto&i=ESP32_SMA_Inverter::getInstance();i.invData.Serial=UINT32_MAX;a.client.messages.clear();
 assert(m.hassAutoDiscover(2700));size_t sensors=0;
 for(auto&message:a.client.messages){if(message.payload.empty())continue;++sensors;StaticJsonDocument<2048> json;assert(!deserializeJson(json,message.payload));assert(json["device"]["name"]==std::string(32,'x')+"-4294967295");}
 assert(sensors==20);
 m.sapString="SMA-12345678";assert(m.publishEspDiscovery("sma/solar/SMA-12345678/esp/state"));
 size_t diagnostics=0;for(auto&message:a.client.messages){if(message.payload.empty())continue;
  StaticJsonDocument<2048> json;assert(!deserializeJson(json,message.payload));if(json["entity_category"]=="diagnostic")++diagnostics;
 }
 assert(diagnostics==11); // four device sensors plus seven poll/MQTT health sensors
 char tiny[20];m.discoveryPublishOK=true;auto count=a.client.messages.size();
 m.sendHassAutoNoClassNoUnit(tiny,sizeof(tiny),2700,"SMA-1","Status","DevStatus","DevStatus");
 assert(!m.discoveryPublishOK);assert(a.client.messages.size()==count);i.invData.Serial=0;
}
void testTopicValidationAndJsonEscaping(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();m.settingsToken="secret";a.appConfig.mqttTopic="SMA";
 for(const char*prefix:{"bad\"topic", "bad\\topic", "bad/#", "bad+", ""}){
  a.webServer.params={{"token","secret"},{"mqttTopic",prefix}};m.handleForm();assert(a.webServer.code==400);assert(a.appConfig.mqttTopic=="SMA");
 }
 char msg[768];m.discoveryPublishOK=true;
 m.sendHassAutoNoClassNoUnit(msg,sizeof(msg),2700,"SMA-1","quoted \"name\" \\","DevStatus","DevStatus");
 assert(m.discoveryPublishOK);StaticJsonDocument<2048> json;assert(!deserializeJson(json,msg));assert(json["name"]=="quoted \"name\" \\");
 assert(!json["availability_template"].isNull());
}
void testProvisionedWifiSurvivesStartup(){
 auto&m=ESP32_SMA_MQTT::getInstance();fake::nvs.clear();WiFi.state=WL_CONNECTED;
 WiFi.compiledBegins=WiFi.storedBegins=0;m.wifiStartup();assert(WiFi.compiledBegins==1);
 try {m.mySmartConfig();assert(false);}catch(const fake::Restart&){}
 Preferences store;store.begin("sma-wifi",true);assert(store.getBool("provisioned"));
 auto compiled=WiFi.compiledBegins,stored=WiFi.storedBegins;m.wifiStartup();
 assert(WiFi.compiledBegins==compiled);assert(WiFi.storedBegins==stored+1);assert(WiFi.persistentEnabled);
 fake::nvs.clear();
}
void testCompiledWifiProvisionedMarker(){
 auto&m=ESP32_SMA_MQTT::getInstance();
 const auto savedNvs=fake::nvs;const bool savedNvsOpenFail=fake::nvsOpenFail,savedNvsReadFail=fake::nvsReadFail;
 const auto savedNvsFailReadKey=fake::nvsFailReadKey;
 const auto savedSSID=fake::stationSSID,savedPassword=fake::stationPassword;
 const bool savedAssociated=fake::stationAssociated;const int savedWifiState=WiFi.state;
 const unsigned savedCompiledBegins=WiFi.compiledBegins,savedStoredBegins=WiFi.storedBegins;
 const bool savedAutoReconnect=WiFi.autoReconnectEnabled,savedPersistent=WiFi.persistentEnabled;
 auto reset=[&](const char*ssid,const char*password){
  fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=false;fake::nvsFailReadKey.clear();
  fake::stationSSID=ssid;fake::stationPassword=password;fake::stationAssociated=false;
  WiFi.state=WL_CONNECTED;WiFi.compiledBegins=WiFi.storedBegins=0;
 };
 auto startup=[&]{m.wifiStartup();};
 const auto previousSSID=std::string("previous-network"),previousPassword=std::string("previous-password");

 // Missing namespace/key is first setup, as is an explicit false marker.
 reset(previousSSID.c_str(),previousPassword.c_str());
 startup();assert(WiFi.compiledBegins==1&&WiFi.storedBegins==0);
 assert(fake::stationSSID==WIFI_SSID&&fake::stationPassword==WIFI_PASSWORD);

 // A missing key in an otherwise existing namespace is also first setup.
 reset(previousSSID.c_str(),previousPassword.c_str());
 Preferences unrelated;assert(unrelated.begin("sma-wifi",false));assert(unrelated.putUInt("other",42)==sizeof(uint32_t));unrelated.end();
 startup();assert(WiFi.compiledBegins==1&&WiFi.storedBegins==0);
 assert(fake::stationSSID==WIFI_SSID&&fake::stationPassword==WIFI_PASSWORD);

 reset(previousSSID.c_str(),previousPassword.c_str());
 Preferences marker;assert(marker.begin("sma-wifi",false));assert(marker.putBool("provisioned",false)==sizeof(bool));marker.end();
 startup();assert(WiFi.compiledBegins==1&&WiFi.storedBegins==0);
 assert(fake::stationSSID==WIFI_SSID&&fake::stationPassword==WIFI_PASSWORD);

 // A true marker retains the driver-owned, already provisioned network.
 reset(previousSSID.c_str(),previousPassword.c_str());
 assert(marker.begin("sma-wifi",false));assert(marker.putBool("provisioned",true)==sizeof(bool));marker.end();
 startup();assert(WiFi.compiledBegins==0&&WiFi.storedBegins==1);
 assert(fake::stationSSID==previousSSID&&fake::stationPassword==previousPassword);

 // Open/read/type/value errors are ambiguous: retry the saved network without
 // writing the compiled credentials over it, while keeping startup bounded.
 reset(previousSSID.c_str(),previousPassword.c_str());
 assert(marker.begin("sma-wifi",false));assert(marker.putBool("provisioned",true)==sizeof(bool));marker.end();
 fake::nvsOpenFail=true;
 startup();
 assert(WiFi.compiledBegins==0);
 assert(fake::stationSSID==previousSSID&&fake::stationPassword==previousPassword);

 reset(previousSSID.c_str(),previousPassword.c_str());
 assert(marker.begin("sma-wifi",false));assert(marker.putBool("provisioned",true)==sizeof(bool));marker.end();
 fake::nvsFailReadKey="sma-wifi/provisioned";WiFi.state=0;const uint64_t readFailureStart=fake::ticks;
 startup();assert(fake::ticks-readFailureStart>=60000&&fake::ticks-readFailureStart<65000);
 assert(WiFi.compiledBegins==0&&WiFi.storedBegins==1);
 assert(WiFi.autoReconnectEnabled);
 assert(fake::stationSSID==previousSSID&&fake::stationPassword==previousPassword);

 reset(previousSSID.c_str(),previousPassword.c_str());
 uint32_t wrongType=1;assert(marker.begin("sma-wifi",false));assert(marker.putUInt("provisioned",wrongType)==sizeof(wrongType));marker.end();
 startup();assert(WiFi.compiledBegins==0&&WiFi.storedBegins==1);
 assert(fake::stationSSID==previousSSID&&fake::stationPassword==previousPassword);

 reset(previousSSID.c_str(),previousPassword.c_str());
 const uint8_t invalidValue=2;assert(marker.begin("sma-wifi",false));assert(marker.putBytes("provisioned",&invalidValue,sizeof(invalidValue))==sizeof(invalidValue));marker.end();
 startup();assert(WiFi.compiledBegins==0&&WiFi.storedBegins==1);
 assert(fake::stationSSID==previousSSID&&fake::stationPassword==previousPassword);
 fake::nvsOpenFail=fake::nvsReadFail=false;fake::nvsFailReadKey=savedNvsFailReadKey;
 fake::nvs=savedNvs;fake::nvsOpenFail=savedNvsOpenFail;fake::nvsReadFail=savedNvsReadFail;
 fake::stationSSID=savedSSID;fake::stationPassword=savedPassword;fake::stationAssociated=savedAssociated;
 WiFi.state=savedWifiState;WiFi.compiledBegins=savedCompiledBegins;WiFi.storedBegins=savedStoredBegins;
 WiFi.autoReconnectEnabled=savedAutoReconnect;WiFi.persistentEnabled=savedPersistent;
}
void testProvisioningTimeouts(){
 auto&m=ESP32_SMA_MQTT::getInstance();fake::nvs.clear();WiFi.state=0;
 for(bool received:{true,false}){
  fake::stationSSID="previous-network";fake::stationPassword="previous-password";
  WiFi.done=received;WiFi.lastSSID="";auto start=fake::ticks;auto stops=WiFi.stops;
  m.mySmartConfig();assert(fake::ticks-start<(received?65000:485000));assert(WiFi.stops==stops+1);
  assert(WiFi.lastSSID=="previous-network");assert(WiFi.SSID().isEmpty());
  assert(fake::stationSSID=="previous-network");Preferences store;store.begin("sma-wifi",true);assert(!store.getBool("provisioned"));
 }
 WiFi.done=true;WiFi.state=WL_CONNECTED;
}
void testDiscoveryAfterReconnectAndBirth(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 a.appConfig.mqttBroker="broker";a.appConfig.mqttTopic="SMA";a.appConfig.hassDisc=true;a.appConfig.thisSerial=55;i.invData.Serial=55;
 m.wifiStartup();a.firstTime=false;a.client.online=false;assert(m.brokerConnect());
 // Retained discovery survives a reconnect; only a birth message or a new
 // identity re-announces it.
 assert(!a.firstTime);a.requestDiscovery();
 a.nextTime=millis()+100000;a.client.messages.clear();a.appLoop();assert(!a.firstTime);
 size_t configs=0;for(auto&msg:a.client.messages)if(msg.topic.find("homeassistant/sensor/SMA-55/")==0&&!msg.payload.empty()){
  StaticJsonDocument<2048> json;assert(!deserializeJson(json,msg.payload));if(json["entity_category"].isNull())++configs;
 }
 assert(configs==20);
 uint8_t online[]={'o','n','l','i','n','e'};a.client.callback(const_cast<char*>("homeassistant/status"),online,sizeof(online));assert(a.firstTime);
 a.appLoop();assert(!a.firstTime);i.invData.Serial=0;
}
void testClockReplyCorrelation(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;i.invData.SUSyID=0x1234;i.invData.Serial=55;
 i.invData.BTAddress[0]=0x20;
 auto clock=[&]{std::vector<uint8_t> data(24);put(data,0,0x00236d00);put(data,4,1800000000);put(data,8,1790000000);put(data,16,36000);put(data,20,7);auto v=response(i,data);put(v,29,0xf000020b);return v;};
 for(int mode=0;mode<8;mode++){
  b.input.clear();b.output.clear();
  b.sent=[&]{auto bad=clock();
   if(mode==0)put(bad,27,i.pcktID-1,2);
   if(mode==1)put(bad,15,999,2);
   if(mode==2)put(bad,17,999);
   if(mode==3)put(bad,23,1,2);
   if(mode==4)put(bad,29,0x51000201);
   if(mode==5)put(bad,41,0x00263f00);
   queueResponse(b,bad,mode==6);if(mode==7)b.input[4]^=1;queueResponse(b,clock());b.output.clear();};
  int32_t now=0,last=0,offset=0;uint32_t count=0;assert(i.readPlantTime(&now,&last,&offset,&count)==E_OK);
  assert(now==1800000000 && offset==36000 && count==7);b.sent=nullptr;
 }
 unsigned writes=0;b.output.clear();b.input.clear();i.btConnected=true;
 b.sent=[&]{auto tx=decodeOutput(b.output);if(get_u32(tx.data()+45)!=0)++writes;
  auto bad=clock();put(bad,27,i.pcktID-1,2);for(int k=0;k<4;k++)queueResponse(b,bad);b.output.clear();};
 int32_t before=0,after=0;assert(i.syncPlantTime(36000,&before,&after)==E_INVRESP);assert(writes==0);
 b.sent=nullptr;i.btConnected=false;i.invData.Serial=0;i.invData.BTAddress[0]=0;
}
void testClockTargetsOneInverter(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const auto saved=i.invData;
 const uint8_t address[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(address,address+6,i.invData.BTAddress);i.invData.SUSyID=0x1234;i.invData.Serial=55;i.btConnected=true;
 b.input.clear();b.output.clear();unsigned reads=0,writes=0;
 b.sent=[&]{
  assert(std::equal(address,address+6,b.output.begin()+10));
  auto tx=decodeOutput(b.output);assert(get_u16(tx.data()+7)==0x1234);assert(get_u32(tx.data()+9)==55);
  if(get_u32(tx.data()+45)!=0){++writes;b.output.clear();return;}
  ++reads;std::vector<uint8_t> data(24);put(data,0,0x00236d00);
  put(data,4,writes?1800000000:1790000000);put(data,8,writes?1800000000:1790000000);
  put(data,16,36000);put(data,20,writes?8:7);auto v=response(i,data);put(v,29,0xf000020b);
  queueResponse(b,v);b.output.clear();
 };
 int32_t before=0,after=0;assert(i.syncPlantTime(36000,&before,&after)==E_OK);
 assert(reads==2&&writes==1&&before==1790000000&&after==1800000000);
 const auto target=i.invData;
 for(int invalid=0;invalid<6;++invalid){
  i.invData=target;
  if(invalid==0)i.invData.Serial=0;
  if(invalid==1)i.invData.Serial=UINT32_MAX;
  if(invalid==2)i.invData.SUSyID=0;
  if(invalid==3)i.invData.SUSyID=UINT16_MAX;
  if(invalid==4)std::fill(i.invData.BTAddress,i.invData.BTAddress+6,0);
  if(invalid==5)std::fill(i.invData.BTAddress,i.invData.BTAddress+6,0xff);
  assert(i.syncPlantTime(36000,&before,&after)==E_BADARG);assert(reads==2&&writes==1);
 }
 b.sent=nullptr;i.btConnected=false;i.invData=saved;
}
void testClosedWriteIsVerified(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();assert(a.saveConfiguration());
 auto files=fake::files;fake::loseWriteOnClose=true;
 assert(!a.saveConfiguration());fake::loseWriteOnClose=false;
 assert(fake::files["/config.txt"]==files["/config.txt"]);
 assert(fake::files["/config.bak"]==files["/config.bak"]);
}
void testMountFailurePreservesFiles(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto config=a.appConfig;auto files=fake::files;
 auto formats=fake::formats;fake::mountable=false;a.configSetup();
 assert(fake::formats==formats);assert(fake::files==files);assert(!a.saveConfiguration());
 fake::mountable=true;a.configurationStorageAvailable=true;a.appConfig=config;
}
void testLargeEscapedConfiguration(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();auto config=a.appConfig;
 a.appConfig.ntphostname=std::string(2000,'"');m.formPage();assert(a.webServer.code==503);
 a.appConfig.ntphostname=std::string(1300,'&');m.formPage();
 assert(a.webServer.code==200||a.webServer.code==503);assert(a.webServer.body.length()<10000);
 a.appConfig=config;
}
void testPasswordWhitespace(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();auto config=a.appConfig;
 m.settingsToken="secret";a.webServer.params={{"token","secret"},{"mqttPasswd"," secret "},{"smapw"," 0000 "}};
 try{m.handleForm();assert(false);}catch(const fake::Restart&){}
 assert(a.appConfig.mqttPasswd==" secret ");assert(a.appConfig.smaInvPass==" 0000 ");
 StaticJsonDocument<2048> json;assert(!deserializeJson(json,fake::files["/config.txt"]));
 assert(json["mqttPasswd"]==" secret ");assert(json["smaInvPass"]==" 0000 ");
 a.appConfig=config;assert(a.saveConfiguration());
}
void testStaleErrorIsIgnored(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;auto record=numericRecord(GridMsTotW,1,1234);
 b.input.clear();b.output.clear();b.sent=[&]{auto stale=response(i,record,1,1,E_LRINOTAVAIL);
  put(stale,27,(i.pcktID-1)&0x7fff,2);queueResponse(b,stale);queueResponse(b,response(i,record));b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_OK);assert(i.dispData.Pac==1234);b.sent=nullptr;
}
void testMalformedPacketClearsSession(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();
 queueL1(b,{1,2,3,4});b.input[3]^=1;queueL1(b,{5,6,7,8});i.btConnected=true;
 assert(i.getPacket(i.sixff,1)==E_CHKSUM);assert(!i.btConnected);assert(b.input.empty());
}
void testManyValidFragments(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,response(i,numericRecord(GridMsTotW,1,4567)));
  std::vector<uint8_t> wire(b.input.begin()+18,b.input.end());b.input.clear();
  for(size_t k=0;k<wire.size();k+=5){size_t end=std::min(k+5,wire.size());
   queueL1(b,{wire.begin()+k,wire.begin()+end},end==wire.size()?1:8);}
  b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_OK);assert(i.dispData.Pac==4567);b.sent=nullptr;
}
void testCflAttemptsAreTransactional(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const int32_t savedPac=i.invData.Pac,savedFreq=i.invData.Freq;
 const float savedDisplayPac=i.dispData.Pac,savedDisplayFreq=i.dispData.Freq;
 const time_t savedLastTime=i.invData.LastTime;const E_RC savedStatus=i.invData.status;
 i.invData.Pac=111;i.dispData.Pac=111;i.invData.Freq=900;i.dispData.Freq=9;
 i.invData.LastTime=123;i.invData.status=E_OK;

 b.input.clear();b.output.clear();
 b.sent=[&]{auto first=response(i,numericRecord(GridMsTotW,1,2222));put(first,25,1,2);
  queueResponse(b,first);queueResponse(b,response(i,numericRecord(GridMsHz,1,5000)),true);b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_CHKSUM);b.sent=nullptr;
 assert(i.invData.Pac==111&&i.dispData.Pac==111);assert(i.invData.LastTime==123&&i.invData.status==E_OK);

 unsigned attempt=0;b.input.clear();b.output.clear();
 b.sent=[&]{++attempt;
  if(attempt==1){auto first=response(i,numericRecord(GridMsTotW,1,3333));put(first,25,1,2);
   queueResponse(b,first);queueResponse(b,response(i,numericRecord(GridMsHz,1,6000)),true);}
  else queueResponse(b,response(i,numericRecord(GridMsHz,1,5000)));
  b.output.clear();};
 assert(i.getInverterData(SpotACTotalPower)==E_OK);b.sent=nullptr;assert(attempt==2);
 // The successful retry omits Pac, so the rejected first attempt's Pac is not retained.
 assert(i.invData.Pac==111&&i.dispData.Pac==111);assert(i.invData.LastTime==123);
 assert(i.invData.Freq==5000&&i.dispData.Freq==50);

 b.input.clear();b.output.clear();
 b.sent=[&]{auto first=response(i,numericRecord(GridMsTotW,1,4444));put(first,25,1,2);
  queueResponse(b,first);queueResponse(b,response(i,numericRecord(GridMsHz,1,5100)));b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_OK);b.sent=nullptr;
 assert(i.invData.Pac==4444&&i.dispData.Pac==4444);
 assert(i.invData.Freq==5100&&i.dispData.Freq==51);
 i.invData.Pac=savedPac;i.dispData.Pac=savedDisplayPac;i.invData.Freq=savedFreq;i.dispData.Freq=savedDisplayFreq;
 i.invData.LastTime=savedLastTime;i.invData.status=savedStatus;
}
void testBluetoothWriteFailureAndSignalValidity(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();b.output.clear();b.writeFails=true;
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_NODATA);assert(b.output.empty());
 i.dispData.BTSigStrength=70;assert(!i.getBT_SignalStrength());assert(std::isnan(i.dispData.BTSigStrength));
 b.writeFails=false;
}
void testEmptyStatusIsUnavailable(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();
 std::vector<uint8_t> record(40);put(record,0,(8U<<24)|(uint32_t(OperationHealth)<<8));put(record,8,0xfffffe);
 assert(query(record)==E_OK);assert(i.invData.DevStatus==0xfffffd);i.invData.GridRelay=0xfffffd;
 assert(m.publishData());StaticJsonDocument<2048> json;assert(!deserializeJson(json,a.client.messages.back().payload));
 assert(json["DevStatus"].isNull());assert(json["GridRelay"].isNull());
}
void testInitReplyAndTrailerBounds(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 for(size_t length:{size_t(61),size_t(62),size_t(63),size_t(64)}){
  b.input.clear();b.output.clear();queueL1(b,{0,4,0x70,0,1},2);int step=0;
  b.sent=[&]{if(step++==0)queueL1(b,std::vector<uint8_t>(14),5);else{
   std::vector<uint8_t> data(20);put(data,16,123);auto stale=response(i,data);
   put(stale,29,0x51000201);queueResponse(b,stale);data.resize(length-44);
   auto actual=response(i,data);put(actual,29,0x00000201);queueResponse(b,actual);}
   b.output.clear();};
  auto serial=i.invData.Serial;
  assert(i.initialiseSMAConnection()==(length==64?E_OK:E_INVRESP));
  assert(i.invData.Serial==(length==64?123:serial));b.sent=nullptr;
 }
 i.invData.Serial=0;
}
void testInitRefreshesModelWhenSerialChanges(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const uint8_t target[]={0x21,0x32,0x43,0x54,0x65,0x76};
 std::copy(target,target+6,i.invData.BTAddress);
 i.invData.SUSyID=0x1111;i.invData.Serial=111;

 auto initialize=[&](uint32_t serial){
  b.input.clear();b.output.clear();queueL1(b,{0,4,0x70,0,1},2,target);int step=0;
  b.sent=[&]{if(step++==0)queueL1(b,std::vector<uint8_t>(14),5,target);else{
   std::vector<uint8_t> data(20);put(data,16,serial);
   auto reply=response(i,data);put(reply,29,0x00000201);queueResponse(b,reply);
  }b.output.clear();};
  const auto rc=i.initialiseSMAConnection();b.sent=nullptr;return rc;
 };

 assert(initialize(222)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x007D);
 b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,loginResponse(i,0x2222,222));b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x2222);

 // A repeated initialization for the same serial must retain the learned model
 // and reject an otherwise-correlated login reply from another model.
 assert(initialize(222)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x2222);
 b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,loginResponse(i,0x3333,222));
  queueResponse(b,loginResponse(i,0x2222,222));b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x2222);assert(b.input.empty());
 b.sent=nullptr;b.input.clear();b.output.clear();i.invData=savedIdentity;i.pcktID=savedId;
}
void testClockExpiresDuringRead(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;i.btConnected=true;i.invData.Serial=55;
 i.invData.SUSyID=0x1234;i.invData.BTAddress[0]=0x20;
 b.input.clear();b.output.clear();unsigned sends=0;uint32_t deadline=millis()+1000;
 b.sent=[&]{++sends;std::vector<uint8_t> data(24);put(data,0,0x00236d00);put(data,4,1800000000);
  put(data,8,1790000000);put(data,16,36000);put(data,20,7);auto v=response(i,data);put(v,29,0xf000020b);
  fake::ticks+=1500;queueResponse(b,v);b.output.clear();};
 int32_t before=0,after=0;assert(i.syncPlantTime(36000,&before,&after,&deadline)==E_EXPIRED);assert(sends==1);
 b.sent=nullptr;i.btConnected=false;i.invData.Serial=0;i.invData.BTAddress[0]=0;
}
void testConnectRetainsEarlyHandshakeAndCleansFailedSession(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const bool savedConnectResult=b.connectResult;auto savedSent=b.sent;auto savedDuringConnect=b.duringConnect;
 uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=0;i.pcktID=1;
 b.input.clear();b.output.clear();b.connectResult=true;assert(i.begin("early-handshake-regression",true));

 bool injectedBeforeConnectReturned=false;int step=0;
 b.duringConnect=[&]{
  const std::vector<uint8_t> initPayload={0,4,0x70,0,1};
  queueL1(b,initPayload,2,i.invData.BTAddress);
  injectedBeforeConnectReturned=true;
 };
 b.sent=[&]{
  if(step++==0) queueL1(b,std::vector<uint8_t>(14),5,i.invData.BTAddress);
  else {
   std::vector<uint8_t> data(20);put(data,16,123);auto reply=response(i,data);
   put(reply,29,0x00000201);queueResponse(b,reply);
  }
  b.output.clear();
 };
 assert(i.connect(target));assert(injectedBeforeConnectReturned);
 assert(i.initialiseSMAConnection()==E_OK);assert(i.invData.Serial==123);b.sent=nullptr;

 // Bytes left from an old session must be removed before the next attempt.
 const uint8_t oldSessionByte=0x31,newSessionByte=0x52;
 b.inject(&oldSessionByte,1);
 b.duringConnect=[&]{b.inject(&newSessionByte,1);};
 assert(i.connect(target));assert(i.BTgetByte()==newSessionByte);

 // Data received during a failed connection attempt must not seed a retry.
 const uint8_t failedAttemptByte=0x61,retryByte=0x72;
 b.connectResult=false;b.duringConnect=[&]{b.inject(&failedAttemptByte,1);};
 assert(!i.connect(target));
 b.connectResult=true;b.duringConnect=[&]{b.inject(&retryByte,1);};
 assert(i.connect(target));assert(i.BTgetByte()==retryByte);

 i.disconnect();b.duringConnect=savedDuringConnect;b.sent=savedSent;b.connectResult=savedConnectResult;
 i.invData=savedIdentity;i.pcktID=savedId;
}
static unsigned primaryGapCallbackCalls=0, replacementGapCallbackCalls=0;
static void primaryGapCallback(esp_bt_gap_cb_event_t,esp_bt_gap_cb_param_t*){++primaryGapCallbackCalls;}
static void replacementGapCallback(esp_bt_gap_cb_event_t,esp_bt_gap_cb_param_t*){++replacementGapCallbackCalls;}
void testBluetoothAuthRecoveryMatchesTargetPeer(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const bool savedConnectResult=b.connectResult;auto savedDuringConnect=b.duringConnect;
 const unsigned savedUnpairs=b.unpairCalls;
 uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 const uint8_t other[]={0x90,0x80,0x70,0x60,0x50,0x40};
 fake_gap_sdk::reset();primaryGapCallbackCalls=replacementGapCallbackCalls=0;

 // The production wrapper registers itself with the fake SDK in another
 // translation unit and forwards all GAP events to the original core handler.
 fake_gap_sdk::dispatchAuthDuringNextRegistration(target,ESP_BT_STATUS_SUCCESS);
 assert(__wrap_esp_bt_gap_register_callback(primaryGapCallback)==ESP_OK);
 fake_gap_sdk::waitForRegistrationDispatch();
 assert(fake_gap_sdk::registrationCalls()==1);
 assert(fake_gap_sdk::registeredCallback()!=nullptr);
 assert(fake_gap_sdk::registeredCallback()!=primaryGapCallback);
 const auto installedGapProxy=fake_gap_sdk::registeredCallback();
 assert(primaryGapCallbackCalls==1); // event delivered at the install boundary
 assert(__wrap_esp_bt_gap_register_callback(nullptr)==FAKE_NULL_GAP_CALLBACK_ERROR);
 assert(fake_gap_sdk::registrationCalls()==2);
 assert(fake_gap_sdk::registeredCallback()==installedGapProxy);
 fake_gap_sdk::dispatchEvent(ESP_BT_GAP_DISC_STATE_CHANGED_EVT);
 assert(primaryGapCallbackCalls==2); // SDK rejection preserves its prior callback
 fake_gap_sdk::setNextRegistrationResult(-7);
 assert(__wrap_esp_bt_gap_register_callback(replacementGapCallback)==-7);
 assert(fake_gap_sdk::registrationCalls()==3);
 assert(fake_gap_sdk::registeredCallback()==installedGapProxy);
 fake_gap_sdk::dispatchEvent(ESP_BT_GAP_DISC_STATE_CHANGED_EVT);
 assert(primaryGapCallbackCalls==3&&replacementGapCallbackCalls==0);

 assert(i.begin("auth-peer-regression",true));
 b.connectResult=false;b.duringConnect=nullptr;
 const unsigned initialUnpairs=b.unpairCalls;

 // A failed auth event outside a connect attempt is ignored.
 fake_gap_sdk::dispatchAuth(target,1);
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs&&!i.takeReconnectRequest());

 // Another peer's failed pairing during the outgoing attempt must not be
 // mistaken for an inverter failure, even though the connect also fails.
 b.duringConnect=[&]{fake_gap_sdk::dispatchAuth(other,1);};
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs&&!i.takeReconnectRequest());

 // Successful auth events are not recovery triggers. A success from another
 // peer after a matching success remains unrelated to the target.
 b.duringConnect=[&]{
  fake_gap_sdk::dispatchAuth(target,ESP_BT_STATUS_SUCCESS);
  fake_gap_sdk::dispatchAuth(other,ESP_BT_STATUS_SUCCESS);
 };
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs&&!i.takeReconnectRequest());

 // A matching target failure is latched even if an unrelated success follows;
 // the existing one-time recovery removes the target bond and requests retry.
 b.duringConnect=[&]{
  fake_gap_sdk::dispatchAuth(target,1);
  fake_gap_sdk::dispatchAuth(other,ESP_BT_STATUS_SUCCESS);
 };
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs+1&&i.takeReconnectRequest());

 // Later matching failures do not repeat the existing one-time unpair.
 b.duringConnect=[&]{fake_gap_sdk::dispatchAuth(target,1);};
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs+1&&!i.takeReconnectRequest());
 assert(primaryGapCallbackCalls==10&&replacementGapCallbackCalls==0);

 i.disconnect();b.duringConnect=savedDuringConnect;b.connectResult=savedConnectResult;
 i.invData=savedIdentity;i.pcktID=savedId;
}
void testServicesRunDuringReceive(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();
 auto config=a.appConfig;auto savedOnLoop=a.client.onLoop;
 auto savedWiFiStatus=WiFi.onStatus;auto savedResponseWrite=a.webServer.onResponseWrite;
 const bool savedOnline=a.client.online,savedPublishOK=a.client.publishOK;
 const bool savedDiscoveryLoaded=m.discoveryIdentityLoaded;const String savedDiscoveryIdentity=m.discoveryIdentity;
 const unsigned long savedStatusTime=m.lastEspStatusMillis;const int wifiState=WiFi.state;
 a.appConfig.mqttBroker="broker";a.appConfig.hassDisc=false;WiFi.state=WL_CONNECTED;
 a.client.online=true;a.client.publishOK=true;a.client.messages.clear();a.client.loops=0;
 m.discoveryIdentityLoaded=true;m.discoveryIdentity="";m.lastEspStatusMillis=0;
 i.serialBT.input.clear();i.lastServiceMillis=millis()-100;
 assert(i.begin("receive-buffer-regression",true));
 i.serialBT.connectResult=true;uint8_t btAddress[6]={};assert(i.connect(btAddress));
 std::vector<uint8_t> frame(COMMBUFSIZE);
 for(size_t n=0;n<frame.size();++n)frame[n]=static_cast<uint8_t>(n);
 bool injected=false,slowMqttLoopEntered=false,slowHttpWriteEntered=false;
 a.client.onLoop=[&]{slowMqttLoopEntered=true;fake::ticks+=25000UL;};
 a.webServer.on("/slow-response-test",HTTP_GET,[&]{a.webServer.send(200,"text/plain","delayed response");});
 a.webServer.onResponseWrite=[&](size_t){slowHttpWriteEntered=true;fake::ticks+=25000UL;};
 WiFi.onStatus=[&]{if(!injected){injected=true;i.serialBT.inject(frame.data(),frame.size());}};
 WiFiClient httpRequest("GET /slow-response-test HTTP/1.1\r\nHost: test\r\n\r\n");
 a.webServer._server.pending.push_back(httpRequest);
 i.setServiceCallback([]{ESP32_SMA_MQTT::getInstance().wifiLoop(true);});
 auto loops=a.client.loops;auto handles=fake::httpAcceptPolls;assert(i.BTgetByte()==frame[0]);
 assert(!i.readTimeout);assert(injected);assert(a.client.loops==loops);assert(!slowMqttLoopEntered);
 assert(a.client.messages.empty());assert(fake::httpAcceptPolls==handles);
 assert(!slowHttpWriteEntered);assert(httpRequest.socket->open);
 assert(i.BTgetByte()==frame[1]);
 for(size_t n=2;n<frame.size();++n)assert(i.BTgetByte()==frame[n]);
 WiFi.onStatus=nullptr;
 const uint64_t normalStart=fake::ticks;m.wifiLoop();
 assert(slowMqttLoopEntered);assert(a.client.loops==loops+1);assert(fake::ticks-normalStart>=25000UL);
 assert(fake::httpAcceptPolls==handles+1);assert(slowHttpWriteEntered);
 assert(!httpRequest.socket->open);assert(a.webServer.body=="delayed response");
 bool statusPublished=false;
 for(const auto&message:a.client.messages)if(message.topic.find("/esp/state")!=std::string::npos)statusPublished=true;
 assert(statusPublished);
 std::vector<uint8_t> overflow(COMMBUFSIZE*2+1,0x5a);
 i.serialBT.inject(overflow.data(),overflow.size());i.BTgetByte();assert(i.readTimeout);
 i.disconnect();assert(i.connect(btAddress));
 const uint8_t nextSessionByte=0x42;i.serialBT.inject(&nextSessionByte,1);
 assert(i.BTgetByte()==nextSessionByte);assert(!i.readTimeout);
 i.setServiceCallback(nullptr);a.appConfig=config;WiFi.state=wifiState;
 a.client.onLoop=savedOnLoop;a.client.online=savedOnline;a.client.publishOK=savedPublishOK;
 m.discoveryIdentityLoaded=savedDiscoveryLoaded;m.discoveryIdentity=savedDiscoveryIdentity;
 m.lastEspStatusMillis=savedStatusTime;a.client.messages.clear();
 WiFi.onStatus=savedWiFiStatus;
 a.webServer.onResponseWrite=savedResponseWrite;
}
void testBluetoothInitializationRetries(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const AppConfig savedConfig=a.appConfig;
 const bool savedReady=a.bluetoothReady,savedAddressValid=a.bluetoothAddressValid;
 const bool savedRetryScheduled=a.bluetoothInitRetryScheduled;
 const uint32_t savedNextTime=a.nextTime,savedInitDeadline=a.nextBluetoothInitAttempt;
 const uint32_t savedRetryMs=a.bluetoothInitRetryMs;
 const int savedScanRate=a.lastAdjustedScanRate,savedWifiState=WiFi.state;
 const bool savedNightTime=a.nightTime,savedClockRequest=a.clockSyncRequested;
 const uint32_t savedClockDeadline=a.clockSyncRequestDeadline;
 const String savedClockStatus=a.clockSyncStatus;
 const bool savedValidTime=fake::validTime;
 const int savedLocalHour=fake::localHourOverride;
 const uint64_t savedTicks=fake::ticks;
 const bool savedMqttOnline=a.client.online,savedSerialSavePending=a.serialSavePending;
 const bool savedReadingPending=a.readingPending;
 const uint32_t savedSerialSaveAttempt=a.nextSerialSaveAttempt;
 const auto savedSerialInput=Serial.input;
 const auto savedBeginResults=b.beginResults;
 const bool savedBeginResult=b.beginResult,savedConnectResult=b.connectResult;
 const auto savedDuringConnect=b.duringConnect;
 const auto savedServiceCallback=i.serviceCallback;
 const bool savedCallbackActive=i.btRxCallbackActive.load();
 const bool savedDiscard=i.discardBtRx.load();

 a.appConfig.mqttBroker="";a.appConfig.hassDisc=false;
 a.bluetoothReady=false;a.bluetoothAddressValid=true;
 a.bluetoothInitRetryScheduled=false;
 a.bluetoothInitRetryMs=1000;a.nextBluetoothInitAttempt=0;
 a.lastAdjustedScanRate=NIGHTSCANRATE;a.nextTime=0;
 a.clockSyncRequested=false;a.serialSavePending=false;a.readingPending=false;
 WiFi.state=WL_CONNECTED;a.client.online=true;
 fake::validTime=false;fake::localHourOverride=-1;
 b.beginResults.clear();b.beginResults.push_back(false);b.beginResults.push_back(true);
 b.beginResult=true;b.connectResult=false;b.duringConnect=nullptr;
 Serial.input.clear();
 const unsigned beginCalls=b.beginCalls,connectCalls=b.connectCalls,unpairCalls=b.unpairCalls;
 const unsigned endCalls=b.endCalls,loops=a.client.loops,accepts=fake::httpAcceptPolls;

 // The failed attempt schedules its retry across millis() rollover and tears
 // down the pinned core's partially initialized static Bluetooth resources.
 const uint64_t wrap=uint64_t(UINT32_MAX)+1;
 fake::ticks=wrap-500;
 a.nextTime=millis();
 a.initializeBluetoothIfDue();
 assert(!a.bluetoothReady&&b.beginCalls==beginCalls+1);
 assert(a.bluetoothInitRetryScheduled);
 assert(b.endCalls==endCalls+1&&!i.btRxCallbackActive.load());
 assert(i.serviceCallback==nullptr);
 const uint32_t retryAt=a.nextBluetoothInitAttempt;
 assert(retryAt<1000&&a.bluetoothInitRetryMs==2000);

 // A poll remains queued, while unpair is refused, networking and serial
 // handling keep running during the bounded retry backoff at night.
 a.requestClockSync();
 for(char c:std::string("unpair\npoll\n"))Serial.input.push_back(c);
 fake::ticks=wrap+retryAt-500;
 a.appLoop();
 assert(!a.bluetoothReady&&b.beginCalls==beginCalls+1);
 assert(b.connectCalls==connectCalls&&b.unpairCalls==unpairCalls);
 assert(a.clockSyncRequested);
 assert(a.client.loops>loops&&fake::httpAcceptPolls>accepts);

 // Once the retry deadline passes, initialization succeeds and the queued
 // poll is attempted immediately instead of waiting for the night interval.
 fake::ticks=wrap+retryAt+10;
 a.appLoop();
 assert(a.bluetoothReady&&b.beginCalls==beginCalls+2);
 assert(!a.bluetoothInitRetryScheduled);
 assert(i.serviceCallback!=nullptr);
 assert(b.connectCalls==connectCalls+1&&b.unpairCalls==unpairCalls);
 assert(a.clockSyncRequested);

 i.setServiceCallback(nullptr);
 a.appConfig=savedConfig;a.bluetoothReady=savedReady;
 a.bluetoothInitRetryScheduled=savedRetryScheduled;
 a.bluetoothAddressValid=savedAddressValid;a.nextTime=savedNextTime;
 a.nextBluetoothInitAttempt=savedInitDeadline;a.bluetoothInitRetryMs=savedRetryMs;
 a.lastAdjustedScanRate=savedScanRate;a.nightTime=savedNightTime;
 a.clockSyncRequested=savedClockRequest;a.clockSyncRequestDeadline=savedClockDeadline;
 a.clockSyncStatus=savedClockStatus;WiFi.state=savedWifiState;
 fake::validTime=savedValidTime;fake::localHourOverride=savedLocalHour;
 fake::ticks=savedTicks;Serial.input=savedSerialInput;
 a.client.online=savedMqttOnline;a.serialSavePending=savedSerialSavePending;
 a.readingPending=savedReadingPending;a.nextSerialSaveAttempt=savedSerialSaveAttempt;
 b.beginResults=savedBeginResults;b.beginResult=savedBeginResult;b.connectResult=savedConnectResult;
 b.duringConnect=savedDuringConnect;i.setServiceCallback(savedServiceCallback);
 i.btRxCallbackActive.store(savedCallbackActive);i.discardBtRx.store(savedDiscard);
}
void testSerialSaveRetry(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto config=a.appConfig;auto oldSerial=i.invData.Serial;a.appConfig.hassDisc=false;a.appConfig.mqttBroker="";
 m.discoveryIdentityLoaded=true;m.discoveryIdentity="";a.appConfig.thisSerial=122;i.invData.Serial=123;
 a.nextTime=millis()+100000;a.readingPending=false;a.serialSavePending=false;fake::writeLimit=0;a.appLoop();
 assert(a.serialSavePending);assert(a.appConfig.thisSerial==123);fake::writeLimit=SIZE_MAX;fake::ticks+=30001;
 a.appLoop();assert(!a.serialSavePending);StaticJsonDocument<2048> json;assert(!deserializeJson(json,fake::files["/config.txt"]));
 assert(json["thisserial"]==123);a.appConfig=config;i.invData.Serial=oldSerial;assert(a.saveConfiguration());
}
void testPendingReadingRetry(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();auto&m=ESP32_SMA_MQTT::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto config=a.appConfig;auto inv=i.invData;auto disp=i.dispData;
 a.appConfig.mqttBroker="broker";a.appConfig.hassDisc=false;a.appConfig.scanRate=60;a.appConfig.thisSerial=42;i.invData.Serial=42;i.dispData.Pac=321;
 m.discoveryIdentityLoaded=true;m.discoveryIdentity="";a.pendingReading=i.invData;a.pendingDisplay=i.dispData;
 a.nightTime=false;a.pendingReadingMaxAgeMillis=300000;
 fake::ticks=UINT32_MAX-3000ULL;a.pendingReadingAcquiredMillis=millis();
 a.readingPending=true;a.nextPublishAttempt=millis();a.nextTime=millis()+100000;
 WiFi.state=WL_CONNECTED;a.client.messages.clear();
 a.client.publishOK=false;a.appLoop();assert(a.readingPending);
 i.dispData.Pac=999;a.client.publishOK=true;fake::ticks+=5001;a.appLoop();assert(!a.readingPending);
 const std::string stateTopic="SMA-42/state";unsigned statePublishes=0;
 for(const auto&message:a.client.messages)if(message.topic==stateTopic){++statePublishes;StaticJsonDocument<2048> json;assert(!deserializeJson(json,message.payload));assert(json["Pac"]==321);}
 assert(statePublishes==1);

 // A retry at the normal nighttime cadence remains inside the same 45-minute
 // expiry window used by nighttime discovery.
 a.pendingReading=i.invData;a.pendingReading.Serial=42;a.pendingDisplay=i.dispData;a.pendingDisplay.Pac=654;
 a.pendingReadingMaxAgeMillis=2700UL*1000UL;a.pendingReadingAcquiredMillis=millis();a.readingPending=true;
 a.nextPublishAttempt=0;a.client.messages.clear();fake::ticks+=16UL*60UL*1000UL;a.nextTime=millis()+100000;
 a.appLoop();assert(!a.readingPending);statePublishes=0;
 for(const auto&message:a.client.messages)if(message.topic==stateTopic){++statePublishes;StaticJsonDocument<2048> json;assert(!deserializeJson(json,message.payload));assert(json["Pac"]==654);}
 assert(statePublishes==1);

 // Unsigned elapsed-time arithmetic must discard a sample whose expiry passes
 // while millis() wraps, without publishing it after connectivity returns.
 a.pendingReading=i.invData;a.pendingReading.Serial=42;a.pendingDisplay=i.dispData;a.pendingDisplay.Pac=777;
 a.pendingReadingMaxAgeMillis=300000;a.readingPending=true;
 fake::ticks=UINT32_MAX-1000ULL;a.pendingReadingAcquiredMillis=millis();a.nextPublishAttempt=millis();
 a.client.messages.clear();fake::ticks+=300001UL;a.nextTime=millis()+100000;a.appLoop();
 assert(!a.readingPending);statePublishes=0;
 for(const auto&message:a.client.messages)if(message.topic==stateTopic)++statePublishes;
 assert(statePublishes==0);
 a.appConfig=config;i.invData=inv;i.dispData=disp;
}
void testDiscoveryIdentityCleanup(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto config=a.appConfig;auto serial=i.invData.Serial;fake::nvs.clear();
 fake::nvsOpenFail=fake::nvsReadFail=fake::nvsBlobDataReadFail=fake::nvsIdentityTypeMismatch=fake::nvsFail=false;
 m.discoveryIdentityLoaded=false;m.discoveryIdentity="";m.discoveryMigrationAttempted=false;
 a.appConfig.mqttBroker="broker";a.appConfig.mqttTopic="OLD";a.appConfig.hassDisc=true;a.appConfig.thisSerial=55;i.invData.Serial=55;
 // An absent namespace/key is the legacy first-install case and captures the
 // loaded identity before any later topic change can replace it.
 assert(fake::nvs.count("sma-discovery/identity")==0);
 assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="OLD-55");
 const auto savedOldIdentity=fake::nvs.at("sma-discovery/identity");
 assert(savedOldIdentity.size()==sizeof("OLD-55"));
 assert(std::memcmp(savedOldIdentity.data(),"OLD-55",sizeof("OLD-55"))==0);

 // A topic change followed by reboot must not treat a transient NVS read
 // failure as a missing legacy record and overwrite OLD-55 with NEW-55.
 a.appConfig.mqttTopic="NEW";m.discoveryIdentityLoaded=false;m.discoveryIdentity="";
 fake::nvsBlobDataReadFail=true;fake::ticks+=5001;
 a.client.messages.clear();assert(!m.prepareDiscovery(a.appConfig));
 assert(!m.discoveryIdentityLoaded&&m.discoveryIdentity.isEmpty());
 assert(fake::nvs.at("sma-discovery/identity")==savedOldIdentity);
 for(const auto&message:a.client.messages)assert(message.topic!="homeassistant/sensor/OLD-55/esp_ip/config");

 // Open errors and wrong NVS types are also errors, not first-install cases.
 fake::nvsBlobDataReadFail=false;fake::nvsOpenFail=true;
 assert(!m.loadDiscoveryIdentity());assert(!m.discoveryIdentityLoaded);
 assert(fake::nvs.at("sma-discovery/identity")==savedOldIdentity);
 fake::nvsOpenFail=false;fake::nvsReadFail=true;
 assert(!m.loadDiscoveryIdentity());assert(!m.discoveryIdentityLoaded);
 assert(fake::nvs.at("sma-discovery/identity")==savedOldIdentity);
 fake::nvsReadFail=false;fake::nvsIdentityTypeMismatch=true;
 assert(!m.loadDiscoveryIdentity());assert(!m.discoveryIdentityLoaded);
 assert(fake::nvs.at("sma-discovery/identity")==savedOldIdentity);
 fake::nvsIdentityTypeMismatch=false;

 // Once storage recovers, retry the old identity cleanup and only then save
 // the new topic identity.
 fake::ticks+=5001;a.client.messages.clear();
 assert(m.prepareDiscovery(a.appConfig));
 bool removed=false;for(auto&message:a.client.messages)if(message.topic=="homeassistant/sensor/OLD-55/esp_ip/config"){
  assert(message.payload.empty()&&message.retained);removed=true;}
 assert(removed);assert(m.discoveryIdentity=="NEW-55");
 assert(std::string(reinterpret_cast<const char*>(fake::nvs.at("sma-discovery/identity").data()))=="NEW-55");

 // Valid-length but malformed records are left intact for diagnosis/recovery.
 const std::vector<uint8_t> malformed={'N','E','W','-','x',0};
 fake::nvs["sma-discovery/identity"]=malformed;m.discoveryIdentityLoaded=false;m.discoveryIdentity="";
 assert(!m.loadDiscoveryIdentity());assert(!m.discoveryIdentityLoaded);
 assert(fake::nvs.at("sma-discovery/identity")==malformed);
 const std::vector<uint8_t> oversized(45,'x');
 fake::nvs["sma-discovery/identity"]=oversized;
 assert(!m.loadDiscoveryIdentity());assert(!m.discoveryIdentityLoaded);
 assert(fake::nvs.at("sma-discovery/identity")==oversized);
 fake::nvs["sma-discovery/identity"]={};
 assert(!m.loadDiscoveryIdentity());assert(!m.discoveryIdentityLoaded);
 assert(fake::nvs.at("sma-discovery/identity").empty());

 // Restore a valid record before exercising the ordinary serial-change path.
 fake::nvs["sma-discovery/identity"]=savedOldIdentity;m.discoveryIdentityLoaded=false;m.discoveryIdentity="";
 assert(m.loadDiscoveryIdentity());assert(m.discoveryIdentity=="OLD-55");
 a.appConfig.mqttTopic="NEW";fake::ticks+=5001;
 assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="NEW-55");
 i.invData.Serial=66;a.client.publishOK=false;fake::ticks+=5001;assert(!m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="NEW-55");
 a.client.publishOK=true;fake::ticks+=5001;assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="NEW-66");
 a.appConfig.hassDisc=false;fake::nvsFail=true;fake::ticks+=5001;
 assert(!m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="NEW-66");
 fake::nvsFail=false;fake::ticks+=5001;assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity.isEmpty());
 m.discoveryIdentityLoaded=false;assert(m.loadDiscoveryIdentity());assert(m.discoveryIdentity.isEmpty());
 fake::nvsBlobDataReadFail=fake::nvsIdentityTypeMismatch=false;
 a.appConfig=config;i.invData.Serial=serial;
}
void testDiscoveryMigrationLongUptimeAndRollover(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 const AppConfig savedConfig=a.appConfig;const auto savedSerial=i.invData.Serial;const auto savedTicks=fake::ticks;
 const auto savedNvs=fake::nvs;const auto savedMessages=a.client.messages;
 const bool savedNvsOpenFail=fake::nvsOpenFail,savedNvsReadFail=fake::nvsReadFail;
 const bool savedNvsBlobDataReadFail=fake::nvsBlobDataReadFail,savedNvsIdentityTypeMismatch=fake::nvsIdentityTypeMismatch;
 const bool savedNvsFail=fake::nvsFail;
 const bool savedOnline=a.client.online,savedPublishOK=a.client.publishOK,savedConnectOK=a.client.connectOK;
 const bool savedIdentityLoaded=m.discoveryIdentityLoaded,savedMigrationAttempted=m.discoveryMigrationAttempted;
 const String savedIdentity=m.discoveryIdentity;const uint32_t savedLastAttempt=m.lastDiscoveryMigrationAttemptMillis;
 fake::nvs.clear();fake::nvsOpenFail=fake::nvsReadFail=fake::nvsBlobDataReadFail=fake::nvsIdentityTypeMismatch=fake::nvsFail=false;
 a.appConfig.mqttBroker="broker";a.appConfig.mqttTopic="FIRST";a.appConfig.hassDisc=true;a.appConfig.thisSerial=55;
 i.invData.Serial=55;a.client.online=true;a.client.publishOK=true;a.client.connectOK=true;a.client.messages.clear();
 m.discoveryIdentityLoaded=false;m.discoveryIdentity="";m.discoveryMigrationAttempted=false;m.lastDiscoveryMigrationAttemptMillis=0;

 // A new identity can be loaded and recorded immediately even when the first
 // discovery call happens after the signed-millis half range.
 fake::ticks=25ULL*24*60*60*1000;
 assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="FIRST-55");

 // A stable identity check clears any prior throttle, so a replacement after
 // a 25-day idle period can start migration promptly.
 fake::ticks=1000000;
 m.discoveryMigrationAttempted=true;m.lastDiscoveryMigrationAttemptMillis=millis();
 assert(m.prepareDiscovery(a.appConfig));assert(!m.discoveryMigrationAttempted);
 fake::ticks+=25ULL*24*60*60*1000;
 i.invData.Serial=56;a.client.messages.clear();
 assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="FIRST-56");
 bool clearedOldIdentity=false;
 for(const auto&message:a.client.messages)
  if(message.topic=="homeassistant/sensor/FIRST-55/esp_ip/config")
   clearedOldIdentity=message.payload.empty()&&message.retained;
 assert(clearedOldIdentity);

 // A failed attempt remains throttled for five seconds, with unsigned elapsed
 // arithmetic allowing the retry window itself to cross millis() rollover.
 a.appConfig.mqttTopic="RETRY";a.client.publishOK=false;
 fake::ticks=static_cast<uint64_t>(UINT32_MAX)-2000;
 a.client.messages.clear();assert(!m.prepareDiscovery(a.appConfig));
 const uint32_t failedAt=m.lastDiscoveryMigrationAttemptMillis;
 a.client.publishOK=true;
 assert(!m.prepareDiscovery(a.appConfig));
 fake::ticks=static_cast<uint64_t>(failedAt)+4999;
 assert(!m.prepareDiscovery(a.appConfig));
 fake::ticks=static_cast<uint64_t>(failedAt)+5000;
 assert(m.prepareDiscovery(a.appConfig));assert(m.discoveryIdentity=="RETRY-56");

 a.appConfig=savedConfig;i.invData.Serial=savedSerial;fake::ticks=savedTicks;fake::nvs=savedNvs;
 a.client.messages=savedMessages;a.client.online=savedOnline;a.client.publishOK=savedPublishOK;a.client.connectOK=savedConnectOK;
 fake::nvsOpenFail=savedNvsOpenFail;fake::nvsReadFail=savedNvsReadFail;
 fake::nvsBlobDataReadFail=savedNvsBlobDataReadFail;fake::nvsIdentityTypeMismatch=savedNvsIdentityTypeMismatch;
 fake::nvsFail=savedNvsFail;
 m.discoveryIdentityLoaded=savedIdentityLoaded;m.discoveryIdentity=savedIdentity;
 m.discoveryMigrationAttempted=savedMigrationAttempted;m.lastDiscoveryMigrationAttemptMillis=savedLastAttempt;
}
void testBluetoothAddressValidation(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&w=a.webServer;
 const AppConfig original=a.appConfig;const auto originalTicks=fake::ticks;
 m.settingsToken="secret";
 a.appConfig.mqttBroker="before";a.appConfig.smaBTAddress="01:23:45:67:89:AB";
 assert(a.saveConfiguration());const auto committed=fake::files["/config.txt"];
 const std::vector<std::string> malformed={
  "AA:BB:CC:DD:EE:FZ", "AA:BB:CC:DD:EE:+F", "AA:BB:CC:DD:EE:-1",
  "AA-BB-CC-DD-EE-FF", "AA:BB;CC:DD:EE:FF", "A:BB:CC:DD:EE:FF",
  "AA:BB:CC:DD:EE", "AA:BB:CC:DD:EE:FF:"
 };
 for(const auto&value:malformed){
  uint8_t decoded[6]={0xA5,0xA5,0xA5,0xA5,0xA5,0xA5};
  assert(!parseSmaBluetoothAddress(value,decoded));
  for(uint8_t octet:decoded)assert(octet==0xA5);
  w.params={{"token","secret"},{"mqttBroker","must-not-save"},{"btaddress",value}};w.code=0;
  m.handleForm();
  assert(w.code==400);assert(a.appConfig.mqttBroker=="before");
  assert(a.appConfig.smaBTAddress=="01:23:45:67:89:AB");
  assert(fake::files["/config.txt"]==committed);
 }
 const String mixedCase="aA:bB:Cc:dD:eE:fF";
 uint8_t decoded[6]={};assert(parseSmaBluetoothAddress(mixedCase,decoded));
 const uint8_t expected[6]={0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
 for(size_t i=0;i<6;++i)assert(decoded[i]==expected[i]);
 w.params={{"token","secret"},{"mqttBroker","after"},{"btaddress",mixedCase}};w.code=0;
 try{m.handleForm();assert(false);}catch(const fake::Restart&){}
 assert(w.code==200);assert(a.appConfig.mqttBroker=="after");assert(a.appConfig.smaBTAddress==mixedCase);
 StaticJsonDocument<2048> json;assert(!deserializeJson(json,fake::files["/config.txt"]));
 assert(json["smaBTAddress"]==mixedCase.c_str());
 a.appConfig=original;assert(a.saveConfiguration());fake::ticks=originalTicks;
}


void testReannouncementDoesNotHoldReadings(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();auto&i=ESP32_SMA_Inverter::getInstance();
 auto config=a.appConfig;auto inv=i.invData;auto disp=i.dispData;
 a.appConfig.mqttBroker="broker";a.appConfig.mqttTopic="SMA";a.appConfig.hassDisc=true;a.appConfig.thisSerial=77;i.invData.Serial=77;
 m.discoveryIdentityLoaded=true;m.discoveryIdentity="SMA-77";WiFi.state=WL_CONNECTED;a.client.online=true;a.client.publishOK=true;
 a.discoveredSerial=77;a.firstTime=true;a.nextDiscoveryAttempt=millis()+60000;a.dayNight=a.nightTime;
 a.pendingReading=i.invData;a.pendingDisplay=i.dispData;a.pendingDisplay.Pac=432;
 a.pendingReadingAcquiredMillis=millis();a.pendingReadingMaxAgeMillis=300000;a.readingPending=true;a.nextPublishAttempt=millis();
 a.nextTime=millis()+100000;a.client.messages.clear();
 a.appLoop();
 // The pending re-announcement is throttled, but the reading still goes out.
 assert(a.firstTime);assert(!a.readingPending);
 bool published=false;for(auto&msg:a.client.messages)if(msg.topic=="sma/solar/SMA-77/state")published=true;
 assert(published);
 // A reading for an inverter never announced still waits for discovery.
 a.discoveredSerial=0;a.pendingReadingAcquiredMillis=millis();a.readingPending=true;a.nextPublishAttempt=millis();
 a.nextDiscoveryAttempt=millis()+60000;a.client.messages.clear();a.appLoop();
 assert(a.readingPending);
 a.readingPending=false;a.firstTime=false;a.discoveredSerial=0;a.appConfig=config;i.invData=inv;i.dispData=disp;a.client.messages.clear();
}
void testConfigurableTimeoutsAndPollBudget(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const uint32_t savedReply=i.replyTimeoutMs;
 b.input.clear();b.onAvailable=nullptr;fake::ticks=7000000;
 i.replyTimeoutMs=5000;uint64_t start=fake::ticks;
 i.BTgetByte();assert(i.readTimeout);assert(fake::ticks-start>=5000&&fake::ticks-start<6000);
 assert(i.replyTimeoutCount()>=1);
 // The poll budget cuts a wait short even when the reply window is longer.
 i.replyTimeoutMs=20000;i.beginPollBudget(3000);assert(i.replyTimeoutCount()==0);
 start=fake::ticks;i.BTgetByte();assert(i.readTimeout);
 assert(fake::ticks-start>=3000&&fake::ticks-start<4000);assert(i.pollBudgetWasExhausted());
 // Once exhausted, every later wait in the same poll returns immediately.
 start=fake::ticks;i.BTgetByte();assert(i.readTimeout);assert(fake::ticks-start<50);
 i.endPollBudget();start=fake::ticks;i.replyTimeoutMs=1000;i.BTgetByte();assert(fake::ticks-start>=1000);
 i.replyTimeoutMs=savedReply;
}
static bool dueWithin(uint32_t deadline,int32_t expected){
 // The host millis() fake advances on every call, so allow a few ticks.
 const int32_t remaining=(int32_t)(deadline-millis());return remaining<=expected&&remaining>expected-20;
}
void testPollResultScheduling(){
 auto&a=ESP32_SMA_Inverter_App::getInstance();
 const auto savedStats=a.stats;const auto savedNext=a.nextTime;const auto savedRate=a.lastAdjustedScanRate;
 const bool savedClock=a.clockSyncRequested;const auto savedGen=a.clockSyncGeneration;const String savedStatus=a.clockSyncStatus;
 const bool savedPending=a.readingPending,savedRead=a.hasSuccessfulRead;const auto savedFail=a.failCount;
 fake::ticks=8000000;a.lastAdjustedScanRate=60000;a.stats=PollStats();

 // Scheduling counts from the end of a poll, however long it took.
 a.activeJob=PollJob();a.activeJob.nightTime=true;a.activeJob.intervalMs=NIGHTSCANRATE;
 a.pollResult=PollResult();a.pollResult.connected=true;a.pollResult.readOk=false;a.pollResult.durationMs=75000;
 a.pollResult.replyTimeouts=3;a.pollResult.finishedMillis=millis();
 a.pollState.store(ESP32_SMA_Inverter_App::POLL_DONE);a.processPollResult();
 assert(!a.isPolling());assert(dueWithin(a.nextTime,60000));
 assert(a.stats.polls==1&&a.stats.failures==1&&a.stats.lastDurationMs==75000&&a.stats.maxDurationMs==75000);
 assert(a.stats.lastReplyTimeouts==3);assert(std::string(a.stats.lastOutcome)=="reply timeout");

 // A poll request made while busy runs as soon as the poll ends.
 a.pollState.store(ESP32_SMA_Inverter_App::POLL_RUNNING);a.requestPollNow();assert(a.pollRequestedWhileBusy);
 a.pollResult=PollResult();a.pollResult.connected=true;a.pollResult.readOk=true;a.pollResult.durationMs=4000;
 a.pollResult.reading.Serial=91;a.pollResult.finishedMillis=millis();
 a.pollState.store(ESP32_SMA_Inverter_App::POLL_DONE);a.processPollResult();
 assert(dueWithin(a.nextTime,0));assert(!a.pollRequestedWhileBusy);assert(a.readingPending&&a.pendingReading.Serial==91);
 assert(std::string(a.stats.lastOutcome)=="ok"&&a.stats.maxDurationMs==75000);

 // A clock press made during the poll stays queued; the attempted one reports.
 a.clockSyncRequested=true;a.activeJob.clockSyncGeneration=a.clockSyncGeneration;++a.clockSyncGeneration;
 a.pollResult=PollResult();a.pollResult.connected=true;a.pollResult.clockSyncAttempted=true;
 a.pollResult.clockSyncStatus="Verified: test";a.pollResult.finishedMillis=millis();
 a.pollState.store(ESP32_SMA_Inverter_App::POLL_DONE);a.processPollResult();
 assert(a.clockSyncRequested);assert(a.clockSyncStatus=="Verified: test");
 a.activeJob.clockSyncGeneration=a.clockSyncGeneration;
 a.pollState.store(ESP32_SMA_Inverter_App::POLL_DONE);a.processPollResult();assert(!a.clockSyncRequested);

 // Authentication recovery retries after a second, not a full interval.
 a.pollResult=PollResult();a.pollResult.reconnectRequested=true;a.activeJob.nightTime=true;
 a.pollState.store(ESP32_SMA_Inverter_App::POLL_DONE);a.processPollResult();assert(dueWithin(a.nextTime,1000));
 assert(std::string(a.stats.lastOutcome)=="no connection");

 a.stats=savedStats;a.nextTime=savedNext;a.lastAdjustedScanRate=savedRate;a.clockSyncRequested=savedClock;
 a.clockSyncGeneration=savedGen;a.clockSyncStatus=savedStatus;a.readingPending=savedPending;a.hasSuccessfulRead=savedRead;a.failCount=savedFail;
}
void testEspStatusReportsPollHealth(){
 auto&m=ESP32_SMA_MQTT::getInstance();auto&a=ESP32_SMA_Inverter_App::getInstance();
 auto config=a.appConfig;const auto savedStats=a.stats;const bool savedLoaded=m.discoveryIdentityLoaded;const String savedIdentity=m.discoveryIdentity;
 a.appConfig.mqttBroker="broker";a.appConfig.hassDisc=true;a.appConfig.mqttTopic="SMA";a.appConfig.thisSerial=88;
 m.discoveryIdentityLoaded=true;m.discoveryIdentity="SMA-88";WiFi.state=WL_CONNECTED;a.client.online=false;a.client.publishOK=true;
 a.stats.polls=12;a.stats.failures=2;a.stats.lastDurationMs=4321;a.stats.maxDurationMs=91000;a.stats.lastReplyTimeouts=1;a.stats.lastOutcome="ok";
 m.espDiscoveryPublished=false;m.lastEspStatusMillis=0;a.client.messages.clear();const uint32_t connects=m.mqttConnects;
 assert(m.publishEspStatus(true));assert(m.mqttConnects==connects+1);
 bool sawState=false;unsigned diagnosticConfigs=0;
 for(auto&msg:a.client.messages){
  if(msg.topic.find("/esp/state")!=std::string::npos){
   sawState=true;StaticJsonDocument<1024> json;assert(msg.payload.size()<512);assert(!deserializeJson(json,msg.payload));
   assert(json["LastPollMs"]==4321&&json["MaxPollMs"]==91000&&json["PollFailures"]==2&&json["Polls"]==12);
   assert(json["LastPollResult"]=="ok"&&json["MqttConnects"]==connects+1&&json["ResetReason"]=="host");
  }
  if(msg.topic.find("homeassistant/sensor/SMA-88/esp_")==0&&!msg.payload.empty())++diagnosticConfigs;
 }
 assert(sawState);assert(diagnosticConfigs==11);
 a.appConfig=config;a.stats=savedStats;m.discoveryIdentityLoaded=savedLoaded;m.discoveryIdentity=savedIdentity;a.client.messages.clear();
}
int main(){
 testBoundedHttpRequests();
 testBoundedHttpFormCompatibility();
 testBoundedHttpDeadline();
 testClockReplyCorrelation();
 testClockTargetsOneInverter();
 testDiscoveryAfterReconnectAndBirth();
 testProvisioningTimeouts();
 testProvisionedWifiSurvivesStartup();
 testCompiledWifiProvisionedMarker();
 testTopicValidationAndJsonEscaping();
 testDiscoveryCapacity();
 testLoginAndInitCrc();
 testLoginFiltersSenderAndKnownSerial();
 testLoginBoundsUnrelatedReplies();
 testSenderAddressMatching();
 testLogoffRetriesCrcCollision();
 testEnergyPersistenceRetries();
 testEnergyFenceSurvivesReboot();
 testEnergyLegacyMigration();
 testEnergyStorageReadFailures();
 testReplacingInverterResetsEnergyBaseline();
 testEnergyFilteringDoesNotHidePower();
 testUnsupportedTemperature();
 testDcChannels();
 testSignedAndUnavailableMeasurements();
 testStatusRecordBounds();
 testMalformedRecordRanges();
 testFragmentEscapes();
 testSlowPacketDeadline();
 testCallerOperationDeadlinesAndRollover();
 testBluetoothTimerRollover();
 testStaleRelayExpires();
 testNightToDayShortensPollDeadline();
 testFasterModePreservesEarlierDeadlineAcrossRollover();
 testLateWifiStartsNtp();
 testMissingNtpDoesNotBlock();
 testExcessFormArguments();
 testSettingsToken();
 testNtpInput();
 testConfigRecovery();
 testSerialConfigPersistenceAndValidation();
 testSaveFailures();
 testClosedWriteIsVerified();
 testMountFailurePreservesFiles();
 testLargeEscapedConfiguration();
 testPasswordWhitespace();
 testStaleErrorIsIgnored();
 testMalformedPacketClearsSession();
 testManyValidFragments();
 testCflAttemptsAreTransactional();
 testBluetoothWriteFailureAndSignalValidity();
 testEmptyStatusIsUnavailable();
 testInitReplyAndTrailerBounds();
 testInitRefreshesModelWhenSerialChanges();
 testClockExpiresDuringRead();
 testConnectRetainsEarlyHandshakeAndCleansFailedSession();
 testBluetoothAuthRecoveryMatchesTargetPeer();
 testServicesRunDuringReceive();
 testSerialSaveRetry();
 testPendingReadingRetry();
 testDiscoveryIdentityCleanup();
 testDiscoveryMigrationLongUptimeAndRollover();
 testNumericSettingsValidation();
 testTimezoneConfigurationReload();
 testBluetoothAddressValidation();
 testBluetoothInitializationRetries();
 testWebBasicAuthentication();
 testReannouncementDoesNotHoldReadings();
 testConfigurableTimeoutsAndPollBudget();
 testPollResultScheduling();
 testEspStatusReportsPollHealth();
 InverterData identity{}; identity.SUSyID=0x1234; assert(identity.SUSyID==0x1234);
 uint8_t bytes[]={0x78,0x56,0x34,0x12,0,0,0,0};
 assert(get_u16(bytes)==0x5678);
 assert(get_u32(bytes)==0x12345678);
 assert(get_u64(bytes)==0x12345678);
 std::cout << "Host regression checks passed\n";
}
