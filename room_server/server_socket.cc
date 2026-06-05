#include "server_socket.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

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
