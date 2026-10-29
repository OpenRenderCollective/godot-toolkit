#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <pxr/usd/usd/stage.h>

namespace editor {
using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Edit {
  std::string label;
  pxr::SdfLayerHandle layer;
  std::string before;
  std::string after;
  bool lights_only = false;
};

class Document {
public:
  Json dispatch(const Json& request);
  Json describe() const;
  Json scene() const;
  Json inspect(const std::string& path) const;
  void open(const fs::path& path);
  void create(const fs::path& capture, const fs::path& project);
  void save();
  void package(const fs::path& directory);
  void edit(const std::string& label, const std::function<void()>& operation, bool lights_only = false);
  void undo();
  void redo();
  Json ingest(const Json& request);
  pxr::UsdStageRefPtr stage() const { return stage_; }
  uint64_t revision() const { return revision_; }

private:
  Json game_link(const Json& request);
  void export_mod(const fs::path& directory, bool link_assets);
  void require_stage() const;
  void select_layer(const std::string& identifier);
  Json captures() const;
  void select_capture(const fs::path& path, bool add);
  void edit_layer(const std::string& label, const pxr::SdfLayerHandle& layer, const std::function<void()>& operation, bool lights_only = false);
  bool is_light_edit(const Json& request) const;
  void set_property(const Json& request);
  void set_authoring_property(const Json& request, bool reset = false);
  void set_light_type(const Json& request);
  void set_material_model(const Json& request);
  void set_transform(const Json& request);
  std::string anchor_for(const std::string& path) const;
  std::string authored_path(const std::string& path) const;
  std::string authored_asset(const std::string& path) const;
  std::string display_path(const std::string& path, const std::string& context) const;
  std::string parent_path(const std::string& path) const;
  Json parent_candidates(const std::string& path) const;
  std::string set_parent(const Json& request);
  std::string add_reference(const Json& request);
  Json mesh_replacement(const std::string& path) const;
  std::string revert_mesh(const std::string& path);
  std::string add_light(const Json& request);
  Json light_defaults(const std::string& type) const;
  void replace_material(const Json& request);
  void mutate(const Json& request, Json& result);
  bool dirty() const;
  pxr::UsdStageRefPtr stage_;
  pxr::SdfLayerHandle edit_layer_;
  fs::path project_path_;
  std::vector<Edit> undo_;
  std::vector<Edit> redo_;
  std::map<std::string, std::string> saved_layers_;
  uint64_t revision_ = 0;
  bool last_edit_lights_only_ = false;
};

Json compose_scene(const pxr::UsdStageRefPtr& stage, uint64_t revision);
Json compose_lights(const pxr::UsdStageRefPtr& stage, uint64_t revision);
Json describe_authoring(const pxr::UsdPrim& prim, const pxr::SdfLayerHandle& edit_layer);
Json compose_material(const pxr::UsdPrim& prim);
Json preview_asset(const fs::path& path);
std::string resolved_texture_path(const pxr::UsdPrim& prim, const char* name);
using TextureConverter = std::function<void(const fs::path&, const fs::path&, const std::string&)>;
Json ingest_asset(const Json& request, const fs::path& project_directory,
                  const TextureConverter& convert = {}, const std::function<bool()>& canceled = {});
std::string layer_text(const pxr::SdfLayerHandle& layer);
void atomic_save_layer(const pxr::SdfLayerHandle& layer);
}
