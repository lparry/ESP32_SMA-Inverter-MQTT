#pragma once
#include <Arduino.h>
namespace fake {
inline std::map<std::string,std::string> files;
inline size_t writeLimit=SIZE_MAX;
inline std::string failRenameFrom;
inline int renameCount=0, failRenameAt=0;
inline bool loseWriteOnClose=false, mountable=true;
inline int formats=0;
}
class File {
 std::string path; size_t pos=0; bool ok=false, writable=false;
 public:
 File()=default;
 File(std::string p,bool w):path(p),ok(true),writable(w){if(w)fake::files[p]="";}
 operator bool()const{return ok;}
 int read(){auto&s=fake::files[path];return pos<s.size()?static_cast<unsigned char>(s[pos++]):-1;}
 size_t readBytes(char*b,size_t n){size_t k=0;for(;k<n;k++){int c=read();if(c<0)break;b[k]=c;}return k;}
 size_t write(uint8_t c){return write(&c,1);}
 size_t write(const uint8_t*b,size_t n){auto&s=fake::files[path];size_t k=s.size()<fake::writeLimit?std::min(n,fake::writeLimit-s.size()):0;s.append(reinterpret_cast<const char*>(b),k);return k;}
 void close(){if(ok&&writable&&fake::loseWriteOnClose)fake::files[path].clear();ok=false;}
 void flush(){}
 size_t size()const{return fake::files[path].size();}
 bool available(){return pos<size();}
};
struct FSClass {
 bool begin(bool formatOnFail){if(!fake::mountable&&formatOnFail)format();return fake::mountable;}
 bool format(){++fake::formats;fake::files.clear();fake::mountable=true;return true;}
 File open(const String&p,const char*m){if(!fake::mountable||(m[0]=='r'&&!exists(p)))return {};return File(p.s,m[0]=='w');}
 bool exists(const String&p){return fake::files.count(p.s);}
 bool remove(const String&p){return fake::files.erase(p.s);}
 bool rename(const String&a,const String&b){++fake::renameCount;if(a.s==fake::failRenameFrom || (fake::failRenameAt && fake::renameCount==fake::failRenameAt) || !exists(a))return false;fake::files[b.s]=fake::files[a.s];fake::files.erase(a.s);return true;}
};
inline FSClass LittleFS;
