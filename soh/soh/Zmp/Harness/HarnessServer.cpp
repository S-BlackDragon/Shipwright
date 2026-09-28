#ifdef ZMP_HARNESS

// Socket side of the harness: listens on 127.0.0.1 only, one thread per client, JSON lines.
// Nothing in this file touches game state; commands are handed to the game thread.

#include "Harness.h"
#include "HarnessQueue.h"

#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <thread>

#include "soh/Zmp/ZmpLog.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET SockT;
#define ZMP_BAD_SOCK INVALID_SOCKET
#define ZMP_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SockT;
#define ZMP_BAD_SOCK (-1)
#define ZMP_CLOSE close
#endif

namespace Zmp::Harness {

static std::mutex& sQueueMutex = *new std::mutex;
static std::vector<RequestPtr>& sQueue = *new std::vector<RequestPtr>;

void PushRequest(RequestPtr req) {
    std::lock_guard<std::mutex> lock(sQueueMutex);
    sQueue.push_back(std::move(req));
}

std::vector<RequestPtr> TakeRequests() {
    std::lock_guard<std::mutex> lock(sQueueMutex);
    std::vector<RequestPtr> out;
    out.swap(sQueue);
    return out;
}

static std::atomic<bool> sRunning{ false };
static std::atomic<bool> sStop{ false };
static SockT sListen = ZMP_BAD_SOCK;
static std::thread sAcceptThread;
static uint16_t sPort = 0;
// Leaked on purpose: detached client threads may still touch these during process exit.
static std::mutex& sClientsMutex = *new std::mutex;
static std::list<SockT>& sClientSockets = *new std::list<SockT>;

static bool SendLine(SockT s, const std::string& line) {
    std::string data = line + "\n";
    size_t off = 0;
    while (off < data.size()) {
        int n = send(s, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static void ClientLoop(SockT s) {
    std::string buffer;
    char chunk[4096];
    while (!sStop) {
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        buffer.append(chunk, (size_t)n);
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            nlohmann::json resp;
            nlohmann::json cmd = nlohmann::json::parse(line, nullptr, false);
            if (cmd.is_discarded() || !cmd.is_object() || !cmd.contains("cmd")) {
                resp = { { "ok", false }, { "error", "invalid json command" } };
            } else {
                auto req = std::make_shared<Request>();
                req->cmd = cmd;
                auto fut = req->promise.get_future();
                PushRequest(req);
                // Waits are bounded by their own timeout on the game side; this outer limit
                // only protects against a frozen game thread.
                int timeoutMs = cmd.value("timeout_ms", 60000) + 30000;
                if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
                    try {
                        resp = fut.get();
                    } catch (const std::exception& e) {
                        resp = { { "ok", false }, { "error", std::string("internal: ") + e.what() } };
                    }
                } else {
                    resp = { { "ok", false }, { "error", "game thread did not answer" } };
                    if (cmd.contains("id")) {
                        resp["id"] = cmd["id"];
                    }
                }
            }
            if (!SendLine(s, resp.dump())) {
                break;
            }
        }
    }
    ZMP_CLOSE(s);
    std::lock_guard<std::mutex> lock(sClientsMutex);
    sClientSockets.remove(s);
}

static void AcceptLoop() {
    while (!sStop) {
        sockaddr_in addr{};
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        SockT c = accept(sListen, (sockaddr*)&addr, &len);
        if (c == ZMP_BAD_SOCK) {
            if (sStop) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        std::lock_guard<std::mutex> lock(sClientsMutex);
        sClientSockets.push_back(c);
        std::thread(ClientLoop, c).detach();
    }
}

bool Start(uint16_t port) {
    if (sRunning) {
        return true;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sListen == ZMP_BAD_SOCK) {
        Log("harness: socket() failed");
        return false;
    }
#ifdef _WIN32
    // Refuse to share the port with another process (two instances on the same port).
    int excl = 1;
    setsockopt(sListen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Localhost only: the harness must never be reachable from the network.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sListen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(sListen, 8) != 0) {
        Log("harness: cannot bind 127.0.0.1:" + std::to_string(port));
        ZMP_CLOSE(sListen);
        sListen = ZMP_BAD_SOCK;
        return false;
    }
    sPort = port;
    sStop = false;
    sRunning = true;
    sAcceptThread = std::thread(AcceptLoop);
    Log("harness: listening on 127.0.0.1:" + std::to_string(port));
    return true;
}

void Stop() {
    if (!sRunning) {
        return;
    }
    sStop = true;
    ZMP_CLOSE(sListen);
    sListen = ZMP_BAD_SOCK;
    if (sAcceptThread.joinable()) {
        sAcceptThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(sClientsMutex);
        for (SockT s : sClientSockets) {
#ifdef _WIN32
            shutdown(s, SD_BOTH);
#else
            shutdown(s, SHUT_RDWR);
#endif
        }
    }
    // Answer anything still queued so client threads can exit.
    for (auto& req : TakeRequests()) {
        req->Reply({ { "ok", false }, { "error", "shutting down" } });
    }
    sRunning = false;
}

bool IsRunning() {
    return sRunning;
}

uint16_t GetPort() {
    return sPort;
}

} // namespace Zmp::Harness

#endif
