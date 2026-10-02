#pragma once
namespace esp32m {
enum Level {Debug,Info};
struct Logger {void setLevel(Level){};};
class Loggable {protected: Logger& logger(){static Logger l;return l;}virtual const char* logName()const{return "";}};
struct Logging {static void setLevel(Level){} template<class T>static void addAppender(T*){}};
}
#include <cstdarg>
#include <cstdio>
inline void testLog(const char*,...) __attribute__((format(printf,1,2)));
inline void testLog(const char*format,...){char text[1024];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);}
#define logW(...) testLog(__VA_ARGS__)
#define logV(...) testLog(__VA_ARGS__)
#define logD(...) testLog(__VA_ARGS__)
#define logI(...) testLog(__VA_ARGS__)
#define logE(...) testLog(__VA_ARGS__)
#define log_i(...) testLog(__VA_ARGS__)
#define log_e(...) testLog(__VA_ARGS__)
#define log_w(...) testLog(__VA_ARGS__)
#define log_d(...) testLog(__VA_ARGS__)
