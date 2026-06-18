#include "server_socket.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

constexpr char kHeaderTerminator[] = "\r\n\r\n";
constexpr int kHeaderTerminatorLength = sizeof(kHeaderTerminator) - 1;

ServerSocketBase::~ServerSocketBase() {
  Close();
}

void ServerSocketBase::Close() {
  if (socket_ != INVALID_SOCKET) {
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
  return !content_type_.empty() && content_length_ != 0;
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
  return std::make_unique<ServerDataSocket>(client);
}
