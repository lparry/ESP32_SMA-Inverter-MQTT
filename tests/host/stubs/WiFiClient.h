#pragma once
#include <Arduino.h>

namespace fake { inline unsigned httpAcceptPolls = 0; }

struct WiFiClient {
 struct Connection {
  std::deque<char> input;
  std::string output;
  bool open = true;
  unsigned timeout = 0;
 };
 std::shared_ptr<Connection> socket;
 WiFiClient() = default;
 explicit WiFiClient(const std::string &input) : socket(std::make_shared<Connection>()) {
  socket->input.insert(socket->input.end(),input.begin(),input.end());
 }
 explicit operator bool() const { return socket && socket->open; }
 int available() { return socket ? socket->input.size() : 0; }
 bool connected() { return socket && socket->open; }
 int read() {
  if(!available())return -1;
  auto c=static_cast<unsigned char>(socket->input.front());socket->input.pop_front();return c;
 }
 void setTimeout(unsigned timeout) { if(socket)socket->timeout=timeout; }
 void stop() { if(socket)socket->open=false; }
};

struct WiFiServer {
 std::deque<WiFiClient> pending;
 std::function<void()> onAvailable;
 explicit WiFiServer(int) {}
 WiFiClient available() {
  ++fake::httpAcceptPolls;
  if(onAvailable)onAvailable();
  if(pending.empty())return {};
  auto client=pending.front();pending.pop_front();return client;
 }
 void begin() {}
 void setNoDelay(bool) {}
 void close() { for(auto &client:pending)client.stop();pending.clear(); }
};
