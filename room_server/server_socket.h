#ifndef NGM_ROOM_SERVER_SERVER_SOCKET_H_
#define NGM_ROOM_SERVER_SERVER_SOCKET_H_

#include <memory>

typedef int SocketFd;

#ifndef INVALID_SOCKET
#define INVALID_SOCKET static_cast<SocketFd>(-1)
#endif

class ServerSocketBase {
 public:
  ServerSocketBase() : socket_(INVALID_SOCKET) {}
  explicit ServerSocketBase(SocketFd socket) : socket_(socket) {}
  virtual ~ServerSocketBase();

  SocketFd socket() const { return socket_; }
  // 禁止拷贝，允许移动
  ServerSocketBase(const ServerSocketBase&) = delete;
  ServerSocketBase& operator=(const ServerSocketBase&) = delete;

  bool Create();
  void Close();
  bool is_valid() const { return socket_ != INVALID_SOCKET; }

 protected:
  SocketFd socket_;
};

class ServerDataSocket : public ServerSocketBase {
 public:
  explicit ServerDataSocket(SocketFd socket) : ServerSocketBase(socket) {}
  bool onDataAvailable(bool& close_socket);
};

class ServerListenerSocket : public ServerSocketBase {
 public:
  ServerListenerSocket() = default;
  bool Listen(int port);
  std::unique_ptr<ServerDataSocket> Accept();
};

#endif