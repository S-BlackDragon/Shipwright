#include "ZmpProtocol.h"

#include <cstdio>
#include <stdexcept>

namespace Zmp {

std::vector<uint8_t> EncodeFrame(const nlohmann::json& msg) {
    std::vector<uint8_t> body = nlohmann::json::to_msgpack(msg);
    std::vector<uint8_t> frame(4 + body.size());
    uint32_t len = (uint32_t)body.size();
    frame[0] = (uint8_t)(len & 0xFF);
    frame[1] = (uint8_t)((len >> 8) & 0xFF);
    frame[2] = (uint8_t)((len >> 16) & 0xFF);
    frame[3] = (uint8_t)((len >> 24) & 0xFF);
    std::copy(body.begin(), body.end(), frame.begin() + 4);
    return frame;
}

void FrameReader::Append(const uint8_t* data, size_t size) {
    mBuffer.insert(mBuffer.end(), data, data + size);
}

bool FrameReader::Next(nlohmann::json& out) {
    if (mBuffer.size() < 4) {
        return false;
    }
    uint32_t len = (uint32_t)mBuffer[0] | ((uint32_t)mBuffer[1] << 8) | ((uint32_t)mBuffer[2] << 16) |
                   ((uint32_t)mBuffer[3] << 24);
    if (len > kMaxFrameSize) {
        throw std::runtime_error("frame too large");
    }
    if (mBuffer.size() < 4 + (size_t)len) {
        return false;
    }
    std::vector<uint8_t> body(mBuffer.begin() + 4, mBuffer.begin() + 4 + len);
    mBuffer.erase(mBuffer.begin(), mBuffer.begin() + 4 + len);
    out = nlohmann::json::from_msgpack(body);
    return true;
}

std::string HashFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return "missing";
    }
    uint64_t h = 0xcbf29ce484222325ULL;
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 0x100000001b3ULL;
        }
    }
    fclose(f);
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    return hex;
}

} // namespace Zmp
