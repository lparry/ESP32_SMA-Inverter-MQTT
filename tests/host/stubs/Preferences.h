#pragma once
#include <Arduino.h>

namespace fake {
inline std::map<std::string,std::vector<uint8_t>> nvs;
inline bool nvsFail=false;
inline int nvsWrites=0;
inline std::string nvsFailWriteKey;
inline std::string nvsFailReadKey;
inline std::string nvsFailEraseKey;
inline bool nvsOpenFail=false;
inline bool nvsReadFail=false;
inline bool nvsBlobDataReadFail=false;
inline bool nvsIdentityTypeMismatch=false;
inline bool nvsShouldFailWrite(const std::string& nameSpace,const char *key) {
 return nvsFail || nvsFailWriteKey == nameSpace + "/" + key;
}
inline bool nvsShouldFailRead(const std::string& nameSpace,const char *key) {
 return nvsReadFail || nvsFailReadKey == nameSpace + "/" + key;
}
}

class Preferences {
 std::string ns;
 public:
 bool begin(const char*n,bool=false){if(fake::nvsOpenFail)return false;ns=n;return true;}
 void end(){}
 size_t putBytes(const char*k,const void*p,size_t n){++fake::nvsWrites;if(fake::nvsShouldFailWrite(ns,k))return 0;auto*b=static_cast<const uint8_t*>(p);fake::nvs[ns+"/"+k]={b,b+n};return n;}
 size_t getBytes(const char*k,void*p,size_t n){if(fake::nvsShouldFailRead(ns,k))return 0;auto it=fake::nvs.find(ns+"/"+k);if(it==fake::nvs.end())return 0;const auto&b=it->second;size_t sz=std::min(n,b.size());if(sz)memcpy(p,b.data(),sz);return sz;}
 size_t getBytesLength(const char*k){if(fake::nvsShouldFailRead(ns,k))return 0;auto it=fake::nvs.find(ns+"/"+k);return it==fake::nvs.end()?0:it->second.size();}
 template<class T>T get(const char*k,T v){if(fake::nvsShouldFailRead(ns,k))return v;auto it=fake::nvs.find(ns+"/"+k);if(it!=fake::nvs.end()&&it->second.size()==sizeof(T))memcpy(&v,it->second.data(),sizeof(T));return v;}
 uint32_t getUInt(const char*k,uint32_t v=0){return get(k,v);}
 uint64_t getULong64(const char*k,uint64_t v=0){return get(k,v);}
 bool getBool(const char*k,bool v=false){return get(k,v);}
 size_t putUInt(const char*k,uint32_t v){return putBytes(k,&v,sizeof(v));}
 size_t putULong64(const char*k,uint64_t v){return putBytes(k,&v,sizeof(v));}
 size_t putBool(const char*k,bool v){return putBytes(k,&v,sizeof(v));}
};
