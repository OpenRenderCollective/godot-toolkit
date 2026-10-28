#include "document.h"
#include "alpha_state.h"
#include "skinning.h"
#include "capture_categories.h"
#include "scene_buffers.h"

#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <array>
#include <algorithm>
#include <unordered_map>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

PXR_NAMESPACE_USING_DIRECTIVE
namespace editor {
namespace {
std::string source_path(const UsdPrim& prim) {
  for (const auto& spec : prim.GetPrimStack()) {
    const auto path = spec->GetPath().GetString();
    if (path.rfind("/RootNode/meshes/", 0) == 0 || path.rfind("/RootNode/lights/", 0) == 0) return path;
  }
  return prim.GetPath().GetString();
}
Json matrix_json(const GfMatrix4d& matrix) {
  Json values = Json::array();
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) values.push_back(matrix[r][c]);
  return values;
}
template<typename T> T attribute(const UsdPrim& prim, const char* name, const T& fallback) {
  T result = fallback;
  if (!prim) return result;
  auto attr = prim.GetAttribute(TfToken(name));
  if (attr) attr.Get(&result);
  return result;
}
int sampler_mode(const UsdPrim& shader, const char* name, int maximum) {
  // Captures author uints; MDL editors can author ints. Remix defaults to
  // linear filtering and repeat wrapping (both encoded as 1).
  if (!shader) return 1;
  const auto attr = shader.GetAttribute(TfToken(name));
  VtValue value;
  if (!attr || !attr.Get(&value)) return 1;
  if (value.IsHolding<unsigned int>()) {
    const auto mode = value.Get<unsigned int>();
    return mode <= unsigned(maximum) ? int(mode) : 1;
  }
  if (value.IsHolding<int>()) {
    const auto mode = value.Get<int>();
    return mode >= 0 && mode <= maximum ? mode : 1;
  }
  return 1;
}
double number_attribute(const UsdPrim& prim, const char* name, double fallback, double maximum) {
  VtValue value;
  const auto attr = prim ? prim.GetAttribute(TfToken(name)) : UsdAttribute();
  if (!attr || !attr.Get(&value)) return fallback;
  const auto converted = VtValue::Cast<double>(value);
  if (!converted.IsHolding<double>()) return fallback;
  const auto number = converted.Get<double>();
  return std::isfinite(number) && number >= 0 && number <= maximum ? number : fallback;
}
Json capture_alpha(const UsdPrim& prim) {
  auto result = capture_alpha_defaults();
  for (auto it = result.begin(); it != result.end(); ++it) {
    const auto name = "primvars:_remix_metadata:" + it.key();
    if (it.value().is_boolean()) it.value() = attribute(prim, name.c_str(), it.value().get<bool>());
    else {
      const auto key = it.key();
      const double maximum = key == "tFactor" ? double(UINT32_MAX) : key == "alphaTestReferenceValue" ? 255 :
        key == "alphaTestCompareOp" || key.find("Operation") != std::string::npos ? 7 :
        key.find("Source") != std::string::npos ? 3 : key.find("BlendOp") != std::string::npos ? 4 : key == "writeMask" ? 15 : 18;
      it.value() = uint32_t(number_attribute(prim, name.c_str(), it.value().get<double>(), maximum));
    }
  }
  return result;
}
uint32_t packed_color(const GfVec3f& color, float alpha) {
  auto byte = [](float value) { return uint32_t(std::lround(std::clamp(std::isfinite(value) ? value : 1.f, 0.f, 1.f) * 255.f)); };
  // HardcodedVertex's color is VK_FORMAT_B8G8R8A8_UNORM.
  return byte(color[2]) | (byte(color[1]) << 8) | (byte(color[0]) << 16) | (byte(alpha) << 24);
}
std::string texture(const UsdPrim& shader, const char* name, Json& warnings) {
  if (!shader) return {};
  const auto attr = shader.GetAttribute(TfToken(name));
  SdfAssetPath value;
  if (!attr || !attr.Get(&value) || value.GetAssetPath().empty()) return {};
  if (!value.GetResolvedPath().empty()) return value.GetResolvedPath();

  // Captures made on Windows can contain backslashes even when opened on Linux.
  // Anchor the normalized identifier at the strongest authored value's layer:
  // references and sublayers may put it far from the project or capture root.
  auto normalized = value.GetAssetPath();
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  ArResolverContextBinder context(shader.GetStage()->GetPathResolverContext());
  std::string anchor;
  for (const auto& spec : attr.GetPropertyStack(UsdTimeCode::Default())) {
    if (!spec->HasInfo(SdfFieldKeys->Default)) continue;
    anchor = spec->GetLayer()->GetIdentifier();
    auto identifier = SdfComputeAssetPathRelativeToLayer(spec->GetLayer(), normalized);
    auto resolved = ArGetResolver().Resolve(identifier);
    if (!resolved.empty()) return resolved.GetPathString();
    // A missing strong opinion must not silently fall back to a weaker texture.
    break;
  }
  warnings.push_back("Unresolved texture " + attr.GetPath().GetString() + ": " + value.GetAssetPath() +
    (anchor.empty() ? "" : " (authored in " + anchor + ")"));
  return {};
}
Json material_json(const UsdShadeMaterial& material, Json& warnings) {
  auto connected = material.ComputeSurfaceSource(TfToken("mdl"));
  UsdPrim shader = connected ? connected.GetPrim() : UsdPrim();
  if (!shader) for (auto child : material.GetPrim().GetChildren()) if (child.IsA<UsdShadeShader>()) { shader = child; break; }
  auto color = attribute(shader, "inputs:diffuse_color_constant", GfVec3f(0.7f));
  auto emission = attribute(shader, "inputs:emissive_color_constant", GfVec3f(0.0f));
  auto transmission = attribute(shader, "inputs:transmittance_color", GfVec3f(1.0f));
  auto source = attribute(shader, "info:mdl:sourceAsset", SdfAssetPath()).GetAssetPath();
  const bool translucent = source.find("Translucent") != std::string::npos;
  return {
    {"id", material.GetPath().GetString()},
    {"kind", translucent ? "translucent" : "opaque"},
    {"albedo", {color[0], color[1], color[2]}},
    {"roughness", attribute(shader, "inputs:reflection_roughness_constant", 0.5f)},
    {"metallic", attribute(shader, "inputs:metallic_constant", 0.0f)},
    {"opacity", attribute(shader, "inputs:opacity_constant", 1.0f)},
    {"use_legacy_alpha", attribute(shader, "inputs:use_legacy_alpha_state", true)},
    {"blend_enabled", attribute(shader, "inputs:blend_enabled", false)},
    {"blend_type", int(number_attribute(shader, "inputs:blend_type", 0, 10))},
    {"inverted_blend", attribute(shader, "inputs:inverted_blend", false)},
    {"alpha_test", int(number_attribute(shader, "inputs:alpha_test_type", 7, 7))},
    {"alpha_reference", int(number_attribute(shader, "inputs:alpha_test_reference_value", 0, 255))},
    {"emissive", {emission[0], emission[1], emission[2]}},
    {"emissive_intensity", attribute(shader, "inputs:enable_emission", false) ? attribute(shader, "inputs:emissive_intensity", 40.0f) : 0.0f},
    {"ior", attribute(shader, "inputs:ior_constant", 1.3f)},
    {"transmittance", {transmission[0], transmission[1], transmission[2]}},
    {"filter_mode", sampler_mode(shader, "inputs:filter_mode", 1)},
    {"wrap_mode_u", sampler_mode(shader, "inputs:wrap_mode_u", 3)},
    {"wrap_mode_v", sampler_mode(shader, "inputs:wrap_mode_v", 3)},
    {"albedo_texture", texture(shader, translucent ? "inputs:transmittance_texture" : "inputs:diffuse_texture", warnings)},
    {"normal_texture", texture(shader, "inputs:normalmap_texture", warnings)},
    {"normal_encoding", attribute(shader, "inputs:encoding", 0)},
    {"roughness_texture", texture(shader, "inputs:reflectionroughness_texture", warnings)},
    {"metallic_texture", texture(shader, "inputs:metallic_texture", warnings)},
    {"emissive_texture", texture(shader, "inputs:emissive_mask_texture", warnings)}
  };
}
int element_index(const TfToken& interpolation, int face, int corner, int vertex) {
  if (interpolation == UsdGeomTokens->constant) return 0;
  if (interpolation == UsdGeomTokens->uniform) return face;
  if (interpolation == UsdGeomTokens->faceVarying) return corner;
  return vertex;
}
// Project onto the dominant plane so concave polygon faces can be ear-clipped.
void triangulate(const VtVec3fArray& points, const VtIntArray& indices, int start, int count, std::vector<std::array<int, 3>>& triangles) {
  triangles.clear();
  if (count < 3) return;
  if (count == 3) { triangles.push_back({start, start + 1, start + 2}); return; }
  GfVec3f normal(0);
  for (int i = 0; i < count; ++i) normal += GfCross(points[indices[start + i]], points[indices[start + (i + 1) % count]]);
  int axis = 0;
  if (std::abs(normal[1]) > std::abs(normal[axis])) axis = 1;
  if (std::abs(normal[2]) > std::abs(normal[axis])) axis = 2;
  int x = (axis + 1) % 3, y = (axis + 2) % 3;
  auto cross = [&](int a, int b, int c) {
    const auto& pa = points[indices[start + a]];
    const auto& pb = points[indices[start + b]];
    const auto& pc = points[indices[start + c]];
    return (pb[x] - pa[x]) * (pc[y] - pa[y]) - (pb[y] - pa[y]) * (pc[x] - pa[x]);
  };
  const float sign = normal[axis] >= 0 ? 1.f : -1.f;
  std::vector<int> corners;
  for (int i = 0; i < count; ++i) corners.push_back(i);
  while (corners.size() > 3) {
    bool found = false;
    for (size_t i = 0; i < corners.size(); ++i) {
      int a = corners[(i + corners.size() - 1) % corners.size()], b = corners[i], c = corners[(i + 1) % corners.size()];
      if (cross(a, b, c) * sign <= 1e-12f) continue;
      bool contains = false;
      for (int p : corners) {
        if (p == a || p == b || p == c) continue;
        if (cross(a, b, p) * sign >= 0 && cross(b, c, p) * sign >= 0 && cross(c, a, p) * sign >= 0) { contains = true; break; }
      }
      if (contains) continue;
      triangles.push_back({start + a, start + b, start + c});
      corners.erase(corners.begin() + i); found = true; break;
    }
    if (!found) throw std::runtime_error("Degenerate or self-intersecting polygon");
  }
  triangles.push_back({start + corners[0], start + corners[1], start + corners[2]});
}
uint64_t mix(uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  size_t i = 0;
  for (uint64_t word; i + 8 <= size; i += 8) { std::memcpy(&word, bytes + i, 8); hash = (hash ^ word) * 0x100000001b3ull; hash ^= hash >> 29; }
  for (; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
  return (hash ^ size) * 0x9e3779b97f4a7c15ull;
}
template<typename T> uint64_t mix(uint64_t hash, const VtArray<T>& values) { return mix(hash, values.cdata(), values.size() * sizeof(T)); }
uint64_t mix(uint64_t hash, const std::string& value) { return mix(hash, value.data(), value.size()); }
// Bytewise, so arrays holding NaN still match themselves.
template<typename T> bool same(const VtArray<T>& a, const VtArray<T>& b) {
  return a.size() == b.size() && (a.empty() || a.cdata() == b.cdata() || !std::memcmp(a.cdata(), b.cdata(), a.size() * sizeof(T)));
}
// Everything that determines a prototype's buffers. Instances whose sources
// compare equal share one prototype, whichever prims or layers authored them.
struct GeometrySource {
  VtVec3fArray points, normals, colors;
  VtVec2fArray uv;
  VtFloatArray opacities;
  VtIntArray indices, counts, holes;
  TfToken normal_interpolation, uv_interpolation, color_interpolation, opacity_interpolation;
  bool left_handed = false;
  // The mesh binding, then one material and face list per materialBind subset.
  std::vector<std::string> materials;
  std::vector<VtIntArray> subsets;
  uint64_t hash() const {
    uint64_t hash = left_handed ? 1 : 2;
    hash = mix(mix(mix(mix(mix(hash, points), normals), colors), uv), opacities);
    hash = mix(mix(mix(hash, indices), counts), holes);
    for (const auto* token : {&normal_interpolation, &uv_interpolation, &color_interpolation, &opacity_interpolation}) hash = mix(hash, token->GetString());
    for (const auto& material : materials) hash = mix(hash, material);
    for (const auto& subset : subsets) hash = mix(hash, subset);
    return hash;
  }
  bool operator==(const GeometrySource& other) const {
    return left_handed == other.left_handed && normal_interpolation == other.normal_interpolation && uv_interpolation == other.uv_interpolation &&
      color_interpolation == other.color_interpolation && opacity_interpolation == other.opacity_interpolation && materials == other.materials &&
      same(points, other.points) && same(normals, other.normals) && same(colors, other.colors) && same(uv, other.uv) &&
      same(opacities, other.opacities) && same(indices, other.indices) && same(counts, other.counts) && same(holes, other.holes) &&
      std::equal(subsets.begin(), subsets.end(), other.subsets.begin(), other.subsets.end(), [](const auto& a, const auto& b) { return same(a, b); });
  }
};
struct Prototype {
  GeometrySource source;
  std::vector<UsdShadeMaterial> materials;
};
struct CornerHash {
  size_t operator()(const std::array<int, 5>& key) const {
    uint64_t hash = 14695981039346656037ull;
    for (int value : key) hash = (hash ^ uint32_t(value)) * 0x100000001b3ull;
    return size_t(hash ^ hash >> 32);
  }
};
struct SurfaceBuffers {
  UsdShadeMaterial material;
  std::vector<float> positions, normals, uvs;
  std::vector<uint32_t> colors;
  std::vector<int32_t> indices, by_point;
  std::unordered_map<std::array<int, 5>, int32_t, CornerHash> corners;
};
// Corners share a vertex when they use the same point and primvar elements.
// Faces without valid normals get a per-triangle key for their flat normal.
// When every primvar is constant or per point, the point index is that key.
Json prototype_json(const GeometrySource& s, const std::vector<UsdShadeMaterial>& bound, std::vector<UsdShadeMaterial>& surface_materials,
    std::set<std::string>& materials, Json& result) {
  std::vector<int> face_material(s.counts.size(), 0);
  for (size_t i = 0; i < s.subsets.size(); ++i)
    for (int face : s.subsets[i]) if (face >= 0 && size_t(face) < face_material.size()) face_material[face] = int(i + 1);
  std::vector<bool> hole(s.counts.size());
  for (int face : s.holes) if (face >= 0 && size_t(face) < hole.size()) hole[face] = true;
  const bool has_colors = !s.colors.empty() || !s.opacities.empty();
  const auto& points = s.points;
  const auto& indices = s.indices;
  auto per_point = [](const TfToken& interpolation) {
    return interpolation != UsdGeomTokens->uniform && interpolation != UsdGeomTokens->faceVarying;
  };
  const bool by_point = per_point(s.normal_interpolation) && per_point(s.uv_interpolation) && per_point(s.color_interpolation) &&
    per_point(s.opacity_interpolation) && (s.normal_interpolation == UsdGeomTokens->constant ? !s.normals.empty() : s.normals.size() >= points.size());
  auto element = [](const TfToken& interpolation, int face, int corner, int vertex, size_t size) {
    if (interpolation.IsEmpty()) return -1;
    const int index = element_index(interpolation, face, corner, vertex);
    return index >= 0 && size_t(index) < size ? index : -1;
  };
  std::map<std::string, SurfaceBuffers> surfaces;
  std::vector<SurfaceBuffers*> slots(s.materials.size());
  int corner = 0;
  std::vector<std::array<int, 3>> triangles;
  for (size_t face = 0; face < s.counts.size(); ++face) {
    const int count = s.counts[face], first = corner; corner += count;
    if (hole[face]) continue;
    auto*& slot = slots[face_material[face]];
    if (!slot) {
      const auto& material = bound[face_material[face]];
      const auto& id = s.materials[face_material[face]];
      if (material && materials.insert(id).second) result["materials"].push_back(material_json(material, result["warnings"]));
      slot = &surfaces[id]; slot->material = material;
      if (by_point && slot->by_point.empty()) slot->by_point.assign(points.size(), -1);
    }
    auto& surface = *slot;
    triangulate(points, indices, first, count, triangles);
    for (auto triangle : triangles) {
      if (s.left_handed) std::swap(triangle[1], triangle[2]);
      std::optional<GfVec3f> geometric;
      const int flat_key = -2 - int(surface.indices.size() / 3);
      for (int c : triangle) {
        const int vertex = indices[c];
        const auto next = int32_t(surface.positions.size() / 3);
        int32_t index = by_point ? surface.by_point[vertex] : -1;
        const int n = index >= 0 ? 0 : element(s.normal_interpolation, int(face), c, vertex, s.normals.size());
        if (index < 0 && n < 0 && !geometric)
          geometric = GfCross(points[indices[triangle[1]]] - points[indices[triangle[0]]], points[indices[triangle[2]]] - points[indices[triangle[0]]]).GetNormalized();
        if (index < 0) {
          const int u = element(s.uv_interpolation, int(face), c, vertex, s.uv.size());
          const int ci = has_colors ? element(s.color_interpolation, int(face), c, vertex, s.colors.size()) : -1;
          const int ai = has_colors ? element(s.opacity_interpolation, int(face), c, vertex, s.opacities.size()) : -1;
          bool inserted = true;
          if (by_point) index = surface.by_point[vertex] = next;
          else {
            const auto entry = surface.corners.try_emplace({vertex, n >= 0 ? n : flat_key, u, ci, ai}, next);
            index = entry.first->second; inserted = entry.second;
          }
          if (inserted) {
            const auto& point = points[vertex];
            const auto normal = n >= 0 ? s.normals[n] : *geometric;
            const auto texcoord = u >= 0 ? s.uv[u] : GfVec2f(0);
            surface.positions.insert(surface.positions.end(), {point[0], point[1], point[2]});
            surface.normals.insert(surface.normals.end(), {normal[0], normal[1], normal[2]});
            surface.uvs.insert(surface.uvs.end(), {texcoord[0], texcoord[1]});
            if (has_colors) surface.colors.push_back(packed_color(ci >= 0 ? s.colors[ci] : GfVec3f(1), ai >= 0 ? s.opacities[ai] : 1.f));
          }
        }
        surface.indices.push_back(index);
      }
    }
  }
  Json parts = Json::array();
  for (auto& [id, surface] : surfaces) {
    Json part{{"material", id}, {"positions", make_buffer(surface.positions, BufferType::vec3)}, {"normals", make_buffer(surface.normals, BufferType::vec3)},
      {"uvs", make_buffer(surface.uvs, BufferType::vec2)}, {"indices", make_buffer(surface.indices, BufferType::i32)}};
    // Consumers treat missing colors as opaque white.
    if (std::any_of(surface.colors.begin(), surface.colors.end(), [](uint32_t color) { return color != 0xffffffffu; }))
      part["colors"] = make_buffer(surface.colors, BufferType::u32);
    parts.push_back(std::move(part));
    surface_materials.push_back(surface.material);
  }
  return {{"surfaces", std::move(parts)}};
}
Json light_json(const UsdPrim& prim, const GfMatrix4d& world, Json& warnings) {
  const auto path = prim.GetPath().GetString();
  auto color = attribute(prim, "inputs:color", GfVec3f(1));
  if (attribute(prim, "inputs:enableColorTemperature", false))
    color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(attribute(prim, "inputs:colorTemperature", 6500.f)));
  return {{"id", path}, {"source_path", source_path(prim)}, {"type", prim.GetTypeName().GetString()},
    {"transform", matrix_json(world)}, {"color", {color[0], color[1], color[2]}},
    {"intensity", attribute(prim, "inputs:intensity", 1.f) * std::pow(2.f, attribute(prim, "inputs:exposure", 0.f))},
    {"radius", attribute(prim, "inputs:radius", 0.5f)}, {"width", attribute(prim, "inputs:width", 1.f)},
    {"height", attribute(prim, "inputs:height", 1.f)}, {"length", attribute(prim, "inputs:length", 1.f)},
    {"angle", attribute(prim, "inputs:angle", 0.53f)}, {"cone_angle", attribute(prim, "inputs:shaping:cone:angle", 180.f)},
    {"cone_softness", attribute(prim, "inputs:shaping:cone:softness", 0.f)},
    {"focus", attribute(prim, "inputs:shaping:focus", 0.f)},
    {"texture", texture(prim, "inputs:texture:file", warnings)}};
}

}

std::string resolved_texture_path(const UsdPrim& prim, const char* name) {
  Json warnings = Json::array();
  return texture(prim, name, warnings);
}
Json compose_material(const UsdPrim& prim) {
  Json warnings = Json::array();
  auto material = UsdShadeMaterial(prim);
  if (!material && prim.IsA<UsdShadeShader>()) material = UsdShadeMaterial(prim.GetParent());
  if (!material) throw std::runtime_error("Expected a USD material");
  auto result = material_json(material, warnings);
  result["warnings"] = warnings;
  return result;
}
Json compose_lights(const UsdStageRefPtr& stage, uint64_t revision) {
  Json result{{"revision", revision}, {"lights", Json::array()}, {"warnings", Json::array()}};
  UsdGeomXformCache transforms(UsdTimeCode::EarliestTime());
  for (auto prim : UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies())) {
    // Mesh emitters take the geometry branch in compose_scene as well.
    if (!UsdLuxLightAPI(prim) || prim.IsA<UsdGeomMesh>() || UsdGeomImageable(prim).ComputeVisibility() == UsdGeomTokens->invisible) continue;
    result["lights"].push_back(light_json(prim, transforms.GetLocalToWorldTransform(prim), result["warnings"]));
  }
  return result;
}
Json compose_scene(const UsdStageRefPtr& stage, uint64_t revision) {
  Json result{{"revision", revision}, {"prototypes", Json::array()}, {"meshes", Json::array()}, {"materials", Json::array()},
    {"lights", Json::array()}, {"cameras", Json::array()}, {"warnings", Json::array()},
    {"up_axis", UsdGeomGetStageUpAxis(stage).GetString()}, {"meters_per_unit", UsdGeomGetStageMetersPerUnit(stage)}};
  const auto time = UsdTimeCode::EarliestTime();
  UsdGeomXformCache transforms(time);
  SceneSkinning skinning(transforms);
  CaptureCategories categories(stage, result["warnings"]);
  result["category_sources"] = categories.sources();
  std::set<std::string> materials;
  std::vector<Prototype> prototypes;
  std::unordered_multimap<uint64_t, size_t> prototype_index;
  for (auto prim : UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies())) {
    auto imageable = UsdGeomImageable(prim);
    if (imageable && imageable.ComputeVisibility() == UsdGeomTokens->invisible) continue;
    const auto path = prim.GetPath().GetString();
    const auto world = transforms.GetLocalToWorldTransform(prim);
    if (auto mesh = UsdGeomMesh(prim)) {
      try {
        GeometrySource source;
        mesh.GetPointsAttr().Get(&source.points, time); mesh.GetFaceVertexIndicesAttr().Get(&source.indices, time);
        mesh.GetFaceVertexCountsAttr().Get(&source.counts, time); mesh.GetNormalsAttr().Get(&source.normals, time);
        mesh.GetHoleIndicesAttr().Get(&source.holes, time);
        std::string reason;
        if (!UsdGeomMesh::ValidateTopology(source.indices, source.counts, source.points.size(), &reason)) throw std::runtime_error(reason);
        source.normal_interpolation = mesh.GetNormalsInterpolation();
        auto normal_primvar = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("normals"));
        if (normal_primvar && normal_primvar.ComputeFlattened(&source.normals, time)) source.normal_interpolation = normal_primvar.GetInterpolation();
        std::string skeleton;
        try { skeleton = skinning.deform(mesh, source.points, source.normals, source.normal_interpolation, source.indices, source.counts); }
        catch (const std::exception& error) {
          result["warnings"].push_back(path + ": " + error.what() + "; showing undeformed geometry");
        }
        if (auto st = UsdGeomPrimvarsAPI(mesh).GetPrimvar(TfToken("st"))) {
          st.ComputeFlattened(&source.uv, time); source.uv_interpolation = st.GetInterpolation();
        }
        if (auto color = UsdGeomPrimvarsAPI(mesh).FindPrimvarWithInheritance(TfToken("displayColor"))) {
          color.ComputeFlattened(&source.colors, time); source.color_interpolation = color.GetInterpolation();
        }
        if (auto opacity = UsdGeomPrimvarsAPI(mesh).FindPrimvarWithInheritance(TfToken("displayOpacity"))) {
          opacity.ComputeFlattened(&source.opacities); source.opacity_interpolation = opacity.GetInterpolation();
        }
        TfToken orientation; mesh.GetOrientationAttr().Get(&orientation, time);
        source.left_handed = orientation == UsdGeomTokens->leftHanded;
        std::vector<UsdShadeMaterial> bound{UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial()};
        source.materials.push_back(bound[0] ? bound[0].GetPath().GetString() : "default");
        for (auto subset : UsdGeomSubset::GetGeomSubsets(mesh, UsdGeomTokens->face, TfToken("materialBind"))) {
          source.subsets.emplace_back(); subset.GetIndicesAttr().Get(&source.subsets.back(), time);
          bound.push_back(UsdShadeMaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial());
          source.materials.push_back(bound.back() ? bound.back().GetPath().GetString() : "default");
        }
        const auto key = source.hash();
        size_t index = prototypes.size();
        for (auto [it, end] = prototype_index.equal_range(key); it != end; ++it)
          if (prototypes[it->second].source == source) { index = it->second; break; }
        if (index == prototypes.size()) {
          Prototype prototype;
          result["prototypes"].push_back(prototype_json(source, bound, prototype.materials, materials, result));
          prototype.source = std::move(source);
          prototypes.push_back(std::move(prototype)); prototype_index.emplace(key, index);
        }
        Json surfaces = Json::array();
        for (const auto& material : prototypes[index].materials) surfaces.push_back({{"category_flags", categories.flags(prim, material)}});
        result["meshes"].push_back({{"id", path}, {"source_path", source_path(prim)}, {"prototype", index}, {"transform", matrix_json(world)},
          {"surfaces", std::move(surfaces)}, {"skinned", !skeleton.empty()}, {"skeleton", skeleton},
          {"double_sided", attribute(prim, "doubleSided", false)}, {"alpha_state", capture_alpha(prim)},
          {"ignore_alpha", attribute(prim, "remix_category:ignore_alpha_channel", false)},
          {"alpha_blend_to_cutout", attribute(prim, "remix_category:alpha_blend_to_cutout", false)}});
      } catch (const std::exception& error) { result["warnings"].push_back(path + ": " + error.what()); }
    } else if (auto light = UsdLuxLightAPI(prim)) {
      result["lights"].push_back(light_json(prim, world, result["warnings"]));
    } else if (auto camera = UsdGeomCamera(prim)) {
      result["cameras"].push_back({{"id", path}, {"transform", matrix_json(world)},
        {"focal_length", attribute(prim, "focalLength", 50.f)}, {"horizontal_aperture", attribute(prim, "horizontalAperture", 20.955f)}});
    }
  }
  result["skeletons"] = skinning.skeletons();
  return result;
}
}
