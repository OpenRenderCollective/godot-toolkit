#include "channel.h"
#include "shared_frame.h"
#include "render_settings.h"
#include "openremix/remix_editor_ext.h"
#include "openremix/remix_native_ext.h"
#include "openremix/remix_reconstruction_ext.h"
#include "alpha_state.h"
#include "render_categories.h"
#include "scene_buffers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <remix/remix_c.h>

using Json = nlohmann::json;
static_assert(sizeof(EditorSharedFrame) == sizeof(remixapi_EditorSharedFrame));
#if defined(_WIN32) && defined(REMIX_PLUS_API)
#ifndef REMIXAPI_EXTERNAL_FRAME_VERSION
// Remix Plus external frame export, for headers that do not declare it yet.
#define REMIXAPI_EXTERNAL_FRAME_VERSION 1u
#define REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT ((remixapi_StructType)0x4E560003)
#define REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE ((remixapi_ErrorCode)0x4E560001)
typedef struct remixapi_ExternalFrameEXT {
  remixapi_StructType sType; void* pNext;
  uint32_t version;
  uint32_t width, height;
  uint32_t format;
  uint32_t memoryTypeIndex;
  uint64_t allocationSize;
  uint8_t deviceUUID[16];
  void* memoryHandle;
  void* readySemaphore;
  void* releasedSemaphore;
} remixapi_ExternalFrameEXT;
#endif
using ConfigureExternalFrame = remixapi_ErrorCode(REMIXAPI_CALL*)(remixapi_ExternalFrameEXT*);
#endif
namespace {
void checked(remixapi_ErrorCode code, const char* operation) {
  if (code != REMIXAPI_ERROR_CODE_SUCCESS) throw std::runtime_error(std::string(operation) + " failed (Remix code " + std::to_string(code) + ")");
}
uint64_t hash64(const std::string& value) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : value) { h ^= c; h *= 1099511628211ull; }
  return h ? h : 1;
}
template<typename Bytes> uint64_t hash_bytes(uint64_t h, const Bytes& bytes) {
  for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
  return h;
}
// Describes every binary value of a message, so transports can be verified without a GPU.
void describe_buffers(const Json& value, Json& result) {
  if (value.is_binary()) {
    const auto& bytes = value.get_binary();
    result.push_back({{"type", editor::buffer_type_name(bytes.has_subtype() ? bytes.subtype() : 0)}, {"size", bytes.size()}, {"hash", hash_bytes(14695981039346656037ull, bytes)}});
  } else if (value.is_structured()) for (const auto& item : value) describe_buffers(item, result);
}
remixapi_Float3D vector3(const Json& v) { return {v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()}; }
remixapi_InstanceInfoBlendEXT blend_state(const Json& values) {
  auto data = editor::capture_alpha_defaults(); data.update(values);
  remixapi_InstanceInfoBlendEXT state{}; state.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  state.alphaTestEnabled = data.at("alphaTestEnabled").get<bool>();
  state.alphaTestReferenceValue = data.at("alphaTestReferenceValue").get<uint8_t>();
  state.alphaTestCompareOp = state.alphaTestEnabled ? data.at("alphaTestCompareOp").get<uint32_t>() : 7u;
  state.alphaBlendEnabled = data.at("alphaBlendEnabled").get<bool>();
  state.srcColorBlendFactor = data.at("srcColorBlendFactor"); state.dstColorBlendFactor = data.at("dstColorBlendFactor");
  state.colorBlendOp = data.at("colorBlendOp");
  state.srcAlphaBlendFactor = data.at("srcAlphaBlendFactor"); state.dstAlphaBlendFactor = data.at("dstAlphaBlendFactor");
  state.alphaBlendOp = data.at("alphaBlendOp"); state.writeMask = data.at("writeMask");
  state.textureColorArg1Source = data.at("textureColorArg1Source"); state.textureColorArg2Source = data.at("textureColorArg2Source");
  state.textureColorOperation = data.at("textureColorOperation");
  state.textureAlphaArg1Source = data.at("textureAlphaArg1Source"); state.textureAlphaArg2Source = data.at("textureAlphaArg2Source");
  state.textureAlphaOperation = data.at("textureAlphaOperation"); state.tFactor = data.at("tFactor");
  state.isTextureFactorBlend = data.at("isTextureFactorBlend").get<bool>();
  state.isVertexColorBakedLighting = data.at("isVertexColorBakedLighting").get<bool>();
  return state;
}
remixapi_Transform transform(const Json& values) {
  if (values.size() != 16) throw std::runtime_error("Expected 16 transform elements");
  remixapi_Transform result{};
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) result.matrix[r][c] = values.at(c * 4 + r).get<float>();
  return result;
}
remixapi_Transform transformed(const remixapi_Transform& delta, const remixapi_Transform& original) {
  remixapi_Transform result{};
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) {
    for (int k = 0; k < 3; ++k) result.matrix[r][c] += delta.matrix[r][k] * original.matrix[k][c];
    if (c == 3) result.matrix[r][c] += delta.matrix[r][3];
  }
  return result;
}
#ifdef _WIN32
using NativePath = std::wstring;
NativePath native_path(const std::string& value) { return std::filesystem::u8path(value).wstring(); }
LRESULT CALLBACK preview_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  // The swapchain follows the client area, so it may exceed the desktop size.
  if (message == WM_GETMINMAXINFO) { reinterpret_cast<MINMAXINFO*>(lparam)->ptMaxTrackSize = {16384, 16384}; return 0; }
  return DefWindowProcW(window, message, wparam, lparam);
}
#else
using NativePath = std::string;
NativePath native_path(const std::string& value) { return value; }
#endif

class Renderer {
public:
  ~Renderer() {
    if (running_) { clear(); api_.Shutdown(); }
#ifdef _WIN32
    // The runtime keeps the window procedure it installed after Shutdown, so
    // destroying the window or unloading the runtime calls into freed state.
    // Process exit releases both.
    if (!running_) {
      if (window_) DestroyWindow(window_);
      if (library_) FreeLibrary(reinterpret_cast<HMODULE>(library_));
    }
#else
    if (library_) dlclose(library_);
#endif
  }
  Json start(const Json& message) {
    if (running_) throw std::runtime_error("Renderer already started");
    const auto renderer = message.value("renderer", std::string("remix"));
    if (renderer != "remix" && renderer != "openremix") throw std::runtime_error("Unknown renderer");
    openremix_ = renderer == "openremix";
    auto path = native_path(message.at("library").get<std::string>());
    checked(remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &api_, &library_), "Load runtime");
    remixapi_StartupInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    width_ = message.value("width", 1280u); height_ = message.value("height", 720u);
    if (message.contains("render_settings")) {
      const auto preferences = studio::RenderSettings::parse(message.at("render_settings"), openremix_);
      width_ = preferences.width; height_ = preferences.height;
    }
    if (width_ < 16 || height_ < 16 || width_ > 8192 || height_ > 8192) throw std::runtime_error("Invalid viewport size");
#ifdef _WIN32
    // An exported frame replaces the preview window, which stays hidden but keeps its swapchain.
    const bool export_frame = message.contains("frame_process");
    const wchar_t* class_name = L"GodotRemixRenderer";
    WNDCLASSW window_class{}; window_class.lpfnWndProc = preview_window_procedure;
    window_class.hInstance = GetModuleHandleW(nullptr); window_class.lpszClassName = class_name;
    RegisterClassW(&window_class);
    RECT bounds{0, 0, LONG(width_), LONG(height_)};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    window_ = CreateWindowExW(0, class_name, L"Remix render preview", WS_OVERLAPPEDWINDOW | (export_frame ? 0 : WS_VISIBLE),
      CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window_) throw std::runtime_error("Could not create renderer window");
    info.hwnd = reinterpret_cast<remixapi_HWND>(window_);
#else
    info.windowTitle = "Remix render preview"; info.windowWidth = width_; info.windowHeight = height_;
#endif
    remixapi_EditorStartupEXT editor{REMIXAPI_STRUCT_TYPE_EDITOR_STARTUP_EXT, nullptr, 1, true, true, true};
    if (openremix_) {
      if (!symbol("remixapi_EditorConfigureSharedFrame")) throw std::runtime_error("OpenRemix requires the editor integration build");
#ifdef _WIN32
      editor.headless = export_frame;
#else
      editor.headless = message.contains("frame_channel");
#endif
      info.pNext = &editor;
    }
    info.editorModeEnabled = true;
    info.combineGuiInFinalColor = false;
    checked(api_.Startup(&info), "Startup");
    running_ = true;
#ifdef REMIX_PLUS_API
    // The Remix Plus API has no feature query; its runtime resolves unsupported upscalers itself.
    dlss_ = ray_reconstruction_ = !openremix_;
    if (!openremix_) {
      retained_api_.create = reinterpret_cast<PFN_remixapi_CreateRetainedInstances>(symbol("remixapi_CreateRetainedInstances"));
      retained_api_.update = reinterpret_cast<PFN_remixapi_UpdateRetainedInstances>(symbol("remixapi_UpdateRetainedInstances"));
      retained_api_.destroy = reinterpret_cast<PFN_remixapi_DestroyRetainedInstances>(symbol("remixapi_DestroyRetainedInstances"));
      retained_api_.draw = reinterpret_cast<PFN_remixapi_DrawRetainedInstances>(symbol("remixapi_DrawRetainedInstances"));
    }
#else
    remixapi_FeatureAvailability features{};
    if (!openremix_ && api_.GetFeatureAvailability && api_.GetFeatureAvailability(&features) == REMIXAPI_ERROR_CODE_SUCCESS) {
      dlss_ = (features.availableFeatures & REMIXAPI_FEATURE_DLSS_SUPER_RESOLUTION_BIT) != 0;
      ray_reconstruction_ = dlss_ && (features.availableFeatures & REMIXAPI_FEATURE_DLSS_RAY_RECONSTRUCTION_BIT) != 0;
    }
#endif
    if (openremix_) {
      auto backend = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(symbol("remixapi_GetBackendCapabilitiesEXT"));
      auto reconstruction = reinterpret_cast<remixapi_ErrorCode (*)(remixapi_ReconstructionDiagnosticsEXT*)>(symbol("remixapi_GetReconstructionDiagnosticsEXT"));
      auto denoising = reinterpret_cast<PFN_remixapi_GetDenoisingCapabilitiesEXT>(symbol("remixapi_GetDenoisingCapabilitiesEXT"));
      query_backend_ = backend; query_reconstruction_ = reconstruction;
      query_denoising_ = reinterpret_cast<PFN_remixapi_GetDenoisingStatisticsEXT>(symbol("remixapi_GetDenoisingStatisticsEXT"));
      query_sharc_ = reinterpret_cast<PFN_remixapi_GetSharcStatisticsEXT>(symbol("remixapi_GetSharcStatisticsEXT"));
      query_shadows_ = reinterpret_cast<PFN_remixapi_GetShadowStatisticsEXT>(symbol("remixapi_GetShadowStatisticsEXT"));
      query_local_lights_ = reinterpret_cast<PFN_remixapi_GetLocalLightStatisticsEXT>(symbol("remixapi_GetLocalLightStatisticsEXT"));
      if (!backend || !reconstruction || !denoising) throw std::runtime_error("OpenRemix capability extensions are unavailable");
      remixapi_BackendCapabilitiesEXT caps{}; caps.structSize = sizeof(caps);
      checked(backend(&caps), "Query OpenRemix backends"); pathtrace_ = caps.supportedBackends & 2;
      pathtrace_reason_ = caps.unavailableReason;
      remixapi_ReconstructionDiagnosticsEXT up{}; up.structSize = sizeof(up);
      checked(reconstruction(&up), "Query OpenRemix reconstruction"); dlss_ = up.capabilities & 2; ray_reconstruction_ = up.capabilities & 4;
      reconstruction_reason_ = up.fallbackReason;
      // OpenRemix initializes DLSS when a setting first requests it and reports that as its
      // reason until then. Offer it; the per-frame diagnostics say what actually runs.
      if (!dlss_ && reconstruction_reason_ == "DLSS will initialize when requested") dlss_ = ray_reconstruction_ = true;
      remixapi_DenoisingCapabilitiesEXT denoise{}; denoise.structSize = sizeof(denoise);
      checked(denoising(&denoise), "Query OpenRemix temporal support"); fsr_ = pathtrace_ && denoise.taaSupported;
      nrd_ = denoise.reblurShSupported; taa_ = denoise.taaSupported;
      nis_ = false;
    }
#ifndef _WIN32
    if (message.contains("frame_channel")) {
      auto configure = reinterpret_cast<ConfigureSharedFrame>(dlsym(library_, "remixapi_EditorConfigureSharedFrame"));
      if (!configure) throw std::runtime_error("Runtime lacks the editor shared-frame patch");
      EditorSharedFrame frame;
      frame.width = width_; frame.height = height_;
      struct Fds { EditorSharedFrame& f; ~Fds() { for (int fd : {f.memory_fd, f.ready_fd, f.released_fd}) if (fd >= 0) close(fd); } } fds{frame};
      int result = configure(&frame);
      if (result) throw std::runtime_error("Could not configure shared GPU output (" + std::to_string(result) + ")");
      auto endpoint = message.at("frame_channel").get<std::string>();
      sockaddr_un address{}; address.sun_family = AF_UNIX;
      if (endpoint.size() >= sizeof(address.sun_path)) throw std::runtime_error("GPU frame channel path is too long");
      std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
      int connection = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (connection < 0) throw std::runtime_error("Could not create the GPU frame channel");
      struct Socket { int fd; ~Socket() { close(fd); } } socket_owner{connection};
      if (connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) throw std::runtime_error("Could not connect the GPU frame channel");
      iovec payload{&frame, sizeof(frame)};
      alignas(cmsghdr) char control[CMSG_SPACE(3 * sizeof(int))]{};
      msghdr message{}; message.msg_iov = &payload; message.msg_iovlen = 1;
      message.msg_control = control; message.msg_controllen = sizeof(control);
      auto* header = CMSG_FIRSTHDR(&message);
      header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(3 * sizeof(int));
      int descriptors[]{frame.memory_fd, frame.ready_fd, frame.released_fd};
      std::memcpy(CMSG_DATA(header), descriptors, sizeof(descriptors));
      if (sendmsg(connection, &message, MSG_NOSIGNAL) != sizeof(frame)) throw std::runtime_error("Could not send GPU frame handles");
      shared_ = true;
    }
#else
    Json exported, export_error;
    if (export_frame) {
      try { exported = export_frame_to(message.at("frame_process").get<uint32_t>()); shared_ = true; }
      catch (const std::exception& error) {
        // OpenRemix runs without a swapchain here, so there is no window to fall back to.
        if (openremix_) throw;
        export_error = error.what(); ShowWindow(window_, SW_SHOWNORMAL);
      }
    }
#endif
    if (!openremix_) {
    checked(api_.SetConfigVariable("rtx.fallbackLightMode", "0"), "Disable fallback lighting");
    // The pinned native fork's RTXDI path gives black analytical lighting in the
    // embedded viewport. Its RIS path is covered by our rendered-image test.
    checked(api_.SetConfigVariable("rtx.useRTXDI", "False"), "Select preview light sampling");
    // Preserve the automatic preset's settings, but let the editor override
    // them. Otherwise the Quality layer outranks SetConfigVariable's User
    // layer (the native Low preset disables stochastic alpha blending).
    checked(api_.SetConfigVariable("rtx.graphicsPreset", "4"), "Select custom viewport settings");
    }
    api_.SetUIState(REMIXAPI_UI_STATE_NONE);
    camera_ = {{"position", {0, 2, 5}}, {"forward", {0, 0, -1}}, {"up", {0, 1, 0}}, {"right", {1, 0, 0}}, {"fov", 60.0}};
    Json result{{"status", "ready"}, {"presentation", shared_ ? "shared_vulkan" : "native_window"}, {"width", width_}, {"height", height_},
      {"capabilities", {{"dlss", dlss_}, {"ray_reconstruction", ray_reconstruction_}, { "nis", nis_}, {"fsr31", fsr_}, {"pathtrace", pathtrace_}, {"pathtrace_unavailable", pathtrace_reason_}, {"reconstruction_unavailable", reconstruction_reason_}, {"nrd", nrd_}, {"taa", taa_}, {"renderer", renderer}}}};
#ifdef _WIN32
    if (shared_) result["frame"] = exported;
    if (!export_error.is_null()) result["frame_error"] = export_error;
#endif
    if (message.contains("render_settings")) result["render_settings"] = render_settings(message.at("render_settings"));
    return result;
  }
#ifdef _WIN32
  // The runtime's output image and semaphores, as NT handles this process owns.
  struct RuntimeFrame {
    uint32_t width = 0, height = 0, format = 0, memory_type = 0;
    uint64_t allocation_size = 0;
    uint8_t device_uuid[16]{};
    HANDLE handles[3]{};
    std::function<void()> release;
    RuntimeFrame() = default;
    RuntimeFrame(const RuntimeFrame&) = delete;
    ~RuntimeFrame() { for (auto handle : handles) if (handle) CloseHandle(handle); }
  };
  void request_frame(RuntimeFrame& out) {
    constexpr uint32_t rgba8_unorm = 37;
    if (openremix_) {
      // OpenRemix always exports RGBA8 at the requested size.
      auto configure = reinterpret_cast<ConfigureSharedFrame>(symbol("remixapi_EditorConfigureSharedFrame"));
      if (!configure) throw std::runtime_error("OpenRemix requires the editor integration build");
      EditorSharedFrame frame;
      frame.width = width_; frame.height = height_;
      const int code = configure(&frame);
      out.handles[0] = frame.memory_handle; out.handles[1] = frame.ready_handle; out.handles[2] = frame.released_handle;
      if (code) throw std::runtime_error("Could not configure shared GPU output (" + std::to_string(code) + ")");
      out.width = frame.width; out.height = frame.height; out.format = rgba8_unorm;
      out.memory_type = frame.memory_type; out.allocation_size = frame.allocation_size;
      std::memcpy(out.device_uuid, frame.device_uuid, sizeof(out.device_uuid));
      return;
    }
#ifdef REMIX_PLUS_API
    auto configure = reinterpret_cast<ConfigureExternalFrame>(symbol("remixapi_ConfigureExternalFrame"));
    if (!configure) throw std::runtime_error("The runtime does not export editor frames");
    remixapi_ExternalFrameEXT frame{};
    frame.sType = REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT; frame.version = REMIXAPI_EXTERNAL_FRAME_VERSION;
    auto code = configure(&frame);
    out.handles[0] = frame.memoryHandle; out.handles[1] = frame.readySemaphore; out.handles[2] = frame.releasedSemaphore;
    if (code == REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE)
      throw std::runtime_error("The runtime cannot export editor frames on this GPU or with this output format");
    if (code != REMIXAPI_ERROR_CODE_SUCCESS) throw std::runtime_error("The runtime could not export editor frames (Remix code " + std::to_string(code) + ")");
    out.release = [configure] { configure(nullptr); };
    out.width = frame.width; out.height = frame.height; out.format = frame.format;
    out.memory_type = frame.memoryTypeIndex; out.allocation_size = frame.allocationSize;
    std::memcpy(out.device_uuid, frame.deviceUUID, sizeof(out.device_uuid));
#else
    throw std::runtime_error("This renderer build cannot export frames to the editor");
#endif
  }
  // Exports the runtime's output image and semaphores into the editor process.
  // The editor owns the duplicated NT handles; the worker closes its own.
  Json export_frame_to(DWORD process_id) {
    RuntimeFrame frame;
    request_frame(frame);
    try {
      if (!frame.handles[0] || !frame.handles[1] || !frame.handles[2]) throw std::runtime_error("The runtime returned no frame handles");
      constexpr uint32_t rgba8_unorm = 37;
      if (frame.format != rgba8_unorm) throw std::runtime_error("The runtime exported an unsupported frame format");
      if (frame.width != width_ || frame.height != height_)
        throw std::runtime_error("The runtime exported a " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + " frame instead of " +
          std::to_string(width_) + "x" + std::to_string(height_));
      HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, process_id);
      if (!process) throw std::runtime_error("Could not open the editor process (error " + std::to_string(GetLastError()) + ")");
      struct Process { HANDLE h; ~Process() { CloseHandle(h); } } process_owner{process};
      HANDLE shared[3]{};
      for (int i = 0; i < 3; ++i) if (!DuplicateHandle(GetCurrentProcess(), frame.handles[i], process, &shared[i], 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        auto error = GetLastError();
        for (int j = 0; j < i; ++j) DuplicateHandle(process, shared[j], nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
        throw std::runtime_error("Could not share frame handles with the editor (error " + std::to_string(error) + ")");
      }
      static const char digits[] = "0123456789abcdef";
      std::string uuid;
      for (auto byte : frame.device_uuid) { uuid += digits[byte >> 4]; uuid += digits[byte & 15]; }
      return {{"width", frame.width}, {"height", frame.height}, {"memory_type", frame.memory_type}, {"allocation_size", frame.allocation_size},
        {"device_uuid", uuid}, {"memory", uint64_t(uintptr_t(shared[0]))}, {"ready", uint64_t(uintptr_t(shared[1]))}, {"released", uint64_t(uintptr_t(shared[2]))}};
    } catch (...) { if (frame.release) frame.release(); throw; }
  }
#endif
  void* symbol(const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(library_, name));
#else
    return dlsym(library_, name);
#endif
  }
  void clear() {
#ifdef REMIX_PLUS_API
    // Retained instances must go before the meshes they draw.
    if (!retained_.empty()) retained_api_.destroy(retained_.data(), uint32_t(retained_.size()));
    retained_.clear();
#endif
    for (auto mesh : meshes_) api_.DestroyMesh(mesh);
    for (auto light : lights_) api_.DestroyLight(light);
    for (const auto& material : materials_) api_.DestroyMaterial(material.second);
    meshes_.clear(); lights_.clear(); materials_.clear(); instances_.clear(); blend_states_.clear();
    mesh_cache_.clear(); instance_origins_.clear(); light_origins_.clear(); previewed_lights_.clear(); moving_instances_.clear();
  }
  void scene(const Json& scene) {
    require_running();
    const auto meters_per_unit = scene.value("meters_per_unit", 0.01);
    if (!std::isfinite(meters_per_unit) || meters_per_unit <= 0) throw std::runtime_error("Invalid USD scene units");
    auto scale = std::to_string(0.01 / meters_per_unit);
    if (!openremix_) checked(api_.SetConfigVariable("rtx.sceneScale", scale.c_str()), "Set USD scene scale");
    // Light handles are the light hashes, and Remix applies DestroyLight only at
    // the next Present. Destroying a light that this scene creates again would
    // remove the new light, so only lights missing from the new scene go.
    auto previous_lights = std::move(lights_);
    lights_.clear(); light_origins_.clear();
    auto destroy_replaced = [&] {
      for (auto light : previous_lights)
        if (std::find(lights_.begin(), lights_.end(), light) == lights_.end()) api_.DestroyLight(light);
    };
    clear();
    try {
      Json fallback{{"id", "default"}, {"albedo", {0.7, 0.7, 0.7}}};
      material(fallback);
      for (auto& m : scene.at("materials")) material(m);
      for (auto& m : scene.at("meshes")) mesh(m, scene.at("prototypes"));
      for (auto& light : scene.at("lights")) add_light(light);
      destroy_replaced();
      revision_ = scene.at("revision").get<uint64_t>();
      geometry_revision_ = scene.value("geometry_revision", revision_);
      retain_instances();
    } catch (...) { destroy_replaced(); clear(); throw; }
  }
  // Registers the scene's draws once, so frames replay them with one call.
  void retain_instances() {
#ifdef REMIX_PLUS_API
    if (!retained_api_ || instances_.empty()) return;
    std::vector<remixapi_InstanceInfo> infos(instances_);
    for (size_t i = 0; i < infos.size(); ++i) infos[i].pNext = &blend_states_[i];
    std::vector<uint64_t> handles(infos.size());
    auto code = retained_api_.create(infos.data(), uint32_t(infos.size()), handles.data());
    if (code == REMIXAPI_ERROR_CODE_SUCCESS) retained_ = std::move(handles);
    else std::cerr << "Retained instances unavailable (Remix code " << code << "); drawing per frame\n";
#endif
  }
  void camera(const Json& data) { camera_ = data; }
  Json update_lights(const Json& request) {
    require_running();
    const auto revision = request.at("revision").get<uint64_t>();
    if (request.at("geometry_revision").get<uint64_t>() != geometry_revision_ || revision < revision_)
      return {{"revision", revision_}, {"updated", 0}};
    std::map<std::string, size_t> previous;
    for (size_t i = 0; i < light_origins_.size(); ++i) previous.emplace(light_origins_[i].at("id").get<std::string>(), i);
    std::vector<remixapi_LightHandle> next;
    std::vector<Json> origins;
    size_t updated = 0, removed = 0;
    for (const auto& data : request.at("lights")) {
      const auto id = data.at("id").get<std::string>();
      const auto found = previous.find(id);
      remixapi_LightHandle handle{};
      if (found != previous.end()) {
        const auto index = found->second;
        handle = lights_[index];
        if (data.at("type") != light_origins_[index].at("type")) {
          checked(api_.DestroyLight(handle), "DestroyLight"); handle = {};
        }
        if (data != light_origins_[index] || previewed_lights_.count(id)) {
          handle = create_light(data, transform(data.at("transform"))); ++updated;
        }
        previous.erase(found);
      } else {
        handle = create_light(data, transform(data.at("transform"))); ++updated;
      }
      if (handle) { next.push_back(handle); origins.push_back(data); }
    }
    for (const auto& item : previous) { checked(api_.DestroyLight(lights_[item.second]), "DestroyLight"); ++removed; }
    lights_ = std::move(next); light_origins_ = std::move(origins); previewed_lights_.clear();
    revision_ = revision;
    return {{"revision", revision_}, {"updated", updated}, {"removed", removed}};
  }
  Json preview_transform(const Json& request) {
    require_running();
    // A queued drag from the previous scene must never affect a newer snapshot.
    if (request.at("revision").get<uint64_t>() != revision_) return {{"updated", 0}};
    const auto path = request.at("path").get<std::string>();
    if (path.empty() || path.front() != '/') throw std::runtime_error("Invalid preview path");
    const auto delta = transform(request.at("delta"));
    for (const auto& row : delta.matrix) for (float value : row)
      if (!std::isfinite(value)) throw std::runtime_error("Non-finite preview transform");
    auto matches = [&](const std::string& id) { return id == path || id.rfind(path + "/", 0) == 0; };
    size_t updated = 0;
#ifdef REMIX_PLUS_API
    std::vector<remixapi_RetainedInstanceUpdate> retained;
#endif
    for (size_t i = 0; i < instances_.size(); ++i) if (matches(instance_origins_[i].first)) {
      instances_[i].transform = transformed(delta, instance_origins_[i].second);
      bool moving = false;
      for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 4; ++col)
        moving |= instances_[i].transform.matrix[row][col] != instance_origins_[i].second.matrix[row][col];
      if (moving) moving_instances_.insert(i); else moving_instances_.erase(i);
#ifdef REMIX_PLUS_API
      if (!retained_.empty()) retained.push_back({retained_[i], instances_[i].transform, REMIXAPI_RETAINED_UPDATE_TRANSFORM, true});
#endif
      ++updated;
    }
#ifdef REMIX_PLUS_API
    if (!retained.empty()) checked(retained_api_.update(retained.data(), uint32_t(retained.size())), "UpdateRetainedInstances");
#endif
    for (size_t i = 0; i < lights_.size(); ++i) if (matches(light_origins_[i].at("id").get<std::string>())) {
      // Remix keys lights by hash: creating the same ID updates its definition.
      // Always transform the baseline, so preview deltas never accumulate.
      lights_[i] = create_light(light_origins_[i], transformed(delta, transform(light_origins_[i].at("transform"))));
      previewed_lights_.insert(light_origins_[i].at("id").get<std::string>());
      ++updated;
    }
    return {{"updated", updated}};
  }
  void settings(const Json& data) {
    require_running();
    for (auto it = data.begin(); it != data.end(); ++it) {
      if (openremix_ && it.key().compare(0, 11, "rtx.native.") != 0) throw std::runtime_error("OpenRemix accepts only native renderer settings");
      checked(api_.SetConfigVariable(it.key().c_str(), it.value().get<std::string>().c_str()), ("Set render setting " + it.key()).c_str());
    }
  }
  Json render_settings(const Json& data) {
    require_running();
    const auto requested = studio::RenderSettings::parse(data, openremix_);
    const auto effective = openremix_ ? requested.effective_openremix(pathtrace_, dlss_, ray_reconstruction_, fsr_) : requested.effective(dlss_, ray_reconstruction_, nis_);
    if (effective.width != width_ || effective.height != height_) throw std::runtime_error("Changing viewport resolution requires restarting the renderer");
    render_variables_ = openremix_ ? effective.openremix_variables() : effective.variables();
    settings(render_variables_);
    auto result = effective.json(openremix_);
    requested_settings_ = requested;
    effective_settings_ = result;
    return result;
  }
  Json frame() {
    require_running();
#ifdef _WIN32
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (!IsWindow(window_)) return {{"status", "closed"}};
#else
    if (!(openremix_ && shared_)) {
    checked(api_.PumpEvents(), "PumpEvents");
    remixapi_WindowState state{};
    checked(api_.GetWindowState(&state), "GetWindowState");
    if (state.closeRequested) return {{"status", "closed"}};
    if (!shared_) { width_ = state.drawableWidth; height_ = state.drawableHeight; }
    }
#endif
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.position = vector3(camera_.at("position")); parameters.forward = vector3(camera_.at("forward"));
    parameters.up = vector3(camera_.at("up")); parameters.right = vector3(camera_.at("right"));
    parameters.fovYInDegrees = camera_.value("fov", 60.f);
    parameters.nearPlane = camera_.value("near", 0.01f); parameters.farPlane = camera_.value("far", 100000.f);
    parameters.aspect = float(width_) / float(std::max(height_, 1u));
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO; camera.pNext = &parameters; camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    checked(api_.SetupCamera(&camera), "SetupCamera");
#ifdef REMIX_PLUS_API
    const bool replay = !retained_.empty();
    if (replay) checked(retained_api_.draw(nullptr), "DrawRetainedInstances");
#else
    const bool replay = false;
#endif
    for (size_t i = 0; !replay && i < instances_.size(); ++i) {
      // Attach only for the call: vector growth and transform previews must not
      // leave stored pNext pointers referring to moved or expired extensions.
      auto instance = instances_[i]; instance.pNext = &blend_states_[i];
      remixapi_InstanceIdentityEXT identity{};
      if (openremix_) {
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT; identity.pNext = instance.pNext;
        identity.instanceId = hash64(instance_origins_[i].first + ":" + std::to_string(instance.categoryFlags));
        // The toolkit submits frozen USD snapshots. Static receivers allow
        // SHARC reuse; only an active transform preview needs dynamic storage.
        identity.classification = moving_instances_.count(i) ? REMIXAPI_INSTANCE_DYNAMIC_EXT : REMIXAPI_INSTANCE_STATIC_EXT;
        identity.rasterVisible = true;
        instance.pNext = &identity;
      }
      checked(api_.DrawInstance(&instance), "DrawInstance");
    }
    for (auto light : lights_) checked(api_.DrawLightInstance(light), "DrawLightInstance");
    remixapi_PresentInfo present{}; present.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    checked(api_.Present(&present), "Present");
    if (frame_ == 0 && !openremix_) {
      // Switching to Custom migrates Quality values over User values at the
      // first frame boundary. Reapply the editor's settings after migration.
      settings(render_variables_);
      checked(api_.SetConfigVariable("rtx.enableStochasticAlphaBlend", "True"), "Enable alpha blending in the viewport");
    }
    Json result{{"status", "presented"}, {"revision", revision_}, {"frame", ++frame_}};
    if (openremix_) {
      remixapi_BackendCapabilitiesEXT backend{}; backend.structSize = sizeof(backend);
      remixapi_ReconstructionDiagnosticsEXT reconstruction{}; reconstruction.structSize = sizeof(reconstruction);
      checked(query_backend_(&backend), "Query effective backend");
      checked(query_reconstruction_(&reconstruction), "Query effective reconstruction");
      effective_settings_["backend"] = backend.activeBackend ? "pathtrace" : "raster";
      effective_settings_["upscaler"] = reconstruction.activeBackend >= 2 ? "dlss" : reconstruction.activeBackend == 1 ? "fsr31" : "none";
      effective_settings_["ray_reconstruction"] = reconstruction.activeBackend == 3;
      effective_settings_["fallback_reason"] = requested_settings_.backend == "pathtrace" && backend.activeBackend == 0 ? backend.unavailableReason : requested_settings_.backend == "pathtrace" && requested_settings_.upscaler != "none" ? reconstruction.fallbackReason : "";
      Json runtime = Json::object();
      const bool tracing = backend.activeBackend != 0;
      if (query_denoising_) {
        remixapi_DenoisingStatisticsEXT state{}; state.structSize = sizeof(state);
        checked(query_denoising_(&state), "Query active denoising");
        runtime["nrd"] = tracing && state.activeDenoiser != 0;
        runtime["taa"] = tracing && state.activeTaa != 0;
        runtime["denoising_reason"] = state.unavailableReason;
      }
      if (query_shadows_) {
        remixapi_ShadowStatisticsEXT state{}; state.structSize = sizeof(state);
        checked(query_shadows_(&state), "Query active shadow filter");
        runtime["sigma"] = tracing && state.active != 0;
        runtime["sigma_reason"] = state.unavailableReason;
      }
      if (query_sharc_) {
        remixapi_SharcStatisticsEXT state{}; state.structSize = sizeof(state);
        checked(query_sharc_(&state), "Query active radiance cache");
        runtime["sharc"] = tracing && state.active != 0;
        runtime["sharc_caching"] = tracing && state.active && state.updatePaths;
        runtime["sharc_reason"] = state.unavailableReason;
        runtime["sharc_memory_mib"] = double(state.memoryBytes) / (1024 * 1024);
      }
      if (query_local_lights_) {
        remixapi_LocalLightStatisticsEXT state{}; state.structSize = sizeof(state);
        checked(query_local_lights_(&state), "Query active local lighting");
        runtime["local_denoiser"] = tracing && state.denoiserActive != 0;
        runtime["active_lights"] = tracing ? state.activeLights : 0;
        const auto& options = requested_settings_.pathtrace;
        const bool filter = options.at("localFireflyFilter").get<bool>();
        const bool receivesLocal = state.activeLights && (state.denoiserActive || options.at("bounces").get<int>() > 0);
        runtime["local_firefly_filter"] = filter && runtime.value("nrd", false) && receivesLocal && reconstruction.activeBackend != 3;
      }
      effective_settings_["runtime"] = std::move(runtime);
      result["render_settings"] = effective_settings_;
    }
    return result;
  }
private:
  void require_running() { if (!running_) throw std::runtime_error("Start the renderer first"); }
  void material(const Json& data) {
    remixapi_MaterialInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = hash64(data.dump());
    auto albedo = native_path(data.value("albedo_texture", std::string()));
    auto normal = native_path(data.value("normal_texture", std::string()));
    auto roughness = native_path(data.value("roughness_texture", std::string()));
    auto metal = native_path(data.value("metallic_texture", std::string()));
    auto emission = native_path(data.value("emissive_texture", std::string()));
    info.albedoTexture = albedo.c_str(); info.normalTexture = normal.c_str(); info.emissiveTexture = emission.c_str();
    info.emissiveIntensity = data.value("emissive_intensity", 0.f);
    info.emissiveColorConstant = vector3(data.value("emissive", Json::array({0, 0, 0})));
    info.spriteSheetRow = 1; info.spriteSheetCol = 1;
    // Zero-initialized API fields mean nearest/clamp, unlike the MDL defaults.
    info.filterMode = data.value("filter_mode", 1);
    info.wrapModeU = data.value("wrap_mode_u", 1); info.wrapModeV = data.value("wrap_mode_v", 1);
    remixapi_MaterialInfoOpaqueEXT opaque{}; opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = vector3(data.value("albedo", Json::array({0.7, 0.7, 0.7})));
    opaque.opacityConstant = data.value("opacity", 1.f); opaque.roughnessConstant = data.value("roughness", 0.5f);
    opaque.metallicConstant = data.value("metallic", 0.f); opaque.roughnessTexture = roughness.c_str(); opaque.metallicTexture = metal.c_str();
    opaque.alphaTestType = data.value("alpha_test", 7); opaque.alphaReferenceValue = data.value("alpha_reference", 128);
    opaque.useDrawCallAlphaState = data.value("use_legacy_alpha", true);
    opaque.blendType_hasvalue = data.value("blend_enabled", false);
    opaque.blendType_value = data.value("blend_type", 0);
    opaque.invertedBlend = data.value("inverted_blend", false);
    remixapi_MaterialInfoTranslucentEXT glass{}; glass.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    glass.refractiveIndex = data.value("ior", 1.5f);
    glass.transmittanceTexture = albedo.c_str();
    glass.transmittanceColor = vector3(data.value("transmittance", Json::array({1, 1, 1})));
    glass.transmittanceMeasurementDistance = 1.f;
    info.pNext = data.value("kind", std::string("opaque")) == "translucent" ? static_cast<void*>(&glass) : static_cast<void*>(&opaque);
    remixapi_MaterialNormalEncodingEXT normal_encoding{};
    if (openremix_) { normal_encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT; normal_encoding.pNext = info.pNext; normal_encoding.encoding = uint32_t(data.value("normal_encoding", 0)) + 1; info.pNext = &normal_encoding; }
    // The thin film (scene.cpp material_json): OpenRemix's iridescence
    // extension, or the API's own thin-film fields on other runtimes.
    // Authored values are clamped to the extension's ranges.
    remixapi_MaterialIridescenceEXT film{};
    NativePath film_texture;
    if (auto found = data.find("iridescence"); found != data.end() && data.value("kind", std::string("opaque")) != "translucent") {
      const auto& f = *found;
      auto value = [&](const char* key, float fallback, float low, float high) {
        const float v = f.value(key, fallback);
        return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
      };
      opaque.alphaIsThinFilmThickness = f.value("from_alpha", false);
      if (openremix_) {
        film.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT;
        film.strength = value("strength", 1, 0, 1);
        film.filmRefractiveIndex = value("ior", 1.5f, 1, 3);
        film.thickness = value("thickness", 200, 0, 2000);
        film.thicknessMin = value("min", 100, 0, 2000);
        film.thicknessMax = std::max(film.thicknessMin, value("max", 800, 0, 2000));
        film.flowAmplitude = value("flow_amplitude", 0, 0, 2000);
        film.flowScale = value("flow_scale", 1, 1e-3f, 1e6f);
        film.flowSpeed = value("flow_speed", .1f, -100, 100);
        film.structural = value("structural", 0, 0, 1);
        film_texture = native_path(f.value("texture", std::string()));
        film.thicknessTexture = film_texture.c_str();
        film.pNext = info.pNext; info.pNext = &film;
      } else {
        opaque.thinFilmThickness_hasvalue = true;
        opaque.thinFilmThickness_value = value("thickness", 200, 0, 1500);
      }
    }
    remixapi_MaterialHandle handle{};
    checked(api_.CreateMaterial(&info, &handle), "CreateMaterial");
    materials_.emplace(data.at("id").get<std::string>(), handle);
  }
  // Each category group of an instance draws its own Remix mesh. Instances
  // share one mesh per prototype and surface selection.
  void mesh(const Json& data, const Json& prototypes) {
    const auto index = data.at("prototype").get<size_t>();
    const auto& prototype = prototypes.at(index);
    if (data.at("surfaces").size() != prototype.at("surfaces").size()) throw std::runtime_error("Mesh surfaces do not match their prototype");
    for (const auto& [flags, selection] : editor::category_surface_groups(data)) {
      // Keep hidden/ignored geometry in the editor snapshot, but do not submit
      // it. The native runtime can allocate a zero-sized material buffer when
      // its first frame contains only hidden surfaces.
      if (flags & (REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE)) continue;
      auto cached = mesh_cache_.find({index, selection});
      if (cached == mesh_cache_.end()) cached = mesh_cache_.emplace(std::make_pair(index, selection), create_mesh(prototype, selection)).first;
      // The USD id stays on every group, so transform previews move all subsets together.
      if (cached->second) add_instance(data, cached->second, flags);
    }
  }
  remixapi_MeshHandle create_mesh(const Json& prototype, const std::vector<size_t>& selection) {
    std::vector<std::vector<remixapi_HardcodedVertex>> vertices;
    std::vector<std::vector<uint32_t>> indices;
    std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
    uint64_t geometry_hash = 14695981039346656037ull;
    for (auto index : selection) {
      const auto& surface = prototype.at("surfaces").at(index);
      const auto positions = editor::read_buffer<float>(surface.at("positions"));
      const auto normals = editor::read_buffer<float>(surface.at("normals"));
      const auto uvs = editor::read_buffer<float>(surface.at("uvs"));
      const auto colors = surface.contains("colors") ? editor::read_buffer<uint32_t>(surface.at("colors")) : std::vector<uint32_t>();
      const size_t count = positions.size() / 3;
      if (positions.size() % 3 || normals.size() != positions.size() || uvs.size() != count * 2 || (!colors.empty() && colors.size() != count))
        throw std::runtime_error("Invalid mesh vertex data");
      indices.push_back(editor::read_buffer<uint32_t>(surface.at("indices")));
      if (indices.back().size() % 3) throw std::runtime_error("Mesh indices must form triangles");
      const auto& material = surface.at("material").get_ref<const std::string&>();
      for (const auto* name : {"positions", "normals", "uvs", "colors", "indices"})
        if (surface.contains(name)) geometry_hash = hash_bytes(geometry_hash, surface.at(name).get_binary());
      geometry_hash = hash_bytes(geometry_hash, material);
      vertices.emplace_back(count);
      for (size_t v = 0; v < count; ++v) {
        auto& vertex = vertices.back()[v];
        for (int i = 0; i < 3; ++i) { vertex.position[i] = positions[v * 3 + i]; vertex.normal[i] = normals[v * 3 + i]; }
        // Some captures contain NaN/Inf UVs. Keep the mesh renderable with a
        // zero fallback for just the invalid component.
        for (int i = 0; i < 2; ++i) vertex.texcoord[i] = std::isfinite(uvs[v * 2 + i]) ? uvs[v * 2 + i] : 0.f;
        // Match Remix's USD importer: captures store V flipped from runtime texture coordinates.
        vertex.texcoord[1] = 1.f - vertex.texcoord[1];
        vertex.color = colors.empty() ? 0xffffffffu : colors[v];
      }
      for (auto i : indices.back()) if (i >= count) throw std::runtime_error("Mesh index is out of bounds");
      // USD uses counter-clockwise fronts; the Remix external mesh API uses clockwise fronts.
      for (size_t i = 0; i + 2 < indices.back().size(); i += 3) std::swap(indices.back()[i + 1], indices.back()[i + 2]);
      remixapi_MeshInfoSurfaceTriangles info{};
      info.vertices_values = vertices.back().data(); info.vertices_count = vertices.back().size();
      info.indices_values = indices.back().data(); info.indices_count = indices.back().size();
      info.material = materials_.at(material);
      surfaces.push_back(info);
    }
    if (surfaces.empty()) return {};
    remixapi_MeshInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = geometry_hash ? geometry_hash : 1; info.surfaces_values = surfaces.data(); info.surfaces_count = uint32_t(surfaces.size());
    remixapi_MeshHandle handle{};
    checked(api_.CreateMesh(&info, &handle), "CreateMesh"); meshes_.push_back(handle);
    return handle;
  }
  void add_instance(const Json& data, remixapi_MeshHandle handle, uint32_t flags) {
    remixapi_InstanceInfo instance{}; instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.mesh = handle; instance.transform = transform(data.at("transform")); instance.doubleSided = data.value("double_sided", false);
    instance.categoryFlags = flags;
    instances_.push_back(instance);
    blend_states_.push_back(blend_state(data.value("alpha_state", Json::object())));
    instance_origins_.emplace_back(data.at("id").get<std::string>(), instance.transform);
  }
  void add_light(const Json& data) {
    if (auto light = create_light(data, transform(data.at("transform")))) {
      lights_.push_back(light); light_origins_.push_back(data);
    }
  }
  remixapi_LightHandle create_light(const Json& data, const remixapi_Transform& matrix) {
    remixapi_LightInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = hash64(data.at("id").get<std::string>() + ":" + data.at("type").get<std::string>());
    // The fork deliberately ignores updates to sleeping static lights. Editor
    // lights must remain movable after any number of preview/inspector edits.
    info.isDynamic = true;
    remixapi_LightInfoUSDEXT usd{}; usd.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    const std::map<std::string, remixapi_StructType> types{{"SphereLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT},
      {"RectLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT}, {"DiskLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT},
      {"CylinderLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT}, {"DistantLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT}};
    const auto type = data.at("type").get<std::string>();
    auto color = vector3(data.at("color"));
    float intensity = data.value("intensity", 1.f), radius = data.value("radius", 0.5f), width = data.value("width", 1.f);
    float height = data.value("height", 1.f), length = data.value("length", 1.f);
    float angle = data.value("angle", 0.53f) * float(3.141592653589793 / 180.0);
    float cone = data.value("cone_angle", 180.f) * float(3.141592653589793 / 180.0);
    float softness = data.value("cone_softness", 0.f), focus = data.value("focus", 0.f);
    remixapi_LightInfoDomeEXT dome{}; dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    auto texture = native_path(data.value("texture", std::string()));
    if (type == "DomeLight") {
      dome.transform = matrix; dome.colorTexture = texture.c_str(); info.pNext = &dome;
      info.radiance = {color.x * intensity, color.y * intensity, color.z * intensity};
    } else {
      auto found = types.find(type);
      if (found == types.end()) return {};
      usd.lightType = found->second; usd.transform = matrix; usd.pColor = &color; usd.pIntensity = &intensity;
      usd.pRadius = &radius; usd.pWidth = &width; usd.pHeight = &height; usd.pLength = &length;
      usd.pAngleRadians = &angle; usd.pConeAngleRadians = &cone; info.pNext = &usd;
      usd.pConeSoftness = &softness; usd.pFocus = &focus;
    }
    remixapi_LightHandle light{};
    checked(api_.CreateLight(&info, &light), ("CreateLight " + data.at("id").get<std::string>() + " (" + type + ", " + data.value("texture", std::string()) + ")").c_str()); return light;
  }
  remixapi_Interface api_{};
  remixapi_HMODULE library_{};
  PFN_remixapi_GetBackendCapabilitiesEXT query_backend_ = nullptr;
  PFN_remixapi_GetDenoisingStatisticsEXT query_denoising_ = nullptr;
  PFN_remixapi_GetSharcStatisticsEXT query_sharc_ = nullptr;
  PFN_remixapi_GetShadowStatisticsEXT query_shadows_ = nullptr;
  PFN_remixapi_GetLocalLightStatisticsEXT query_local_lights_ = nullptr;
  remixapi_ErrorCode (*query_reconstruction_)(remixapi_ReconstructionDiagnosticsEXT*) = nullptr;
#ifdef REMIX_PLUS_API
  struct RetainedApi {
    PFN_remixapi_CreateRetainedInstances create = nullptr;
    PFN_remixapi_UpdateRetainedInstances update = nullptr;
    PFN_remixapi_DestroyRetainedInstances destroy = nullptr;
    PFN_remixapi_DrawRetainedInstances draw = nullptr;
    explicit operator bool() const { return create && update && destroy && draw; }
  } retained_api_;
  std::vector<uint64_t> retained_;
#endif
  Json effective_settings_;
  studio::RenderSettings requested_settings_;
  std::string pathtrace_reason_, reconstruction_reason_;
  bool openremix_ = false, pathtrace_ = false, fsr_ = false, nrd_ = false, taa_ = false;
  bool running_ = false;
  bool shared_ = false;
  bool dlss_ = false;
  bool ray_reconstruction_ = false;
  // The pinned Linux native renderer compiles out NIS with NO_VENDOR_APIS.
#ifdef _WIN32
  bool nis_ = true;
#else
  bool nis_ = false;
#endif
  uint32_t width_ = 1280, height_ = 720;
  uint64_t revision_ = 0, geometry_revision_ = 0, frame_ = 0;
  Json camera_;
  Json render_variables_ = Json::object();
  std::map<std::string, remixapi_MaterialHandle> materials_;
  std::vector<remixapi_MeshHandle> meshes_;
  std::map<std::pair<size_t, std::vector<size_t>>, remixapi_MeshHandle> mesh_cache_;
  std::vector<remixapi_InstanceInfo> instances_;
  std::vector<remixapi_InstanceInfoBlendEXT> blend_states_;
  std::vector<std::pair<std::string, remixapi_Transform>> instance_origins_;
  std::vector<remixapi_LightHandle> lights_;
  std::vector<Json> light_origins_;
  std::set<std::string> previewed_lights_;
  std::set<size_t> moving_instances_;
#ifdef _WIN32
  HWND window_ = nullptr;
#endif
};
}

int main() {
#ifdef _WIN32
#ifdef REMIX_PLUS_API
  constexpr bool shared_frames = true;
#else
  constexpr bool shared_frames = false;
#endif
  editor::Channel channel(reinterpret_cast<intptr_t>(GetStdHandle(STD_INPUT_HANDLE)), reinterpret_cast<intptr_t>(GetStdHandle(STD_OUTPUT_HANDLE)));
#else
  constexpr bool shared_frames = true;
  signal(SIGPIPE, SIG_IGN);
  editor::Channel channel(STDIN_FILENO, STDOUT_FILENO);
#endif
  Renderer renderer;
  try {
    Json request;
    while (channel.receive(request)) {
      Json response{{"id", request.value("id", 0)}, {"ok", true}};
      bool shutdown = false;
      try {
        const auto op = request.at("op").get<std::string>();
        if (op == "hello") response["result"] = {{"protocol", editor::protocol_version}, {"renderer", "dxvk-remix"}, {"shared_frames", shared_frames}};
        else if (op == "start") response["result"] = renderer.start(request);
        else if (op == "scene") renderer.scene(request.at("scene"));
        else if (op == "lights") response["result"] = renderer.update_lights(request);
        else if (op == "camera") renderer.camera(request.at("camera"));
        else if (op == "preview_transform") response["result"] = renderer.preview_transform(request);
        else if (op == "settings") renderer.settings(request.at("settings"));
        else if (op == "render_settings") response["result"] = renderer.render_settings(request.at("settings"));
        else if (op == "frame") response["result"] = renderer.frame();
        else if (op == "inspect_buffers") { response["result"] = Json::array(); describe_buffers(request.value("data", Json()), response["result"]); }
        else if (op == "shutdown") shutdown = true;
        else throw std::runtime_error("Unknown renderer operation: " + op);
      } catch (const std::exception& error) { response["ok"] = false; response["error"] = error.what(); }
      channel.send(response);
      if (shutdown) break;
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
