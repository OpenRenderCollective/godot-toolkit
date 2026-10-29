#include "document.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

using editor::Json;
namespace fs = std::filesystem;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string text(const fs::path& path) { std::ifstream file(path); return {std::istreambuf_iterator<char>(file), {}}; }
Json field(editor::Document& doc, const std::string& path, const std::string& name) {
  const auto inspected = doc.inspect(path);
  for (const auto& field : inspected["authoring"]["fields"]) if (field["property"] == name) return field;
  return nullptr;
}
}
int main() {
  const auto root = fs::temp_directory_path() / ("remix-authoring-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  try {
    TfErrorMark errors;
    const auto capture = fs::path(FIXTURE_DIR) / "capture.usda";
    const auto original = text(capture);
    editor::Document doc;
    doc.create(capture, root / "project.usda");
    const std::string mesh = "/RootNode/instances/inst_0123456789ABCDEF_0/mesh";
    const std::string material = "/RootNode/Looks/mat_0123456789ABCDEF";
    const std::string shader = material + "/Shader";
    auto set = [&](const std::string& path, const std::string& property, Json value) {
      doc.dispatch({{"op", "set_authoring_property"}, {"path", path}, {"property", property}, {"value", value}});
    };
    check(doc.inspect(mesh)["authoring"]["kind"] == "mesh", "Mesh classification");
    check(field(doc, mesh, "points").is_null() && field(doc, mesh, "primvars:st").is_null(), "Raw geometry must stay out of authoring fields");
    check(doc.inspect(mesh)["authoring"]["materials"][0]["path"] == material, "Bound material resolution");
    set(mesh, "doubleSided", true);
    check(field(doc, mesh, "doubleSided")["overridden"], "Instance edits must identify the replacement-layer override");
    const auto shared = doc.scene();
    for (const auto& instance : shared["meshes"]) check(instance["double_sided"], "Shared capture edits affect all instances");
    doc.dispatch({{"op", "reset_authoring_property"}, {"path", mesh}, {"property", "doubleSided"}});
    check(field(doc, mesh, "doubleSided")["value"] == false, "Reset must reveal the inherited default");
    doc.undo(); check(field(doc, mesh, "doubleSided")["value"] == true, "Reset must be undoable");
    doc.redo(); check(field(doc, mesh, "doubleSided")["value"] == false, "Reset redo");
    set(shader, "inputs:metallic_constant", 0.8);
    check(std::abs(doc.scene()["materials"][0]["metallic"].get<float>() - 0.8f) < 0.001, "Omitted shader inputs must be authorable");
    set(material, "inputs:wrap_mode_u", 2);
    check(doc.scene()["materials"][0]["wrap_mode_u"] == 2, "Material selection must target its shader, including uint attributes");
    check(!doc.scene()["materials"][0].contains("iridescence"), "No thin film until enabled");
    check(field(doc, shader, "openremix:iridescence:filmIor")["enabled_by"] == "inputs:enable_thin_film", "Iridescence controls follow the thin-film switch");
    set(shader, "inputs:enable_thin_film", true);
    set(shader, "inputs:thin_film_thickness_constant", 450);
    set(shader, "openremix:iridescence:filmIor", 2.4);
    set(shader, "openremix:iridescence:flowAmplitude", 300);
    const auto film = doc.scene()["materials"][0]["iridescence"];
    check(film["thickness"] == 450 && std::abs(film["ior"].get<float>() - 2.4f) < 0.001 && film["flow_amplitude"] == 300 &&
          film["strength"] == 1, "OpenRemix iridescence attributes must be authorable beside the MDL inputs");
    check(field(doc, shader, "openremix:iridescence:filmIor")["overridden"], "Iridescence edits land in the edit layer");
    // Converting the surface model swaps AperturePBR's module and carries the
    // texture into the transmission slot.
    std::ofstream(root / "glass.dds") << "DDS ";
    set(shader, "inputs:diffuse_texture", (root / "glass.dds").string());
    check(field(doc, shader, "inputs:thin_walled").is_null(), "Opaque materials have no transmission controls");
    doc.dispatch({{"op", "set_material_model"}, {"path", material}, {"model", "translucent"}});
    auto glass = doc.scene()["materials"][0];
    check(glass["kind"] == "translucent" && doc.inspect(material)["authoring"]["surface_model"] == "translucent",
          "Materials convert to translucent");
    check(fs::path(glass["albedo_texture"].get<std::string>()).filename() == "glass.dds", "Conversion keeps the material's texture");
    check(field(doc, shader, "inputs:transmittance_measurement_distance")["disabled_by"] == "inputs:thin_walled",
          "A thin wall has no measurement distance");
    set(shader, "inputs:thin_walled", true); set(shader, "inputs:transmittance_measurement_distance", 4);
    glass = doc.scene()["materials"][0];
    check(glass["thin_walled"] == true && glass["transmittance_distance"] == 4, "Translucent body controls reach the renderer snapshot");
    doc.dispatch({{"op", "set_material_model"}, {"path", material}, {"model", "opaque"}});
    check(doc.scene()["materials"][0]["kind"] == "opaque", "Materials convert back to opaque");
    doc.undo(); check(doc.scene()["materials"][0]["kind"] == "translucent", "Surface model undo");
    doc.redo();
    bool rejected = false;
    try { doc.dispatch({{"op", "set_material_model"}, {"path", material}, {"model", "portal"}}); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Unsupported surface models are rejected");
    const auto before = editor::layer_text(doc.stage()->GetEditTarget().GetLayer());
    for (auto invalid : Json::array({
      {{"op", "set_authoring_property"}, {"path", shader}, {"property", "inputs:metallic_constant"}, {"value", 2}},
      {{"op", "set_authoring_property"}, {"path", mesh}, {"property", "points"}, {"value", 0}}})) {
      bool rejected = false;
      try { doc.dispatch(invalid); } catch (const std::exception&) { rejected = true; }
      check(rejected && editor::layer_text(doc.stage()->GetEditTarget().GetLayer()) == before, "Invalid edits must roll back cleanly");
    }
    const auto light = doc.dispatch({{"op", "add_light"}, {"path", mesh}, {"type", "SphereLight"}, {"translation", {1, 2, 3}}})["selection"].get<std::string>();
    set(light, "inputs:intensity", 2); set(light, "inputs:exposure", 3);
    set(light, "inputs:enableColorTemperature", true); set(light, "inputs:colorTemperature", 2500);
    doc.dispatch({{"op", "set_light_type"}, {"path", light}, {"type", "RectLight"}});
    check(doc.inspect(light)["translation"] == Json::array({1, 2, 3}), "Type conversion must keep transforms");
    check(field(doc, light, "inputs:radius").is_null() && !field(doc, light, "inputs:width").is_null(), "Shape controls must follow the light type");
    set(light, "inputs:shaping:cone:softness", 0.4); set(light, "inputs:shaping:focus", 2);
    auto rendered = doc.scene()["lights"][0];
    check(rendered["intensity"] == 16, "Exposure must be applied once");
    check(rendered["color"][0] > rendered["color"][2], "Color temperature must affect rendered color");
    check(std::abs(rendered["cone_softness"].get<float>() - 0.4f) < 0.001 && rendered["focus"] == 2, "Cone parameters must reach the renderer snapshot");
    doc.dispatch({{"op", "set_light_type"}, {"path", light}, {"type", "SpotLight"}});
    check(doc.inspect(light)["authoring"]["light_type"] == "SpotLight", "Spot is a shaped sphere");
    doc.undo(); check(doc.inspect(light)["authoring"]["light_type"] == "RectLight", "Type conversion undo");
    doc.redo();
    doc.dispatch({{"op", "set_light_type"}, {"path", light}, {"type", "SphereLight"}});
    check(doc.scene()["lights"][0]["cone_angle"] == 180, "Sphere conversion must disable previous spotlight shaping");
    // An IES profile (UsdLuxShapingAPI) and OpenRemix's light cookie.
    std::ofstream(root / "fixture.ies") << "IESNA:LM-63-2002\nTILT=NONE\n1 1000 1 3 1 1 2 0 0 0\n1 1 100\n0 45 90\n0\n100 80 10\n";
    std::ofstream(root / "cookie.dds") << "DDS ";
    check(field(doc, light, "inputs:shaping:ies:file")["control"] == "file", "Sphere lights take an IES profile");
    set(light, "inputs:shaping:ies:file", (root / "fixture.ies").string());
    check(fs::path(doc.scene()["lights"][0]["ies"].get<std::string>()).filename() == "fixture.ies", "IES profile reaches the renderer snapshot");
    {
      const auto fresh = doc.dispatch({{"op", "add_light"}, {"type", "SphereLight"}})["selection"].get<std::string>();
      set(fresh, "inputs:shaping:ies:file", (root / "fixture.ies").string());
      for (const auto& rendered : doc.scene()["lights"]) if (rendered["id"] == fresh)
        check(rendered["cone_angle"] == 180, "An IES profile must not narrow a light to the shaping API's fallback cone");
      doc.undo(); doc.undo();
    }
    set(light, "inputs:remix:cookie:texture", (root / "cookie.dds").string());
    auto cookie = doc.scene()["lights"][0];
    check(cookie["cookie_fov"] == 60 && cookie["cookie_up"] == Json::array({0, 0, 1}), "A new cookie gets a projection width");
    set(light, "inputs:remix:cookie:up", Json::array({1, 0, 0}));
    check(doc.scene()["lights"][0]["cookie_up"] == Json::array({1, 0, 0}), "Cookie up is authorable");
    rejected = false;
    try { set(light, "inputs:remix:cookie:up", Json::array({0, 0, 0})); } catch (const std::exception&) { rejected = true; }
    check(rejected, "A zero cookie up is rejected");
    doc.dispatch({{"op", "set_light_type"}, {"path", light}, {"type", "RectLight"}});
    check(field(doc, light, "inputs:remix:cookie:texture").is_null() && !field(doc, light, "inputs:shaping:ies:file").is_null(),
          "Cookies are for sphere lights; IES profiles for area lights too");
    doc.dispatch({{"op", "set_light_type"}, {"path", light}, {"type", "SphereLight"}});
    auto second = UsdShadeMaterial::Define(doc.stage(), SdfPath("/RootNode/Looks/Second"));
    auto subset = UsdGeomSubset::Define(doc.stage(), SdfPath("/RootNode/meshes/mesh_0123456789ABCDEF/mesh/Subset"));
    subset.CreateElementTypeAttr().Set(UsdGeomTokens->face);
    subset.CreateFamilyNameAttr().Set(TfToken("materialBind"));
    subset.CreateIndicesAttr().Set(VtIntArray{0});
    UsdShadeMaterialBindingAPI::Apply(subset.GetPrim()).Bind(second);
    check(doc.inspect(mesh)["authoring"]["materials"].size() == 2, "Material subsets must appear as slots");
    doc.save();
    check(text(capture) == original, "Authoring must never modify the capture");
    auto imported = doc.ingest({{"source", (fs::path(FIXTURE_DIR) / "replacement.usda").string()}});
    auto preview = editor::preview_asset(imported["asset"].get<std::string>());
    check(preview["kind"] == "mesh" && !preview["scene"]["meshes"].empty() && !preview["dependencies"].empty(), "Preview extraction must include geometry and dependencies");
    editor::Document defaults;
    defaults.create(capture, root / "defaults/project.usda");
    for (double units : {1.0, 0.01}) {
      { UsdEditContext context(defaults.stage(), defaults.stage()->GetRootLayer()); UsdGeomSetStageMetersPerUnit(defaults.stage(), units); }
      for (const auto& entry : std::map<std::string, double>{{"SphereLight", 100}, {"SpotLight", 100}, {"RectLight", 400},
             {"DiskLight", 500}, {"CylinderLight", 140}, {"DistantLight", 25}, {"DomeLight", 1}}) {
        const auto created = defaults.dispatch({{"op", "add_light"}, {"type", entry.first}})["selection"].get<std::string>();
        check(field(defaults, created, "inputs:intensity")["value"] == entry.second, "New light intensity must match Toolkit's creator");
        if (entry.first == "SphereLight" || entry.first == "SpotLight" || entry.first == "DiskLight" || entry.first == "CylinderLight")
          check(field(defaults, created, "inputs:radius")["value"] == 5.0 / units, "Toolkit emitter radius must use the capture's units");
        if (entry.first == "RectLight") check(field(defaults, created, "inputs:width")["value"] == 10.0 / units &&
          field(defaults, created, "inputs:height")["value"] == 10.0 / units, "Toolkit rectangle dimensions");
        if (entry.first == "CylinderLight") check(field(defaults, created, "inputs:length")["value"] == 10.0 / units, "Toolkit cylinder length");
        if (entry.first == "DistantLight") check(field(defaults, created, "inputs:angle")["value"] == 1, "Toolkit distant light angle");
      }
    }
    check(errors.IsClean(), "Authoring emitted USD errors");
    fs::remove_all(root);
    std::cout << "Authoring tests passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << "\nFixtures: " << root << '\n'; return 1; }
}
