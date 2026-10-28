#include "document.h"
#include "scene_view.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>

using editor::Json;
namespace fs = std::filesystem;
PXR_NAMESPACE_USING_DIRECTIVE
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
  auto root = fs::temp_directory_path() / ("remix-alpha-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    TfErrorMark errors;
    editor::Document doc; doc.create(fs::path(FIXTURE_DIR) / "alpha.usda", root / "project.usda");
    const auto original = doc.scene();
    const auto& cutout = original.at("meshes").at(0);
    const auto& blended = original.at("meshes").at(1);
    const auto& opaque = original.at("meshes").at(2);
    check(original.at("prototypes").size() == 2 && cutout.at("prototype") == blended.at("prototype") && cutout.at("prototype") == opaque.at("prototype"), "Shared mesh geometry must remain reusable across different alpha states");
    check(cutout.at("alpha_state").at("alphaTestEnabled") && cutout.at("alpha_state").at("alphaTestCompareOp") == 4 && cutout.at("alpha_state").at("alphaTestReferenceValue") == 128, "Capture cutout state must reach the instance");
    check(blended.at("alpha_state").at("alphaBlendEnabled") && blended.at("alpha_state").at("srcColorBlendFactor") == 6 && !opaque.at("alpha_state").at("alphaTestEnabled").get<bool>(), "Blend/opaque states must remain independent");
    check(original.at("materials").at(0).at("use_legacy_alpha") && original.at("materials").at(0).at("alpha_test") == 7, "MDL defaults and unsigned enum inputs must be honored");
    const std::string shader = "/RootNode/Looks/mat_0123456789ABCDEF/Shader";
    auto set = [&](const char* property, Json value) { doc.dispatch({{"op", "set_authoring_property"}, {"path", shader}, {"property", property}, {"value", value}}); };
    set("inputs:use_legacy_alpha_state", false); set("inputs:blend_enabled", true); set("inputs:blend_type", 6); set("inputs:inverted_blend", true);
    set("inputs:alpha_test_type", 6); set("inputs:alpha_test_reference_value", 127.0);
    auto material = doc.scene().at("materials").at(0);
    check(!material.at("use_legacy_alpha").get<bool>() && material.at("blend_enabled") && material.at("blend_type") == 6 && material.at("inverted_blend") && material.at("alpha_test") == 6 && material.at("alpha_reference") == 127, "Authored alpha controls must reach the snapshot");
    doc.undo(); check(doc.scene().at("materials").at(0).at("alpha_reference") == 0, "Alpha edits must undo");
    doc.redo(); doc.save();
    editor::Document reopened; reopened.open(root / "project.usda");
    check(reopened.scene().at("materials").at(0) == material, "Alpha edits must survive save/reopen");
    doc.dispatch({{"op", "reset_authoring_property"}, {"path", shader}, {"property", "inputs:use_legacy_alpha_state"}});
    check(doc.scene().at("materials").at(0).at("use_legacy_alpha"), "Reset must restore captured state");
    const auto mesh = doc.stage()->GetPrimAtPath(SdfPath("/RootNode/meshes/mesh_0123456789ABCDEF/mesh"));
    auto primvars = UsdGeomPrimvarsAPI(mesh);
    auto opacity = primvars.CreatePrimvar(TfToken("displayOpacity"), SdfValueTypeNames->FloatArray, UsdGeomTokens->faceVarying);
    opacity.Set(VtFloatArray{0.f, 0.5f, 1.f}); opacity.SetIndices(VtIntArray{0, 1, 2, 1});
    auto color = primvars.CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->constant);
    color.Set(VtVec3fArray{GfVec3f(1, 0.5f, 0)});
    const auto colored = editor::expanded_mesh(doc.scene(), 0).at("surfaces").at(0);
    for (size_t i = 0; i < colored.at("vertices").size(); ++i) {
      const auto& point = colored.at("vertices").at(i);
      const uint32_t packed = colored.at("colors").at(i);
      const uint32_t expected_alpha = point.at(0).get<float>() < 0 && point.at(1).get<float>() < 0 ? 0 : point.at(0).get<float>() > 0 && point.at(1).get<float>() > 0 ? 255 : 128;
      check(packed == ((expected_alpha << 24) | 0xff8000u), "Indexed face-varying alpha and constant RGB must follow triangulated corners");
    }
    check(errors.IsClean(), "Alpha support emitted USD errors");
    fs::remove_all(root); std::cout << "Capture and authored alpha tests passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "\nFixtures: " << root << '\n'; return 1; }
}
