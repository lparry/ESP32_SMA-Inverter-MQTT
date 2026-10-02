#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
#define WL_CONNECTED 3
#define WIFI_STA 1
#define ARDUINO_EVENT_WIFI_STA_DISCONNECTED 0
using WiFiEvent_t=int;
struct WiFiEventInfo_t{struct{int reason;}wifi_sta_disconnected;};
struct IPAddress {String toString(){return "192.0.2.1";}};
struct WiFiClass {
 int state=3; bool done=true; bool persistentEnabled=false; bool autoReconnectEnabled=false;
 bool connectOnSmartConfig=false; bool smartConfigProvidesCredentials=false;
 bool smartConfigStartResult=true;
 std::function<void()> afterSmartConfigCredentials;
 unsigned compiledBegins=0, storedBegins=0, smartConfigBegins=0;
 String smartConfigSSID="new-network", smartConfigPassword="new-password";
 std::function<void()> onStatus;
 unsigned stops=0;String lastSSID="previous-network";
 String SSID(){return state==WL_CONNECTED?lastSSID:String();}
 String psk(){wifi_config_t config{};esp_wifi_get_config(WIFI_IF_STA,&config);char password[sizeof(config.sta.password)+1]={};memcpy(password,config.sta.password,sizeof(config.sta.password));return String(password);}
 template<class T>void onEvent(T,int){}
 void mode(int){}
 void setAutoReconnect(bool enabled){autoReconnectEnabled=enabled;}
 void hostname(String){}
 void begin(){++storedBegins;lastSSID=String(fake::stationSSID.c_str());}
 void begin(const char*s,const char*p){
  lastSSID=s;++compiledBegins;wifi_config_t config{};
  if(s)memcpy(config.sta.ssid,s,std::min(strlen(s),sizeof(config.sta.ssid)));
  if(p)memcpy(config.sta.password,p,std::min(strlen(p),sizeof(config.sta.password)));
  esp_wifi_set_config(WIFI_IF_STA,&config);
 }
 int status(){if(onStatus)onStatus();return state;}
 void disconnect(){}
 void reconnect(){}
 void persistent(bool p){persistentEnabled=p;}
 bool beginSmartConfig(){
  ++smartConfigBegins;
  if(!smartConfigStartResult)return false;
  if(smartConfigProvidesCredentials || connectOnSmartConfig){
   lastSSID=smartConfigSSID;wifi_config_t config{};
   memcpy(config.sta.ssid,smartConfigSSID.c_str(),std::min(smartConfigSSID.length(),sizeof(config.sta.ssid)));
   memcpy(config.sta.password,smartConfigPassword.c_str(),std::min(smartConfigPassword.length(),sizeof(config.sta.password)));
   if(esp_wifi_set_config(WIFI_IF_STA,&config)==0&&afterSmartConfigCredentials)afterSmartConfigCredentials();
   if(connectOnSmartConfig) {state=WL_CONNECTED;fake::stationAssociated=true;}
   else {state=0;fake::stationAssociated=false;}
  }
  return true;
 }
 bool smartConfigDone(){return done;}
 bool stopSmartConfig(){++stops;return true;}
 IPAddress localIP(){return {};}
 int RSSI(){return -50;}
};
inline WiFiClass WiFi;
