module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:asset;
import vecgltf;
import rstd;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::asset {
auto load(ref<str> path) -> Arc<Asset> {
  auto result = LoadAsset(path);
  if (result.is_err()) {
    auto error = result.unwrap_err();
    rstd::io::eprintln("{}: {}: {}", error.source, error.location,
                       error.message);
    rstd::panic("asset load failed");
  }
  return result.unwrap();
}
TEST(Asset, Loading) {
  auto json = load("tests/data/triangle.gltf"_str);
  rstd_assert(json->source == "tests/data/triangle.gltf"_str);
  auto binary = load("tests/data/triangle.glb"_str);
  rstd_assert(count(json->meshes) == 1 && count(binary->meshes) == 1);
  rstd_assert(count(json->nodes) == 2 && count(json->scenes) == 1);
  rstd_assert(at(json->nodes, 0).name == at(json->nodes, 1).name);
  rstd_assert(at(json->nodes, 0).mesh == at(json->nodes, 1).mesh);
  const auto &first = at(at(json->meshes, 0).primitives, 0);
  const auto &other = at(at(binary->meshes, 0).primitives, 0);
  rstd_assert(first.indexed && count(first.indices) == 3);
  rstd_assert(at(first.vertices, 1).position == at(other.vertices, 1).position);
  rstd_assert(at(first.vertices, 0).normal.z > 0.999f);
  rstd_assert(at(first.vertices, 0).color.x == 1 &&
              at(first.vertices, 0).color.y == 0);
  rstd_assert(!at(at(json->meshes, 0).primitives, 1).indexed);
  rstd_assert(at(at(json->meshes, 0).primitives, 1).indices.is_empty());
  rstd_assert(at(json->images, 0).width == 1 &&
              count(at(json->images, 0).rgba) == 4);
  auto external = load("tests/data/external.gltf"_str);
  rstd_assert(at(external->images, 0).rgba == at(json->images, 0).rgba);
  auto fallback = load("tests/data/fallback.gltf"_str);
  rstd_assert(count(fallback->images) == 1 &&
              at(fallback->textures, 0).image == 0);
  rstd_assert(!fallback->diagnostics.is_empty());
  rstd_assert(LoadAsset("tests/data/no-fallback.gltf"_str).is_err());
  auto flat = load("tests/data/flat.gltf"_str);
  const auto &faces = at(at(flat->meshes, 0).primitives, 0);
  rstd_assert(count(faces.vertices) == 6);
  rstd_assert(at(faces.vertices, 0).normal.z > 0.99f);
  rstd_assert(at(faces.vertices, 3).normal.y > 0.99f);
  auto hdr = LoadEnvironment("tests/data/constant.hdr"_str).unwrap();
  rstd_assert(hdr.width == 4 && hdr.height == 2);
  rstd_assert(at(hdr.rgba, 0) == 1 && at(hdr.rgba, 1) == 0.5f);
  auto id = json->node_id(0);
  rstd_assert(json->node(id).is_some());
  rstd_assert(binary->node(id).is_none());
  SceneInstance instance(json.clone());
  rstd_assert(instance.bounds().min.x == 0 && instance.bounds().max.x == 3);
  rstd_assert(instance.pose(1).world.point({0, 0, 0}).x == 2);
  auto sparse = load("tests/data/sparse.gltf"_str);
  rstd_assert(
      at(at(at(sparse->meshes, 0).primitives, 0).vertices, 1).position.x == 2);
  auto limited =
      LoadAsset("tests/data/triangle.gltf"_str, {.max_vertices = usize(2)});
  rstd_assert(limited.is_err() &&
              limited.unwrap_err().kind == ErrorKind::Limit);
  auto required = LoadAsset("tests/data/unsupported.gltf"_str);
  rstd_assert(required.is_err() &&
              required.unwrap_err().kind == ErrorKind::Unsupported);
  rstd_assert(LoadAsset("tests/data/missing.gltf"_str).is_err());
  auto matrix = compose({2, 3, 4}, {0, 0, 0, 1}, {-2, 3, 4});
  auto inverse = matrix.inverse();
  rstd_assert(inverse.is_some());
  auto restored = inverse->point(matrix.point({1, 2, 3}));
  rstd_assert(absolute(restored.x - 1) < 1e-5f &&
              absolute(restored.y - 2) < 1e-5f &&
              absolute(restored.z - 3) < 1e-5f);
  rstd_assert(matrix.determinant3() < 0);
  auto projection = perspective(0.8f, 1.0f, 0.1f, 10);
  rstd_assert(absolute(projection.point({0, 0, -0.1f}).z) < 1e-5f);
  rstd_assert(absolute(projection.point({0, 0, -10}).z - 1) < 1e-5f);
  rstd::io::println("asset loading, identity, accessor decoding, images and "
                    "matrix checks passed");
}
} // namespace vecgltf_test::asset
