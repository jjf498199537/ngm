#include "server_socket.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "absl/strings/string_view.h"
#include "rtc_base/base64.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/message_digest.h"

constexpr char kHeaderTerminator[] = "\r\n\r\n";
constexpr int kHeaderTerminatorLength = sizeof(kHeaderTerminator) - 1;

ServerSocketBase::~ServerSocketBase() {
  Close();
}

void ServerSocketBase::Close() {
  if (socket_ != INVALID_SOCKET) {
    // SO_LINGER with a positive timeout tells the kernel to wait for
    // queued data to be delivered and acknowledged before closing.
    // Without this, close() on a non-blocking socket may send RST
    // instead of FIN, discarding unsent data.
    struct linger ling = {};
    ling.l_onoff = 1;
    ling.l_linger = 5;
    ::setsockopt(socket_, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
    // Notify peer that we're done writing → sends FIN.
    ::shutdown(socket_, SHUT_WR);
    ::close(socket_);
    socket_ = INVALID_SOCKET;
  }
}

bool ServerSocketBase::Create() {
  RTC_DCHECK(!is_valid());
  socket_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (socket_ == INVALID_SOCKET) {
    RTC_LOG(LS_ERROR) << "Failed to create socket";
    return false;
  }
  return true;
}

bool ServerDataSocket::Send(const std::string& data) const {
  int total_len = static_cast<int>(data.length());
  int sent = send(socket_, data.data(), static_cast<size_t>(total_len), 0);
  if (sent == -1) {
    RTC_LOG(LS_ERROR) << "[send] failed: errno=" << errno
                      << " (len=" << total_len << ")";
    return false;
  }
  if (sent < total_len) {
    RTC_LOG(LS_WARNING) << "[send] partial: sent=" << sent
                        << " of " << total_len;
  }
  return true;
}

bool ServerDataSocket::Send(const std::string& status,
                            bool connection_close,
                            const std::string& content_type,
                            const std::string& extra_headers,
                            const std::string& data) const {
  RTC_DCHECK(is_valid());
  RTC_DCHECK(!status.empty());
  std::string buffer("HTTP/1.1 " + status + "\r\n");
  buffer +=
      "Server: NGMRoomServer/0.1\r\n"
      "Cache-Control: no-cache\r\n";
  if (connection_close)
    buffer += "Connection: close\r\n";
  if (!content_type.empty())
    buffer += "Content-Type: " + content_type + "\r\n";
  buffer += "Content-Length: " + absl::StrCat(data.size()) + "\r\n";
  if (!extra_headers.empty())
    buffer += extra_headers;
  buffer += "\r\n";
  buffer += data;
  return Send(buffer);
}

bool ServerDataSocket::SendWebSocketUpgradeResponse() {
  RTC_DCHECK(is_valid());
  RTC_DCHECK(upgrade_received());

  // 提取 Sec-WebSocket-Key
  static constexpr absl::string_view kKeyHeader = "Sec-WebSocket-Key: ";
  size_t pos = header_.find(kKeyHeader.data());
  if (pos == std::string::npos)
    return false;
  pos += kKeyHeader.size();
  size_t end = header_.find("\r\n", pos);
  if (end == std::string::npos)
    return false;
  std::string key = header_.substr(pos, end - pos);

  // RFC 6455: SHA-1(key + GUID) → Base64
  static constexpr char kWebSocketGUID[] =
      "258EAFA5-E914-47DA-95CA-5AB0DC85B11B";
  std::string source = key + kWebSocketGUID;
  uint8_t digest[20];
  size_t digest_len =
      webrtc::ComputeDigest(webrtc::DIGEST_SHA_1, source.data(), source.size(),
                            digest, sizeof(digest));
  RTC_DCHECK_EQ(digest_len, 20u);
  std::string accept = webrtc::Base64Encode(
      absl::string_view(reinterpret_cast<const char*>(digest), digest_len));

  // 发送 101 响应
  std::string response =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      accept + "\r\n\r\n";
  return Send(response);
}

bool ServerDataSocket::onDataAvailable(bool& close_socket) {
  RTC_DCHECK(is_valid());
  char buffer[4096] = {0};
  int bytes = recv(socket_, buffer, sizeof(buffer), 0);
  if (bytes == -1 || bytes == 0) {
    close_socket = true;
    return false;
  }
  close_socket = false;

  bool ret = true;
  if (headers_received()) {
    if (method_ != RequestMethod::POST) {
      ret = false;
    } else {
      data_.append(buffer, bytes);
    }
  } else {
    header_.append(buffer, bytes);
    size_t found = header_.find(kHeaderTerminator);
    if (found != std::string::npos) {
      data_ = header_.substr(found + kHeaderTerminatorLength);
      header_.resize(found + kHeaderTerminatorLength);
      ret = ParseHeaders();
    }
  }

  return ret;
}

bool ServerDataSocket::PathEquals(const char* path) const {
  RTC_DCHECK(path);
  size_t args = path_.find('?');
  if (args != std::string::npos)
    return path_.substr(0, args).compare(path) == 0;
  return path_.compare(path) == 0;
}

bool ServerDataSocket::PathStartsWith(const char* prefix) const {
  RTC_DCHECK(prefix);
  size_t args = path_.find('?');
  absl::string_view clean_path = (args != std::string::npos)
                                      ? absl::string_view(path_).substr(0, args)
                                      : absl::string_view(path_);
  absl::string_view prefix_view(prefix);
  if (!clean_path.starts_with(prefix_view))
    return false;
  // 精确匹配或下一个字符是 '/'
  return clean_path.size() == prefix_view.size() ||
         clean_path[prefix_view.size()] == '/';
}

bool ServerDataSocket::ParseContentLengthAndType(const char* headers,
                                                 size_t length) {
  RTC_DCHECK_EQ(content_length_, 0);
  RTC_DCHECK(content_type_.empty());

  const char* end = headers + length;
  while (headers && headers < end) {
    static constexpr absl::string_view kContentLength = "Content-Length:";
    static constexpr absl::string_view kContentType = "Content-Type:";
    if (absl::string_view(headers, end - headers)
            .starts_with((kContentLength))) {
      headers += kContentLength.size();
      while (headers[0] == ' ')
        ++headers;
      content_length_ = atoi(headers);
    } else if (absl::string_view(headers, end - headers)
                   .starts_with(kContentType)) {
      headers += kContentType.size();
      while (headers[0] == ' ')
        ++headers;
      const char* type_end = strstr(headers, "\r\n");
      if (type_end == nullptr)
        type_end = end;
      content_type_.assign(headers, type_end);
    } else {
      ++headers;
    }
    headers = strstr(headers, "\r\n");
    if (headers)
      headers += 2;
  }
  return true;
}

bool ServerDataSocket::ParseHeaders() {
  RTC_DCHECK(!header_.empty());
  RTC_DCHECK_EQ(method_, RequestMethod::INVALID);

  size_t i = header_.find("\r\n");
  if (i == std::string::npos)
    return false;

  if (!ParseMethodAndPath(header_.data(), i))
    return false;

  if (method_ == RequestMethod::POST) {
    const char* headers = header_.data() + i + 2;
    size_t len = header_.length() - i - 2;
    if (!ParseContentLengthAndType(headers, len))
      return false;
  }
  return true;
}

bool ServerDataSocket::ParseMethodAndPath(const char* begin, size_t len) {
  struct {
    const char* method_name;
    size_t method_name_len;
    RequestMethod id;
  } supported_methods[] = {
      {.method_name = "GET", .method_name_len = 3, .id = RequestMethod::GET},
      {.method_name = "POST", .method_name_len = 4, .id = RequestMethod::POST},
      //{.method_name = "OPTIONS", .method_name_len = 7, .id =
      // RequestMethod::OPTIONS},
  };

  const char* path = nullptr;
  for (const auto& method : supported_methods) {
    if (len > method.method_name_len &&
        isspace(begin[method.method_name_len]) &&
        strncmp(begin, method.method_name, method.method_name_len) == 0) {
      method_ = method.id;
      path = begin + method.method_name_len;
      break;
    }
  }

  const char* end = begin + len;
  if (!path || path >= end)
    return false;

  ++path;
  begin = path;
  while (!isspace(*path) && path < end)
    ++path;

  path_.assign(begin, path - begin);

  return true;
}

bool ServerListenerSocket::Listen(int port) {
  RTC_DCHECK(is_valid());
  int enabled = 1;
  if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&enabled),
                 sizeof(enabled)) != 0) {
    RTC_LOG(LS_ERROR) << "setsockopt SO_REUSEADDR failed";
    return false;
  }
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) ==
      -1) {
    RTC_LOG(LS_ERROR) << "bind failed on port " << port;
    return false;
  }
  if (listen(socket_, 5) == -1) {
    RTC_LOG(LS_ERROR) << "listen failed";
    return false;
  }
  return true;
}

std::unique_ptr<ServerDataSocket> ServerListenerSocket::Accept() {
  RTC_DCHECK(is_valid());
  struct sockaddr_in addr = {};
  socklen_t size = sizeof(addr);
  SocketFd client = accept4(socket_, reinterpret_cast<sockaddr*>(&addr), &size,
                            SOCK_NONBLOCK);
  if (client == INVALID_SOCKET) {
    return nullptr;
  }
  // Set SO_LINGER early so that when the socket is eventually closed,
  // the kernel will wait for queued data to be delivered (up to 5s).
  struct linger ling = {};
  ling.l_onoff = 1;
  ling.l_linger = 5;
  ::setsockopt(client, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
  return std::make_unique<ServerDataSocket>(client);
}
