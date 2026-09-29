#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Zmp {

// Incremented on any incompatible protocol change. The server rejects other values.
constexpr int kProtocolVersion = 3; // 2: lockstep (docs/PROTOCOLO.md); 3: one group per scene (phase 5)
// Frames larger than this are treated as a protocol error.
constexpr uint32_t kMaxFrameSize = 16u * 1024u * 1024u;

// Frame = uint32 little-endian body length + MessagePack body (PLAN.md 8.1).
std::vector<uint8_t> EncodeFrame(const nlohmann::json& msg);

// Accumulates received bytes and extracts complete messages.
class FrameReader {
  public:
    void Append(const uint8_t* data, size_t size);
    // Returns true and fills `out` when a complete frame is available. Throws on a
    // malformed or oversized frame.
    bool Next(nlohmann::json& out);

  private:
    std::vector<uint8_t> mBuffer;
};

// FNV-1a 64-bit hash of a whole file as a 16-char lowercase hex string, or "missing"
// if the file cannot be read.
std::string HashFile(const std::string& path);

// Content hash of the simulation-relevant resources of an .o2r archive (scenes, rooms, collision, paths,
// cutscenes, animations, skeletons, blobs, text; not textures, display lists, vertices, audio). Two
// extractions of the same ROM give the same value even though the zip files differ (timestamps).
// Same rule as tools/o2r_manifest.py `simhash` (reports/fase2/ROMS.md). "missing" if unreadable.
std::string HashAssetsSim(const std::string& path, uint32_t* entries = nullptr);

} // namespace Zmp
