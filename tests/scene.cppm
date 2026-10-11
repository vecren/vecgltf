module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:scene;
import vecgltf;
import rstd.test;
using namespace vecgltf;
namespace vecgltf_test::scene {
void near(float a, float b) { rstd_assert(absolute(a - b) < 0.0001f); }
Animation translation(Interpolation interpolation) {
  Animation clip;
  clip.end = 2;
  AnimationSampler sampler;
  sampler.interpolation = interpolation;
  sampler.components = 3;
  sampler.times.push(0.0f);
  sampler.times.push(2.0f);
  if (interpolation == Interpolation::CubicSpline) {
    float values[]{0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0};
    for (float value : values)
      sampler.values.push(float(value));
  } else {
    float values[]{0, 0, 0, 4, 0, 0};
    for (float value : values)
      sampler.values.push(float(value));
  }
  clip.samplers.push(rstd::move(sampler));
  clip.channels.push({1, 0, AnimationPath::Translation});
  return clip;
}
TEST(Scene, AnimationAndCamera) {
  auto asset = Arc<Asset>::make();
  asset->generation = u64(1);
  Mesh mesh;
  Primitive primitive;
  Vertex vertex;
  vertex.weights = {1, 0, 0, 0};
  primitive.vertices.push(rstd::move(vertex));
  MorphTarget morph;
  morph.positions.push({2, 0, 0});
  morph.normals.push({});
  morph.tangents.push({});
  primitive.morphs.push(rstd::move(morph));
  mesh.primitives.push(rstd::move(primitive));
  asset->meshes.push(rstd::move(mesh));
  Node node;
  node.mesh = 0;
  node.skin = 0;
  node.translation = {100, 0, 0};
  node.weights.push(0.25f);
  asset->nodes.push(rstd::move(node));
  Node joint;
  joint.translation = {1, 0, 0};
  asset->nodes.push(rstd::move(joint));
  Skin skin;
  skin.joints.push(1);
  skin.inverse_bind.push(Mat4::Identity());
  asset->skins.push(rstd::move(skin));
  Scene scene;
  scene.roots.push(0);
  scene.roots.push(1);
  asset->scenes.push(rstd::move(scene));
  asset->animations.push(translation(Interpolation::Linear));
  asset->animations.push(translation(Interpolation::Step));
  asset->animations.push(translation(Interpolation::CubicSpline));
  SceneInstance a(asset.clone()), b(asset.clone());
  const auto &geometry = at(at(asset->meshes, 0).primitives, 0);
  near(a.world_position(0, geometry, 0).x, 1.5f);
  near(a.bounds().center().x, 1.5f);
  near(at(a.pose(0).primitive_bounds, 0).center().x, 1.5f);
  a.set_animation(0);
  a.set_time(1);
  near(a.world_position(0, geometry, 0).x, 2.5f);
  near(at(a.pose(0).primitive_bounds, 0).center().x, 2.5f);
  near(b.world_position(0, geometry, 0).x, 1.5f);
  a.set_playing(false);
  a.update(0.5f);
  near(a.time(), 1);
  a.set_playing(true);
  a.update(1.5f);
  near(a.time(), 0.5f);
  a.set_looping(false);
  a.update(5);
  near(a.time(), 2);
  a.set_animation(1);
  a.set_time(1.5f);
  near(a.world_position(0, geometry, 0).x, 0.5f);
  a.set_time(2);
  near(a.world_position(0, geometry, 0).x, 4.5f);
  a.set_animation(2);
  a.set_time(1);
  near(a.world_position(0, geometry, 0).x, 2.0f);
  Animation weights;
  weights.end = 2;
  AnimationSampler weight_sampler;
  weight_sampler.components = 1;
  weight_sampler.times.push(0.0f);
  weight_sampler.times.push(2.0f);
  weight_sampler.values.push(0.0f);
  weight_sampler.values.push(1.0f);
  weights.samplers.push(rstd::move(weight_sampler));
  weights.channels.push({0, 0, AnimationPath::Weights});
  asset->animations.push(rstd::move(weights));
  a.set_animation(3);
  a.set_time(1);
  near(a.world_position(0, geometry, 0).x, 2);
  Animation rotation;
  rotation.end = 2;
  AnimationSampler rotation_sampler;
  rotation_sampler.components = 4;
  rotation_sampler.times.push(0.0f);
  rotation_sampler.times.push(2.0f);
  float rotations[]{0, 0, 0, 1, 0, 0, 1, 0};
  for (float v : rotations)
    rotation_sampler.values.push(float(v));
  rotation.samplers.push(rstd::move(rotation_sampler));
  rotation.channels.push({1, 0, AnimationPath::Rotation});
  asset->animations.push(rstd::move(rotation));
  a.set_animation(4);
  a.set_time(1);
  auto rotated = a.pose(1).world.vector({1, 0, 0});
  near(rotated.x, 0);
  near(rotated.y, 1);
  Camera camera;
  camera.orthographic = true;
  camera.near = 1;
  camera.far = 11;
  camera.xmag = 2;
  camera.ymag = 1;
  asset->cameras.push(rstd::move(camera));
  at(asset->nodes, 1).camera = 0;
  a.set_animation(invalid_index);
  auto view = a.camera_view(asset->node_id(1), 2).unwrap();
  near(view.eye.x, 1);
  near(view.projection.point({0, 0, -1}).z, 0);
  near(view.projection.point({0, 0, -11}).z, 1);
  rstd_assert(a.camera_view({1, u64(2)}, 2).is_none());
  rstd::io::println("scene: linear, step, cubic, quaternion, skin, morph, "
                    "independent instances and camera passed");
}
} // namespace vecgltf_test::scene
