#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace editor {
constexpr uint32_t protocol_version = 3;
// Each frame has a little-endian uint32 header: length in the low 31 bits,
// with the high bit set when another frame belongs to the same chain. A
// message is one chain of JSON text. Binary values appear in it as
// {"$buffer": index, "type": name}; when the top-level "buffers" array lists
// their byte lengths, a second chain follows with all buffers concatenated.
constexpr uint32_t more_frames = 0x80000000u;
constexpr uint32_t max_frame_bytes = 64 * 1024 * 1024;

class Channel {
public:
  Channel(intptr_t input, intptr_t output) : input_(input), output_(output) {}
  bool receive(nlohmann::json& message);
  void send(const nlohmann::json& message);
private:
  bool read_chain(std::string& payload);
  void write_chain(const std::vector<std::pair<const uint8_t*, size_t>>& parts);
  bool read_exact(void* data, size_t size);
  void write_exact(const void* data, size_t size);
  intptr_t input_, output_;
  std::mutex write_mutex_;
};
}
