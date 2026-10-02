#pragma once
namespace esp32m {
class UDPAppender {
public:
 enum Format {Text,Syslog};
 UDPAppender(const char*,unsigned short=514){}
 void setMode(Format){}
};
}
