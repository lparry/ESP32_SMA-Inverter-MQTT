#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <deque>
#include <map>
#include <stdexcept>
#include <climits>
using byte = uint8_t;
using boolean = bool;
using prog_uint16_t = uint16_t;
#define PROGMEM
#define HEX 16
#define DEC 10
class String {
 public:
  std::string s;
  String() = default;
  String(const char *v):s(v ? v : "") {}
  String(char *v):s(v ? v : "") {}
  String(const std::string &v):s(v) {}
  String(char c):s(1,c) {}
  template<class T> String(T v):s(std::to_string(v)) {}
  const char* c_str() const {return s.c_str();}
  bool isEmpty() const {return s.empty();}
  size_t length() const {return s.size();}
  bool reserve(size_t n) {s.reserve(n); return true;}
  bool concat(const char *v, size_t n) {s.append(v,n); return true;}
  bool concat(const char *v) {s += v; return true;}
  void trim() {auto a=s.find_first_not_of(" \r\n\t"); auto b=s.find_last_not_of(" \r\n\t"); s=a==s.npos?"":s.substr(a,b-a+1);}
  String substring(size_t a,size_t b) const {return s.substr(a,b-a);}
  long toInt() const {return std::strtol(s.c_str(),nullptr,10);}
  char operator[](size_t i) const {return s[i];}
  String& operator+=(const String &v){s+=v.s;return *this;}
  String& operator+=(char v){s+=v;return *this;}
  friend String operator+(const String&a,const String&b){return a.s+b.s;}
  friend bool operator==(const String&a,const String&b){return a.s==b.s;}
  friend bool operator!=(const String&a,const String&b){return !(a==b);}
};
namespace fake {
inline uint64_t ticks=0;
inline unsigned delayCalls=0, timeCalls=0, ntpCalls=0;
inline bool validTime=true;
inline int localHourOverride=-1;
inline uint32_t maxTimeWait=0;
struct Restart:std::exception{};
}
inline uint32_t millis(){return static_cast<uint32_t>(fake::ticks++);}
inline void delay(unsigned long n){fake::ticks+=n; ++fake::delayCalls;}
inline void yield(){delay(1);}
template<class T,class A,class B> T constrain(T x,A a,B b){return std::min<T>(b,std::max<T>(a,x));}
inline size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){memcpy(d,s,std::min(l,n-1));d[std::min(l,n-1)]=0;}return l;}
inline uint32_t esp_random(){static uint32_t n=123;return ++n;}
inline bool getLocalTime(tm*t,uint32_t timeout=5000){++fake::timeCalls;fake::maxTimeWait=std::max(fake::maxTimeWait,timeout);if(!fake::validTime){fake::ticks+=timeout;return false;}time_t n=1800000000;localtime_r(&n,t);if(fake::localHourOverride>=0)t->tm_hour=fake::localHourOverride;return true;}
inline void configTime(long,int,const char*){++fake::ntpCalls;}
struct SerialClass {
 std::deque<char> input;
 void begin(int){}
 int available(){return input.size();}
 int read(){char c=input.front();input.pop_front();return c;}
 template<class... T> void printf(const char*,T...){}
 template<class T> void print(T){}
 template<class T> void println(T){}
 void println(){}
};
inline SerialClass Serial;
struct EspClass {uint64_t getEfuseMac(){return 0x1234567890ULL;}uint32_t getFreeHeap(){return 100000;}void restart(){throw fake::Restart();}};
inline EspClass ESP;

template<class A,class B> auto max(A a,B b)->decltype(a+b){return a>b?a:b;}
