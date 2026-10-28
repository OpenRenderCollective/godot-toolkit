#include "document.h"
#include "scene_buffers.h"
#include <cstring>
#include <future>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace editor {
void register_gpu_bridge();
namespace {
template<typename Packed, typename Element> Packed copy_packed(const Json::binary_t& bytes, size_t components) {
  Packed result;
  const size_t count = bytes.size() / (sizeof(float) * components);
  if (result.resize(int64_t(count)) != godot::OK) throw std::runtime_error("Not enough memory for scene data");
  // Double-precision Godot builds store real_t components and need a conversion.
  if (sizeof(godot::real_t) == sizeof(float)) {
    if (count) std::memcpy(static_cast<void*>(result.ptrw()), bytes.data(), count * sizeof(float) * components);
  } else {
    for (size_t i = 0; i < count; ++i) {
      float values[3]{};
      std::memcpy(values, bytes.data() + i * components * sizeof(float), components * sizeof(float));
      if constexpr (std::is_same_v<Element, godot::Vector3>) result.set(int64_t(i), godot::Vector3(values[0], values[1], values[2]));
      else result.set(int64_t(i), godot::Vector2(values[0], values[1]));
    }
  }
  return result;
}
// Scene buffers become packed arrays, ready for ArrayMesh without per-number Variants.
godot::Variant packed(const Json::binary_t& bytes) {
  const auto type = BufferType(bytes.has_subtype() ? bytes.subtype() : 0);
  if (type == BufferType::vec3) return copy_packed<godot::PackedVector3Array, godot::Vector3>(bytes, 3);
  if (type == BufferType::vec2) return copy_packed<godot::PackedVector2Array, godot::Vector2>(bytes, 2);
  if (type == BufferType::i32 || type == BufferType::u32) {
    godot::PackedInt32Array result;
    if (result.resize(int64_t(bytes.size() / 4)) != godot::OK) throw std::runtime_error("Not enough memory for scene data");
    if (result.size()) std::memcpy(result.ptrw(), bytes.data(), size_t(result.size()) * 4);
    return result;
  }
  if (type == BufferType::f32) {
    godot::PackedFloat32Array result;
    if (result.resize(int64_t(bytes.size() / 4)) != godot::OK) throw std::runtime_error("Not enough memory for scene data");
    if (result.size()) std::memcpy(result.ptrw(), bytes.data(), size_t(result.size()) * 4);
    return result;
  }
  godot::PackedByteArray result;
  if (result.resize(int64_t(bytes.size())) != godot::OK) throw std::runtime_error("Not enough memory for scene data");
  if (!bytes.empty()) std::memcpy(result.ptrw(), bytes.data(), bytes.size());
  return result;
}
godot::Variant variant(const Json& value) {
  if (value.is_object()) {
    godot::Dictionary result;
    for (auto item = value.begin(); item != value.end(); ++item)
      result[godot::String::utf8(item.key().data(), item.key().size())] = variant(item.value());
    return result;
  }
  if (value.is_array()) {
    godot::Array result;
    if (result.resize(value.size()) != godot::OK) throw std::runtime_error("Not enough memory for scene data");
    for (size_t i = 0; i < value.size(); ++i) result[i] = variant(value[i]);
    return result;
  }
  if (value.is_string()) {
    const auto& string = value.get_ref<const std::string&>();
    return godot::String::utf8(string.data(), string.size());
  }
  if (value.is_binary()) return packed(value.get_binary());
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number()) {
    auto number = value.get<double>();
    // JSON serializes non-finite capture components as null. Keep the same
    // contract without constructing/parsing a potentially >2 GiB string.
    return std::isfinite(number) ? godot::Variant(number) : godot::Variant();
  }
  return {};
}
}
void convert_texture(const fs::path& source, const fs::path& target, const std::string& kind) {
  using godot::Image;
  auto image = Image::load_from_file(source.string().c_str());
  if (image.is_null() || image->is_empty()) throw std::runtime_error("Cannot decode texture: " + source.string());
  if (image->is_compressed() && image->decompress() != godot::OK) throw std::runtime_error("Cannot decompress texture");
  bool normal = kind == "normal_ogl" || kind == "normal_dx";
  bool scalar = kind == "roughness_g" || kind == "metallic_b" || kind == "scalar";
  auto extension = source.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
  bool hdr = extension == ".exr" || extension == ".hdr";
  image->clear_mipmaps();
  if (scalar) {
    image->convert(Image::FORMAT_RGBA8);
    auto pixels = image->get_data();
    godot::PackedByteArray channel;
    channel.resize(pixels.size() / 4);
    int component = kind == "roughness_g" ? 1 : kind == "metallic_b" ? 2 : 0;
    for (int64_t i = 0; i < channel.size(); ++i) channel.ptrw()[i] = pixels.ptr()[i * 4 + component];
    image = Image::create_from_data(image->get_width(), image->get_height(), false, Image::FORMAT_R8, channel);
    if (image->generate_mipmaps() != godot::OK || image->compress_from_channels(Image::COMPRESS_S3TC, Image::USED_CHANNELS_R) != godot::OK)
      throw std::runtime_error("BC4 scalar compression is unavailable");
    if (int64_t(image->call("save_dds", godot::String(target.string().c_str()))) != godot::OK) throw std::runtime_error("Could not write DDS texture");
    return;
  }
  image->convert(normal ? Image::FORMAT_RGBA8 : Image::FORMAT_RGBAH);
  if (!normal && !hdr && kind == "color") image->srgb_to_linear();
  if (image->generate_mipmaps(normal) != godot::OK) throw std::runtime_error("Cannot generate texture mipmaps");
  if (normal) {
    auto pixels = image->get_data();
    godot::PackedByteArray encoded;
    encoded.resize(pixels.size() / 2);
    auto* output = encoded.ptrw();
    const auto* input = pixels.ptr();
    for (int64_t i = 0; i < pixels.size() / 4; ++i) {
      float x = input[i * 4] / 127.5f - 1.f;
      float y = input[i * 4 + 1] / 127.5f - 1.f;
      float z = std::abs(input[i * 4 + 2] / 127.5f - 1.f);
      if (kind == "normal_ogl") y = -y;
      float denominator = std::max(std::abs(x) + std::abs(y) + z, 0.00001f);
      output[i * 2] = uint8_t(std::clamp(std::round(((x + y) / denominator * 0.5f + 0.5f) * 255.f), 0.f, 255.f));
      output[i * 2 + 1] = uint8_t(std::clamp(std::round(((x - y) / denominator * 0.5f + 0.5f) * 255.f), 0.f, 255.f));
    }
    image = Image::create_from_data(image->get_width(), image->get_height(), image->has_mipmaps(), Image::FORMAT_RG8, encoded);
    if (image->compress_from_channels(Image::COMPRESS_S3TC, Image::USED_CHANNELS_RG) != godot::OK)
      throw std::runtime_error("BC5 normal compression is unavailable");
  } else {
    if (!hdr) {
      if (kind == "color") image->linear_to_srgb();
      image->convert(Image::FORMAT_RGBA8);
    }
    if (image->compress(Image::COMPRESS_BPTC) != godot::OK) throw std::runtime_error("BC7/BC6H texture compression is unavailable");
  }
  // save_dds was added after the minimum GDExtension ABI used for this library.
  if (int64_t(image->call("save_dds", godot::String(target.string().c_str()))) != godot::OK)
    throw std::runtime_error("Could not write DDS texture");
}

class RemixDocument : public godot::RefCounted {
  GDCLASS(RemixDocument, godot::RefCounted)
public:
  ~RemixDocument() { cancel_ingestion(); cancel_asset_preview(); if (job_.valid()) job_.wait(); if (preview_job_.valid()) preview_job_.wait(); }
  godot::String execute(const godot::String& json) {
    try {
      auto result = document_.dispatch(Json::parse(json.utf8().get_data()));
      return Json({{"ok", true}, {"result", text_buffers(result)}}).dump().c_str();
    } catch (const std::exception& error) {
      return Json({{"ok", false}, {"error", error.what()}}).dump().c_str();
    }
  }
  godot::Dictionary execute_data(const godot::String& json) {
    godot::Dictionary response;
    try {
      // Convert directly to Godot containers. Serializing this result to JSON
      // first exceeds Godot's signed 32-bit String/JSON limits on big captures,
      // and holds extra copies of the entire scene in memory.
      auto result = document_.dispatch(Json::parse(json.utf8().get_data()));
      response["result"] = variant(result);
      response["ok"] = true;
    } catch (const std::exception& error) {
      response.clear();
      response["ok"] = false;
      response["error"] = godot::String::utf8(error.what());
    }
    return response;
  }
  godot::String start_ingestion(const godot::String& json) {
    try {
      if (job_.valid()) throw std::runtime_error("An ingestion job is already running");
      auto request = Json::parse(json.utf8().get_data());
      auto description = document_.describe();
      if (!description.value("open", false)) throw std::runtime_error("Open a project first");
      auto directory = fs::path(description.at("path").get<std::string>()).parent_path();
      canceled_ = std::make_shared<std::atomic_bool>(false);
      job_ = std::async(std::launch::async, [request, directory, canceled = canceled_] {
        try { return Json{{"ok", true}, {"result", ingest_asset(request, directory, convert_texture, [canceled] { return canceled->load(); })}}; }
        catch (const std::exception& error) { return Json{{"ok", false}, {"error", error.what()}}; }
      });
      return "{\"ok\":true}";
    } catch (const std::exception& error) { return Json({{"ok", false}, {"error", error.what()}}).dump().c_str(); }
  }
  godot::String poll_ingestion() {
    if (!job_.valid() || job_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return "";
    return job_.get().dump().c_str();
  }
  void cancel_ingestion() { if (canceled_) canceled_->store(true); }
  godot::String start_asset_preview(const godot::String& path) {
    try {
      if (preview_job_.valid()) throw std::runtime_error("A preview job is already running");
      preview_canceled_ = std::make_shared<std::atomic_bool>(false);
      const std::string file(path.utf8().get_data());
      preview_job_ = std::async(std::launch::async, [file, canceled = preview_canceled_] {
        try {
          if (canceled->load()) return Json{{"ok", false}, {"error", "Preview canceled"}};
          auto result = preview_asset(file);
          if (canceled->load()) return Json{{"ok", false}, {"error", "Preview canceled"}};
          return Json{{"ok", true}, {"result", result}};
        } catch (const std::exception& error) { return Json{{"ok", false}, {"error", error.what()}}; }
      });
      return "{\"ok\":true}";
    } catch (const std::exception& error) { return Json{{"ok", false}, {"error", error.what()}}.dump().c_str(); }
  }
  godot::Dictionary poll_asset_preview() {
    if (!preview_job_.valid() || preview_job_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return {};
    auto response = preview_job_.get();
    try { return variant(response); }
    catch (const std::exception& error) { return variant(Json{{"ok", false}, {"error", error.what()}}); }
  }
  void cancel_asset_preview() { if (preview_canceled_) preview_canceled_->store(true); }
  godot::String asset_file_stamp(const godot::String& path) {
    try {
      const fs::path file(path.utf8().get_data());
      if (!fs::is_regular_file(file)) return "";
      return (std::to_string(fs::file_size(file)) + ":" +
        std::to_string(fs::last_write_time(file).time_since_epoch().count())).c_str();
    } catch (const fs::filesystem_error&) { return ""; }
  }
protected:
  static void _bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("execute", "request"), &RemixDocument::execute);
    godot::ClassDB::bind_method(godot::D_METHOD("execute_data", "request"), &RemixDocument::execute_data);
    godot::ClassDB::bind_method(godot::D_METHOD("start_ingestion", "request"), &RemixDocument::start_ingestion);
    godot::ClassDB::bind_method(godot::D_METHOD("poll_ingestion"), &RemixDocument::poll_ingestion);
    godot::ClassDB::bind_method(godot::D_METHOD("cancel_ingestion"), &RemixDocument::cancel_ingestion);
    godot::ClassDB::bind_method(godot::D_METHOD("start_asset_preview", "path"), &RemixDocument::start_asset_preview);
    godot::ClassDB::bind_method(godot::D_METHOD("poll_asset_preview"), &RemixDocument::poll_asset_preview);
    godot::ClassDB::bind_method(godot::D_METHOD("cancel_asset_preview"), &RemixDocument::cancel_asset_preview);
    godot::ClassDB::bind_method(godot::D_METHOD("asset_file_stamp", "path"), &RemixDocument::asset_file_stamp);
  }
private:
  Document document_;
  std::future<Json> job_;
  std::shared_ptr<std::atomic_bool> canceled_;
  std::future<Json> preview_job_;
  std::shared_ptr<std::atomic_bool> preview_canceled_;
};

void initialize(godot::ModuleInitializationLevel level) {
  if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
    godot::ClassDB::register_class<RemixDocument>();
    register_gpu_bridge();
  }
}
void terminate(godot::ModuleInitializationLevel) {}
}

extern "C" GDExtensionBool GDE_EXPORT remix_editor_init(GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization) {
  godot::GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
  init.register_initializer(editor::initialize);
  init.register_terminator(editor::terminate);
  init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
  return init.init();
}
