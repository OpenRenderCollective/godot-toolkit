#include "document.h"
#include "usd_authoring.h"

#include <cmath>
#include <algorithm>
#include <set>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdUtils/dependencies.h>

PXR_NAMESPACE_USING_DIRECTIVE
namespace editor {
namespace {
UsdPrim shader_for(const UsdPrim& prim) {
  if (prim.IsA<UsdShadeShader>() && prim.GetParent().IsA<UsdShadeMaterial>()) return prim;
  if (auto material = UsdShadeMaterial(prim)) {
    auto connected = material.ComputeSurfaceSource(TfToken("mdl"));
    if (connected) return connected.GetPrim();
    for (auto child : prim.GetChildren()) if (child.IsA<UsdShadeShader>()) return child;
  }
  return {};
}
Json read_value(const UsdAttribute& attr, const Json& fallback) {
  VtValue value;
  if (!attr || !attr.Get(&value)) return fallback;
  if (value.IsHolding<float>()) return value.Get<float>();
  if (value.IsHolding<double>()) return value.Get<double>();
  if (value.IsHolding<int>()) return value.Get<int>();
  if (value.IsHolding<unsigned>()) return value.Get<unsigned>();
  if (value.IsHolding<bool>()) return value.Get<bool>();
  if (value.IsHolding<TfToken>()) return value.Get<TfToken>().GetString();
  if (value.IsHolding<SdfAssetPath>()) return value.Get<SdfAssetPath>().GetAssetPath();
  if (value.IsHolding<GfVec3f>()) { auto v = value.Get<GfVec3f>(); return {v[0], v[1], v[2]}; }
  return fallback;
}
Json fields_for(const UsdPrim& selected) {
  Json fields = Json::array();
  auto owner = selected;
  auto add = [&](const char* name, const char* label, const char* group, const char* type, Json fallback,
                 double minimum = 0, double maximum = 1e12, const char* control = "number") -> Json& {
    const auto attr = owner.GetAttribute(TfToken(name));
    fields.push_back({{"property", name}, {"label", label}, {"group", group}, {"type", type},
      {"path", owner.GetPath().GetString()}, {"control", control}, {"default", fallback},
      {"value", read_value(attr, fallback)}, {"min", minimum}, {"max", maximum}});
    if (std::string(type) == "asset") fields.back()["resolved"] = resolved_texture_path(owner, name);
    return fields.back();
  };
  auto choices = [](Json& field, const Json& values, const Json& labels) {
    field["control"] = "choice"; field["choices"] = values; field["labels"] = labels;
  };
  if (UsdGeomImageable(selected)) {
    auto& visibility = add("visibility", "Visibility", "Object", "token", "inherited");
    choices(visibility, {"inherited", "invisible"}, {"Visible", "Hidden"});
  }
  if (UsdGeomMesh(selected)) {
    add("doubleSided", "Double-sided", "Object", "bool", false, 0, 1, "bool");
    add("remix_category:ignore_alpha_channel", "Ignore alpha channel", "Opacity", "bool", false, 0, 1, "bool");
    add("remix_category:alpha_blend_to_cutout", "Force cutout", "Opacity", "bool", false, 0, 1, "bool")
      ["tooltip"] = "Render blended surfaces as cutouts using Remix's global cutout threshold.";
  }
  if (UsdLuxLightAPI(selected)) {
    add("inputs:color", "Color", "Light", "color3f", {1, 1, 1}, 0, 1, "color");
    add("inputs:intensity", "Radiance", "Light", "float", 1.f);
    add("inputs:exposure", "Exposure (stops)", "Light", "float", 0.f, -100, 100);
    add("inputs:enableColorTemperature", "Use color temperature", "Light", "bool", false, 0, 1, "bool");
    auto& temperature = add("inputs:colorTemperature", "Temperature (K)", "Light", "float", 6500.f, 1000, 10000);
    temperature["enabled_by"] = "inputs:enableColorTemperature";
    const auto type = selected.GetTypeName().GetString();
    if (type == "SphereLight" || type == "DiskLight" || type == "CylinderLight") add("inputs:radius", "Radius", "Shape", "float", 0.5f, 0.0001);
    if (type == "RectLight") {
      add("inputs:width", "Width", "Shape", "float", 1.f, 0.0001);
      add("inputs:height", "Height", "Shape", "float", 1.f, 0.0001);
    }
    if (type == "CylinderLight") add("inputs:length", "Length", "Shape", "float", 1.f, 0.0001);
    if (type == "DistantLight") add("inputs:angle", "Angular size (degrees)", "Shape", "float", 0.53f, 0, 180);
    if (type == "DomeLight") add("inputs:texture:file", "Environment texture", "Light", "asset", "", 0, 1, "texture");
    if (type == "SphereLight" || type == "RectLight" || type == "DiskLight") {
      add("inputs:shaping:cone:angle", "Cone angle (degrees)", "Cone", "float", 180.f, 0, 180);
      add("inputs:shaping:cone:softness", "Cone softness", "Cone", "float", 0.f, 0, 1);
      add("inputs:shaping:focus", "Focus", "Cone", "float", 0.f, 0, 100);
    }
    // UsdLuxShapingAPI's IES profile, and OpenRemix's light cookie (inputs
    // outside UsdLux; usd/capture_writer.cpp), which other runtimes ignore.
    if (type == "SphereLight" || type == "RectLight" || type == "DiskLight" || type == "CylinderLight") {
      auto& ies = add("inputs:shaping:ies:file", "IES profile", "Light profile", "asset", "", 0, 1, "file");
      ies["filters"] = {"*.ies ; IES photometric profiles"};
      ies["tooltip"] = "A measured fixture's light distribution (IESNA LM-63 file), aimed along the light's axis. OpenRemix only.";
    }
    if (type == "SphereLight") {
      add("inputs:remix:cookie:texture", "Cookie texture", "Light cookie", "asset", "", 0, 1, "texture")
        ["tooltip"] = "A texture projected from the light like a slide projector: its colour times alpha tints the light, and outside it the light is dark. OpenRemix only.";
      add("inputs:remix:cookie:fov", "Projection width (degrees)", "Light cookie", "float", 0.f, 0, 179.9)
        ["tooltip"] = "How wide the cookie is projected. Zero turns the cookie off. OpenRemix only.";
      add("inputs:remix:cookie:up", "Cookie up (world)", "Light cookie", "float3", {0, 0, 1}, -1e6, 1e6, "vector")
        ["tooltip"] = "The world direction the top of the cookie faces. It must not point along the light. OpenRemix only.";
    }
  }
  auto shader = shader_for(selected);
  if (!shader) return fields;
  owner = shader;
  const auto source = read_value(shader.GetAttribute(TfToken("info:mdl:sourceAsset")), "").get<std::string>();
  const bool glass = source.find("Translucent") != std::string::npos;
  if (glass) {
    add("inputs:transmittance_color", "Transmission color", "Transmission", "color3f", {1, 1, 1}, 0, 1, "color");
    add("inputs:transmittance_texture", "Transmission texture", "Transmission", "asset", "", 0, 1, "texture");
    add("inputs:ior_constant", "Index of refraction", "Transmission", "float", 1.3f, 1, 3);
    auto& distance = add("inputs:transmittance_measurement_distance", "Measurement distance", "Transmission", "float", 1.f, 0.001, 65504);
    distance["disabled_by"] = "inputs:thin_walled";
    distance["tooltip"] = "The depth, in scene units, at which light passing through has the transmission color. Shorter distances make thick glass or water darker.";
    add("inputs:thin_walled", "Thin-walled", "Transmission", "bool", false, 0, 1, "bool")
      ["tooltip"] = "A sheet with no inside, like a window pane or bubble: light passes through without bending.";
    add("inputs:thin_wall_thickness", "Wall thickness", "Transmission", "float", 1.f, 0.001, 65504)
      ["enabled_by"] = "inputs:thin_walled";
    add("inputs:use_diffuse_layer", "Diffuse layer", "Transmission", "bool", false, 0, 1, "bool")
      ["tooltip"] = "Scatters light passing through, like frosted plastic, using the transmission texture as its color. Not shown by OpenRemix's path tracer yet.";
  } else {
    add("inputs:diffuse_color_constant", "Base color", "Base color", "color3f", {0.7, 0.7, 0.7}, 0, 1, "color");
    add("inputs:diffuse_texture", "Base color texture", "Base color", "asset", "", 0, 1, "texture");
    add("inputs:reflection_roughness_constant", "Roughness", "Surface", "float", 0.5f, 0, 1);
    add("inputs:reflectionroughness_texture", "Roughness texture", "Surface", "asset", "", 0, 1, "texture");
    add("inputs:metallic_constant", "Metallic", "Surface", "float", 0.f, 0, 1);
    add("inputs:metallic_texture", "Metallic texture", "Surface", "asset", "", 0, 1, "texture");
    add("inputs:opacity_constant", "Opacity", "Opacity", "float", 1.f, 0, 1)
      ["tooltip"] = "Opacity used when no base color texture is assigned. Textured materials use the texture's alpha channel.";
    add("inputs:use_legacy_alpha_state", "Use captured alpha state", "Opacity", "bool", true, 0, 1, "bool")
      ["tooltip"] = "Use each captured instance's alpha test and blend settings. Disable to override them with this material.";
    add("inputs:blend_enabled", "Enable blending", "Opacity", "bool", false, 0, 1, "bool")["disabled_by"] = "inputs:use_legacy_alpha_state";
    auto& blend = add("inputs:blend_type", "Blend mode", "Opacity", "int", 0);
    choices(blend, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
      {"Alpha", "Alpha + emissive", "Reverse alpha + emissive", "Color", "Color + emissive", "Reverse color + emissive", "Additive", "Multiply", "Double multiply", "Reverse alpha", "Reverse color"});
    blend["enabled_by"] = "inputs:blend_enabled"; blend["disabled_by"] = "inputs:use_legacy_alpha_state";
    auto& inverted = add("inputs:inverted_blend", "Invert blend", "Opacity", "bool", false, 0, 1, "bool");
    inverted["enabled_by"] = "inputs:blend_enabled"; inverted["disabled_by"] = "inputs:use_legacy_alpha_state";
    auto& cutout = add("inputs:alpha_test_type", "Alpha test", "Opacity", "int", 7);
    choices(cutout, {0, 1, 2, 3, 4, 5, 6, 7}, {"Never", "Less", "Equal", "Less or equal", "Greater", "Not equal", "Greater or equal (cutout)", "Always (no cutout)"});
    cutout["disabled_by"] = "inputs:use_legacy_alpha_state";
    add("inputs:alpha_test_reference_value", "Cutout threshold (0–255)", "Opacity", "float", 0.f, 0, 255)["disabled_by"] = "inputs:use_legacy_alpha_state";
    // AperturePBR's thin film, and OpenRemix's iridescence controls beyond it
    // (attributes outside the MDL inputs, which other runtimes ignore).
    add("inputs:enable_thin_film", "Enable iridescence", "Iridescence", "bool", false, 0, 1, "bool")
      ["tooltip"] = "A thin-film coating whose interference colours the reflection, like a soap bubble or oil slick.";
    auto& thickness = add("inputs:thin_film_thickness_constant", "Thickness (nm)", "Iridescence", "float", 200.f, 0.001, 1500);
    thickness["enabled_by"] = "inputs:enable_thin_film"; thickness["disabled_by"] = "inputs:thin_film_thickness_from_albedo_alpha";
    thickness["tooltip"] = "Film thickness in nanometres. 200–600 gives vivid colours; thicker films fade to pastel.";
    auto& from_alpha = add("inputs:thin_film_thickness_from_albedo_alpha", "Thickness from albedo alpha", "Iridescence", "bool", false, 0, 1, "bool");
    from_alpha["enabled_by"] = "inputs:enable_thin_film";
    from_alpha["tooltip"] = "Read the thickness from the base color texture's alpha (times 1500 nm). The material becomes fully opaque.";
    auto openremix = [&](const char* name, const char* label, const char* group, const char* type, Json fallback,
                         double minimum, double maximum, const char* tooltip, const char* control = "number") {
      auto& field = add(name, label, group, type, fallback, minimum, maximum, control);
      field["enabled_by"] = "inputs:enable_thin_film";
      field["tooltip"] = std::string(tooltip) + " OpenRemix only.";
    };
    openremix("openremix:iridescence:strength", "Strength", "Iridescence", "float", 1.f, 0, 1,
      "How much of the reflection is the film's.");
    openremix("openremix:iridescence:filmIor", "Film index of refraction", "Iridescence", "float", 1.5f, 1, 3,
      "Higher indices give stronger, more saturated colours (soap 1.33, oil 1.5, titanium oxide 2.4).");
    openremix("openremix:iridescence:structural", "Structural colour", "Iridescence", "float", 0.f, 0, 1,
      "Tints diffuse reflection with the film's colour too, like a beetle shell or butterfly wing, so rough surfaces shimmer.");
    openremix("openremix:iridescence:thicknessTexture", "Thickness texture", "Iridescence", "asset", "", 0, 1,
      "A map whose red channel spans the thickness range below, replacing the constant thickness.", "texture");
    openremix("openremix:iridescence:thicknessMin", "Texture thickness min (nm)", "Iridescence", "float", 100.f, 0, 2000,
      "The thickness where the map is black.");
    openremix("openremix:iridescence:thicknessMax", "Texture thickness max (nm)", "Iridescence", "float", 800.f, 0, 2000,
      "The thickness where the map is white.");
    openremix("openremix:iridescence:flowAmplitude", "Flow amplitude (nm)", "Iridescence flow", "float", 0.f, 0, 2000,
      "Animated swirls added to the thickness, like oil on water. Zero keeps the film still.");
    openremix("openremix:iridescence:flowScale", "Flow scale", "Iridescence flow", "float", 1.f, 0.001, 1e6,
      "The swirls' size, in scene units.");
    openremix("openremix:iridescence:flowSpeed", "Flow speed", "Iridescence flow", "float", 0.1f, -100, 100,
      "How fast the swirls drift, per second.");
  }
  add("inputs:normalmap_texture", "Normal texture", "Normal", "asset", "", 0, 1, "texture");
  add("inputs:enable_emission", "Enable emission", "Emission", "bool", false, 0, 1, "bool");
  add("inputs:emissive_color_constant", "Emission color", "Emission", "color3f", {0, 0, 0}, 0, 1, "color")["enabled_by"] = "inputs:enable_emission";
  add("inputs:emissive_intensity", "Emission intensity", "Emission", "float", 40.f)["enabled_by"] = "inputs:enable_emission";
  add("inputs:emissive_mask_texture", "Emission texture", "Emission", "asset", "", 0, 1, "texture")["enabled_by"] = "inputs:enable_emission";
  auto& filter = add("inputs:filter_mode", "Filtering", "Sampling", "uint", 1);
  choices(filter, {0, 1}, {"Nearest", "Linear"});
  for (auto axis : {"u", "v"}) {
    const auto name = std::string("inputs:wrap_mode_") + axis;
    auto& wrap = add(name.c_str(), axis[0] == 'u' ? "Wrap U" : "Wrap V", "Sampling", "uint", 1);
    choices(wrap, {0, 1, 2, 3}, {"Clamp", "Repeat", "Mirror", "Clip"});
  }
  return fields;
}
}

Json describe_authoring(const UsdPrim& prim, const SdfLayerHandle& edit_layer) {
  auto shader = shader_for(prim);
  std::string kind = shader ? "material" : UsdLuxLightAPI(prim) ? "light" : UsdGeomMesh(prim) ? "mesh" : "object";
  Json result{{"kind", kind}, {"type", prim.GetTypeName().GetString()}, {"fields", fields_for(prim)}, {"materials", Json::array()}};
  if (kind == "light") {
    auto cone = read_value(prim.GetAttribute(TfToken("inputs:shaping:cone:angle")), 180.f).get<double>();
    result["light_type"] = prim.GetTypeName() == TfToken("SphereLight") && cone < 180 ? "SpotLight" : prim.GetTypeName().GetString();
  }
  if (shader) result["surface_model"] = compose_material(prim)["kind"];
  auto add_material = [&](const UsdPrim& mesh, const std::string& slot) {
    auto material = UsdShadeMaterialBindingAPI(mesh).ComputeBoundMaterial();
    if (!material) return;
    auto path = material.GetPath().GetString();
    result["materials"].push_back({{"path", path}, {"name", material.GetPrim().GetName().GetString()}, {"slot", slot}});
  };
  if (auto mesh = UsdGeomMesh(prim)) {
    add_material(prim, "Mesh");
    for (auto subset : UsdGeomSubset::GetGeomSubsets(mesh, UsdGeomTokens->face, TfToken("materialBind")))
      add_material(subset.GetPrim(), subset.GetPrim().GetName().GetString());
  } else if (!shader && !UsdLuxLightAPI(prim)) {
    for (auto child : prim.GetChildren()) if (child.IsA<UsdGeomMesh>()) add_material(child, child.GetName().GetString());
  }
  for (auto& field : result["fields"]) {
    auto spec = edit_layer->GetAttributeAtPath(SdfPath(field["path"].get<std::string>()).AppendProperty(TfToken(field["property"].get<std::string>())));
    field["overridden"] = bool(spec && spec->HasDefaultValue());
  }
  return result;
}

void Document::set_authoring_property(const Json& request, bool reset) {
  const auto path = authored_path(request.at("path").get<std::string>());
  auto prim = stage_->GetPrimAtPath(SdfPath(path));
  if (!prim) throw std::runtime_error("Selection no longer exists");
  const auto name = request.at("property").get<std::string>();
  auto fields = fields_for(prim);
  auto found = std::find_if(fields.begin(), fields.end(), [&](const Json& f) { return f["property"] == name; });
  if (found == fields.end()) throw std::runtime_error("This property is not an authoring control for the selection");
  const auto& field = *found;
  prim = stage_->GetPrimAtPath(SdfPath(field["path"].get<std::string>()));
  if (reset) {
    auto attr = prim.GetAttribute(TfToken(name));
    if (attr) attr.Clear();
    update_light_extent(prim);
    return;
  }
  auto value = request.at("value");
  const auto control = field["control"].get<std::string>();
  if (control == "number") {
    if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < field["min"].get<double>() || value.get<double>() > field["max"].get<double>())
      throw std::runtime_error("Value is outside the supported range");
  } else if (control == "choice") {
    const auto& choices = field.at("choices");
    if (std::find(choices.begin(), choices.end(), value) == choices.end()) throw std::runtime_error("Invalid choice");
  } else if (control == "color") {
    if (!value.is_array() || value.size() != 3) throw std::runtime_error("Expected RGB color");
    for (auto& component : value) if (!component.is_number() || !std::isfinite(component.get<double>()) || component.get<double>() < 0 || component.get<double>() > 1)
      throw std::runtime_error("Color components must be between zero and one");
  } else if (control == "bool" && !value.is_boolean()) throw std::runtime_error("Expected a checkbox value");
  else if (control == "vector") {
    if (!value.is_array() || value.size() != 3) throw std::runtime_error("Expected an X, Y, Z vector");
    for (auto& component : value) if (!component.is_number() || !std::isfinite(component.get<double>()))
      throw std::runtime_error("Vector components must be finite numbers");
    if (value[0] == 0 && value[1] == 0 && value[2] == 0) throw std::runtime_error("The direction must not be zero");
  } else if (control == "texture" || control == "file") {
    if (!value.is_string()) throw std::runtime_error("Expected a file path");
    auto file = value.get<std::string>();
    if (!file.empty()) {
      auto absolute = fs::path(file);
      if (!absolute.is_absolute()) absolute = project_path_.parent_path() / absolute;
      if (!fs::is_regular_file(absolute)) throw std::runtime_error(control == "texture" ? "Texture file does not exist" : "File does not exist");
      value = fs::relative(absolute, fs::path(edit_layer_->GetRealPath()).parent_path()).generic_string();
    }
  }
  // UsdLuxShapingAPI's fallback cone is 90 degrees: keep an unshaped light
  // emitting everywhere when an IES profile, focus or softness first applies it.
  if (name.rfind("inputs:shaping:", 0) == 0 && !prim.HasAPI<UsdLuxShapingAPI>()) {
    auto shaping = UsdLuxShapingAPI::Apply(prim);
    if (name != "inputs:shaping:cone:angle" && !shaping.GetShapingConeAngleAttr().HasAuthoredValue())
      shaping.CreateShapingConeAngleAttr().Set(180.f);
  }
  if (!prim.GetAttribute(TfToken(name))) {
    const auto type = SdfSchema::GetInstance().FindType(TfToken(field["type"].get<std::string>()));
    if (!type || !prim.CreateAttribute(TfToken(name), type, false)) throw std::runtime_error("Could not create authoring attribute");
  }
  set_property({{"path", prim.GetPath().GetString()}, {"property", name}, {"value", value}});
  // A cookie shows only with a projection width: give a new one a usable width.
  if (name == "inputs:remix:cookie:texture" && !value.get<std::string>().empty() &&
      read_value(prim.GetAttribute(TfToken("inputs:remix:cookie:fov")), 0.f).get<double>() <= 0) {
    auto fov = prim.GetAttribute(TfToken("inputs:remix:cookie:fov"));
    if (!fov) fov = prim.CreateAttribute(TfToken("inputs:remix:cookie:fov"), SdfValueTypeNames->Float, false);
    fov.Set(60.f);
  }
}

// AperturePBR's surface model: the shader's MDL module, keeping its inputs.
// The texture moves between the opaque base color and the transmission slot
// when the other is empty, so captured glass keeps its tint.
void Document::set_material_model(const Json& request) {
  auto prim = stage_->GetPrimAtPath(SdfPath(authored_path(request.at("path").get<std::string>())));
  auto shader = UsdShadeShader(shader_for(prim));
  if (!shader) throw std::runtime_error("Select a material");
  const auto model = request.at("model").get<std::string>();
  if (model != "opaque" && model != "translucent") throw std::runtime_error("Unsupported surface model");
  const bool translucent = model == "translucent";
  const std::string name = translucent ? "AperturePBR_Translucent" : "AperturePBR_Opacity";
  SdfAssetPath current;
  shader.GetSourceAsset(&current, TfToken("mdl"));
  auto module = current.GetAssetPath();
  const auto slash = module.find_last_of("/\\");
  module = (slash == std::string::npos ? std::string() : module.substr(0, slash + 1)) + name + ".mdl";
  shader.CreateImplementationSourceAttr().Set(TfToken("sourceAsset"));
  shader.SetSourceAsset(SdfAssetPath(module), TfToken("mdl"));
  shader.SetSourceAssetSubIdentifier(TfToken(name), TfToken("mdl"));
  const std::string from = translucent ? "diffuse_texture" : "transmittance_texture";
  const std::string to = translucent ? "transmittance_texture" : "diffuse_texture";
  const auto texture = resolved_texture_path(shader.GetPrim(), ("inputs:" + from).c_str());
  if (!texture.empty() && resolved_texture_path(shader.GetPrim(), ("inputs:" + to).c_str()).empty()) {
    const auto relative = fs::relative(fs::path(texture), fs::path(edit_layer_->GetRealPath()).parent_path()).generic_string();
    shader.CreateInput(TfToken(to), SdfValueTypeNames->Asset).Set(SdfAssetPath(relative));
  }
}

void Document::set_light_type(const Json& request) {
  auto prim = stage_->GetPrimAtPath(SdfPath(authored_path(request.at("path").get<std::string>())));
  if (!UsdLuxLightAPI(prim)) throw std::runtime_error("Select a light");
  const auto type = request.at("type").get<std::string>();
  const std::set<std::string> types{"SphereLight", "SpotLight", "RectLight", "DiskLight", "CylinderLight", "DistantLight", "DomeLight"};
  if (!types.count(type)) throw std::runtime_error("Unsupported light type");
  if (!prim.SetTypeName(TfToken(type == "SpotLight" ? "SphereLight" : type))) throw std::runtime_error("Could not change light type");
  // Clear a previous spotlight's shaping effect while keeping unrelated data.
  if (type == "SpotLight" || prim.HasAPI<UsdLuxShapingAPI>() || prim.GetAttribute(TfToken("inputs:shaping:cone:angle"))) {
    auto shaping = UsdLuxShapingAPI::Apply(prim);
    shaping.CreateShapingConeAngleAttr().Set(type == "SpotLight" ? 35.f : 180.f);
    shaping.CreateShapingConeSoftnessAttr().Set(0.f);
    shaping.CreateShapingFocusAttr().Set(0.f);
  }
  update_light_extent(prim);
}

Json preview_asset(const fs::path& path) {
  auto stage = UsdStage::Open(fs::absolute(path).string());
  if (!stage || !stage->GetDefaultPrim()) throw std::runtime_error("Asset has no USD default prim");
  const auto root = stage->GetDefaultPrim();
  Json result{{"asset", fs::absolute(path).generic_string()}, {"kind", root.IsA<UsdShadeMaterial>() ? "material" : "mesh"}};
  if (root.IsA<UsdShadeMaterial>()) result["material"] = compose_material(root);
  else result["scene"] = compose_scene(stage, 0);
  std::vector<SdfLayerRefPtr> layers;
  std::vector<std::string> assets, missing;
  UsdUtilsComputeAllDependencies(SdfAssetPath(stage->GetRootLayer()->GetRealPath()), &layers, &assets, &missing);
  std::set<std::string> files;
  files.insert(fs::absolute(path).generic_string());
  for (const auto& layer : layers) files.insert(layer->GetRealPath());
  for (const auto& asset : assets) files.insert(asset);
  const auto materials = root.IsA<UsdShadeMaterial>() ? Json::array({result["material"]}) : result["scene"]["materials"];
  for (const auto& material : materials) for (auto it = material.begin(); it != material.end(); ++it)
    if (it.key().find("_texture") != std::string::npos && it.value().is_string()) files.insert(it.value().get<std::string>());
  result["dependencies"] = Json::array();
  for (const auto& file : files) if (!file.empty() && fs::is_regular_file(file))
    result["dependencies"].push_back({{"path", fs::absolute(file).generic_string()}, {"size", fs::file_size(file)},
      {"modified", fs::last_write_time(file).time_since_epoch().count()}});
  return result;
}
}
