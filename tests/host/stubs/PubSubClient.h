#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
struct Publication {std::string topic,payload;bool retained;};
class PubSubClient {
 public:
 bool online=true, publishOK=true,connectOK=true;
 int loops=0;
 std::function<void()> onLoop;
 std::vector<Publication> messages;
 std::function<void(char*,uint8_t*,unsigned)> callback;
 PubSubClient(WiFiClient&){}
 void setBufferSize(int){}
 void setKeepAlive(int){}
 void setSocketTimeout(int){}
 void setServer(const char*,uint16_t){}
 bool connected(){return online;}
 template<class...T>bool connect(T...){online=connectOK;return online;}
 int state(){return 0;}
 void loop(){++loops;if(onLoop)onLoop();}
 bool publish(const char*t,const uint8_t*b,unsigned n,bool r){if(!publishOK)return false;messages.push_back({t,std::string(reinterpret_cast<const char*>(b),n),r});return true;}
 bool publish(const char*t,const char*b,bool r){return publish(t,reinterpret_cast<const uint8_t*>(b),strlen(b),r);}
 bool subscribe(const char*){return true;}
 template<class T>void setCallback(T cb){callback=cb;}
};
