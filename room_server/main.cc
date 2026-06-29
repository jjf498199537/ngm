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

// Extract a string value from a simple JSON object by key.
// e.g., ExtractJsonField("{\"cmd\":\"register\"}", "cmd") → "register"
static std::string ExtractJsonField(const std::string& json,
                                    const std::string& key) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
    return "";
  pos += search.size();
  // Skip colon and whitespace.
  while (pos < json.size() && (json[pos] == ':' || json[pos] == ' '))
    ++pos;
  if (pos >= json.size() || json[pos] != '"')
    return "";
  ++pos;
  size_t end = json.find('"', pos);
  if (end == std::string::npos)
    return "";
  return json.substr(pos, end - pos);
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
    webrtc::LogMessage::AddLogToStream(log_sink.get(),
                                       webrtc::LoggingSeverity::LS_INFO);
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
  std::unordered_map<std::string, PeerRoom> room_map;
  while (!quit) {
    int num_events = epoll_wait(epoll_fd, events, kMaxEvents, 100);

    if (num_events > 0) {
      RTC_LOG(LS_INFO) << "[epoll] Received " << num_events << " event(s)";
    }

    std::vector<SocketFd> fds_to_remove;

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
        auto& client = it->second;

        // Handle connection errors/hangups for any pending connection.
        if (event_flags & (EPOLLERR | EPOLLHUP)) {
          RTC_LOG(LS_INFO) << "[HTTP] Connection error/hangup: fd="
                           << received_fd;
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, received_fd, nullptr);
          pending_connections.erase(it);
          continue;
        }

        // Already upgraded — incoming data is WebSocket frames.
        if (client->upgrade_received()) {
          std::string message;
          bool closed = false;
          if (client->ReadWebSocketFrame(message, closed)) {
            RTC_LOG(LS_INFO) << "[ws] Frame from pending fd=" << received_fd
                             << ": " << message;
            std::string cmd = ExtractJsonField(message, "cmd");
            if (cmd == "register") {
              std::string room_id = ExtractJsonField(message, "roomid");
              std::string client_id_str = ExtractJsonField(message, "clientid");
              auto room_it = room_map.find(room_id);
              if (room_it != room_map.end()) {
                auto& room = room_it->second;
                // TODO: bind this socket to the RoomMember in PeerRoom
                RTC_LOG(LS_INFO) << "[ws] Register: room=" << room_id
                                 << ", client=" << client_id_str
                                 << ", fd=" << received_fd;
                room.AddMember(std::move(client), std::stoi(client_id_str));
                pending_connections.erase(it);
              } else {
                RTC_LOG(LS_WARNING) << "[ws] Register for unknown room: "
                                    << room_id;
              }
            }
          }
          if (closed) {
            RTC_LOG(LS_INFO) << "[ws] Pending connection closed: fd="
                             << received_fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, received_fd, nullptr);
            pending_connections.erase(it);
          }
          continue;
        }

        bool socket_done = false;
        if (client->onDataAvailable(socket_done)) {
          if (client->upgrade_received()) {
            // WebSocket upgrade — send 101 and keep connection alive.
            client->SendWebSocketUpgradeResponse();
            continue;
          }
          if (client->request_received()) {
            RTC_LOG(LS_INFO) << "[HTTP] Request received: "
                             << client->method_name() << " " << client->path()
                             << " (fd=" << received_fd
                             << ", content_length=" << client->content_length()
                             << ")";
            RTC_LOG(LS_INFO) << "[HTTP] Headers:\n" << client->raw_header();
            if (!client->body().empty()) {
              RTC_LOG(LS_INFO)
                  << "[HTTP] Body (" << client->body().size() << " bytes):\n"
                  << client->body();
            }
            if (client->PathStartsWith("/join")) {
              // POST /join/{roomId}
              std::string path = client->path();
              std::string room_id;
              size_t pos = path.find("/join/");
              if (pos != std::string::npos) {
                room_id = path.substr(pos + 6);
                size_t q = room_id.find('?');
                if (q != std::string::npos)
                  room_id = room_id.substr(0, q);
              }
              if (room_id.empty()) {
                client->Send(
                    "400 Bad Request", true, "application/json", "",
                    "{\"result\":\"ERROR\",\"error\":\"Missing room id\"}");
              } else {
                auto [room_it, inserted] = room_map.try_emplace(room_id, room_id);
                auto& room = room_it->second;
                if (room.count() >= 2) {
                  client->Send(
                      "400 Bad Request", true, "application/json", "",
                      "{\"result\":\"FULL\"}");
                } else {
                  int client_id = room.HandleJoin();
                  bool is_initiator = (client_id == 1);
                  std::string host = GetLocalAddr(received_fd, port);
                  std::string queued = room.DrainMessages(client_id);
                  std::string json = absl::StrCat(
                      "{\"result\":\"SUCCESS\","
                      "\"params\":{"
                      "\"is_initiator\":",
                      is_initiator ? "true" : "false",
                      ",\"room_id\":\"",
                      room_id,
                      "\","
                      "\"client_id\":\"",
                      absl::StrCat(client_id),
                      "\","
                      "\"messages\":",
                      queued,
                      ","
                      "\"wss_url\":\"ws://",
                      host,
                      "/ws\","
                      "\"wss_post_url\":\"http://",
                      host,
                      "\""
                      "}}");
                  RTC_LOG(LS_INFO) << "[join] room=" << room_id
                                   << ", client=" << client_id
                                   << ", initiator=" << is_initiator;
                  client->Send("200 OK", true, "application/json", "", json);
                }
              }
              pending_connections.erase(it);
            } else if (client->PathStartsWith("/message")) {
              // POST /message/{roomId}/{clientId}
              std::string path = client->path();
              std::string room_id;
              int sender_id = 0;
              size_t pos = path.find("/message/");
              if (pos != std::string::npos) {
                std::string rest = path.substr(pos + 9);
                size_t slash = rest.find('/');
                if (slash != std::string::npos) {
                  room_id = rest.substr(0, slash);
                  sender_id = std::stoi(rest.substr(slash + 1));
                }
              }
              auto room_it = room_map.find(room_id);
              if (room_it != room_map.end()) {
                RoomMember* other =
                    room_it->second.GetOtherMember(sender_id);
                if (other && other->socket()) {
                  other->SendMessage(client->body());
                  RTC_LOG(LS_INFO) << "[msg] Forwarded from client="
                                   << sender_id << " room=" << room_id
                                   << " (" << client->body().size()
                                   << " bytes)";
                } else {
                  // Peer not connected yet — queue for delivery on /join.
                  int peer_id = (sender_id == 1) ? 2 : 1;
                  room_it->second.EnqueueMessage(peer_id, client->body());
                  RTC_LOG(LS_INFO)
                      << "[msg] Queued for client=" << peer_id
                      << " in room=" << room_id
                      << " (" << client->body().size() << " bytes)";
                }
              }
              client->Send("200 OK", true, "application/json", "",
                           "{\"result\":\"SUCCESS\"}");
              pending_connections.erase(it);
            } else if (client->PathStartsWith("/leave")) {
              // POST /leave/123456/78
              client->Send("200 OK", true, "application/json", "",
                           "{\"result\":\"SUCCESS\"}");
              pending_connections.erase(it);
            } else if (client->PathEquals("/params")) {
              // GET /params — return empty ICE server config for LAN.
              client->Send("200 OK", true, "application/json", "",
                           "{\"ice_servers\":[]}");
              pending_connections.erase(it);
            } else {
              RTC_LOG(LS_WARNING) << "Unknown request: " << client->path();
              client->Send("404 Not Found", true, "text/plain", "", "");
              pending_connections.erase(it);
            }
          }
        } else if (socket_done) {
          // Client disconnected before sending a complete request.
          RTC_LOG(LS_INFO) << "[HTTP] Connection closed: fd=" << received_fd;
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, received_fd, nullptr);
          pending_connections.erase(it);
        }
      } else if (auto rm_it = room_members.find(received_fd);
                 rm_it != room_members.end()) {
        // WebSocket message from a room member.
        if (event_flags & (EPOLLERR | EPOLLHUP)) {
          RTC_LOG(LS_INFO) << "[ws] Connection lost: fd=" << received_fd;
          fds_to_remove.push_back(received_fd);
          continue;
        }
        auto& member = rm_it->second;
        std::string message;
        bool closed = false;
        if (member->socket()->ReadWebSocketFrame(message, closed)) {
          RTC_LOG(LS_INFO) << "[ws] Frame from fd=" << received_fd << ": "
                           << message;
          std::string cmd = ExtractJsonField(message, "cmd");
          if (cmd == "register") {
            std::string room_id = ExtractJsonField(message, "roomid");
            std::string client_id = ExtractJsonField(message, "clientid");
            member->set_room_id(room_id);
            member->set_client_id(client_id);
            member->set_registered(true);
            RTC_LOG(LS_INFO)
                << "[ws] Registered: room=" << room_id
                << ", client=" << client_id << ", fd=" << received_fd;
          } else if (cmd == "send") {
            // Forward signaling message to other members in the same room.
            std::string msg = ExtractJsonField(message, "msg");
            RTC_LOG(LS_INFO)
                << "[ws] Forwarding from fd=" << received_fd << ": " << msg;
            for (auto& [fd, other] : room_members) {
              if (fd != received_fd && other->registered() &&
                  other->room_id() == member->room_id()) {
                std::string wrapped = "{\"msg\":\"" + msg + "\"}";
                other->SendMessage(wrapped);
                RTC_LOG(LS_INFO) << "[ws] -> forwarded to fd=" << fd;
              }
            }
          }
        }
        if (closed) {
          RTC_LOG(LS_INFO) << "[ws] Close frame from fd=" << received_fd;
          fds_to_remove.push_back(received_fd);
        }
      } else {
        // fd not tracked in any map — clean up to prevent repeated events.
        RTC_LOG(LS_WARNING) << "[epoll] Untracked fd=" << received_fd
                             << ", removing from epoll";
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, received_fd, nullptr);
        ::close(received_fd);
      }
    }

    // Clean up disconnected WebSocket members.
    for (SocketFd fd : fds_to_remove) {
      auto it = room_members.find(fd);
      if (it != room_members.end()) {
        RTC_LOG(LS_INFO) << "[ws] Removing member: fd=" << fd
                         << ", room=" << it->second->room_id()
                         << ", client=" << it->second->client_id();
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        room_members.erase(it);
      }
    }
  }

  close(epoll_fd);

  // Remove file log sink before exiting.
  webrtc::LogMessage::RemoveLogToStream(log_sink.get());

  return 0;
}