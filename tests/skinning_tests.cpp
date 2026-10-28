#include "document.h"
#include "scene_view.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>

PXR_NAMESPACE_USING_DIRECTIVE
using editor::Json;
namespace fs = std::filesystem;
namespace {
const std::string first = "/RootNode/instances/inst_0123456789ABCDEF_0";
const std::string second = "/RootNode/instances/inst_0123456789ABCDEF_1";
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
GfMatrix4d matrix(const Json& values) {
  GfMatrix4d result(1);
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) result[r][c] = values[r*4+c];
  return result;
}
GfVec3d vector(const Json& value, int start = 0) { return {value[start].get<double>(), value[start+1].get<double>(), value[start+2].get<double>()}; }
Json mesh(const Json& scene, const std::string& path) {
  for (const auto& mesh : scene["meshes"]) if (mesh["id"] == path + "/mesh") return editor::expanded_mesh(scene, mesh);
  throw std::runtime_error("Expected skinned mesh was not composed: " + path);
}
void pose(const Json& scene) {
  check(scene["warnings"].empty() && scene["meshes"].size() == 2, "Both skinned instances must compose without warnings");
  const auto a = mesh(scene, first); const auto b = mesh(scene, second);
  check(scene["prototypes"].size() == 2, "Different skeleton poses must not share a geometry prototype");
  check(a["skinned"].get<bool>() && b["skinned"].get<bool>(), "Animated and rest-pose meshes must both use their skeletons");
  check(a["skeleton"] == first + "/skel" && b["skeleton"] == second + "/skel", "Overlay bindings must identify each skeleton instance");
  check(scene["skeletons"].size() == 2, "Scene must retain separate skeleton poses");
  const auto& rig = scene["skeletons"][first + "/skel"];
  check(rig["parents"] == Json::array({-1, 0}) && rig["joints"] == Json::array({"root", "root/tip"}), "Overlay must retain the authored joint hierarchy");
  check(GfIsClose(vector(rig["positions"][0]), GfVec3d(101,0,4), 1e-4) &&
        GfIsClose(vector(rig["positions"][1]), GfVec3d(103,0,4), 1e-4),
    "Bone overlay must use posed world-space joints, without mesh bind offsets");
  check(GfIsClose(vector(scene["skeletons"][second + "/skel"]["positions"][0]), GfVec3d(1,20,4), 1e-4),
    "Other skeleton instances must use their own world transforms");
  const GfVec3d expected[] = {{101,1,4}, {102,0,4}, {101,0,4}};
  const GfVec3d rest[] = {{1,21,4}, {3,21,4}, {3,22,4}};
  for (int i = 0; i < 3; ++i) {
    const auto& vertex = a["surfaces"][0]["vertices"][i];
    if (!GfIsClose(matrix(a["transform"]).Transform(vector(vertex)), expected[i], 1e-4))
      std::cerr << "Snapshot " << scene["revision"] << ", vertex " << i << ": " << matrix(a["transform"]).Transform(vector(vertex)) << " expected " << expected[i] << '\n';
    check(GfIsClose(matrix(a["transform"]).Transform(vector(vertex)), expected[i], 1e-4),
      "Pose must include inverse bind, geom bind, skeleton/mesh offsets and instance transform exactly once");
    check(GfIsClose(vector(vertex, 3), i == 0 ? GfVec3d(1,0,0) : GfVec3d(0,1,0), 1e-4),
      "Even a constant input normal must follow each vertex's joints");
    check(GfIsClose(matrix(b["transform"]).Transform(vector(b["surfaces"][0]["vertices"][i])), rest[i], 1e-4),
      "Instances sharing captured geometry must not share different skeleton poses");
  }
  check(a["surfaces"][0]["vertices"][2][6] == 1 && a["surfaces"][0]["vertices"][2][7] == 1, "Skinning must preserve UVs");
}
UsdStageRefPtr fixture() {
  auto stage = UsdStage::Open((fs::path(FIXTURE_DIR) / "skinned/capture.usda").string());
  check(bool(stage), "Could not open skeletal fixture");
  stage->SetEditTarget(stage->GetSessionLayer());
  return stage;
}
void variants() {
  auto stage = fixture();
  std::map<std::string, std::string> layers;
  for (const auto& layer : stage->GetUsedLayers()) layers[layer->GetIdentifier()] = editor::layer_text(layer);
  pose(editor::compose_scene(stage, 0));
  for (const auto& layer : stage->GetUsedLayers())
    check(layers.at(layer->GetIdentifier()) == editor::layer_text(layer), "Evaluating poses must not author baked geometry");
  const SdfPath path(first + "/mesh");
  auto binding = UsdSkelBindingAPI(stage->GetPrimAtPath(path));
  binding.CreateJointsAttr().Set(VtTokenArray{TfToken("root/tip"), TfToken("root")});
  binding.CreateJointIndicesPrimvar(false, 1).Set(VtIntArray{1,0,0});
  pose(editor::compose_scene(stage, 1));
  binding.CreateSkinningMethodAttr().Set(TfToken("dualQuaternion"));
  pose(editor::compose_scene(stage, 2));
  // Indexed influences are common USD primvars, even though Remix exports flat arrays.
  binding.GetJointIndicesPrimvar().Set(VtIntArray{1,0});
  binding.GetJointIndicesPrimvar().SetIndices(VtIntArray{0,1,1});
  binding.GetJointWeightsPrimvar().Set(VtFloatArray{1});
  binding.GetJointWeightsPrimvar().SetIndices(VtIntArray{0,0,0});
  pose(editor::compose_scene(stage, 3));
  auto geom = UsdGeomMesh(stage->GetPrimAtPath(path));
  geom.CreateNormalsAttr().Set(VtVec3fArray{GfVec3f(1,0,0), GfVec3f(1,0,0), GfVec3f(1,0,0)});
  geom.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
  pose(editor::compose_scene(stage, 4));

  stage = fixture();
  auto animation = UsdSkelAnimation(stage->GetPrimAtPath(SdfPath(first + "/pose")));
  // Time-sampled captures lack default animation values. Preview their first sample.
  for (const auto& attr : {animation.GetTranslationsAttr(), animation.GetRotationsAttr(), animation.GetScalesAttr()}) {
    VtValue value; attr.Get(&value);
    attr.Block(); attr.Set(value, UsdTimeCode(5));
  }
  pose(editor::compose_scene(stage, 5));
  // Instance proxies must resolve inherited skeletons too. Keep animation on
  // the non-native instance: USD ignores descendant overrides on native instances.
  stage->GetPrimAtPath(SdfPath(second)).SetInstanceable(true);
  pose(editor::compose_scene(stage, 6));
}
void invalid_data() {
  auto stage = fixture();
  auto binding = UsdSkelBindingAPI(stage->GetPrimAtPath(SdfPath(first + "/mesh")));
  binding.CreateJointIndicesPrimvar(false, 1).Set(VtIntArray{99,1,1});
  auto scene = editor::compose_scene(stage, 0);
  check(scene["meshes"].size() == 2 && scene["warnings"].size() == 1 && !mesh(scene, first)["skinned"].get<bool>(),
    "Malformed influences must warn and retain undeformed geometry without losing the rest of the scene");
  check(vector(mesh(scene, first)["surfaces"][0]["vertices"][0]) == GfVec3d(1,0,0), "Failed skinning must not leave partially deformed vertices");
  stage = fixture();
  UsdSkelBindingAPI(stage->GetPrimAtPath(SdfPath(first + "/mesh"))).CreateSkeletonRel().SetTargets({SdfPath("/MissingSkeleton")});
  scene = editor::compose_scene(stage, 0);
  check(scene["meshes"].size() == 2 && !scene["warnings"].empty(), "Missing skeleton must produce a useful warning and keep the mesh");
}
void workflow(const fs::path& directory) {
  editor::Document doc;
  const auto project = directory / "work.usda";
  doc.create(fs::path(FIXTURE_DIR) / "skinned/capture.usda", project);
  pose(doc.scene());
  check(doc.describe()["warnings"].empty(), "Supported SkelRoot must no longer be reported as unsupported");
  auto before = doc.scene();
  auto replacement = doc.dispatch({{"op","replace_mesh"}, {"path",first + "/mesh"},
    {"asset",(fs::path(FIXTURE_DIR)/"replacement.usda").string()}})["selection"].get<std::string>();
  check(doc.scene()["meshes"].size() == 2, "Replacing a skinned capture must suppress its original geometry");
  check(doc.scene()["skeletons"].empty(), "Replaced meshes must not leave stale bone overlays");
  doc.dispatch({{"op","revert_mesh"}, {"path",replacement}});
  pose(doc.scene());
  doc.save(); doc.open(project);
  pose(doc.scene());
  check(!doc.describe()["dirty"].get<bool>(), "Evaluating a reopened capture must not dirty it");
}
void real_capture(const fs::path& capture) {
  auto stage = UsdStage::Open(capture.string());
  check(bool(stage), "Could not open supplied capture");
  std::map<std::string, std::string> before;
  for (const auto& layer : stage->GetUsedLayers()) before[layer->GetIdentifier()] = editor::layer_text(layer);
  auto scene = editor::compose_scene(stage, 0);
  UsdSkelCache cache;
  UsdGeomXformCache transforms(UsdTimeCode::EarliestTime());
  size_t skinned = 0;
  for (const auto& mesh : scene["meshes"]) {
    if (!mesh.value("skinned", false)) continue;
    ++skinned;
    auto prim = stage->GetPrimAtPath(SdfPath(mesh["id"].get<std::string>()));
    auto root = UsdSkelRoot::Find(prim); cache.Populate(root, UsdTraverseInstanceProxies());
    auto skin = cache.GetSkinningQuery(prim);
    auto skeleton = UsdSkelBindingAPI(prim).GetInheritedSkeleton();
    check(mesh["skeleton"] == skeleton.GetPath().GetString(), "Capture overlay must reference the correct skeleton");
    VtMatrix4dArray joint_world;
    auto query = cache.GetSkelQuery(skeleton);
    check(query.ComputeJointWorldTransforms(&joint_world, &transforms), "Reference joint evaluation failed");
    const auto& overlay = scene["skeletons"][skeleton.GetPath().GetString()];
    check(overlay["positions"].size() == joint_world.size(), "Overlay must include every captured joint");
    for (size_t i = 0; i < joint_world.size(); ++i)
      check(GfIsClose(vector(overlay["positions"][i]), joint_world[i].ExtractTranslation(), 1e-5), "Capture overlay joint is out of place");
    VtMatrix4dArray matrices; check(cache.GetSkelQuery(skeleton).ComputeSkinningTransforms(&matrices, transforms.GetTime()), "Reference skeleton evaluation failed");
    VtVec3fArray points; UsdGeomMesh(prim).GetPointsAttr().Get(&points, transforms.GetTime());
    check(skin.ComputeSkinnedPoints(matrices, &points, transforms.GetTime()), "Reference skinning failed");
    std::vector<GfVec3d> world_points;
    const auto skeleton_world = transforms.GetLocalToWorldTransform(skeleton.GetPrim());
    for (const auto& point : points) world_points.push_back(skeleton_world.Transform(GfVec3d(point)));
    const auto mesh_world = matrix(mesh["transform"]);
    // Every emitted vertex must match an evaluated source point in world space.
    for (const auto& surface : editor::expanded_mesh(scene, mesh)["surfaces"]) for (const auto& vertex : surface["vertices"]) {
      const auto actual = mesh_world.Transform(vector(vertex));
      bool found = false;
      for (const auto& point : world_points)
        if (GfIsClose(actual, point, 1e-3)) { found = true; break; }
      check(found, "Rendered capture vertex does not match its USD skeleton pose");
    }
  }
  check(skinned > 0 && scene["warnings"].empty(), "Supplied capture should compose skinned geometry without warnings");
  for (const auto& layer : stage->GetUsedLayers()) check(before.at(layer->GetIdentifier()) == editor::layer_text(layer), "Capture evaluation must be read-only");
  std::cout << "Capture: " << scene["meshes"].size() << " meshes, " << skinned << " skinned, no composition warnings\n";
}
}
int main(int argc, char** argv) {
  const auto directory = fs::temp_directory_path() / ("remix-skinning-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    TfErrorMark errors;
    variants(); invalid_data(); workflow(directory);
    if (argc == 2) real_capture(argv[1]);
    check(errors.IsClean(), "Skinning emitted USD errors");
    fs::remove_all(directory);
    std::cout << "Skeleton capture tests passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "\nFixtures: " << directory << '\n'; return 1; }
}
