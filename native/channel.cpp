#include "channel.h"
#include "scene_buffers.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace editor {
bool Channel::read_exact(void* data, size_t size) {
  auto* bytes = static_cast<char*>(data);
  size_t received = 0;
  while (received < size) {
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(input_), bytes + received, DWORD(size - received), &count, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE && received == 0) return false;
      throw std::runtime_error("Renderer channel read failed");
    }
#else
    const auto count = ::read(int(input_), bytes + received, size - received);
    if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("Renderer channel read failed"); }
#endif
    if (count == 0) {
      if (!received) return false;
      throw std::runtime_error("Truncated renderer message");
    }
    received += size_t(count);
  }
  return true;
}
void Channel::write_exact(const void* data, size_t size) {
  auto* bytes = static_cast<const char*>(data);
  while (size) {
#ifdef _WIN32
    DWORD count = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(output_), bytes, DWORD(size), &count, nullptr)) throw std::runtime_error("Renderer channel closed");
#else
    auto count = ::write(int(output_), bytes, size);
    if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("Renderer channel closed"); }
#endif
    if (count == 0) throw std::runtime_error("Renderer channel made no progress");
    bytes += count; size -= size_t(count);
  }
}
bool Channel::read_chain(std::string& payload) {
  payload.clear();
  while (true) {
    std::array<uint8_t, 4> header{};
    if (!read_exact(header.data(), header.size())) {
      if (payload.empty()) return false;
      throw std::runtime_error("Missing renderer message continuation");
    }
    const auto flags = uint32_t(header[0]) | uint32_t(header[1]) << 8 | uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
    const auto length = flags & ~more_frames;
    if (!length || length > max_frame_bytes) throw std::runtime_error("Renderer frame exceeds protocol limit");
    const auto offset = payload.size();
    if (length > payload.max_size() - offset) throw std::runtime_error("Renderer message exceeds available storage");
    payload.resize(offset + length);
    if (!read_exact(payload.data() + offset, length)) throw std::runtime_error("Missing renderer message body");
    if (!(flags & more_frames)) return true;
  }
}
namespace {
void resolve(nlohmann::json& value, const std::string& data, const std::vector<std::pair<size_t, size_t>>& ranges) {
  if (value.is_object()) {
    if (const auto found = value.find("$buffer"); found != value.end()) {
      const auto index = found->get<size_t>();
      if (index >= ranges.size()) throw std::runtime_error("Renderer message refers to a missing buffer");
      const auto* bytes = reinterpret_cast<const uint8_t*>(data.data()) + ranges[index].first;
      value = nlohmann::json::binary(std::vector<uint8_t>(bytes, bytes + ranges[index].second), uint8_t(buffer_type(value.at("type").get<std::string>())));
      return;
    }
    for (auto& item : value) resolve(item, data, ranges);
  } else if (value.is_array()) for (auto& item : value) resolve(item, data, ranges);
}
nlohmann::json strip(const nlohmann::json& value, std::vector<const nlohmann::json::binary_t*>& buffers) {
  if (value.is_binary()) {
    const auto& bytes = value.get_binary();
    buffers.push_back(&bytes);
    return {{"$buffer", buffers.size() - 1}, {"type", buffer_type_name(bytes.has_subtype() ? bytes.subtype() : 0)}};
  }
  if (!value.is_structured()) return value;
  auto result = value.is_object() ? nlohmann::json::object() : nlohmann::json::array();
  if (value.is_object()) for (auto item = value.begin(); item != value.end(); ++item) result[item.key()] = strip(item.value(), buffers);
  else for (const auto& item : value) result.push_back(strip(item, buffers));
  return result;
}
}
bool Channel::receive(nlohmann::json& message) {
  message = nullptr;
  std::string payload;
  if (!read_chain(payload)) return false;
  message = nlohmann::json::parse(payload);
  if (!message.is_object() || message.value("version", 0u) != protocol_version) throw std::runtime_error("Incompatible renderer protocol version");
  if (const auto found = message.find("buffers"); found != message.end()) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t total = 0;
    for (const auto& length : *found) {
      const auto size = length.get<uint64_t>();
      if (size > payload.max_size() - total) throw std::runtime_error("Renderer message exceeds available storage");
      ranges.emplace_back(total, size_t(size)); total += size_t(size);
    }
    message.erase(found);
    if (total && !read_chain(payload)) throw std::runtime_error("Missing renderer message buffers");
    if (!total) payload.clear();
    if (payload.size() != total) throw std::runtime_error("Renderer buffers do not match their declared sizes");
    resolve(message, payload, ranges);
  }
  return true;
}
void Channel::write_chain(const std::vector<std::pair<const uint8_t*, size_t>>& parts) {
  size_t remaining = 0;
  for (const auto& part : parts) remaining += part.second;
  size_t part = 0, offset = 0;
  while (remaining) {
    const auto length = std::min(size_t(max_frame_bytes), remaining);
    remaining -= length;
    const auto flags = uint32_t(length) | (remaining ? more_frames : 0u);
    std::array<uint8_t, 4> header{uint8_t(flags), uint8_t(flags >> 8), uint8_t(flags >> 16), uint8_t(flags >> 24)};
    write_exact(header.data(), header.size());
    for (size_t left = length; left;) {
      while (offset == parts[part].second) { ++part; offset = 0; }
      const auto count = std::min(left, parts[part].second - offset);
      write_exact(parts[part].first + offset, count);
      left -= count; offset += count;
    }
  }
}
void Channel::send(const nlohmann::json& message) {
  std::lock_guard lock(write_mutex_);
  std::vector<const nlohmann::json::binary_t*> buffers;
  auto envelope = strip(message, buffers); envelope["version"] = protocol_version;
  std::vector<std::pair<const uint8_t*, size_t>> parts;
  if (!buffers.empty()) {
    envelope["buffers"] = nlohmann::json::array();
    for (const auto* buffer : buffers) { envelope["buffers"].push_back(buffer->size()); parts.emplace_back(buffer->data(), buffer->size()); }
  }
  const auto payload = envelope.dump();
  write_chain({{reinterpret_cast<const uint8_t*>(payload.data()), payload.size()}});
  write_chain(parts);
}
}
