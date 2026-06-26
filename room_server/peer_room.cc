#include "peer_room.h"

#include <string>
#include <utility>

#include "rtc_base/logging.h"
#include "rtc_base/checks.h"
#include "server_socket.h"

RoomMember::RoomMember(std::unique_ptr<ServerDataSocket> data_socket)
    : data_socket_(std::move(data_socket)) {}

RoomMember::~RoomMember() = default;

bool RoomMember::SendMessage(const std::string& message) const {
  if (!data_socket_)
    return false;
  return data_socket_->SendWebSocketFrame(message);
}

PeerRoom::~PeerRoom() = default;

bool PeerRoom::AddMember(std::unique_ptr<ServerDataSocket> client,
                          int client_id) {
  RTC_DCHECK(client);
  RTC_DCHECK(client_id > 0 && client_id <= 2);

  auto member = std::make_unique<RoomMember>(std::move(client));
  member->set_room_id(room_id_);
  member->set_client_id(std::to_string(client_id));
  member->set_registered(true);

  if (client_id == static_cast<int>(MemberID::Initiator)) {
    RTC_DCHECK(!initiator_);
    initiator_ = std::move(member);
  } else {
    RTC_DCHECK(!callee_);
    callee_ = std::move(member);
  }
  return true;
}

int PeerRoom::HandleJoin() {
  RTC_DCHECK(member_count_ < 2);
  member_count_++;
  return static_cast<int>(member_count_ == 1 ? MemberID::Initiator
                                             : MemberID::Callee);
}

RoomMember* PeerRoom::GetMember(int client_id) const {
  if (client_id == static_cast<int>(MemberID::Initiator))
    return initiator_.get();
  if (client_id == static_cast<int>(MemberID::Callee))
    return callee_.get();
  return nullptr;
}

RoomMember* PeerRoom::GetOtherMember(int client_id) const {
  // Return the member that is NOT client_id.
  if (client_id == static_cast<int>(MemberID::Initiator))
    return callee_.get();
  if (client_id == static_cast<int>(MemberID::Callee))
    return initiator_.get();
  return nullptr;
}
