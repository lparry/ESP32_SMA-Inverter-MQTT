#pragma once
#include <Arduino.h>
#include <esp_gap_bt_api.h>
using BluetoothSerialDataCb=std::function<void(const uint8_t*,size_t)>;
inline int esp_bt_gap_get_bond_device_num(){return 0;}
inline int esp_bt_gap_get_bond_device_list(int*,esp_bd_addr_t*){return 0;}
class BluetoothSerial {
public:
 std::deque<uint8_t> input;
 std::vector<uint8_t> output;
 std::function<void()> sent;
 std::function<void()> onAvailable;
 std::function<void()> duringConnect;
 BluetoothSerialDataCb dataCallback;
 bool writeFails=false;
 bool connectResult=false;
 bool beginResult=true;
 std::deque<bool> beginResults;
 unsigned beginCalls=0,endCalls=0,connectCalls=0,unpairCalls=0;
 bool begin(const char*,bool){
  ++beginCalls;
  if(beginResults.empty())return beginResult;
  bool result=beginResults.front();beginResults.pop_front();return result;
 }
 void end(){++endCalls;dataCallback=nullptr;input.clear();}
 bool setPin(const char*){return true;}
 void onAuthComplete(std::function<void(bool)>){ }
 void onData(BluetoothSerialDataCb cb){dataCallback=cb;}
 void inject(const uint8_t*data,size_t n){if(dataCallback)dataCallback(data,n);else input.insert(input.end(),data,data+n);}
 bool connect(uint8_t*){++connectCalls;if(duringConnect)duringConnect();return connectResult;}
 bool disconnect(){return true;}
 bool unpairDevice(uint8_t*){++unpairCalls;return true;}
 int available(){if(onAvailable)onAvailable();return input.size();}
 int read(){auto c=input.front();input.pop_front();return c;}
 size_t write(uint8_t c){output.push_back(c);if(output.size()>=4 && output.size()==(output[1]|(output[2]<<8))){if(sent)sent();}return 1;}
 size_t write(const uint8_t*b,size_t n){if(writeFails)return 0;for(size_t k=0;k<n;++k)write(b[k]);return n;}
 void flush(){}
};
