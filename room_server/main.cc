#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_cat.h"
#include "peer_room.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/logging.h"
#include "server_socket.h"

ABSL_FLAG(int, port, 8888, "The port on which the server listens.");
using SocketArray = std::vector<std::unique_ptr<ServerDataSocket>>;

static std::atomic<int> g_client_id_counter{1};

// Get the local address and port of a connected socket.
static std::string GetLocalAddr(SocketFd fd, int default_port) {
  struct sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return absl::StrCat(ip, ":", ntohs(addr.sin_port));
  }
  return absl::StrCat("0.0.0.0:", default_port);
}

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "NGM Room Server.\n"
      "Example usage: ./room_server --port=8888\n");
  absl::ParseCommandLine(argc, argv);
  int port = absl::GetFlag(FLAGS_port);

  // Set up file-based rotating log sink.
  // Logs are written to ./logs/room_server_*.log with rotation.
  const std::string log_dir = "./logs";
  mkdir(log_dir.c_str(), 0755);  // Create log directory (ignore if exists).

  auto log_sink = std::make_unique<webrtc::FileRotatingLogSink>(
      log_dir, "room_server",
      /*max_log_size=*/10 * 1024 * 1024,
      /*num_log_files=*/5);

  if (log_sink->Init()) {
    webrtc::LogMessage::AddLogToStream(log_sink.get(), webrtc::LoggingSeverity::LS_INFO);
  } else {
    RTC_LOG(LS_ERROR) << "Failed to initialize file log sink in '" << log_dir
                      << "'";
  }

  if (port < 1 || port > 65535) {
    RTC_LOG(LS_ERROR) << "Error: port must be between 1 and 65535.";
    return -1;
  }

  RTC_LOG(LS_INFO) << "NGM Room Server starting on port " << port;

  ServerListenerSocket listener;
  if (!listener.Create()) {
    RTC_LOG(LS_ERROR) << "Error: Failed to Create listener";
    return -1;
  } else if (!listener.Listen(port)) {
    RTC_LOG(LS_ERROR) << "Error: Failed to listen on port " << port;
    return -1;
  }

  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) {
    RTC_LOG(LS_ERROR) << "epoll_create1 failed";
    return -1;
  }

  struct epoll_event listen_event = {};
  listen_event.events = EPOLLIN;
  listen_event.data.fd = listener.socket();
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener.socket(), &listen_event);

  const int kMaxEvents = 64;
  struct epoll_event events[kMaxEvents];

  bool quit = false;
  std::unordered_map<SocketFd, std::unique_ptr<ServerDataSocket>>
      pending_connections;
  std::unordered_map<SocketFd, std::unique_ptr<RoomMember>> room_members;
  while (!quit) {
    int num_events = epoll_wait(epoll_fd, events, kMaxEvents, 100);

    if (num_events > 0) {
      RTC_LOG(LS_INFO) << "[epoll] Received " << num_events << " event(s)";
    }

    for (int i = 0; i < num_events; i++) {
      SocketFd received_fd = events[i].data.fd;
      uint32_t event_flags = events[i].events;
      char hex_buf[16];
      std::snprintf(hex_buf, sizeof(hex_buf), "0x%x", event_flags);
      RTC_LOG(LS_INFO) << "[epoll] Event #" << i << ": fd=" << received_fd
                       << ", events=" << hex_buf
                       << (event_flags & EPOLLIN ? " [EPOLLIN]" : "")
                       << (event_flags & EPOLLOUT ? " [EPOLLOUT]" : "")
                       << (event_flags & EPOLLERR ? " [EPOLLERR]" : "")
                       << (event_flags & EPOLLHUP ? " [EPOLLHUP]" : "");
      if (received_fd == listener.socket()) {
        auto client = listener.Accept();
        if (client) {
          struct epoll_event client_event = {};
          client_event.events = EPOLLIN;
          client_event.data.fd = client->socket();
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->socket(), &client_event);
          RTC_LOG(LS_INFO) << "[epoll] New connection accepted: fd="
                           << client->socket();
          pending_connections[client->socket()] = std::move(client);
        }
      } else if (auto it = pending_connections.find(received_fd);
                 it != pending_connections.end()) {
        bool socket_done = false;
        auto& client = it->second;
        if (client->onDataAvailable(socket_done) &&
            client->request_received()) {
          RTC_LOG(LS_INFO) << "[HTTP] Request received: "
                           << client->method_name() << " " << client->path()
                           << " (fd=" << received_fd
                           << ", content_length=" << client->content_length()
                           << ")";
          RTC_LOG(LS_INFO) << "[HTTP] Headers:\n" << client->raw_header();
          if (!client->body().empty()) {
            RTC_LOG(LS_INFO) << "[HTTP] Body (" << client->body().size()
                             << " bytes):\n" << client->body();
          }
          if (client->upgrade_received()) {
            client->SendWebSocketUpgradeResponse();
            room_members[received_fd] =
                std::make_unique<RoomMember>(std::move(client));
            pending_connections.erase(it);
          } else if (client->PathStartsWith("/join")) {
            // POST /join/{roomId}
            // Extract roomId from path (e.g., /join/112233 -> 112233)
            std::string path = client->path();
            std::string room_id;
            size_t pos = path.find("/join/");
            if (pos != std::string::npos) {
              room_id = path.substr(pos + 6);
              // Remove query string if present
              size_t q = room_id.find('?');
              if (q != std::string::npos)
                room_id = room_id.substr(0, q);
            }
            if (room_id.empty()) {
              client->Send("400 Bad Request", true, "application/json", "",
                           "{\"result\":\"ERROR\",\"error\":\"Missing room id\"}");
            } else {
              int client_id = g_client_id_counter.fetch_add(1);
              std::string host = GetLocalAddr(received_fd, port);
              std::string json = absl::StrCat(
                  "{\"result\":\"SUCCESS\","
                  "\"params\":{"
                  "\"is_initiator\":true,"
                  "\"room_id\":\"", room_id, "\","
                  "\"client_id\":\"", absl::StrCat(client_id), "\","
                  "\"messages\":[],"
                  "\"wss_url\":\"ws://", host, "/ws\","
                  "\"wss_post_url\":\"http://", host, "\""
                  "}}");
              RTC_LOG(LS_INFO) << "[join] room=" << room_id
                               << ", client=" << client_id
                               << ", host=" << host;
              client->Send("200 OK", true, "application/json", "", json);
            }
            pending_connections.erase(it);
          } else if (client->PathStartsWith("/message")) {
            // POST /message/123456/78
            client->Send("200 OK", true, "text/plain", "", "");
            pending_connections.erase(it);
          } else if (client->PathStartsWith("/leave")) {
            // POST /leave/123456/78
            client->Send("200 OK", true, "text/plain", "", "");
            pending_connections.erase(it);
          } else {
            RTC_LOG(LS_WARNING) << "Unknown request: " << client->path();
            client->Send("404 Not Found", true, "text/plain", "", "");
            pending_connections.erase(it);
          }
        }
      }
    }
  }

  close(epoll_fd);

  // Remove file log sink before exiting.
  webrtc::LogMessage::RemoveLogToStream(log_sink.get());

  return 0;
}