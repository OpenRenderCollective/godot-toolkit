#pragma once
#include "scene_buffers.h"

namespace editor {
// Expands one composed instance into per-surface triangle-corner vertices
// [px, py, pz, nx, ny, nz, u, v], with the instance's category flags.
inline nlohmann::json expanded_mesh(const nlohmann::json& scene, const nlohmann::json& mesh) {
  auto result = mesh;
  result.erase("prototype");
  result["surfaces"] = nlohmann::json::array();
  const auto& prototype = scene.at("prototypes").at(mesh.at("prototype").get<size_t>());
  for (size_t s = 0; s < prototype.at("surfaces").size(); ++s) {
    const auto& source = prototype.at("surfaces").at(s);
    const auto positions = read_buffer<float>(source.at("positions")), normals = read_buffer<float>(source.at("normals")), uvs = read_buffer<float>(source.at("uvs"));
    const auto colors = source.contains("colors") ? read_buffer<uint32_t>(source.at("colors")) : std::vector<uint32_t>();
    nlohmann::json surface{{"material", source.at("material")}, {"category_flags", mesh.at("surfaces").at(s).at("category_flags")},
      {"vertices", nlohmann::json::array()}, {"indices", nlohmann::json::array()}};
    for (auto index : read_buffer<int32_t>(source.at("indices"))) {
      surface["indices"].push_back(surface["vertices"].size());
      surface["vertices"].push_back({positions[index * 3], positions[index * 3 + 1], positions[index * 3 + 2],
        normals[index * 3], normals[index * 3 + 1], normals[index * 3 + 2], uvs[index * 2], uvs[index * 2 + 1]});
      if (!colors.empty()) surface["colors"].push_back(colors[index]);
    }
    result["surfaces"].push_back(std::move(surface));
  }
  return result;
}
inline nlohmann::json expanded_mesh(const nlohmann::json& scene, size_t index) { return expanded_mesh(scene, scene.at("meshes").at(index)); }
}
