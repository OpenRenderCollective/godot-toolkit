#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace editor {
// Scene geometry is carried as typed nlohmann binary values. The subtype
// selects the Godot packed array type and is named on the renderer wire.
enum class BufferType : std::uint8_t { bytes = 0, vec3 = 1, vec2 = 2, i32 = 3, u32 = 4, f32 = 5 };
inline const char* buffer_type_name(std::uint64_t type) {
  static const char* names[] = {"bytes", "vec3", "vec2", "i32", "u32", "f32"};
  return type < std::size(names) ? names[type] : "bytes";
}
inline BufferType buffer_type(const std::string& name) {
  for (std::uint8_t i = 0; i <= std::uint8_t(BufferType::f32); ++i) if (name == buffer_type_name(i)) return BufferType(i);
  throw std::runtime_error("Unknown buffer type: " + name);
}
template<typename T> nlohmann::json make_buffer(const std::vector<T>& values, BufferType type) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
  return nlohmann::json::binary(std::vector<std::uint8_t>(bytes, bytes + values.size() * sizeof(T)), std::uint8_t(type));
}
// Element-wise copy, so binary storage needs no particular alignment.
template<typename T> std::vector<T> read_buffer(const nlohmann::json& value) {
  if (!value.is_binary()) throw std::runtime_error("Expected a binary buffer");
  const auto& bytes = value.get_binary();
  if (bytes.size() % sizeof(T)) throw std::runtime_error("Binary buffer size does not match its element type");
  std::vector<T> result(bytes.size() / sizeof(T));
  if (!result.empty()) std::memcpy(result.data(), bytes.data(), bytes.size());
  return result;
}
// Text transports (CLI, string bridge) encode binary values as base64 objects.
inline nlohmann::json text_buffers(const nlohmann::json& value) {
  if (value.is_binary()) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto& bytes = value.get_binary();
    std::string text; text.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
      const std::uint32_t n = std::uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? std::uint32_t(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
      text += alphabet[n >> 18 & 63]; text += alphabet[n >> 12 & 63];
      text += i + 1 < bytes.size() ? alphabet[n >> 6 & 63] : '='; text += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
    }
    return {{"type", buffer_type_name(bytes.has_subtype() ? bytes.subtype() : 0)}, {"base64", std::move(text)}};
  }
  if (!value.is_structured()) return value;
  auto result = value.is_object() ? nlohmann::json::object() : nlohmann::json::array();
  if (value.is_object()) for (auto item = value.begin(); item != value.end(); ++item) result[item.key()] = text_buffers(item.value());
  else for (const auto& item : value) result.push_back(text_buffers(item));
  return result;
}
}
