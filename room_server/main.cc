#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>
#include <unordered_map>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "rtc_base/logging.h"
#include "server_socket.h"
#include "peer_room.h"

ABSL_FLAG(int, port, 8888, "The port on which the server listens.");
using SocketArray = std::vector<std::unique_ptr<ServerDataSocket>>;

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "NGM Room Server.\n"
      "Example usage: ./room_server --port=8888\n");
  absl::ParseCommandLine(argc, argv);
  int port = absl::GetFlag(FLAGS_port);

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
  epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listener.socket(),&listen_event);

  const int kMaxEvents = 64;
  struct epoll_event events[kMaxEvents];

  bool quit = false;
  std::unordered_map<SocketFd, std::unique_ptr<ServerDataSocket>> pending_connections;
  std::unordered_map<SocketFd, std::unique_ptr<RoomMember>> room_members;
  while (!quit) {
    int num_events = epoll_wait(epoll_fd,events,kMaxEvents,100);

    for (int i = 0; i < num_events; i++) {
      SocketFd received_fd = events[i].data.fd;
      if (received_fd == listener.socket()) {
        auto client = listener.Accept();
        if (client) {
          struct epoll_event client_event = {};
          client_event.events = EPOLLIN;
          client_event.data.fd = client->socket();
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->socket(), &client_event);
          pending_connections[client->socket()] = std::move(client);
        }
      } else if (auto it = pending_connections.find(received_fd); it != pending_connections.end()) {
        bool socket_done = false;
        auto& client = it->second;
        if (client->onDataAvailable(socket_done) && client->request_received()) {

        }
        }
      }
    }
  }

  close(epoll_fd);
  return 0;
}