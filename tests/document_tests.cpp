#include "document.h"
#include "scene_view.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

using editor::Json;
namespace fs = std::filesystem;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejects(Function function, const std::string& label) {
  bool rejected = false;
  try { function(); } catch (const std::exception&) { rejected = true; }
  check(rejected, label);
}
std::string text(const fs::path& path) {
  std::ifstream input(path); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void material_sampling(const fs::path& root) {
  editor::Document document;
  document.create(fs::path(FIXTURE_DIR) / "capture.usda", root / "sampling/project.usda");
  auto check_modes = [&](int filter, int u, int v) {
    const auto material = document.scene()["materials"][0];
    check(material["filter_mode"] == filter && material["wrap_mode_u"] == u && material["wrap_mode_v"] == v,
      "Material sampling must preserve authored modes or use Remix's linear/repeat defaults");
  };
  check_modes(1, 1, 1);
  auto shader = UsdShadeShader(document.stage()->GetPrimAtPath(SdfPath("/RootNode/Looks/mat_0123456789ABCDEF/Shader")));
  auto filter = shader.CreateInput(TfToken("filter_mode"), SdfValueTypeNames->UInt).GetAttr();
  auto u = shader.CreateInput(TfToken("wrap_mode_u"), SdfValueTypeNames->UInt).GetAttr();
  auto v = shader.CreateInput(TfToken("wrap_mode_v"), SdfValueTypeNames->Int).GetAttr();
  filter.Set(0u); u.Set(2u); v.Set(0); // Nearest, mirrored U, clamped V.
  check_modes(0, 2, 0);
  filter.Set(1u); u.Set(0u); v.Set(3); // Linear, clamped U, clipped V.
  check_modes(1, 0, 3);
  filter.Set(2u); u.Set(4u); v.Set(-1);
  check_modes(1, 1, 1);
  filter.Block(); u.Block(); v.Block();
  check_modes(1, 1, 1);
}
// A project linked into a game's mods folder is opened through the link, but
// its capture paths are relative to the real directory.
void linked_project(const fs::path& root) {
  const auto directory = root / "linked";
  fs::create_directories(directory / "game/rtx-remix/captures");
  fs::create_directories(directory / "game/rtx-remix/mods");
  fs::create_directories(directory / "projects/real");
  fs::copy_file(fs::path(FIXTURE_DIR) / "capture.usda", directory / "game/rtx-remix/captures/capture.usda");
  const auto link = directory / "game/rtx-remix/mods/linked";
  std::error_code error;
  fs::create_directory_symlink(directory / "projects/real", link, error);
#ifdef _WIN32
  // Without symlink privileges, a junction behaves the same way for ".." paths.
  if (error) {
    const auto command = "mklink /J \"" + link.string() + "\" \"" + (directory / "projects/real").string() + "\" >nul 2>&1";
    if (std::system(command.c_str()) == 0) error.clear();
  }
#endif
  if (error || !fs::is_directory(link)) { std::cout << "skipped linked project: cannot create a directory link\n"; return; }
  editor::Document created;
  created.create(directory / "game/rtx-remix/captures/capture.usda", directory / "projects/real/project.usda");
  editor::Document document;
  document.open(link / "project.usda");
  const auto captures = document.describe()["captures"];
  check(captures.size() == 1 && !captures[0]["missing"].get<bool>(), "A project opened through a link must find its capture");
  check(!document.scene()["meshes"].empty(), "A project opened through a link must compose its capture");
}
void texture_resolution(const fs::path& root) {
  const auto directory = root / "texture_paths";
  fs::create_directories(directory / "reference/materials");
  fs::create_directories(directory / "reference/textures");
  fs::create_directories(directory / "project/maps");
  for (const auto& path : {directory / "reference/textures/diffuse map.dds", directory / "reference/textures/normal.dds", directory / "project/maps/override.dds"})
    std::ofstream(path) << "Texture resolution fixture";

  const auto material_file = directory / "reference/materials/material.usda";
  auto source = UsdStage::CreateNew(material_file.string());
  auto material = UsdShadeMaterial::Define(source, SdfPath("/Material"));
  source->SetDefaultPrim(material.GetPrim());
  auto shader = UsdShadeShader::Define(source, SdfPath("/Material/Shader"));
  shader.CreateInput(TfToken("diffuse_texture"), SdfValueTypeNames->Asset).Set(SdfAssetPath("..\\textures\\diffuse map.dds"));
  shader.CreateInput(TfToken("normalmap_texture"), SdfValueTypeNames->Asset).Set(SdfAssetPath("../textures/normal.dds"));
  source->GetRootLayer()->Save();
  const auto original_material = text(material_file);

  fs::copy_file(fs::path(FIXTURE_DIR) / "capture.usda", directory / "capture.usda");
  auto capture = UsdStage::Open((directory / "capture.usda").string());
  const auto material_path = SdfPath("/RootNode/Looks/mat_0123456789ABCDEF");
  capture->GetPrimAtPath(material_path).GetReferences().AddReference("reference/materials/material.usda");
  capture->GetRootLayer()->Save();
  const auto original_capture = text(directory / "capture.usda");

  editor::Document document;
  document.create(directory / "capture.usda", directory / "project/project.usda");
  auto attr = document.stage()->GetPrimAtPath(material_path.AppendChild(TfToken("Shader"))).GetAttribute(TfToken("inputs:diffuse_texture"));
  // A stronger metadata-only spec must not steal the weaker value's anchor.
  attr.SetDisplayName("Captured diffuse texture");
  auto scene = document.scene();
  check(scene["warnings"].empty(), "Existing capture textures must resolve without warnings");
  check(fs::equivalent(scene["materials"][0]["albedo_texture"].get<std::string>(), directory / "reference/textures/diffuse map.dds"), "Windows paths must resolve against their referenced material layer");
  check(fs::equivalent(scene["materials"][0]["normal_texture"].get<std::string>(), directory / "reference/textures/normal.dds"), "Standard USD-resolved texture paths must remain valid");
  attr.Set(SdfAssetPath(".\\maps\\override.dds"));
  scene = document.scene();
  check(fs::equivalent(scene["materials"][0]["albedo_texture"].get<std::string>(), directory / "project/maps/override.dds"), "Authored overrides must resolve against the edit layer");
  attr.Set(SdfAssetPath(".\\maps\\missing.dds"));
  scene = document.scene();
  check(scene["materials"][0]["albedo_texture"] == "", "A missing strong texture must not fall back to the capture's weaker texture");
  check(scene["warnings"].size() == 1 && scene["warnings"][0].get<std::string>().find("missing.dds") != std::string::npos, "Missing textures must produce a useful scene warning");
  attr.Block();
  scene = document.scene();
  check(scene["materials"][0]["albedo_texture"] == "" && scene["warnings"].empty(), "An explicitly cleared texture is not a missing asset");
  document.save();
  check(text(material_file) == original_material && text(directory / "capture.usda") == original_capture, "Preview path resolution must not rewrite capture assets");
}

void multiple_captures(const fs::path& root) {
  const auto directory = root / "captures";
  fs::create_directories(directory);
  const auto a = directory / "a.usda", b = directory / "b.usda";
  fs::copy_file(fs::path(FIXTURE_DIR) / "capture.usda", a);
  fs::copy_file(a, b);
  {
    auto source = UsdStage::Open(b.string());
    source->RemovePrim(SdfPath("/RootNode/instances/inst_0123456789ABCDEF_1"));
    UsdGeomSetStageUpAxis(source, TfToken("Z"));
    UsdGeomSetStageMetersPerUnit(source, 0.5);
    source->GetRootLayer()->Save();
  }
  const auto original_a = text(a), original_b = text(b);
  editor::Document doc;
  const auto project = directory / "project/project.usda";
  doc.create(a, project);
  // Legacy workfile with a nested replacement layer and unrelated root metadata.
  auto work = doc.stage()->GetRootLayer();
  auto metadata = work->GetCustomLayerData();
  metadata.erase("remix_studio_captures");
  metadata["custom_data"] = VtValue(std::string("preserve"));
  work->SetCustomLayerData(metadata);
  auto nested = SdfLayer::CreateNew((project.parent_path() / "nested.usda").string());
  nested->SetSubLayerPaths({"mod.usda"}); nested->Save();
  work->SetSubLayerPaths({"nested.usda", "../a.usda"});
  work->SetSubLayerOffset(SdfLayerOffset(3, 2), 0);
  doc.save();
  check(doc.describe()["captures"].size() == 1 && !doc.describe()["dirty"].get<bool>(), "Legacy captures must be discovered without edits");
  const auto first = doc.describe()["active_capture"];
  const auto before_add = editor::layer_text(work);
  const std::string instance = "/RootNode/instances/inst_0123456789ABCDEF_0";
  doc.dispatch({{"op", "add_light"}, {"path", instance}, {"type", "SphereLight"}});
  const auto authored = editor::layer_text(doc.stage()->GetEditTarget().GetLayer());
  doc.dispatch({{"op", "add_capture"}, {"path", b.string()}});
  const auto second = doc.describe()["active_capture"];
  check(second != first && doc.describe()["captures"].size() == 2, "Add capture must activate it and retain the original");
  check(doc.scene()["meshes"].size() == 1 && doc.scene()["lights"].size() == 1, "Only the active capture should compose, with shared replacements");
  check(doc.describe()["up_axis"] == "Z" && doc.describe()["meters_per_unit"] == 0.5, "Capture units and up axis must switch");
  check(editor::layer_text(doc.stage()->GetEditTarget().GetLayer()) == authored, "Switching must retain unsaved authoring and edit target");
  check(work->GetSubLayerPaths()[0] == "nested.usda" && work->GetSubLayerOffsets()[0] == SdfLayerOffset(3, 2), "Switching must preserve replacement order and offsets");
  const auto revision = doc.revision();
  doc.dispatch({{"op", "add_capture"}, {"path", (directory / "./b.usda").string()}});
  check(doc.revision() == revision && doc.describe()["captures"].size() == 2, "Adding the same capture must not duplicate it or create an undo entry");
  const auto unchanged = editor::layer_text(work);
  rejects([&] { doc.dispatch({{"op", "add_capture"}, {"path", (directory / "missing.usda").string()}}); }, "Missing captures must be rejected");
  rejects([&] { doc.dispatch({{"op", "add_capture"}, {"path", (fs::path(FIXTURE_DIR) / "material.usda").string()}}); }, "Non-capture USD must be rejected");
  check(editor::layer_text(work) == unchanged && doc.revision() == revision, "Failed capture loads must preserve the current project");
  doc.undo();
  check(editor::layer_text(work) == before_add && doc.describe()["active_capture"] == first, "Undo must restore the original workfile and capture");
  check(doc.scene()["lights"].size() == 2, "Undoing capture addition must retain unsaved replacements");
  doc.redo();
  check(doc.describe()["active_capture"] == second, "Redo must restore the added capture");
  doc.dispatch({{"op", "select_capture"}, {"path", first}});
  check(doc.scene()["meshes"].size() == 2 && doc.scene()["lights"].size() == 2 && doc.describe()["up_axis"] == "Y", "Switching back must restore the first capture's geometry and units");
  doc.undo();
  check(doc.describe()["active_capture"] == second, "Capture switches must support undo");
  doc.save();
  check(!doc.describe()["dirty"].get<bool>(), "Saving a multi-capture project must succeed");
  editor::Document reopened;
  reopened.open(project);
  check(reopened.describe()["captures"].size() == 2 && reopened.describe()["active_capture"] == second, "Save/reopen must retain the capture list and active capture");
  check(reopened.scene()["meshes"].size() == 1 && reopened.scene()["lights"].size() == 1, "Shared replacements must survive reopening");
  reopened.package(directory / "package");
  auto packaged = UsdStage::Open((directory / "package/mod.usda").string());
  check(!packaged->GetRootLayer()->GetCustomLayerData().count("remix_studio_captures"), "Packages must exclude the capture catalog");
  check(packaged->GetRootLayer()->GetCustomLayerData().count("custom_data") && !packaged->GetPrimAtPath(SdfPath("/RootNode/instances")), "Packages must preserve unrelated metadata and exclude capture geometry");
  fs::remove(a);
  check(reopened.describe()["captures"][0]["missing"], "Missing inactive captures must stay listed");
  rejects([&] { reopened.dispatch({{"op", "select_capture"}, {"path", first}}); }, "Switching to a missing capture must fail safely");
  check(reopened.describe()["active_capture"] == second, "Missing capture must not displace the active one");
  std::ofstream(a) << original_a;
  check(text(a) == original_a && text(b) == original_b, "Capture assets must remain unchanged");
  // A mod layer opened on its own takes no captures: they would load into the game with it.
  editor::Document mod;
  mod.open(project.parent_path() / "mod.usda");
  rejects([&] { mod.dispatch({{"op", "add_capture"}, {"path", b.string()}}); }, "A mod layer must not take captures");
  check(SdfLayer::FindOrOpen((project.parent_path() / "mod.usda").string())->GetSubLayerPaths().empty(), "A rejected capture must leave the mod layer alone");
  // A workfile whose mod layer already holds a capture names it.
  auto modLayer = SdfLayer::FindOrOpen((project.parent_path() / "mod.usda").string());
  modLayer->SetSubLayerPaths({"../b.usda"}); modLayer->Save();
  editor::Document nestedCapture;
  nestedCapture.open(project);
  std::string message;
  try { nestedCapture.dispatch({{"op", "add_capture"}, {"path", a.string()}}); } catch (const std::exception& e) { message = e.what(); }
  check(message.find("mod.usda sublayers the capture b.usda") != std::string::npos, "A capture nested in a mod layer must be named");
}
}
int main() {
  auto root = fs::temp_directory_path() / ("godot-remix-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  try {
    TfErrorMark usd_errors;
    material_sampling(root);
    texture_resolution(root);
    linked_project(root);
    multiple_captures(root);
    fs::copy_file(fs::path(FIXTURE_DIR) / "capture.usda", root / "capture.usda");
    fs::copy_file(fs::path(FIXTURE_DIR) / "replacement.usda", root / "replacement.usda");
    const auto original = text(root / "capture.usda");
    editor::Document doc;
    rejects([&] { doc.open(root / "capture.usda"); }, "Opening a capture as an editable project must fail");
    doc.create(root / "capture.usda", root / "project" / "project.usda");
    check(doc.describe()["open"], "Project did not open");
    check(!doc.describe()["dirty"].get<bool>(), "New project should start clean");
    auto scene = doc.scene();
    check(scene["meshes"].size() == 2, "Capture must render two instances, not its hidden prototype");
    check(editor::expanded_mesh(scene, 0)["surfaces"][0]["indices"].size() == 6, "Quad must triangulate to two faces");
    check(scene["prototypes"].size() == 1 && scene["meshes"][0]["prototype"] == scene["meshes"][1]["prototype"], "Capture instances must share one geometry prototype");
    const std::string instance = "/RootNode/instances/inst_0123456789ABCDEF_0";
    const std::string anchor = "/RootNode/meshes/mesh_0123456789ABCDEF";
    check(doc.inspect(instance)["anchor"] == anchor, "Instance must resolve to its captured hash anchor");
    auto added = doc.dispatch({{"op", "add_light"}, {"path", instance}, {"type", "SphereLight"}});
    auto light = added["selection"].get<std::string>();
    check(light.rfind(anchor + "/", 0) == 0, "New light must live under replacement hash");
    check(doc.scene()["lights"].size() == 2, "Light must appear on both captured instances");
    check(doc.describe()["dirty"], "Editing must mark project dirty");
    doc.undo(); check(doc.scene()["lights"].empty(), "Undo must remove the authored light");
    check(!doc.describe()["dirty"].get<bool>(), "Undo to saved content must clear dirty state");
    doc.redo(); check(doc.scene()["lights"].size() == 2, "Redo must restore attached lights");
    doc.dispatch({{"op", "transform"}, {"path", instance + "/SphereLight"}, {"translation", {2, 3, 4}}});
    check(doc.inspect(light)["translation"] == Json::array({2.0, 3.0, 4.0}), "Instance selection must edit the shared attachment");
    check(!doc.stage()->GetEditTarget().GetLayer()->GetPrimAtPath(SdfPath(instance)), "Edits must not leak into capture instance paths");
    auto before = editor::layer_text(doc.stage()->GetEditTarget().GetLayer());
    rejects([&] { doc.dispatch({{"op", "batch"}, {"commands", Json::array({
      {{"op", "transform"}, {"path", light}, {"translation", {1, 2, 3}}},
      {{"op", "set_property"}, {"path", light}, {"property", "missing"}, {"value", 0}}
    })}}); }, "Invalid batch must fail");
    check(editor::layer_text(doc.stage()->GetEditTarget().GetLayer()) == before, "Failed edit must roll back all authored opinions");
    doc.dispatch({{"op", "transform"}, {"path", light}, {"translation", {1, 2, 3}}});
    check(doc.inspect(light)["translation"] == Json::array({1.0, 2.0, 3.0}), "Transform was not applied");
    doc.save();
    check(text(root / "capture.usda") == original, "Saving modified the capture");
    check(!doc.describe()["dirty"].get<bool>(), "Save must clear dirty state");
    editor::Document reopened;
    reopened.open(root / "project" / "project.usda");
    check(reopened.scene()["lights"].size() == 2, "Attached lights must survive save/reopen");
    auto imported = doc.ingest({{"source", (root / "replacement.usda").string()}});
    check(fs::exists(imported["asset"].get<std::string>()), "Ingestion must publish a USD asset");
    doc.dispatch({{"op", "add_mesh"}, {"path", instance}, {"asset", imported["asset"]}});
    check(doc.scene()["meshes"].size() == 4, "Added mesh must preserve original and appear in both instances");
    doc.dispatch({{"op", "replace_mesh"}, {"path", instance}, {"asset", imported["asset"]}});
    check(doc.scene()["meshes"].size() == 4, "Replacing an inline capture must suppress its mesh but retain authored additions");
    doc.undo();
    doc.dispatch({{"op", "replace_material"}, {"path", instance + "/mesh"}, {"asset", (fs::path(FIXTURE_DIR) / "material.usda").string()}});
    auto materials = doc.scene()["materials"];
    check(std::abs(materials[0]["albedo"][1].get<double>() - 0.8) < 0.001, "Replacement shader must override local capture material inputs");
    check(materials[0]["emissive_intensity"] == 0, "Disabled emission must ignore the material's intensity value");
    doc.package(root / "package");
    check(fs::exists(root / "package/mod.usda"), "Package must contain mod.usda");
    auto packaged = UsdStage::Open((root / "package/mod.usda").string());
    check(bool(packaged), "Package must open in OpenUSD");
    check(!packaged->GetPrimAtPath(SdfPath("/RootNode/instances")), "Package must not include capture instances");
    check(packaged->GetPrimAtPath(SdfPath(light)).IsValid(), "Package must preserve new light");
    check(text(root / "capture.usda") == original, "Packaging modified capture");
    auto nested = SdfLayer::CreateNew((root / "project/nested.usda").string());
    nested->SetSubLayerPaths({"mod.usda"}); nested->Save();
    doc.stage()->GetRootLayer()->SetSubLayerPaths({"nested.usda", "../capture.usda"});
    doc.stage()->GetRootLayer()->SetCustomLayerData({{"unknown_project_metadata", VtValue(std::string("keep"))}});
    doc.package(root / "nested_package");
    auto nested_package = UsdStage::Open((root / "nested_package/mod.usda").string());
    check(nested_package->GetRootLayer()->GetSubLayerPaths().size() == 1, "Nested replacement layers must not be duplicated at the root");
    check(nested_package->GetRootLayer()->GetCustomLayerData().count("unknown_project_metadata"), "Packaging must preserve authored root metadata");
    check(nested_package->GetPrimAtPath(SdfPath(light)).IsValid(), "Nested package lost attachments");
    rejects([&] { doc.package(root / "package"); }, "Packaging must not overwrite an existing mod");
    rejects([&] { doc.dispatch({{"op", "select_layer"}, {"layer", (root / "capture.usda").string()}}); }, "Capture edit target must be rejected");
    rejects([&] { doc.ingest({{"source", (root / "replacement.usda").string()}}); }, "Duplicate ingestion must not overwrite assets");
    check(usd_errors.IsClean(), "Document workflow emitted USD errors");
    fs::remove_all(root);
    std::cout << "All document workflow tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nFixtures retained at " << root << '\n'; return 1;
  }
}
