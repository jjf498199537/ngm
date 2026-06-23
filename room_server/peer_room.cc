#include "peer_room.h"

#include <utility>

#include "server_socket.h"

RoomMember::RoomMember(std::unique_ptr<ServerDataSocket> data_socket)
    : data_socket_(std::move(data_socket)) {}

RoomMember::~RoomMember() = default;

PeerRoom::~PeerRoom() = default;

bool PeerRoom::AddMember(std::unique_ptr<ServerDataSocket> data_socket) {
  return false;
}
