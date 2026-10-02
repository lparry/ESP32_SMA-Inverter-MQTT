#pragma once

#include <WebServer.h>
#include <cstring>
#include <new>

// WebServer's Arduino 2.0.9 request parser allocates before route callbacks and
// may wait indefinitely on malformed multipart data. Receive only the GET and
// URL-encoded POST requests used by this UI, without entering that parser.
// Keep the upstream routing, authentication and response implementation.
// Interface: https://github.com/espressif/arduino-esp32/blob/2.0.9/libraries/WebServer/src/WebServer.h
class BoundedWebServer : public WebServer {
 public:
  explicit BoundedWebServer(int port = 80) : WebServer(port) {}
  ~BoundedWebServer() override { finishRequest(); }

  void close() override {
    finishRequest();
    WebServer::close();
  }

  void handleClient() override {
    if (_currentStatus == HC_NONE) {
      WiFiClient client = _server.available();
      if (!client) return;
      _currentClient = client;
      _currentClient.setTimeout(HTTP_MAX_SEND_WAIT / 1000);
      _currentStatus = HC_WAIT_READ;
      started = millis();
      received = bodyStart = bodyLength = 0;
      headersComplete = false;
      query = nullptr;
      request[0] = '\0';
      _currentVersion = 1;
      _currentUri = _hostHeader = _responseHeaders = "";
      _currentHandler = nullptr;
      _contentLength = CONTENT_LENGTH_NOT_SET;
      _clientContentLength = 0;
      _chunked = false;
      for (int i = 0; i < _headerKeysCount; ++i) _currentHeaders[i].value = "";
    }

    // A total deadline, rather than an inactivity timeout that a trickle of
    // bytes can reset. Each call consumes at most 256 already available bytes.
    if ((uint32_t)(millis() - started) >= requestTimeoutMs) {
      reject(408, "Request timed out");
      return;
    }
    for (size_t budget = 256; budget && _currentClient.available(); --budget) {
      int next = _currentClient.read();
      if (next < 0) break;
      if (next == 0) {
        reject(400, "Invalid request byte");
        return;
      }
      if (!headersComplete && received >= maxHeaderBytes) {
        reject(431, "Request headers too large");
        return;
      }
      if (received >= sizeof(request) - 1) {
        reject(413, "Request too large");
        return;
      }
      request[received++] = (char)next;
      request[received] = '\0';
      if (!headersComplete && received >= 4 &&
          memcmp(request + received - 4, "\r\n\r\n", 4) == 0) {
        bodyStart = received;
        if (!parseHeaders()) return;
        headersComplete = true;
      }
      if (headersComplete && received - bodyStart == bodyLength) {
        dispatchRequest();
        return;
      }
    }
    if (!_currentClient.connected() && !_currentClient.available()) finishRequest();
  }

 private:
  static constexpr size_t maxHeaderBytes = 4096;
  static constexpr size_t maxBodyBytes = 4096;
  static constexpr size_t maxTargetBytes = 512;
  static constexpr size_t maxHeaderCount = 32;
  static constexpr size_t maxArguments = 24;
  static constexpr uint32_t requestTimeoutMs = 5000;

  // This server is a static application member: no large request buffer on the
  // ESP32 loop task's stack, and no allocation proportional to an advertised size.
  char request[maxHeaderBytes + maxBodyBytes + 1] = {};
  size_t received = 0, bodyStart = 0, bodyLength = 0;
  uint32_t started = 0;
  bool headersComplete = false;
  char *query = nullptr;

  static bool equalIgnoringCase(const char *a, const char *b) {
    while (*a && *b) {
      char x = *a++, y = *b++;
      if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
      if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
      if (x != y) return false;
    }
    return *a == *b;
  }

  static char *trimHeaderValue(char *value) {
    while (*value == ' ' || *value == '\t') ++value;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = '\0';
    return value;
  }

  bool parseHeaders() {
    char *lineEnd = strstr(request, "\r\n");
    if (!lineEnd) return reject(400, "Invalid request line");
    *lineEnd = '\0';
    char *target = strchr(request, ' ');
    if (!target) return reject(400, "Invalid request line");
    *target++ = '\0';
    char *version = strchr(target, ' ');
    if (!version) return reject(400, "Invalid request line");
    *version++ = '\0';
    if (strcmp(version, "HTTP/1.1") == 0) _currentVersion = 1;
    else if (strcmp(version, "HTTP/1.0") == 0) _currentVersion = 0;
    else return reject(505, "Unsupported HTTP version");
    if (strcmp(request, "GET") == 0) _currentMethod = HTTP_GET;
    else if (strcmp(request, "POST") == 0) _currentMethod = HTTP_POST;
    else return reject(405, "Method Not Allowed");
    if (strlen(target) > maxTargetBytes) return reject(414, "Request target too large");
    if (*target != '/') return reject(400, "Invalid request target");
    for (const char *p = target; *p; ++p)
      if ((unsigned char)*p <= 32 || *p == 127 || *p == '#')
        return reject(400, "Invalid request target");
    query = strchr(target, '?');
    if (query) *query++ = '\0';
    _currentUri = target;

    bool lengthSeen = false, typeSeen = false, encoded = false;
    size_t headerCount = 0;
    char *line = lineEnd + 2;
    while (*line) {
      lineEnd = strstr(line, "\r\n");
      if (!lineEnd) return reject(400, "Invalid request header");
      if (lineEnd == line) break;
      if (++headerCount > maxHeaderCount) return reject(431, "Too many request headers");
      *lineEnd = '\0';
      char *value = strchr(line, ':');
      if (!value || value == line) return reject(400, "Invalid request header");
      *value++ = '\0';
      for (const char *p = line; *p; ++p) {
        bool alphanumeric = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                            (*p >= '0' && *p <= '9');
        if (!alphanumeric && !strchr("!#$%&'*+-.^_`|~", *p))
          return reject(400, "Invalid header name");
      }
      value = trimHeaderValue(value);
      for (const char *p = value; *p; ++p)
        if (((unsigned char)*p < 32 && *p != '\t') || *p == 127)
          return reject(400, "Invalid header value");
      if (equalIgnoringCase(line, "Content-Length")) {
        if (lengthSeen || !*value) return reject(400, "Invalid Content-Length");
        lengthSeen = true;
        for (const char *p = value; *p; ++p) {
          if (*p < '0' || *p > '9') return reject(400, "Invalid Content-Length");
          bodyLength = bodyLength * 10 + (*p - '0');
          // Check every digit, before an integer overflow or a body allocation.
          if (bodyLength > maxBodyBytes) return reject(413, "Request body too large");
        }
      } else if (equalIgnoringCase(line, "Transfer-Encoding")) {
        return reject(400, "Transfer-Encoding is not supported");
      } else if (equalIgnoringCase(line, "Expect")) {
        return reject(417, "Expect is not supported");
      } else if (equalIgnoringCase(line, "Content-Type")) {
        if (typeSeen) return reject(400, "Duplicate Content-Type");
        typeSeen = true;
        char *parameters = strchr(value, ';');
        if (parameters) *parameters = '\0';
        encoded = equalIgnoringCase(trimHeaderValue(value), "application/x-www-form-urlencoded");
        if (!encoded) return reject(415, "Only URL-encoded forms are supported");
      } else if (equalIgnoringCase(line, "Host")) {
        _hostHeader = value;
      }
      _collectHeader(line, value);
      line = lineEnd + 2;
    }
    if (_currentMethod == HTTP_GET && bodyLength != 0)
      return reject(400, "GET request bodies are not supported");
    if (bodyLength && !encoded) return reject(415, "Only URL-encoded forms are supported");
    _clientContentLength = (int)bodyLength;
    return true;
  }

  static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool validateArguments(const char *data, size_t &count) {
    if (!data || !*data) return true;
    if (++count > maxArguments) return reject(413, "Too many form arguments");
    for (const char *p = data; *p; ++p) {
      if (*p == '&' && ++count > maxArguments) return reject(413, "Too many form arguments");
      if ((unsigned char)*p < 32 || *p == 127) return reject(400, "Invalid form argument");
      if (*p == '%') {
        if (!p[1] || !p[2] || hexDigit(p[1]) < 0 || hexDigit(p[2]) < 0 ||
            (p[1] == '0' && p[2] == '0')) return reject(400, "Invalid form encoding");
        p += 2;
      }
    }
    return true;
  }

  void appendArguments(char *data) {
    while (data && *data) {
      char *next = strchr(data, '&');
      if (next) *next++ = '\0';
      char *value = strchr(data, '=');
      if (value) {
        *value++ = '\0';
        RequestArgument &arg = _currentArgs[_currentArgCount++];
        arg.key = urlDecode(String(data));
        arg.value = urlDecode(String(value));
      }
      data = next;
    }
  }

  void dispatchRequest() {
    if ((uint32_t)(millis() - started) >= requestTimeoutMs) {
      reject(408, "Request timed out");
      return;
    }
    size_t count = 0;
    char *body = request + bodyStart;
    if (!validateArguments(query, count) || !validateArguments(body, count)) return;
    // Both query and body counts are validated before allocating decoded arguments.
    _currentArgs = new (std::nothrow) RequestArgument[count ? count : 1];
    if (!_currentArgs) {
      reject(503, "Unable to allocate request arguments");
      return;
    }
    _currentArgCount = 0;
    appendArguments(query);
    appendArguments(body);
    for (RequestHandler *handler = _firstHandler; handler; handler = handler->next()) {
      if (handler->canHandle(_currentMethod, _currentUri)) {
        _currentHandler = handler;
        break;
      }
    }
    _handleRequest();
    finishRequest();
  }

  bool reject(int status, const char *message) {
    send(status, "text/plain", message);
    finishRequest();
    return false;
  }

  void finishRequest() {
    _currentClient.stop();
    _currentClient = WiFiClient();
    _currentStatus = HC_NONE;
    delete[] _currentArgs;
    _currentArgs = nullptr;
    _currentArgCount = 0;
    _currentUri = _hostHeader = "";
    _currentHandler = nullptr;
    for (int i = 0; i < _headerKeysCount; ++i) _currentHeaders[i].value = "";
  }
};
