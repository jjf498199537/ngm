#ifndef NGM_ROOM_SERVER_PEER_ROOM_H_
#define NGM_ROOM_SERVER_PEER_ROOM_H_

#include <memory>
class ServerDataSocket;

class RoomMember {
 public:
  explicit RoomMember(std::unique_ptr<ServerDataSocket> data_socket);
  ~RoomMember();

 protected:
  std::unique_ptr<ServerDataSocket> data_socket_;
};

class PeerRoom {
 public:
  PeerRoom() {}
  ~PeerRoom();
  bool AddMember(std::unique_ptr<ServerDataSocket>);
};
#endif