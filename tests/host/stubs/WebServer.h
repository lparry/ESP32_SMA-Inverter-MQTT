#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
enum HTTPMethod{HTTP_ANY,HTTP_GET,HTTP_POST};
enum HTTPClientStatus{HC_NONE,HC_WAIT_READ,HC_WAIT_CLOSE};
#define HTTP_MAX_SEND_WAIT 5000
#define CONTENT_LENGTH_NOT_SET ((size_t)-2)

class WebServer;
struct RequestHandler {
 String uri;
 HTTPMethod method;
 std::function<void()> callback;
 RequestHandler *following=nullptr;
 bool canHandle(HTTPMethod m,const String &u){return uri==u&&(method==HTTP_ANY||method==m);}
 RequestHandler *next(){return following;}
};

class WebServer {
 public:
 HTTPMethod verb=HTTP_POST;int code=0;String body;bool auth=true;unsigned authenticateCalls=0;
 std::map<std::string,std::string> responseHeaders;
 std::vector<std::pair<String,String>> params;
 explicit WebServer(int port):_server(port){}
 virtual ~WebServer(){
  delete[] _currentArgs;delete[] _currentHeaders;
  while(_firstHandler){auto next=_firstHandler->next();delete _firstHandler;_firstHandler=next;}
 }
 bool authenticate(const char*,const char*){++authenticateCalls;return auth;}
 void requestAuthentication(){sendHeader("WWW-Authenticate","Basic realm=\"Login Required\"");code=401;}
 HTTPMethod method(){return _currentArgs?_currentMethod:verb;}
 int args(){return _currentArgs?_currentArgCount:params.size();}
 String argName(int i){return _currentArgs?_currentArgs[i].key:params.at(i).first;}
 String arg(int i){return _currentArgs?_currentArgs[i].value:params.at(i).second;}
 String arg(const String&k){for(int i=0;i<args();++i)if(argName(i)==k)return arg(i);return "";}
 bool hasArg(const String&k){for(int i=0;i<args();++i)if(argName(i)==k)return true;return false;}
 std::function<void(size_t)> onResponseWrite;
 void send(int c,const char*,const String&b){
  code=c;body=b;
  if(onResponseWrite)onResponseWrite(b.length());
  if(_currentClient.socket)_currentClient.socket->output+=b.s;
 }
 void sendHeader(const String&name,const String&value,bool=false){responseHeaders[name.s]=value.s;}
 void on(const String &uri,std::function<void()> callback){on(uri,HTTP_ANY,callback);}
 void on(const String &uri,HTTPMethod method,std::function<void()> callback){
  auto handler=new RequestHandler{uri,method,callback};
  if(_lastHandler)_lastHandler->following=handler;else _firstHandler=handler;
  _lastHandler=handler;
 }
 virtual void begin(){close();_server.begin();}
 virtual void close(){_server.close();_currentStatus=HC_NONE;if(!_headerKeysCount)collectHeaders(nullptr,0);}
 virtual void handleClient(){}
 void collectHeaders(const char **keys,size_t count){
  delete[] _currentHeaders;_headerKeysCount=count+1;_currentHeaders=new RequestArgument[_headerKeysCount];
  _currentHeaders[0].key="Authorization";
  for(size_t i=0;i<count;++i)_currentHeaders[i+1].key=keys[i];
 }
 String header(const String &name){for(int i=0;i<_headerKeysCount;++i)if(_currentHeaders[i].key==name)return _currentHeaders[i].value;return "";}
 static String urlDecode(const String &encoded){
  std::string decoded;
  for(size_t i=0;i<encoded.length();++i){
   char c=encoded[i];
   if(c=='+')decoded+=' ';
   else if(c=='%'&&i+2<encoded.length()){
    char hex[]={encoded[i+1],encoded[i+2],0};decoded+=char(std::strtol(hex,nullptr,16));i+=2;
   }else decoded+=c;
  }
  return decoded;
 }
 protected:
 struct RequestArgument{String key,value;};
 WiFiServer _server;
 WiFiClient _currentClient;
 HTTPMethod _currentMethod=HTTP_ANY;
 HTTPClientStatus _currentStatus=HC_NONE;
 uint8_t _currentVersion=0;
 String _currentUri,_hostHeader,_responseHeaders;
 RequestHandler *_currentHandler=nullptr,*_firstHandler=nullptr,*_lastHandler=nullptr;
 RequestArgument *_currentArgs=nullptr,*_currentHeaders=nullptr;
 int _currentArgCount=0,_headerKeysCount=0,_clientContentLength=0;
 size_t _contentLength=0;
 bool _chunked=false;
 bool _collectHeader(const char *name,const char *value){
  for(int i=0;i<_headerKeysCount;++i){
   std::string a=_currentHeaders[i].key.s,b=name;
   for(auto &c:a)if(c>='A'&&c<='Z')c+='a'-'A';
   for(auto &c:b)if(c>='A'&&c<='Z')c+='a'-'A';
   if(a==b){_currentHeaders[i].value=value;return true;}
  }
  return false;
 }
 void _handleRequest(){if(_currentHandler)_currentHandler->callback();else send(404,"text/plain","Not found");}
};
