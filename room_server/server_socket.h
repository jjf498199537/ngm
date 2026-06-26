#ifndef NGM_ROOM_SERVER_SERVER_SOCKET_H_
#define NGM_ROOM_SERVER_SERVER_SOCKET_H_

#include <memory>
#include <string>

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
  enum class RequestMethod {
    INVALID,
    GET,
    POST,
    OPTIONS,
  };
  explicit ServerDataSocket(SocketFd socket) : ServerSocketBase(socket) {}
  std::string path() const { return path_; }
  std::string method_name() const {
    switch (method_) {
      case RequestMethod::GET: return "GET";
      case RequestMethod::POST: return "POST";
      case RequestMethod::OPTIONS: return "OPTIONS";
      default: return "INVALID";
    }
  }
  const std::string& raw_header() const { return header_; }
  const std::string& body() const { return data_; }
  size_t content_length() const { return content_length_; }
  bool headers_received() const { return method_ != RequestMethod::INVALID; }
  bool request_received() const {
    return headers_received() && data_.length() >= content_length_;
  }
  bool upgrade_received() const {
    return method_ == RequestMethod::GET &&
           header_.find("Upgrade: websocket") != std::string::npos;
  }

  bool Send(const std::string& data) const;
  bool Send(const std::string& status,
            bool connection_close,
            const std::string& content_type,
            const std::string& extra_headers,
            const std::string& data) const;
  bool SendWebSocketUpgradeResponse();

  // WebSocket frame I/O (after upgrade).
  // Returns true if a complete text frame was read into |message|.
  // |closed| is set to true if a close frame was received or the
  // connection was lost.
  bool ReadWebSocketFrame(std::string& message, bool& closed);
  // Send a text WebSocket frame.
  bool SendWebSocketFrame(const std::string& message) const;

  bool onDataAvailable(bool& close_socket);
  bool PathEquals(const char* path) const;
  bool PathStartsWith(const char* prefix) const;

 protected:
  bool ParseHeaders();
  bool ParseMethodAndPath(const char* begin, size_t len);
  bool ParseContentLengthAndType(const char* headers, size_t length);

 protected:
  RequestMethod method_{};
  std::string data_{};
  std::string header_{};
  std::string path_{};
  size_t content_length_{};
  std::string content_type_{};
  // Buffer for accumulating partial WebSocket frame data.
  std::string ws_buffer_{};
};

class ServerListenerSocket : public ServerSocketBase {
 public:
  ServerListenerSocket() = default;
  bool Listen(int port);
  std::unique_ptr<ServerDataSocket> Accept();
};

#endif