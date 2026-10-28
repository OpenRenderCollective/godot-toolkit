#include "document.h"
#include "usd_authoring.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/namespaceEdit.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdUtils/dependencies.h>
#ifdef _WIN32
#include <windows.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE
namespace editor {
namespace {
std::string required(const Json& data, const char* key) {
  auto value = data.at(key).get<std::string>();
  if (value.empty()) throw std::runtime_error(std::string(key) + " must not be empty");
  return value;
}
std::string relative_asset(const fs::path& path, const SdfLayerHandle& layer) {
  return fs::relative(fs::absolute(path), fs::path(layer->GetRealPath()).parent_path()).generic_string();
}
bool is_capture(const SdfLayerHandle& layer) {
  auto data = layer->GetCustomLayerData();
  auto it = data.find("lightspeed_layer_type");
  return it != data.end() && it->second.IsHolding<std::string>() && it->second.Get<std::string>() == "capture";
}
constexpr const char* capture_catalog = "remix_studio_captures";
constexpr const char* standalone_root = "/RootNode/StudioAssets";
const TfToken replacement_tag("remixStudioMeshReplacement");
const TfToken replacement_restore("remixStudioMeshRestore");
void define_ancestors(const SdfLayerHandle& layer, SdfPath path) {
  for (; !path.IsAbsoluteRootPath(); path = path.GetParentPath()) {
    auto spec = SdfCreatePrimInLayer(layer, path);
    if (!spec) throw std::runtime_error("Could not define attachment parent");
    if (spec->GetSpecifier() == SdfSpecifierOver) spec->SetSpecifier(SdfSpecifierDef);
  }
}
Json matrix_json(const GfMatrix4d& matrix) {
  Json values = Json::array();
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) values.push_back(matrix[r][c]);
  return values;
}
GfMatrix4d invertible_inverse(const GfMatrix4d& matrix) {
  if (!std::isfinite(matrix.GetDeterminant()) || std::abs(matrix.GetDeterminant()) < 1e-12)
    throw std::runtime_error("Parent transform is singular");
  return matrix.GetInverse();
}
GfVec3d vector3(const Json& data) {
  if (!data.is_array() || data.size() != 3) throw std::runtime_error("Expected three numeric components");
  GfVec3d v(data[0].get<double>(), data[1].get<double>(), data[2].get<double>());
  for (int i = 0; i < 3; ++i) if (!std::isfinite(v[i])) throw std::runtime_error("Non-finite transform");
  return v;
}
std::string unique_child(const UsdStageRefPtr& stage, const std::string& parent, const std::string& name) {
  auto stem = TfMakeValidIdentifier(name);
  if (stem.empty()) stem = "asset";
  auto result = parent + "/" + stem;
  for (int i = 2; stage->GetPrimAtPath(SdfPath(result)); ++i) result = parent + "/" + stem + "_" + std::to_string(i);
  return result;
}
Json value_json(const VtValue& v) {
  if (v.IsHolding<bool>()) return v.Get<bool>();
  if (v.IsHolding<int>()) return v.Get<int>();
  if (v.IsHolding<unsigned int>()) return v.Get<unsigned int>();
  if (v.IsHolding<float>()) return v.Get<float>();
  if (v.IsHolding<double>()) return v.Get<double>();
  if (v.IsHolding<std::string>()) return v.Get<std::string>();
  if (v.IsHolding<TfToken>()) return v.Get<TfToken>().GetString();
  if (v.IsHolding<SdfAssetPath>()) return v.Get<SdfAssetPath>().GetAssetPath();
  if (v.IsHolding<GfVec3f>()) { auto x = v.Get<GfVec3f>(); return {x[0], x[1], x[2]}; }
  if (v.IsHolding<GfVec3d>()) { auto x = v.Get<GfVec3d>(); return {x[0], x[1], x[2]}; }
  return nullptr;
}
void set_value(const UsdAttribute& attr, const Json& value) {
  const auto type = attr.GetTypeName();
  bool ok = false;
  if (type == SdfValueTypeNames->Bool) ok = attr.Set(value.get<bool>());
  else if (type == SdfValueTypeNames->Int) ok = attr.Set(value.get<int>());
  else if (type == SdfValueTypeNames->UInt) ok = attr.Set(value.get<unsigned int>());
  else if (type == SdfValueTypeNames->Float) ok = attr.Set(value.get<float>());
  else if (type == SdfValueTypeNames->Double) ok = attr.Set(value.get<double>());
  else if (type == SdfValueTypeNames->String) ok = attr.Set(value.get<std::string>());
  else if (type == SdfValueTypeNames->Token) ok = attr.Set(TfToken(value.get<std::string>()));
  else if (type == SdfValueTypeNames->Asset) ok = attr.Set(SdfAssetPath(value.get<std::string>()));
  else if (type.GetType() == TfType::Find<GfVec3f>()) ok = attr.Set(GfVec3f(vector3(value)));
  else if (type.GetType() == TfType::Find<GfVec3d>()) ok = attr.Set(vector3(value));
  else throw std::runtime_error("This attribute type is read-only: " + type.GetAsToken().GetString());
  if (!ok) throw std::runtime_error("USD rejected the property value");
}
}

std::string layer_text(const SdfLayerHandle& layer) {
  std::string text;
  if (!layer->ExportToString(&text)) throw std::runtime_error("Could not serialize USD layer");
  return text;
}

void atomic_save_layer(const SdfLayerHandle& layer) {
  fs::path path(layer->GetRealPath());
  if (path.empty()) throw std::runtime_error("Cannot save an anonymous layer");
  auto temp = path.parent_path() / (path.stem().string() + ".saving" + path.extension().string());
  if (!layer->Export(temp.string())) throw std::runtime_error("Could not write " + temp.string());
#ifdef _WIN32
  // The replacement must work when the destination already exists on Windows.
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Could not replace " + path.string());
#else
  fs::rename(temp, path);
#endif
}

void Document::require_stage() const {
  if (!stage_) throw std::runtime_error("Open a project first");
}

void Document::open(const fs::path& requested) {
  // Open by the real location. Capture paths are stored relative to the workfile's
  // directory, and ".." means something else through a symlink (a project linked into a
  // game's mods folder), so USD and the capture list must both start from the target.
  const auto path = fs::weakly_canonical(fs::absolute(requested));
  auto next = UsdStage::Open(path.string());
  if (!next) throw std::runtime_error("Could not open USD file: " + path.string());
  if (is_capture(next->GetRootLayer())) throw std::runtime_error("This is a capture. Use New from capture to create writable project layers.");
  SdfLayerHandle target;
  auto metadata = next->GetRootLayer()->GetCustomLayerData();
  auto omni = metadata.find("omni_layer");
  if (omni != metadata.end() && omni->second.IsHolding<VtDictionary>()) {
    const auto& dict = omni->second.Get<VtDictionary>();
    auto authoring = dict.find("authoring_layer");
    if (authoring != dict.end() && authoring->second.IsHolding<std::string>())
      target = SdfLayer::FindOrOpenRelativeToLayer(next->GetRootLayer(), authoring->second.Get<std::string>());
  }
  for (auto layer : next->GetLayerStack(false)) {
    if (!target && !is_capture(layer) && layer != next->GetRootLayer()) target = layer;
  }
  if (!target) target = next->GetRootLayer();
  if (is_capture(target) || !target->PermissionToEdit()) throw std::runtime_error("No writable replacement layer");
  stage_ = next;
  edit_layer_ = target;
  stage_->SetEditTarget(edit_layer_);
  project_path_ = path;
  undo_.clear(); redo_.clear(); saved_layers_.clear();
  for (auto layer : stage_->GetLayerStack(false)) saved_layers_[layer->GetIdentifier()] = layer_text(layer);
  ++revision_;
}

void Document::create(const fs::path& capture, const fs::path& project) {
  if (fs::exists(project)) throw std::runtime_error("Project already exists");
  auto source = UsdStage::Open(fs::absolute(capture).string());
  if (!source || !source->GetPrimAtPath(SdfPath("/RootNode"))) throw std::runtime_error("Expected a Remix capture with /RootNode");
  auto root = fs::absolute(project).parent_path();
  fs::create_directories(root);
  auto mod_path = root / "mod.usda";
  if (fs::exists(mod_path)) throw std::runtime_error("mod.usda already exists; choose a new project directory");
  auto mod = SdfLayer::CreateNew(mod_path.string());
  auto work = SdfLayer::CreateNew(fs::absolute(project).string());
  if (!mod || !work) throw std::runtime_error("Could not create project files");
  replacement_metadata(mod, source);
  work->SetCustomLayerData({
    {"lightspeed_layer_type", VtValue(std::string("workfile"))},
    {capture_catalog, VtValue(VtStringArray{fs::relative(fs::absolute(capture), root).generic_string()})},
    {"omni_layer", VtValue(VtDictionary{{"authoring_layer", VtValue(std::string("./mod.usda"))}})}
  });
  work->SetSubLayerPaths({"./mod.usda", fs::relative(fs::absolute(capture), root).generic_string()});
  auto stage = UsdStage::Open(work);
  UsdGeomSetStageUpAxis(stage, UsdGeomGetStageUpAxis(source));
  UsdGeomSetStageMetersPerUnit(stage, UsdGeomGetStageMetersPerUnit(source));
  if (!work->Save() || !mod->Save()) throw std::runtime_error("Could not save new project");
  open(project);
}

bool Document::dirty() const {
  if (!stage_) return false;
  for (auto layer : stage_->GetLayerStack(false)) {
    auto found = saved_layers_.find(layer->GetIdentifier());
    if (found == saved_layers_.end() || found->second != layer_text(layer)) return true;
  }
  return false;
}

Json Document::captures() const {
  auto root = stage_->GetRootLayer();
  Json result = Json::array();
  std::set<std::string> seen;
  const auto metadata = root->GetCustomLayerData();
  auto append = [&](const std::string& asset) {
    auto path = fs::weakly_canonical(fs::path(root->GetRealPath()).parent_path() / asset);
    const auto id = relative_asset(path, root);
    if (!seen.insert(id).second) return;
    bool active = false;
    for (const auto& sublayer : root->GetSubLayerPaths()) {
      auto resolved = fs::weakly_canonical(fs::path(root->GetRealPath()).parent_path() / std::string(sublayer));
      if (resolved == path) active = true;
    }
    result.push_back({{"id", id}, {"path", path.generic_string()}, {"name", path.filename().string()},
      {"active", active}, {"missing", !fs::is_regular_file(path)}});
  };
  auto stored = metadata.find(capture_catalog);
  if (stored != metadata.end() && stored->second.IsHolding<VtStringArray>())
    for (const auto& path : stored->second.Get<VtStringArray>()) append(path);
  // Discover captures in existing workfiles without changing their metadata.
  for (const auto& asset : root->GetSubLayerPaths()) {
    auto layer = SdfLayer::FindOrOpenRelativeToLayer(root, asset);
    if (layer && is_capture(layer)) append(asset);
  }
  return result;
}

void Document::select_capture(const fs::path& path, bool add) {
  require_stage();
  auto root = stage_->GetRootLayer();
  const auto absolute = fs::weakly_canonical(path.is_absolute() ? path : project_path_.parent_path() / path);
  if (!fs::is_regular_file(absolute)) throw std::runtime_error("Capture file is missing: " + absolute.string());
  auto source = UsdStage::Open(absolute.string());
  if (!source || !is_capture(source->GetRootLayer()) || !source->GetPrimAtPath(SdfPath("/RootNode")))
    throw std::runtime_error("Expected a Remix capture with /RootNode");
  auto entries = captures();
  const auto id = relative_asset(absolute, root);
  std::set<std::string> paths;
  VtStringArray catalog;
  for (const auto& entry : entries) {
    paths.insert(entry["path"].get<std::string>());
    catalog.push_back(entry["id"].get<std::string>());
  }
  if (!paths.count(absolute.generic_string())) {
    if (!add) throw std::runtime_error("Capture is not attached to this project");
    catalog.push_back(id);
  }
  // Swap only capture sublayers. Replacement layers, ordering, and offsets stay intact.
  std::vector<std::string> sublayers;
  std::vector<SdfLayerOffset> offsets;
  const auto old_offsets = root->GetSubLayerOffsets();
  bool inserted = false;
  size_t index = 0;
  for (const auto& asset : root->GetSubLayerPaths()) {
    auto resolved = fs::weakly_canonical(fs::path(root->GetRealPath()).parent_path() / std::string(asset));
    if (paths.count(resolved.generic_string())) {
      if (!inserted) { sublayers.push_back(id); offsets.emplace_back(); inserted = true; }
    } else {
      auto layer = SdfLayer::FindOrOpenRelativeToLayer(root, asset);
      if (layer) {
        auto nested = UsdStage::Open(layer);
        if (nested) for (auto child : nested->GetLayerStack(false))
          if (is_capture(child)) throw std::runtime_error("Move nested capture sublayers to the workfile before switching captures");
      }
      sublayers.push_back(asset); offsets.push_back(old_offsets[index]);
    }
    ++index;
  }
  if (!inserted) { sublayers.push_back(id); offsets.emplace_back(); }
  edit_layer(add ? "Add capture" : "Switch capture", root, [&] {
    auto metadata = root->GetCustomLayerData();
    metadata[capture_catalog] = VtValue(catalog);
    root->SetCustomLayerData(metadata);
    root->SetSubLayerPaths(sublayers);
    for (size_t i = 0; i < offsets.size(); ++i) root->SetSubLayerOffset(offsets[i], int(i));
    UsdEditContext context(stage_, root);
    UsdGeomSetStageUpAxis(stage_, UsdGeomGetStageUpAxis(source));
    UsdGeomSetStageMetersPerUnit(stage_, UsdGeomGetStageMetersPerUnit(source));
  });
  // Newly composed capture layers are baselines, never new authoring to save.
  for (auto layer : stage_->GetLayerStack(false))
    if (!saved_layers_.count(layer->GetIdentifier())) saved_layers_[layer->GetIdentifier()] = layer_text(layer);
}

Json Document::describe() const {
  if (!stage_) return {{"open", false}, {"revision", revision_}};
  Json prims = Json::array(), layers = Json::array(), warnings = Json::array();
  for (auto prim : stage_->Traverse()) {
    auto imageable = UsdGeomImageable(prim);
    prims.push_back({{"path", prim.GetPath().GetString()}, {"name", prim.GetName().GetString()},
      {"type", prim.GetTypeName().GetString()}, {"active", prim.IsActive()},
      {"visible", !imageable || imageable.ComputeVisibility() != UsdGeomTokens->invisible}});
    if (prim.GetTypeName() == TfToken("OmniGraph"))
      warnings.push_back("Preserved without editing support: " + prim.GetPath().GetString());
  }
  for (auto layer : stage_->GetLayerStack(false)) {
    layers.push_back({{"id", layer->GetIdentifier()}, {"name", layer->GetDisplayName()},
      {"capture", is_capture(layer)}, {"editable", !is_capture(layer) && layer->PermissionToEdit()},
      {"selected", layer == edit_layer_}});
  }
  for (const auto& error : stage_->GetCompositionErrors()) warnings.push_back(error->ToString());
  auto capture_list = captures();
  std::string active_capture;
  for (const auto& capture : capture_list) if (capture["active"].get<bool>()) { active_capture = capture["id"]; break; }
  return {{"open", true}, {"path", project_path_.generic_string()}, {"dirty", dirty()},
    {"captures", capture_list}, {"active_capture", active_capture},
    {"revision", revision_}, {"up_axis", UsdGeomGetStageUpAxis(stage_).GetString()},
    {"meters_per_unit", UsdGeomGetStageMetersPerUnit(stage_)}, {"prims", prims},
    {"layers", layers}, {"warnings", warnings}, {"can_undo", !undo_.empty()}, {"can_redo", !redo_.empty()}};
}

Json Document::inspect(const std::string& path) const {
  require_stage();
  auto prim = stage_->GetPrimAtPath(SdfPath(path));
  if (!prim || !prim.IsActive()) throw std::runtime_error("Prim no longer exists");
  Json properties = Json::array();
  for (auto attr : prim.GetAttributes()) {
    VtValue value;
    if (!attr.Get(&value)) continue;
    auto json = value_json(value);
    if (json.is_null()) continue;
    properties.push_back({{"name", attr.GetName().GetString()}, {"type", attr.GetTypeName().GetAsToken().GetString()}, {"value", json}});
  }
  Json result{{"path", path}, {"properties", properties}};
  auto transform = UsdGeomXformable(prim);
  if (transform) {
    GfMatrix4d matrix; bool reset;
    transform.GetLocalTransformation(&matrix, &reset, UsdTimeCode::EarliestTime());
    auto translation = matrix.ExtractTranslation();
    result["translation"] = {translation[0], translation[1], translation[2]};
    GfTransform components(matrix);
    auto scale = components.GetScale();
    auto rotation = components.GetRotation().Decompose(GfVec3d::ZAxis(), GfVec3d::YAxis(), GfVec3d::XAxis());
    result["scale"] = {scale[0], scale[1], scale[2]};
    result["rotation"] = {rotation[2], rotation[1], rotation[0]};
    UsdGeomXformCache cache(UsdTimeCode::EarliestTime());
    result["world_transform"] = matrix_json(cache.GetLocalToWorldTransform(prim));
  }
  try { result["anchor"] = anchor_for(path); } catch (const std::runtime_error&) { result["anchor"] = nullptr; }
  result["authoring"] = describe_authoring(prim, edit_layer_);
  result["authoring"]["references"] = Json::array();
  for (auto owner = prim; owner && !owner.IsPseudoRoot(); owner = owner.GetParent()) {
    for (auto spec : owner.GetPrimStack()) for (const auto& reference : spec->GetReferenceList().GetAppliedItems()) {
      if (!reference.GetAssetPath().empty())
        result["authoring"]["references"].push_back(reference.GetAssetPath());
    }
    if (!result["authoring"]["references"].empty()) break;
  }
  try {
    const auto target = authored_path(path);
    result["authoring"]["edit_path"] = target;
    result["authoring"]["editable"] = true;
  } catch (const std::runtime_error&) {
    result["authoring"]["editable"] = false;
  }
  for (auto& field : result["authoring"]["fields"]) {
    try {
      auto owner = SdfPath(authored_path(field["path"].get<std::string>()));
      auto spec = edit_layer_->GetAttributeAtPath(owner.AppendProperty(TfToken(field["property"].get<std::string>())));
      field["overridden"] = bool(spec && spec->HasDefaultValue());
    } catch (const std::runtime_error&) { field["overridden"] = false; }
  }
  try {
    const auto owner = display_path(authored_asset(path), path);
    const auto parent = SdfPath(owner).GetParentPath().GetString();
    result["attachment"] = {{"path", owner}, {"parent", parent == standalone_root ? "" : parent}};
  } catch (const std::runtime_error&) {}
  const auto replacement = mesh_replacement(path);
  if (!replacement.empty()) result["mesh_replacement"] = replacement;
  return result;
}

void Document::edit(const std::string& label, const std::function<void()>& operation, bool lights_only) {
  require_stage();
  edit_layer(label, edit_layer_, operation, lights_only);
}

void Document::edit_layer(const std::string& label, const SdfLayerHandle& layer, const std::function<void()>& operation, bool lights_only) {
  if (!layer || is_capture(layer) || !layer->PermissionToEdit()) throw std::runtime_error("Layer is read-only");
  const auto before = layer_text(layer);
  try { operation(); }
  catch (...) { layer->ImportFromString(before); throw; }
  const auto after = layer_text(layer);
  if (before == after) return;
  undo_.push_back({label, layer, before, after, lights_only});
  last_edit_lights_only_ = lights_only;
  redo_.clear();
  ++revision_;
}

void Document::undo() {
  if (undo_.empty()) return;
  auto entry = undo_.back();
  if (!entry.layer->ImportFromString(entry.before)) throw std::runtime_error("Could not undo USD edit");
  undo_.pop_back(); redo_.push_back(entry); ++revision_;
  last_edit_lights_only_ = entry.lights_only;
}
void Document::redo() {
  if (redo_.empty()) return;
  auto entry = redo_.back();
  if (!entry.layer->ImportFromString(entry.after)) throw std::runtime_error("Could not redo USD edit");
  redo_.pop_back(); undo_.push_back(entry); ++revision_;
  last_edit_lights_only_ = entry.lights_only;
}

void Document::select_layer(const std::string& identifier) {
  require_stage();
  for (auto layer : stage_->GetLayerStack(false)) {
    if (layer->GetIdentifier() != identifier) continue;
    if (is_capture(layer) || !layer->PermissionToEdit()) throw std::runtime_error("Layer is read-only");
    edit_layer_ = layer; stage_->SetEditTarget(layer); return;
  }
  throw std::runtime_error("Layer is not in this project");
}

void Document::set_property(const Json& request) {
  auto prim = stage_->GetPrimAtPath(SdfPath(authored_path(required(request, "path"))));
  if (!prim) throw std::runtime_error("Unknown prim");
  auto attr = prim.GetAttribute(TfToken(required(request, "property")));
  if (!attr) throw std::runtime_error("Unknown attribute");
  set_value(attr, request.at("value"));
  update_light_extent(prim);
}

void Document::set_transform(const Json& request) {
  auto selected_path = required(request, "path");
  auto path = authored_path(selected_path);
  auto prim = stage_->GetPrimAtPath(SdfPath(path));
  UsdGeomXformable xform(prim);
  if (!xform) throw std::runtime_error("Selection cannot be transformed");
  GfMatrix4d matrix; bool reset;
  xform.GetLocalTransformation(&matrix, &reset, UsdTimeCode::EarliestTime());
  if (request.contains("matrix") || request.contains("world_matrix")) {
    const auto& elements = request.at(request.contains("world_matrix") ? "world_matrix" : "matrix");
    if (elements.size() != 16) throw std::runtime_error("Expected a 4x4 matrix");
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
      matrix[r][c] = elements[r * 4 + c].get<double>();
      if (!std::isfinite(matrix[r][c])) throw std::runtime_error("Non-finite transform");
    }
    if (request.contains("world_matrix") && !reset) {
      UsdGeomXformCache cache(UsdTimeCode::EarliestTime());
      matrix *= invertible_inverse(cache.GetParentToWorldTransform(stage_->GetPrimAtPath(SdfPath(selected_path))));
    }
  } else {
    if (request.contains("rotation") || request.contains("scale")) {
      GfTransform original(matrix);
      auto old_rotation = original.GetRotation().Decompose(GfVec3d::ZAxis(), GfVec3d::YAxis(), GfVec3d::XAxis());
      auto rotation = request.contains("rotation") ? vector3(request.at("rotation")) : GfVec3d(old_rotation[2], old_rotation[1], old_rotation[0]);
      auto scale = request.contains("scale") ? vector3(request.at("scale")) : original.GetScale();
      if (scale[0] == 0 || scale[1] == 0 || scale[2] == 0) throw std::runtime_error("Scale cannot be zero");
      auto t = matrix.ExtractTranslation();
      matrix.SetScale(scale);
      matrix *= GfMatrix4d(GfRotation(GfVec3d(1, 0, 0), rotation[0]), GfVec3d(0));
      matrix *= GfMatrix4d(GfRotation(GfVec3d(0, 1, 0), rotation[1]), GfVec3d(0));
      matrix *= GfMatrix4d(GfRotation(GfVec3d(0, 0, 1), rotation[2]), GfVec3d(0));
      matrix.SetTranslateOnly(t);
    }
    if (request.contains("translation")) matrix.SetTranslateOnly(vector3(request.at("translation")));
    if (request.contains("world_delta")) {
      UsdGeomXformCache cache(UsdTimeCode::EarliestTime());
      auto selected = stage_->GetPrimAtPath(SdfPath(selected_path));
      auto parent = cache.GetParentToWorldTransform(selected);
      matrix.SetTranslateOnly(matrix.ExtractTranslation() + (reset ? GfMatrix4d(1) : invertible_inverse(parent)).TransformDir(vector3(request.at("world_delta"))));
    }
  }
  author_transform(xform, matrix, reset);
}

std::string Document::authored_path(const std::string& path) const {
  const SdfPath selected(path);
  const SdfPath instances("/RootNode/instances");
  if (!selected.HasPrefix(instances)) return path;
  auto instance = selected;
  while (instance.GetParentPath() != instances && instance != SdfPath::AbsoluteRootPath()) instance = instance.GetParentPath();
  if (instance.GetParentPath() != instances) throw std::runtime_error("Select a captured asset or an authored replacement");
  auto anchor = SdfPath(anchor_for(instance.GetString()));
  if (instance == selected) throw std::runtime_error("Select a mesh or light inside the captured instance to edit it");
  return selected.ReplacePrefix(instance, anchor).GetString();
}

std::string Document::anchor_for(const std::string& path) const {
  auto prim = stage_->GetPrimAtPath(SdfPath(path));
  if (!prim) throw std::runtime_error("Choose a captured asset");
  auto find_hash_root = [](const SdfPath& candidate) -> std::string {
    auto text = candidate.GetString();
    for (const auto& prefix : {std::string("/RootNode/meshes/mesh_"), std::string("/RootNode/lights/light_")}) {
      if (text.rfind(prefix, 0) == 0) return text.substr(0, text.find('/', prefix.size()));
    }
    return {};
  };
  for (auto p = prim; p && !p.IsPseudoRoot(); p = p.GetParent()) {
    auto root = find_hash_root(p.GetPath());
    if (!root.empty()) return root;
    for (auto spec : p.GetPrimStack()) {
      root = find_hash_root(spec->GetPath());
      if (!root.empty()) return root;
    }
  }
  throw std::runtime_error("Select a captured mesh or light to attach this asset");
}

std::string Document::parent_path(const std::string& path) const {
  auto prim = stage_->GetPrimAtPath(SdfPath(path));
  if (!prim || !UsdGeomXformable(prim)) throw std::runtime_error("Choose a transformable parent prim");
  // Authored assets retain their hierarchy. Capture geometry, however, only
  // exists in the editor: a game attachment belongs to the draw-call hash root.
  try { authored_asset(path); return authored_path(path); }
  catch (const std::runtime_error&) {}
  return anchor_for(path);
}

std::string Document::authored_asset(const std::string& path) const {
  auto prim = stage_->GetPrimAtPath(SdfPath(authored_path(path)));
  for (; prim && !prim.IsPseudoRoot(); prim = prim.GetParent()) {
    if (prim.GetPath().GetParentPath() == SdfPath("/RootNode/meshes") ||
        prim.GetPath().GetParentPath() == SdfPath("/RootNode/lights")) break;
    auto spec = edit_layer_->GetPrimAtPath(prim.GetPath());
    bool reference = false;
    auto flag = prim.GetAttribute(TfToken("IsRemixRef"));
    if (flag) flag.Get(&reference);
    if (spec && spec->GetSpecifier() == SdfSpecifierDef && (reference || UsdLuxLightAPI(prim) || prim.IsA<UsdGeomMesh>()))
      return prim.GetPath().GetString();
  }
  throw std::runtime_error("Choose a light or mesh authored in the active layer");
}

std::string Document::display_path(const std::string& path, const std::string& context) const {
  const SdfPath instances("/RootNode/instances");
  auto instance = SdfPath(context);
  if (!instance.HasPrefix(instances)) return path;
  while (instance.GetParentPath() != instances && !instance.IsAbsoluteRootPath()) instance = instance.GetParentPath();
  const SdfPath anchor(anchor_for(context));
  return SdfPath(path).HasPrefix(anchor) ? SdfPath(path).ReplacePrefix(anchor, instance).GetString() : path;
}

Json Document::parent_candidates(const std::string& path) const {
  require_stage();
  const SdfPath asset(authored_asset(path));
  Json candidates = Json::array();
  for (auto prim : stage_->Traverse()) {
    if (!UsdGeomXformable(prim) || prim.GetPath() == SdfPath(standalone_root)) continue;
    try {
      if (SdfPath(parent_path(prim.GetPath().GetString())).HasPrefix(asset)) continue;
      if (UsdGeomImageable(prim).ComputeVisibility() == UsdGeomTokens->invisible) continue;
      candidates.push_back({{"path", prim.GetPath().GetString()}, {"type", prim.GetTypeName().GetString()}});
    } catch (const std::runtime_error&) {}
  }
  return {{"parents", candidates}};
}

std::string Document::set_parent(const Json& request) {
  const auto context = required(request, "path");
  const SdfPath old(authored_asset(context));
  const auto selected = stage_->GetPrimAtPath(SdfPath(display_path(old.GetString(), context)));
  UsdGeomXformCache cache(UsdTimeCode::EarliestTime());
  const auto world = cache.GetLocalToWorldTransform(selected);
  const auto target = request.value("parent", std::string());
  const SdfPath parent(target.empty() ? standalone_root : parent_path(target));
  if (parent.HasPrefix(old)) throw std::runtime_error("An object cannot be parented to itself or its descendants");
  if (target.empty()) UsdGeomXform::Define(stage_, parent);
  const auto displayed_parent = target.empty() ? parent.GetString() : display_path(parent.GetString(), target);
  const auto parent_world = cache.GetLocalToWorldTransform(stage_->GetPrimAtPath(SdfPath(displayed_parent)));
  const auto local = world * invertible_inverse(parent_world);
  const auto next = old.GetParentPath() == parent ? old : SdfPath(unique_child(stage_, parent.GetString(), old.GetName()));
  define_ancestors(edit_layer_, parent);
  if (next != old) {
    SdfCreatePrimInLayer(edit_layer_, parent);
    if (!SdfCopySpec(edit_layer_, old, edit_layer_, next)) throw std::runtime_error("Could not attach object");
    SdfBatchNamespaceEdit removal; removal.Add(SdfNamespaceEdit::Remove(old));
    if (!edit_layer_->Apply(removal)) throw std::runtime_error("Could not remove the previous attachment");
  }
  auto xform = UsdGeomXformable(stage_->GetPrimAtPath(next));
  author_transform(xform, local, false);
  // A replacement moved to a different parent becomes an added attachment.
  // Restoring the old draw call must not later remove this moved asset.
  if (next != old && xform.GetPrim().GetCustomDataByKey(replacement_tag) == VtValue(true))
    xform.GetPrim().SetCustomDataByKey(replacement_tag, VtValue(false));
  if (!target.empty()) {
    std::string anchor;
    try { anchor = anchor_for(target); } catch (const std::runtime_error&) {}
    if (!anchor.empty()) {
      auto preserve = stage_->OverridePrim(SdfPath(anchor)).CreateAttribute(TfToken("preserveOriginalDrawCall"), SdfValueTypeNames->Int, true);
      int previous = 1; preserve.Get(&previous); preserve.Set(previous);
    }
  }
  return SdfPath(displayed_parent).AppendChild(next.GetNameToken()).GetString();
}

std::string Document::add_reference(const Json& request) {
  const auto selected = request.value("path", std::string());
  if (request.value("replace", false) && selected.empty()) throw std::runtime_error("Select a captured mesh to replace");
  const auto anchor = selected.empty() ? std::string(standalone_root) : anchor_for(selected);
  // Remember the visible instance before replacement can deactivate the picked
  // capture geometry. Editing stays shared, but the gizmo uses this instance.
  const auto displayed_anchor = selected.empty() ? anchor : display_path(anchor, selected);
  if (selected.empty()) UsdGeomXform::Define(stage_, SdfPath(anchor));
  fs::path asset = fs::absolute(required(request, "asset"));
  auto source = UsdStage::Open(asset.string());
  if (!source || !source->GetDefaultPrim()) throw std::runtime_error("Asset requires a valid USD default prim; ingest it first");
  define_ancestors(edit_layer_, SdfPath(anchor));
  auto root = stage_->OverridePrim(SdfPath(anchor));
  bool replace = request.value("replace", false);
  if (replace) {
    // Persist only the opinions replacement changes, so reverting after a
    // save/reopen preserves unrelated transform, material and visibility edits.
    auto backup = SdfLayer::CreateAnonymous();
    auto local_root = edit_layer_->GetPrimAtPath(SdfPath(anchor));
    const VtDictionary saved = local_root->GetCustomData();
    auto existing = saved.find(replacement_restore.GetString());
    if (existing != saved.end() && existing->second.IsHolding<std::string>()) {
      if (!backup->ImportFromString(existing->second.Get<std::string>()))
        throw std::runtime_error("Could not read mesh replacement restore data");
    } else {
      auto preserve = edit_layer_->GetAttributeAtPath(SdfPath(anchor).AppendProperty(TfToken("preserveOriginalDrawCall")));
      const bool legacy_replaced = preserve && preserve->GetDefaultValue() == VtValue(0);
      // Upgrade a replacement selected from an older project before adding
      // another one. Its empty reference list/inactive opinions are suppression,
      // not the original state to restore.
      if (legacy_replaced) {
        const auto previous = mesh_replacement(selected);
        if (!previous.empty()) for (const auto& ref : previous.at("references"))
          stage_->GetPrimAtPath(SdfPath(ref.get<std::string>())).SetCustomDataByKey(replacement_tag, VtValue(true));
        for (auto child : local_root->GetNameChildren())
          if (child->GetSpecifier() == SdfSpecifierOver && child->HasActive() && !child->GetActive())
            SdfCreatePrimInLayer(backup, child->GetPath());
      }
      auto record = SdfCreatePrimInLayer(backup, SdfPath(anchor));
      auto refs = local_root->GetReferenceList();
      if (local_root->HasInfo(SdfFieldKeys->References) && !(legacy_replaced && refs.IsExplicit() && refs.GetExplicitItems().empty()))
        record->SetInfo(SdfFieldKeys->References, local_root->GetInfo(SdfFieldKeys->References));
    }
    // Inline captured meshes survive clearing reference arcs. Suppress original
    // children while retaining the user's authored attachments.
    for (auto child : root.GetChildren()) {
      auto spec = edit_layer_->GetPrimAtPath(child.GetPath());
      if (!spec || spec->GetSpecifier() != SdfSpecifierDef) {
        if (!backup->GetPrimAtPath(child.GetPath())) {
          auto record = SdfCreatePrimInLayer(backup, child.GetPath());
          if (spec && spec->HasActive()) record->SetActive(spec->GetActive());
        }
        child.SetActive(false);
      }
    }
    root.SetCustomDataByKey(replacement_restore, VtValue(layer_text(backup)));
    root.GetReferences().SetReferences({});
  }
  if (!selected.empty()) {
    auto preserve = root.CreateAttribute(TfToken("preserveOriginalDrawCall"), SdfValueTypeNames->Int, true);
    int previous = 1; preserve.Get(&previous); preserve.Set(replace ? 0 : previous);
  }
  auto path = unique_child(stage_, anchor, "ref_" + asset.stem().string());
  auto prim = UsdGeomXform::Define(stage_, SdfPath(path)).GetPrim();
  prim.GetReferences().AddReference(relative_asset(asset, edit_layer_));
  prim.CreateAttribute(TfToken("IsRemixRef"), SdfValueTypeNames->Bool, true).Set(true);
  // Distinguish added attachments from replacements, including after reopening.
  prim.SetCustomDataByKey(replacement_tag, VtValue(replace));
  GfMatrix4d normalization;
  normalization.SetScale(UsdGeomGetStageMetersPerUnit(source) / UsdGeomGetStageMetersPerUnit(stage_));
  if (UsdGeomGetStageUpAxis(source) != UsdGeomGetStageUpAxis(stage_)) {
    normalization *= GfMatrix4d(GfRotation(GfVec3d(1, 0, 0), UsdGeomGetStageUpAxis(source) == UsdGeomTokens->y ? 90.0 : -90.0), GfVec3d(0));
  }
  // A stronger xformOpOrder replaces the referenced root's entire transform.
  // Include that transform (notably FBX's cm-to-m conversion) before converting
  // the asset's units/up axis into the project coordinate system.
  GfMatrix4d asset_transform(1);
  bool reset = false;
  if (auto source_xform = UsdGeomXformable(source->GetDefaultPrim()))
    source_xform.GetLocalTransformation(&asset_transform, &reset, UsdTimeCode::EarliestTime());
  author_transform(UsdGeomXformable(prim), asset_transform * normalization, false);
  if (request.contains("translation")) {
    if (selected.empty()) {
      UsdGeomXformCache cache(UsdTimeCode::EarliestTime());
      auto delta = vector3(request.at("translation")) - cache.GetLocalToWorldTransform(prim).ExtractTranslation();
      set_transform({{"path", path}, {"world_delta", {delta[0], delta[1], delta[2]}}});
    } else set_transform({{"path", path}, {"translation", request.at("translation")}});
  }
  return SdfPath(path).ReplacePrefix(SdfPath(anchor), SdfPath(displayed_anchor)).GetString();
}

Json Document::mesh_replacement(const std::string& path) const {
  std::string anchor;
  try { anchor = anchor_for(path); } catch (const std::runtime_error&) { return {}; }
  if (SdfPath(anchor).GetParentPath() != SdfPath("/RootNode/meshes")) return {};
  auto root = edit_layer_->GetPrimAtPath(SdfPath(anchor));
  if (!root) return {};
  // Only expose this for the captured mesh, its replacement or the hash root;
  // selecting an independently attached light/mesh must not offer to remove it.
  SdfPath selected;
  try { selected = SdfPath(authored_path(path)); } catch (const std::runtime_error&) { selected = SdfPath(anchor); }
  if (!selected.HasPrefix(SdfPath(anchor))) return {};
  for (auto prim = stage_->GetPrimAtPath(selected); prim && prim.GetPath() != SdfPath(anchor); prim = prim.GetParent()) {
    if (UsdLuxLightAPI(prim)) return {};
    auto spec = edit_layer_->GetPrimAtPath(prim.GetPath());
    if (spec) {
      const VtDictionary data = spec->GetCustomData();
      auto tag = data.find(replacement_tag.GetString());
      if (tag != data.end() && tag->second == VtValue(false)) return {};
    }
  }
  auto child = selected;
  while (child != SdfPath(anchor) && child.GetParentPath() != SdfPath(anchor)) child = child.GetParentPath();
  auto selected_spec = edit_layer_->GetPrimAtPath(child);
  if (child != SdfPath(anchor) && selected_spec && selected_spec->GetSpecifier() == SdfSpecifierDef) {
    bool reference = false;
    auto prim = stage_->GetPrimAtPath(child);
    if (auto flag = prim.GetAttribute(TfToken("IsRemixRef"))) flag.Get(&reference);
    VtDictionary data = selected_spec->GetCustomData();
    if (!reference || data[replacement_tag.GetString()] == VtValue(false)) return {};
  }
  Json references = Json::array(), legacy = Json::array();
  for (auto spec : root->GetNameChildren()) {
    if (spec->GetSpecifier() != SdfSpecifierDef) continue;
    const VtDictionary data = spec->GetCustomData();
    auto tag = data.find(replacement_tag.GetString());
    if (tag != data.end()) {
      if (tag->second == VtValue(true)) references.push_back(spec->GetPath().GetString());
    } else {
      auto flag = edit_layer_->GetAttributeAtPath(spec->GetPath().AppendProperty(TfToken("IsRemixRef")));
      if (flag && flag->GetDefaultValue() == VtValue(true)) legacy.push_back(spec->GetPath().GetString());
    }
  }
  // Older projects did not distinguish Add from Replace. Revert the picked
  // reference, leaving any other unmarked attachments intact. A root selection
  // is unambiguous only when there is one legacy reference.
  auto preserve = edit_layer_->GetAttributeAtPath(SdfPath(anchor).AppendProperty(TfToken("preserveOriginalDrawCall")));
  if (preserve && preserve->GetDefaultValue() == VtValue(0)) {
    for (const auto& candidate : legacy)
      if (candidate == child.GetString() || (references.empty() && legacy.size() == 1 && child == SdfPath(anchor)))
        references.push_back(candidate);
  }
  const VtDictionary data = root->GetCustomData();
  if (references.empty() && !data.count(replacement_restore.GetString())) return {};
  return {{"anchor", anchor}, {"references", references}};
}

std::string Document::revert_mesh(const std::string& path) {
  const auto replacement = mesh_replacement(path);
  if (replacement.empty()) throw std::runtime_error("Select a mesh replacement in the active authoring layer");
  const SdfPath anchor(replacement.at("anchor").get<std::string>());
  const SdfPath displayed(display_path(anchor.GetString(), path));
  auto root = stage_->GetPrimAtPath(anchor);
  auto local_root = edit_layer_->GetPrimAtPath(anchor);
  const VtDictionary data = local_root->GetCustomData();
  auto saved = data.find(replacement_restore.GetString());
  auto backup = SdfLayer::CreateAnonymous();
  if (saved != data.end()) {
    if (!saved->second.IsHolding<std::string>() || !backup->ImportFromString(saved->second.Get<std::string>()) || !backup->GetPrimAtPath(anchor))
      throw std::runtime_error("Could not read mesh replacement restore data");
  }
  for (const auto& reference : replacement.at("references")) {
    const SdfPath ref(reference.get<std::string>());
    // Keep user-authored attachments even when they were parented inside the
    // imported model. Move their outermost authored roots to the draw-call root.
    std::vector<SdfPath> attachments;
    auto range = UsdPrimRange(stage_->GetPrimAtPath(ref));
    for (auto it = range.begin(); it != range.end(); ++it) {
      if (it->GetPath() == ref) continue;
      auto spec = edit_layer_->GetPrimAtPath(it->GetPath());
      // define_ancestors also creates untyped defs along the attachment path;
      // these are imported model hierarchy, not separately authored assets.
      if (spec && spec->GetSpecifier() == SdfSpecifierDef && !spec->GetTypeName().IsEmpty() &&
          (UsdLuxLightAPI(*it) || it->IsA<UsdGeomMesh>() || it->GetAttribute(TfToken("IsRemixRef")))) {
        attachments.push_back(it->GetPath()); it.PruneChildren();
      }
    }
    for (const auto& attachment : attachments)
      set_parent({{"path", attachment.ReplacePrefix(anchor, displayed).GetString()}, {"parent", displayed.GetString()}});
    SdfBatchNamespaceEdit removal; removal.Add(SdfNamespaceEdit::Remove(ref));
    if (!edit_layer_->Apply(removal)) throw std::runtime_error("Could not remove mesh replacement");
  }
  if (saved != data.end()) {
    auto record = backup->GetPrimAtPath(anchor);
    if (record->HasInfo(SdfFieldKeys->References)) local_root->SetInfo(SdfFieldKeys->References, record->GetInfo(SdfFieldKeys->References));
    else local_root->ClearInfo(SdfFieldKeys->References);
    for (auto child : record->GetNameChildren()) {
      auto spec = edit_layer_->GetPrimAtPath(child->GetPath());
      if (!spec) continue;
      if (child->HasActive()) spec->SetActive(child->GetActive());
      else spec->ClearActive();
    }
  } else {
    // Compatibility with replacements authored before restore records existed.
    auto refs = local_root->GetReferenceList();
    if (refs.IsExplicit() && refs.GetExplicitItems().empty()) root.GetReferences().ClearReferences();
    for (auto child : local_root->GetNameChildren())
      if (child->GetSpecifier() == SdfSpecifierOver && child->HasActive() && !child->GetActive()) child->ClearActive();
  }
  root.ClearCustomDataByKey(replacement_restore);
  root.CreateAttribute(TfToken("preserveOriginalDrawCall"), SdfValueTypeNames->Int, true).Set(1);
  // Keep selection on the restored geometry in the same visible instance.
  for (auto prim : UsdPrimRange(stage_->GetPrimAtPath(displayed))) {
    if (!prim.IsA<UsdGeomMesh>()) continue;
    try { authored_asset(prim.GetPath().GetString()); } catch (const std::runtime_error&) { return prim.GetPath().GetString(); }
  }
  return displayed.GetString();
}

Json Document::light_defaults(const std::string& type) const {
  require_stage();
  // Match Toolkit's omni.flux.light_creator.widget/setup_ui.py. Dimensions are
  // physical sizes, converted into this stage's units rather than USD fallbacks.
  const auto radius = 5.0 / UsdGeomGetStageMetersPerUnit(stage_);
  Json properties{{"inputs:color", {1, 1, 1}}, {"inputs:exposure", 0.0}, {"inputs:enableColorTemperature", false}};
  if (type == "SphereLight" || type == "SpotLight") {
    properties["inputs:radius"] = radius; properties["inputs:intensity"] = 100.0;
    if (type == "SpotLight") properties["inputs:shaping:cone:angle"] = 35.0;
  } else if (type == "RectLight") {
    properties["inputs:width"] = radius * 2; properties["inputs:height"] = radius * 2; properties["inputs:intensity"] = 400.0;
  } else if (type == "DiskLight") {
    properties["inputs:radius"] = radius; properties["inputs:intensity"] = 500.0;
  } else if (type == "CylinderLight") {
    properties["inputs:radius"] = radius; properties["inputs:length"] = radius * 2; properties["inputs:intensity"] = 140.0;
  } else if (type == "DistantLight") {
    properties["inputs:angle"] = 1.0; properties["inputs:intensity"] = 25.0;
  } else if (type == "DomeLight") {
    // Toolkit's creator does not offer dome lights; use the USD schema default.
    properties["inputs:intensity"] = 1.0;
  } else throw std::runtime_error("Unsupported light type");
  return {{"type", type}, {"properties", properties}};
}

std::string Document::add_light(const Json& request) {
  const auto selected = request.value("path", std::string());
  const auto anchor = selected.empty() ? std::string(standalone_root) : anchor_for(selected);
  if (selected.empty()) UsdGeomXform::Define(stage_, SdfPath(anchor));
  auto type = request.value("type", std::string("SphereLight"));
  define_ancestors(edit_layer_, SdfPath(anchor));
  auto path = unique_child(stage_, anchor, request.value("name", type));
  auto sdf = SdfPath(path);
  UsdPrim light;
  if (type == "SphereLight" || type == "SpotLight") light = UsdLuxSphereLight::Define(stage_, sdf).GetPrim();
  else if (type == "RectLight") light = UsdLuxRectLight::Define(stage_, sdf).GetPrim();
  else if (type == "DiskLight") light = UsdLuxDiskLight::Define(stage_, sdf).GetPrim();
  else if (type == "CylinderLight") light = UsdLuxCylinderLight::Define(stage_, sdf).GetPrim();
  else if (type == "DistantLight") light = UsdLuxDistantLight::Define(stage_, sdf).GetPrim();
  else if (type == "DomeLight") light = UsdLuxDomeLight::Define(stage_, sdf).GetPrim();
  else throw std::runtime_error("Unsupported light type");
  if (type == "SpotLight") UsdLuxShapingAPI::Apply(light);
  const auto defaults = light_defaults(type).at("properties");
  for (auto it = defaults.begin(); it != defaults.end(); ++it) set_value(light.GetAttribute(TfToken(it.key())), it.value());
  if (request.contains("intensity")) UsdLuxLightAPI(light).CreateIntensityAttr().Set(request.at("intensity").get<float>());
  update_light_extent(light);
  if (!selected.empty()) {
    auto preserve = stage_->OverridePrim(SdfPath(anchor)).CreateAttribute(TfToken("preserveOriginalDrawCall"), SdfValueTypeNames->Int, true);
    int previous = 1; preserve.Get(&previous); preserve.Set(previous);
  }
  auto position = vector3(request.value("translation", Json::array({0, 0, 0})));
  if (request.contains("world_matrix")) {
    set_transform({{"path", path}, {"world_matrix", request.at("world_matrix")}});
  } else if (selected.empty()) {
    UsdGeomXformCache cache(UsdTimeCode::EarliestTime());
    auto delta = position - cache.GetLocalToWorldTransform(light).ExtractTranslation();
    set_transform({{"path", path}, {"world_delta", {delta[0], delta[1], delta[2]}}});
  } else set_transform({{"path", path}, {"translation", {position[0], position[1], position[2]}}});
  return path;
}

void Document::replace_material(const Json& request) {
  auto path = SdfPath(required(request, "path"));
  auto selected = stage_->GetPrimAtPath(path);
  if (!selected) throw std::runtime_error("Select a material or mesh");
  auto material = UsdShadeMaterial(selected);
  if (!material && selected.IsA<UsdShadeShader>()) material = UsdShadeMaterial(selected.GetParent());
  if (!material) material = UsdShadeMaterialBindingAPI(selected).ComputeBoundMaterial();
  if (!material) throw std::runtime_error("Selection has no bound material");
  auto asset = fs::absolute(required(request, "asset"));
  auto source = UsdStage::Open(asset.string());
  if (!source || !UsdShadeMaterial(source->GetDefaultPrim())) throw std::runtime_error("Choose an ingested material USD");
  auto flattened = source->Flatten();
  UsdUtilsModifyAssetPaths(flattened, [&](const std::string& dependency) {
    if (dependency.empty()) return dependency;
    auto resolved = fs::path(dependency);
    if (!resolved.is_absolute()) resolved = asset.parent_path() / resolved;
    return relative_asset(resolved, edit_layer_);
  });
  // A reference alone is weaker than the capture's local shader inputs.
  if (!SdfCreatePrimInLayer(edit_layer_, material.GetPath().GetParentPath()))
    throw std::runtime_error("Could not author replacement material ancestors");
  if (!SdfCopySpec(flattened, source->GetDefaultPrim().GetPath(), edit_layer_, material.GetPath()))
    throw std::runtime_error("Could not author replacement material");
}

void Document::save() {
  require_stage();
  for (auto layer : stage_->GetLayerStack(false)) {
    auto text = layer_text(layer);
    if (saved_layers_[layer->GetIdentifier()] == text) continue;
    if (is_capture(layer)) throw std::runtime_error("Refusing to write a capture layer");
    atomic_save_layer(layer);
    saved_layers_[layer->GetIdentifier()] = text;
  }
}

void Document::package(const fs::path& directory) {
  export_mod(directory, false);
}

void Document::export_mod(const fs::path& directory, bool link_assets) {
  require_stage();
  if (fs::exists(directory)) throw std::runtime_error("Package destination must not already exist");
  auto temp = fs::absolute(directory).parent_path() / (directory.filename().string() + ".building");
  if (fs::exists(temp)) throw std::runtime_error("An unfinished package exists: " + temp.string());
  fs::create_directories(temp / "assets");
  try {
    // Keep the workfile's authored opinions and direct replacement sublayers. Expanding
    // GetLayerStack into a new sublayer list would apply nested layers more than once.
    auto root = stage_->GetRootLayer();
    auto snapshot = SdfLayer::CreateAnonymous("package.usda");
    snapshot->TransferContent(root);
    std::vector<std::string> roots;
    std::vector<SdfLayerOffset> offsets;
    size_t sublayer_index = 0;
    for (const auto& path : root->GetSubLayerPaths()) {
      auto layer = SdfLayer::FindOrOpenRelativeToLayer(root, path);
      if (!layer) throw std::runtime_error("Missing project layer: " + std::string(path));
      if (!is_capture(layer)) {
        roots.push_back(layer->GetRealPath());
        offsets.push_back(root->GetSubLayerOffsets()[sublayer_index]);
      }
      ++sublayer_index;
    }
    snapshot->SetSubLayerPaths(roots);
    auto metadata = snapshot->GetCustomLayerData();
    metadata["lightspeed_layer_type"] = VtValue(std::string("replacement"));
    metadata.erase("omni_layer");
    metadata.erase(capture_catalog);
    snapshot->SetCustomLayerData(metadata);
    replacement_metadata(snapshot, stage_);
    UsdUtilsModifyAssetPaths(snapshot, [&](const std::string& asset) {
      return asset.empty() ? asset : SdfComputeAssetPathRelativeToLayer(root, asset);
    });
    for (size_t i = 0; i < offsets.size(); ++i) snapshot->SetSubLayerOffset(offsets[i], i);
    auto package_root = temp / "mod.usda";
    if (!snapshot->Export(package_root.string())) throw std::runtime_error("Could not create package root");
    std::vector<SdfLayerRefPtr> layers;
    std::vector<std::string> asset_files, missing;
    if (!UsdUtilsComputeAllDependencies(SdfAssetPath(package_root.string()), &layers, &asset_files, &missing) || !missing.empty())
      throw std::runtime_error("Resolve missing package dependencies before exporting" + (missing.empty() ? std::string() : ": " + missing.front()));
    std::map<std::string, fs::path> destinations;
    size_t index = 0;
    auto asset_id = [&](const std::string& source) {
      if (!link_assets) return std::to_string(index++);
      // Stable names allow live publication to replace small USD files atomically,
      // without invalidating links used by a running game's previous mod revision.
      uint64_t hash = 14695981039346656037ull;
      for (unsigned char c : source) { hash ^= c; hash *= 1099511628211ull; }
      return std::to_string(hash);
    };
    for (auto layer : layers) {
      if (is_capture(layer)) throw std::runtime_error("A replacement layer depends on a capture; remove that capture sublayer before packaging");
      auto source = fs::weakly_canonical(layer->GetRealPath()).string();
      destinations[source] = fs::equivalent(source, package_root) ? fs::path("mod.usda") :
        fs::path("assets") / (asset_id(source) + "_" + fs::path(source).filename().string());
    }
    // MDL imports are not USD asset dependencies. Keep each material module bundle's
    // names and directory structure so sibling and nvidia module imports still resolve.
    std::map<fs::path, fs::path> bundles;
    std::set<std::string> files;
    for (const auto& asset : asset_files) {
      fs::path source = fs::weakly_canonical(asset);
      if (source.extension() == ".mdl") {
        auto parent = source.parent_path();
        if (!bundles.count(parent)) {
          auto target = fs::path("assets") / ("materials_" + asset_id(parent.string()));
          bundles[parent] = target;
          for (const auto& entry : fs::recursive_directory_iterator(parent)) {
            if (!entry.is_regular_file()) continue;
            auto relative = fs::relative(entry.path(), parent);
            if (entry.path().extension() != ".mdl" && relative.begin()->string() != "PACKAGE-LICENSES") continue;
            auto key = fs::weakly_canonical(entry.path()).string();
            destinations[key] = target / relative; files.insert(key);
          }
        }
      }
      if (!destinations.count(source.string())) destinations[source.string()] = fs::path("assets") / (asset_id(source.string()) + "_" + source.filename().string());
      files.insert(source.string());
    }
    for (const auto& source : files) {
      auto target = temp / destinations.at(source);
      fs::create_directories(target.parent_path());
      if (link_assets) fs::create_symlink(source, target);
      else fs::copy_file(source, target);
    }
    std::set<std::string> written;
    for (auto layer : layers) {
      const auto source = fs::weakly_canonical(layer->GetRealPath()).string();
      if (!written.insert(source).second) continue;
      const auto destination = destinations.at(source);
      auto copy = SdfLayer::CreateAnonymous(fs::path(source).filename().string());
      copy->TransferContent(layer);
      const auto project_layers = stage_->GetLayerStack(false);
      if (destination == fs::path("mod.usda") || std::find(project_layers.begin(), project_layers.end(), layer) != project_layers.end()) {
        replacement_metadata(copy, stage_);
        normalize_replacement(copy, stage_);
      }
      // Old projects may define attachments beneath capture-only `over` parents.
      // Make those ancestors explicit in the capture-free mod as Toolkit does.
      std::vector<SdfPath> definitions;
      copy->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& path) {
        auto spec = copy->GetPrimAtPath(path);
        if (path.IsPrimPath() && spec && spec->GetSpecifier() == SdfSpecifierDef) definitions.push_back(path);
      });
      for (const auto& path : definitions) define_ancestors(copy, path.GetParentPath());
      UsdUtilsModifyAssetPaths(copy, [&](const std::string& asset) {
        if (asset.empty()) return asset;
        auto resolved = SdfComputeAssetPathRelativeToLayer(layer, asset);
        auto found = destinations.find(fs::weakly_canonical(resolved).string());
        if (found == destinations.end()) throw std::runtime_error("Unresolved package asset: " + asset);
        // Lexical paths must not follow live asset symlinks back out of the mod.
        return (temp / found->second).lexically_relative((temp / destination).parent_path()).generic_string();
      });
      const auto layer_offsets = layer->GetSubLayerOffsets();
      for (size_t i = 0; i < layer_offsets.size(); ++i) copy->SetSubLayerOffset(layer_offsets[i], i);
      if (!copy->Export((temp / destination).string())) throw std::runtime_error("Failed to write packaged USD");
    }
    fs::rename(temp, fs::absolute(directory));
  } catch (...) { fs::remove_all(temp); throw; }
}

Json Document::scene() const { require_stage(); return compose_scene(stage_, revision_); }
Json Document::ingest(const Json& request) { require_stage(); return ingest_asset(request, project_path_.parent_path()); }

bool Document::is_light_edit(const Json& request) const {
  if (!stage_) return false;
  const auto op = request.value("op", std::string());
  if (op == "add_light") return true;
  if (op == "batch") {
    for (const auto& command : request.at("commands")) if (!is_light_edit(command)) return false;
    return true;
  }
  const std::set<std::string> edits{"transform", "visibility", "set_property", "set_authoring_property",
    "reset_authoring_property", "set_light_type", "set_parent", "duplicate", "delete"};
  if (!edits.count(op)) return false;
  try {
    const auto selected = request.at("path").get<std::string>();
    const auto path = (op == "set_parent" || op == "duplicate") ? authored_asset(selected) : authored_path(selected);
    const auto prim = stage_->GetPrimAtPath(SdfPath(path));
    if (!prim || !prim.IsActive() || !UsdLuxLightAPI(prim) || prim.IsA<UsdGeomMesh>()) return false;
    // Transforming/reparenting a light can also affect geometry parented to it.
    // Use the full scene path whenever its subtree contains anything but lights.
    for (auto child : UsdPrimRange(prim, UsdTraverseInstanceProxies()))
      if (child != prim && (!UsdLuxLightAPI(child) || child.IsA<UsdGeomMesh>())) return false;
    return true;
  } catch (const std::exception&) { return false; }
}

void Document::mutate(const Json& request, Json& result) {
  const auto op = required(request, "op");
  if (op == "set_property") set_property(request);
  else if (op == "set_authoring_property") set_authoring_property(request);
  else if (op == "reset_authoring_property") set_authoring_property(request, true);
  else if (op == "set_light_type") set_light_type(request);
  else if (op == "transform") set_transform(request);
  else if (op == "set_parent") result["selection"] = set_parent(request);
  else if (op == "add_mesh" || op == "replace_mesh") {
    auto data = request; data["replace"] = op == "replace_mesh";
    result["selection"] = add_reference(data);
  } else if (op == "add_light") result["selection"] = add_light(request);
  else if (op == "revert_mesh") result["selection"] = revert_mesh(required(request, "path"));
  else if (op == "replace_material") replace_material(request);
  else if (op == "visibility") {
    auto prim = stage_->GetPrimAtPath(SdfPath(authored_path(required(request, "path"))));
    auto imageable = UsdGeomImageable(prim);
    if (!imageable) throw std::runtime_error("Selection has no visibility");
    imageable.CreateVisibilityAttr().Set(request.value("visible", true) ? UsdGeomTokens->inherited : UsdGeomTokens->invisible);
  } else if (op == "delete") {
    auto path = SdfPath(authored_path(required(request, "path")));
    auto spec = edit_layer_->GetPrimAtPath(path);
    if (!spec || spec->GetSpecifier() != SdfSpecifierDef) throw std::runtime_error("Delete an authored asset; use visibility to hide a captured mesh");
    stage_->GetPrimAtPath(path).SetActive(false);
    result["selection"] = "";
  } else if (op == "duplicate") {
    const auto context = required(request, "path");
    auto path = SdfPath(authored_asset(context));
    auto spec = edit_layer_->GetPrimAtPath(path);
    if (!spec || spec->GetSpecifier() != SdfSpecifierDef) throw std::runtime_error("Duplicate an authored asset rather than a capture instance");
    auto next = unique_child(stage_, path.GetParentPath().GetString(), path.GetName() + "_copy");
    if (!SdfCopySpec(edit_layer_, path, edit_layer_, SdfPath(next))) throw std::runtime_error("Could not duplicate asset");
    result["selection"] = display_path(next, context);
  } else if (op == "remap") {
    const auto from = required(request, "from"), to = required(request, "to");
    UsdUtilsModifyAssetPaths(edit_layer_, [&](const std::string& path) {
      return path.rfind(from, 0) == 0 ? to + path.substr(from.size()) : path;
    });
  } else if (op == "batch") {
    for (const auto& command : request.at("commands")) mutate(command, result);
  } else throw std::runtime_error("Unknown edit command: " + op);
}

Json Document::dispatch(const Json& request) {
  const auto op = required(request, "op");
  const auto before = revision_;
  if (op == "open" || op == "create") last_edit_lights_only_ = false;
  Json result = Json::object();
  if (op == "open") open(required(request, "path"));
  else if (op == "create") create(required(request, "capture"), required(request, "path"));
  else if (op == "add_capture") select_capture(required(request, "path"), true);
  else if (op == "select_capture") select_capture(required(request, "path"), false);
  else if (op == "save") save();
  else if (op == "package") package(required(request, "path"));
  else if (op == "game_link_status" || op == "game_link" || op == "game_sync" || op == "game_unlink") return game_link(request);
  else if (op == "undo") undo();
  else if (op == "redo") redo();
  else if (op == "select_layer") select_layer(required(request, "layer"));
  else if (op == "inspect") return inspect(required(request, "path"));
  else if (op == "parent_candidates") return parent_candidates(required(request, "path"));
  else if (op == "light_defaults") return light_defaults(required(request, "type"));
  else if (op == "scene") return scene();
  else if (op == "lights") { require_stage(); return compose_lights(stage_, revision_); }
  else if (op == "ingest") return ingest(request);
  else if (op != "describe") edit(request.value("label", op), [&] { mutate(request, result); }, is_light_edit(request));
  result["document"] = describe();
  result["scene_update"] = revision_ == before ? "none" : last_edit_lights_only_ ? "lights" : "full";
  return result;
}
}
