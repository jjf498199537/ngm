#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "rtc_base/logging.h"
#include "server_socket.h"

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
  listen_event.data.ptr = &listener;
  epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listener.socket(),&listen_event);

  const int kMaxEvents = 64;
  struct epoll_event events[kMaxEvents];

  bool quit = false;
  SocketArray sockets;
  while (!quit) {
    int num_events = epoll_wait(epoll_fd,events,kMaxEvents,100);

    for (int i = 0; i < num_events; i++) {
      if (events[i].data.ptr == &listener) {
        auto client = listener.Accept();
        if (client) {
          struct epoll_event client_event = {};
          client_event.events = EPOLLIN;
          client_event.data.ptr = client.get();
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->socket(), &client_event);
          sockets.push_back(std::move(client));
        }
      } else {
        bool 
        auto* client = static_cast<ServerDataSocket*>(events[i].data.ptr);
        client
      }
    }
  }

  close(epoll_fd);
  return 0;
}