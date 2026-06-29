#ifndef NGM_ROOM_SERVER_PEER_ROOM_H_
#define NGM_ROOM_SERVER_PEER_ROOM_H_

#include <memory>
#include <string>
#include <vector>
class ServerDataSocket;

enum class MemberID {
  Initiator = 1,
  Callee = 2,
};

class RoomMember {
 public:
  explicit RoomMember(std::unique_ptr<ServerDataSocket> data_socket);
  ~RoomMember();

  ServerDataSocket* socket() const { return data_socket_.get(); }

  bool registered() const { return registered_; }
  void set_registered(bool v) { registered_ = v; }

  const std::string& room_id() const { return room_id_; }
  void set_room_id(const std::string& id) { room_id_ = id; }

  const std::string& client_id() const { return client_id_; }
  void set_client_id(const std::string& id) { client_id_ = id; }

  // Send a WebSocket text frame.
  bool SendMessage(const std::string& message) const;

 protected:
  std::unique_ptr<ServerDataSocket> data_socket_;
  std::string room_id_;
  std::string client_id_;
  bool registered_ = false;
};

class PeerRoom {
 public:
  PeerRoom() {}
  explicit PeerRoom(const std::string& room_id) : room_id_(room_id) {}
  ~PeerRoom();

  PeerRoom(const PeerRoom&) = delete;
  PeerRoom& operator=(const PeerRoom&) = delete;

  PeerRoom(PeerRoom&&) = default;
  PeerRoom& operator=(PeerRoom&&) = default;

  const std::string& room_id() const { return room_id_; }
  void set_room_id(const std::string& id) { room_id_ = id; }

  int HandleJoin();
  bool AddMember(std::unique_ptr<ServerDataSocket> client, int client_id);
  int count() const { return member_count_; }

  // Get the member by client_id (1=initiator, 2=callee).
  RoomMember* GetMember(int client_id) const;
  // Get the other member (for message forwarding).
  RoomMember* GetOtherMember(int client_id) const;

  // Queue a message for a client that hasn't joined yet.
  void EnqueueMessage(int for_client_id, const std::string& message);
  // Drain all queued messages for a client (returned as JSON array string).
  std::string DrainMessages(int for_client_id);

 protected:
  std::string room_id_;
  std::unique_ptr<RoomMember> initiator_;
  std::unique_ptr<RoomMember> callee_;
  int member_count_ = 0;
  // Pending messages for each client (delivered on /join).
  std::vector<std::string> pending_for_initiator_;
  std::vector<std::string> pending_for_callee_;
};
#endif