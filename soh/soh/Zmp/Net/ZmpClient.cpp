#include "ZmpClient.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <filesystem>

#include <SDL2/SDL_net.h>
#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "ZmpProtocol.h"
#include "Lockstep.h"
#include "soh/Zmp/Sim/CVarProfile.h"
#include "soh/Zmp/State/ResourceSlots.h"
#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/ZmpLog.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Zmp {

static std::string ExecutablePath() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD len = GetModuleFileNameW(NULL, buf, 4096);
    if (len == 0 || len >= 4096) {
        return "";
    }
    return std::filesystem::path(std::wstring(buf, len)).string();
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        return "";
    }
    return std::string(buf, (size_t)len);
#endif
}

Client& Client::Get() {
    static Client sInstance;
    return sInstance;
}

const char* Client::StateName(NetState state) {
    switch (state) {
        case NetState::Disconnected:
            return "disconnected";
        case NetState::Connecting:
            return "connecting";
        case NetState::Connected:
            return "connected";
        case NetState::Rejected:
            return "rejected";
        case NetState::Error:
            return "error";
    }
    return "unknown";
}

Handshake Client::GetHandshake() {
    std::lock_guard<std::mutex> lock(mHandshakeMutex);
    if (!mHandshakeReady) {
        mHandshake.buildHash = HashFile(ExecutablePath());
        // Content hash of the simulation resources: two extractions of the same ROM match even though the
        // zip files differ (reports/fase2/ROMS.md), plus the list of resource paths (save states need it).
        uint32_t simEntries = 0;
        std::string sim = HashAssetsSim(Ship::Context::LocateFileAcrossAppDirs("oot.o2r"), &simEntries);
        char files[24];
        snprintf(files, sizeof(files), "%016llx", (unsigned long long)ResSlots::FileListHash());
        mHandshake.ootHash = sim + "-" + files;
        mHandshake.sohHash = HashFile(Ship::Context::LocateFileAcrossAppDirs("soh.o2r"));
        char prof[24];
        snprintf(prof, sizeof(prof), "%016llx", (unsigned long long)CVarProfile::Hash());
        mHandshake.cvarProfileHash = prof;
        Log("handshake: oot sim entries=" + std::to_string(simEntries));
        mHandshake.randoSeed = 0;
        mHandshakeReady = true;
        Log("handshake hashes: build=" + mHandshake.buildHash + " oot=" + mHandshake.ootHash +
            " soh=" + mHandshake.sohHash);
    }
    Handshake hs = mHandshake;
#ifdef ZMP_HARNESS
    const char* fake = CVarGetString(ZMP_CVAR_DEBUG_BUILD_HASH, "");
    if (fake != nullptr && fake[0] != '\0') {
        hs.buildHash = fake;
    }
#endif
    return hs;
}

void Client::SetState(NetState state, const std::string& reason, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mMutex);
    mStatus.state = state;
    mStatus.reason = reason;
    mStatus.detail = detail;
    if (state != NetState::Connected) {
        mStatus.players.clear();
        mStatus.rttMs = -1;
    }
}

NetStatus Client::GetStatus() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mStatus;
}

void Client::Connect(const std::string& host, uint16_t port, const std::string& room, const std::string& name) {
    Disconnect();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStatus = NetStatus();
        mStatus.state = NetState::Connecting;
        mStatus.host = host;
        mStatus.port = port;
        mStatus.room = room;
        mStatus.name = name;
    }
    mStop = false;
    mThread = std::thread(&Client::Run, this, host, port, room, name);
}

void Client::Disconnect() {
    mStop = true;
    if (mThread.joinable()) {
        mThread.join();
    }
    std::lock_guard<std::mutex> lock(mMutex);
    if (mStatus.state == NetState::Connecting || mStatus.state == NetState::Connected ||
        mStatus.state == NetState::Error) {
        mStatus.state = NetState::Disconnected;
        mStatus.players.clear();
        mStatus.rttMs = -1;
    }
}

static bool SendAll(TCPsocket sock, const std::vector<uint8_t>& data) {
    return SDLNet_TCP_Send(sock, data.data(), (int)data.size()) == (int)data.size();
}

// Phase 5b: nothing the game thread sends may hold it (the state of a group for a player who enters is 50 KB; on a
// real connection the socket takes its time). Messages are queued and one writer thread puts them on the socket in
// order. The queue lives for the whole process (never destroyed: the writer thread may outlive the statics).
namespace {
struct Outbox {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> frames;
    bool open = false; // a connection past its HELLO exists
    bool started = false;
};
Outbox* sOut = new Outbox();
} // namespace

bool Client::Send(const nlohmann::json& msg) {
    std::vector<uint8_t> frame = EncodeFrame(msg);
    {
        std::lock_guard<std::mutex> lock(sOut->mutex);
        if (!sOut->open) {
            return false;
        }
        sOut->frames.push_back(std::move(frame));
        if (!sOut->started) {
            sOut->started = true;
            std::thread([this]() {
                for (;;) {
                    std::vector<uint8_t> next;
                    {
                        std::unique_lock<std::mutex> lk(sOut->mutex);
                        sOut->cv.wait(lk, [] { return !sOut->frames.empty(); });
                        next = std::move(sOut->frames.front());
                        sOut->frames.pop_front();
                    }
                    std::lock_guard<std::mutex> lk(mSendMutex);
                    auto t0 = std::chrono::steady_clock::now();
                    if (mSock != nullptr && !SendAll((TCPsocket)mSock, next)) {
                        Log("net: send failed (" + std::to_string(next.size()) + " bytes)");
                    }
                    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    if (ms > 20.0) {
                        Log("net: slow send, " + std::to_string(next.size()) + " bytes took " +
                            std::to_string((int)ms) + " ms on the socket");
                    }
                }
            }).detach();
        }
    }
    sOut->cv.notify_one();
    return true;
}

static uint64_t SteadyMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void ReadPlayers(const nlohmann::json& msg, std::vector<NetPlayer>& out) {
    out.clear();
    if (!msg.contains("players") || !msg["players"].is_array()) {
        return;
    }
    for (auto& p : msg["players"]) {
        NetPlayer np;
        np.id = p.value("id", 0u);
        np.name = p.value("name", std::string());
        np.slot = p.value("slot", -1);
        np.rtt = p.value("rtt", -1);
        np.state = p.value("state", std::string());
        np.leader = p.value("leader", false);
        out.push_back(np);
    }
}

void Client::Run(std::string host, uint16_t port, std::string room, std::string name) {
    Handshake hs = GetHandshake();

    while (!mStop) {
        SetState(NetState::Connecting);
        IPaddress addr;
        TCPsocket sock = nullptr;
        if (SDLNet_ResolveHost(&addr, host.c_str(), port) == 0) {
            sock = SDLNet_TCP_Open(&addr);
        }
        if (sock == nullptr) {
            SetState(NetState::Error, "unreachable", "No se pudo conectar a " + host + ":" + std::to_string(port));
            Log("net: connect failed to " + host + ":" + std::to_string(port));
            // Retry every 2 s until Disconnect() is called.
            for (int i = 0; i < 20 && !mStop; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        Log("net: tcp connected to " + host + ":" + std::to_string(port) + ", sending HELLO");
        nlohmann::json hello = {
            { "t", "HELLO" },
            { "proto", kProtocolVersion },
            { "build_hash", hs.buildHash },
            { "oot_hash", hs.ootHash },
            { "soh_hash", hs.sohHash },
            { "cvar_profile_hash", hs.cvarProfileHash },
            { "rando_seed", hs.randoSeed },
            { "name", name },
            { "room", room },
            { "client_nonce", SteadyMs() },
        };
        SDLNet_SocketSet set = SDLNet_AllocSocketSet(1);
        SDLNet_TCP_AddSocket(set, sock);
        bool ok;
        {
            std::lock_guard<std::mutex> lock(mSendMutex);
            mSock = sock;
            ok = SendAll(sock, EncodeFrame(hello));
        }
        {
            std::lock_guard<std::mutex> lock(sOut->mutex);
            sOut->frames.clear();
            sOut->open = ok;
        }
        bool welcomed = false;
        FrameReader reader;
        uint64_t lastPing = 0;
        uint64_t pingNonce = 0;
        bool rejected = false;

        while (ok && !mStop) {
            int ready = SDLNet_CheckSockets(set, 100);
            if (ready < 0) {
                ok = false;
                break;
            }
            if (ready > 0 && SDLNet_SocketReady(sock)) {
                uint8_t buf[4096];
                int n = SDLNet_TCP_Recv(sock, buf, sizeof(buf));
                if (n <= 0) {
                    ok = false;
                    break;
                }
                reader.Append(buf, (size_t)n);
                nlohmann::json msg;
                try {
                    while (reader.Next(msg)) {
                        std::string t = msg.value("t", std::string());
                        if (t == "WELCOME") {
                            std::lock_guard<std::mutex> lock(mMutex);
                            mStatus.state = NetState::Connected;
                            mStatus.reason.clear();
                            mStatus.detail.clear();
                            mStatus.playerId = msg.value("player_id", 0u);
                            mStatus.room = msg.value("room", room);
                            ReadPlayers(msg, mStatus.players);
                            Log("net: WELCOME player_id=" + std::to_string(mStatus.playerId) + " room=" + mStatus.room +
                                (msg.value("rejoin", false) ? " (rejoin)" : ""));
                            welcomed = true;
                            Lockstep::OnConnected(msg.value("rejoin", false));
                        } else if (t == "REJECT") {
                            std::string reason = msg.value("reason", std::string("rejected"));
                            std::string detail = msg.value("detail", std::string());
                            SetState(NetState::Rejected, reason, detail);
                            Log("net: REJECT reason=" + reason + " detail=" + detail);
                            rejected = true;
                        } else if (t == "PLAYER_LIST") {
                            {
                                std::lock_guard<std::mutex> lock(mMutex);
                                ReadPlayers(msg, mStatus.players);
                            }
                            Lockstep::OnNetMessage(std::move(msg));
                        } else if (t == "PONG") {
                            uint64_t sentAt = msg.value("sent_at", (uint64_t)0);
                            std::lock_guard<std::mutex> lock(mMutex);
                            mStatus.rttMs = (int)(SteadyMs() - sentAt);
                        } else if (t == "PING") {
                            nlohmann::json pong = { { "t", "PONG" },
                                                    { "nonce", msg.value("nonce", (uint64_t)0) },
                                                    { "sent_at", msg.value("sent_at", (uint64_t)0) } };
                            ok = Send(pong);
                        } else if (t == "KICK") {
                            SetState(NetState::Rejected, "kicked", msg.value("reason", std::string()));
                            rejected = true;
                        } else if (!t.empty()) {
                            Lockstep::OnNetMessage(std::move(msg));
                        }
                    }
                } catch (const std::exception& e) {
                    Log(std::string("net: protocol error: ") + e.what());
                    ok = false;
                }
                if (rejected) {
                    break;
                }
            }
            uint64_t now = SteadyMs();
            if (ok && GetStatus().state == NetState::Connected && now - lastPing >= 1000) {
                lastPing = now;
                nlohmann::json ping = { { "t", "PING" }, { "nonce", ++pingNonce }, { "sent_at", now } };
                ok = Send(ping);
            }
        }

        // (what was already handed over still goes out, e.g. the LEAVE_GROUP of a player who quits)
        for (int i = 0; i < 20; i++) {
            {
                std::lock_guard<std::mutex> lock(sOut->mutex);
                if (sOut->frames.empty()) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            std::lock_guard<std::mutex> lock(sOut->mutex);
            sOut->open = false;
            sOut->frames.clear();
        }
        {
            std::lock_guard<std::mutex> lock(mSendMutex);
            mSock = nullptr;
        }
        if (welcomed) {
            Lockstep::OnDisconnected();
        }
        SDLNet_TCP_DelSocket(set, sock);
        SDLNet_FreeSocketSet(set);
        SDLNet_TCP_Close(sock);

        if (rejected || mStop) {
            break;
        }
        SetState(NetState::Error, "connection_lost", "Conexion perdida con " + host);
        Log("net: connection lost, retrying");
        for (int i = 0; i < 20 && !mStop; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (mStop) {
        SetState(NetState::Disconnected);
    }
}

} // namespace Zmp
