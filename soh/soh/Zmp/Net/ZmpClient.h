#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Zmp {

enum class NetState { Disconnected, Connecting, Connected, Rejected, Error };

struct NetPlayer {
    uint32_t id = 0;
    std::string name;
};

struct NetStatus {
    NetState state = NetState::Disconnected;
    std::string host;
    uint16_t port = 0;
    std::string room;
    std::string name;
    // Reject reason code ("build_mismatch", ...) or error code.
    std::string reason;
    std::string detail;
    uint32_t playerId = 0;
    std::vector<NetPlayer> players;
    int rttMs = -1;
};

struct Handshake {
    std::string buildHash;
    std::string ootHash;
    std::string sohHash;
    std::string cvarProfileHash;
    uint32_t randoSeed = 0;
};

// Connection to zmp-server. Phase 0: handshake, room membership, ping. All network I/O
// runs on its own thread; nothing here writes game state.
class Client {
  public:
    static Client& Get();

    void Connect(const std::string& host, uint16_t port, const std::string& room, const std::string& name);
    void Disconnect();
    NetStatus GetStatus();
    // Handshake values of this client (file hashes are computed once, on first use).
    Handshake GetHandshake();

    static const char* StateName(NetState state);

  private:
    void Run(std::string host, uint16_t port, std::string room, std::string name);
    void SetState(NetState state, const std::string& reason = "", const std::string& detail = "");

    std::mutex mMutex;
    NetStatus mStatus;
    std::thread mThread;
    std::atomic<bool> mStop{ false };
    std::mutex mHandshakeMutex;
    bool mHandshakeReady = false;
    Handshake mHandshake;
};

} // namespace Zmp
