#include "ZmpProtocol.h"

#include <cstdio>
#include <algorithm>
#include <map>
#include <stdexcept>

#include <zip.h>

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

namespace {

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001B3ULL;

void Fnv(uint64_t& h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

uint32_t Read32(const uint8_t* p, bool le) {
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : ((uint32_t)p[3] | ((uint32_t)p[2] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24));
}

// True when the entry counts for the simulation (or is unknown); false for presentation resources.
bool IsSimEntry(const uint8_t* head, size_t n) {
    constexpr size_t kHeader = 0x40;
    if (n < kHeader || (head[0] != 0 && head[0] != 1)) {
        return true; // no resource header: unknown, counted
    }
    bool le = head[0] == 0;
    uint32_t code = Read32(head + 4, le);
    if (code == 0x46669697) { // Light
        return false;
    }
    char four[5] = { (char)((code >> 24) & 0xFF), (char)((code >> 16) & 0xFF), (char)((code >> 8) & 0xFF),
                     (char)(code & 0xFF), 0 };
    for (int i = 0; i < 4; i++) {
        char c = four[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!alnum) {
            return true;
        }
    }
    std::string f(four);
    if (f == "OARR") {
        if (n >= kHeader + 4) {
            return Read32(head + kHeader, le) != 25; // Array of vertices: presentation
        }
        return true;
    }
    static const char* kPresentation[] = { "OTEX", "ODLT", "OVTX", "OMTX", "LGTS", "OBGI",
                                           "OAUD", "OSEQ", "OSFT", "OSMP", "SHAD" };
    for (const char* p : kPresentation) {
        if (f == p) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string HashAssetsSim(const std::string& path, uint32_t* entries) {
    int errorp = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &errorp);
    if (z == nullptr) {
        return "missing";
    }
    zip_int64_t count = zip_get_num_entries(z, 0);
    std::map<std::string, zip_uint64_t> byName; // unique names, first occurrence, sorted bytewise
    for (zip_int64_t i = 0; i < count; i++) {
        const char* name = zip_get_name(z, (zip_uint64_t)i, 0);
        if (name == nullptr) {
            continue;
        }
        std::string n(name);
        if (!n.empty() && n.back() == '/') {
            continue;
        }
        byName.emplace(n, (zip_uint64_t)i);
    }
    uint64_t h = kFnvOffset;
    uint32_t selected = 0;
    std::vector<uint8_t> data;
    for (auto& [name, index] : byName) {
        if (name == "version" || name == "portVersion") {
            continue;
        }
        zip_stat_t st;
        if (zip_stat_index(z, index, 0, &st) != 0 || st.size == 0) {
            continue;
        }
        zip_file_t* f = zip_fopen_index(z, index, 0);
        if (f == nullptr) {
            continue;
        }
        uint8_t head[0x44];
        zip_int64_t got = zip_fread(f, head, sizeof(head));
        if (got < 0) {
            zip_fclose(f);
            continue;
        }
        if (!IsSimEntry(head, (size_t)got)) {
            zip_fclose(f);
            continue;
        }
        data.assign(head, head + got);
        if ((zip_uint64_t)got < st.size) {
            size_t off = data.size();
            data.resize((size_t)st.size);
            zip_int64_t rest = zip_fread(f, data.data() + off, st.size - off);
            if (rest < 0) {
                rest = 0;
            }
            data.resize(off + (size_t)rest);
        }
        zip_fclose(f);
        Fnv(h, (const uint8_t*)name.data(), name.size());
        uint8_t sep[9] = { 0 };
        uint64_t len = data.size();
        for (int i = 0; i < 8; i++) {
            sep[1 + i] = (uint8_t)((len >> (8 * i)) & 0xFF);
        }
        Fnv(h, sep, sizeof(sep));
        Fnv(h, data.data(), data.size());
        selected++;
    }
    zip_close(z);
    if (entries != nullptr) {
        *entries = selected;
    }
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    return hex;
}

} // namespace Zmp
