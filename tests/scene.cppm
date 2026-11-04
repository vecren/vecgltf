module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:scene;
import vecgltf;
import rstd.test;
using namespace vecgltf;
namespace vecgltf_test::scene
{
void near(float a, float b) {
    rstd_assert(absolute(a - b) < 0.0001f);
}
void near(Vec3 a, Vec3 b) {
    near(a.x, b.x);
    near(a.y, b.y);
    near(a.z, b.z);
}
void same_view(const View& a, const View& b) {
    near(a.eye, b.eye);
    for (unsigned i = 0; i < 16; ++i) {
        near(a.view.m[i], b.view.m[i]);
        near(a.projection.m[i], b.projection.m[i]);
    }
}
Animation translation(Interpolation interpolation) {
    Animation clip;
    clip.end = 2;
    AnimationSampler sampler;
    sampler.interpolation = interpolation;
    sampler.components    = 3;
    sampler.times.push(0.0f);
    sampler.times.push(2.0f);
    if (interpolation == Interpolation::CubicSpline) {
        float values[] { 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0 };
        for (float value : values) sampler.values.push(float(value));
    } else {
        float values[] { 0, 0, 0, 4, 0, 0 };
        for (float value : values) sampler.values.push(float(value));
    }
    clip.samplers.push(rstd::move(sampler));
    clip.channels.push({ 1, 0, AnimationPath::Translation });
    return clip;
}
TEST(Scene, AnimationAndCamera) {
    auto asset        = Arc<Asset>::make();
    asset->generation = u64(1);
    Mesh      mesh;
    Primitive primitive;
    Vertex    vertex;
    vertex.weights = { 1, 0, 0, 0 };
    primitive.vertices.push(rstd::move(vertex));
    MorphTarget morph;
    morph.positions.push({ 2, 0, 0 });
    morph.normals.push({});
    morph.tangents.push({});
    primitive.morphs.push(rstd::move(morph));
    mesh.primitives.push(rstd::move(primitive));
    asset->meshes.push(rstd::move(mesh));
    Node node;
    node.mesh        = 0;
    node.skin        = 0;
    node.translation = { 100, 0, 0 };
    node.weights.push(0.25f);
    asset->nodes.push(rstd::move(node));
    Node joint;
    joint.translation = { 1, 0, 0 };
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
    const auto&   geometry = at(at(asset->meshes, 0).primitives, 0);
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
    weights.channels.push({ 0, 0, AnimationPath::Weights });
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
    float rotations[] { 0, 0, 0, 1, 0, 0, 1, 0 };
    for (float v : rotations) rotation_sampler.values.push(float(v));
    rotation.samplers.push(rstd::move(rotation_sampler));
    rotation.channels.push({ 1, 0, AnimationPath::Rotation });
    asset->animations.push(rstd::move(rotation));
    a.set_animation(4);
    a.set_time(1);
    auto rotated = a.pose(1).world.vector({ 1, 0, 0 });
    near(rotated.x, 0);
    near(rotated.y, 1);
    Camera camera;
    camera.orthographic = true;
    camera.near         = 1;
    camera.far          = 11;
    camera.xmag         = 2;
    camera.ymag         = 1;
    asset->cameras.push(rstd::move(camera));
    at(asset->nodes, 1).camera = 0;
    a.set_animation(invalid_index);
    auto view = a.camera_view(asset->node_id(1), 2).unwrap();
    near(view.eye.x, 1);
    near(view.projection.point({ 0, 0, -1 }).z, 0);
    near(view.projection.point({ 0, 0, -11 }).z, 1);
    rstd_assert(a.camera_view({ 1, u64(2) }, 2).is_none());
    rstd::io::println("scene: linear, step, cubic, quaternion, skin, morph, "
                      "independent instances and camera passed");
}
TEST(Scene, CameraFrames) {
    Bounds bounds;
    bounds.include({ -2, -1, -3 });
    bounds.include({ 2, 1, 3 });
    OrbitCamera orbit;
    orbit.fit(bounds);
    orbit.rotate(-0.13f, 0.15f);
    orbit.roll(0.73f);
    orbit.pan(0.1f, -0.15f);
    orbit.zoom(0.6f);
    auto captured = orbit.frame(0.6f);
    rstd_assert(captured.valid());
    auto orbit_source = dyn<CameraSource>::from_ref(orbit);
    same_view(captured.view(0.6f).unwrap(),
              orbit_source->Snapshot(0.6f).unwrap().view(0.6f).unwrap());
    rstd_assert(orbit_source->Snapshot(0).is_none());
    OrbitCamera manual;
    manual.fit(bounds);
    rstd_assert(manual.restore(captured, 0.6f));
    same_view(orbit.view(0.6f), manual.view(0.6f));
    manual.rotate(0, 0);
    manual.roll(0);
    manual.pan(0, 0);
    manual.zoom(0);
    same_view(captured.view(0.6f).unwrap(), manual.view(0.6f));

    CameraFrame fixed;
    fixed.position          = { 3, 2, 9 };
    fixed.orientation       = normalized(Vec4 { 0.2f, 0.3f, -0.4f, 0.8f });
    fixed.focus_distance    = 5;
    fixed.projection.yfov   = 1.1f;
    fixed.projection.aspect = 1.7f;
    fixed.projection.near   = 0.3f;
    fixed.projection.far    = 81;
    rstd_assert(manual.restore(fixed, 2));
    same_view(fixed.view(2).unwrap(), manual.view(2));
    same_view(fixed.view(0.5f).unwrap(), manual.view(0.5f));
    near(manual.frame(0.5f).focus_distance, 5);
    manual.zoom(1);
    rstd_assert(manual.frame().focus_distance < 5);
    near(manual.frame().projection.near, 0.3f);
    near(manual.frame().projection.far, 81);
    fixed.projection.aspect = 0;
    fixed.projection.far    = 0;
    rstd_assert(manual.restore(fixed));
    auto wide = manual.view(2), narrow = manual.view(0.5f);
    near(wide.eye, narrow.eye);
    near(narrow.projection.m[0], wide.projection.m[0] * 4);
    near(narrow.projection.m[10], -1);

    auto orthographic                    = fixed;
    orthographic.projection.orthographic = true;
    orthographic.projection.near         = 0;
    orthographic.projection.far          = 50;
    orthographic.projection.xmag         = 3;
    orthographic.projection.ymag         = 2;
    rstd_assert(manual.restore(orthographic));
    same_view(orthographic.view(1).unwrap(), manual.view(1));
    same_view(orthographic.view(0.5f).unwrap(), manual.view(0.5f));
    near(manual.view(1).projection.point({ 0, 0, 0 }).z, 0);
    near(manual.view(1).projection.point({ 0, 0, -50 }).z, 1);
    manual.zoom(1);
    rstd_assert(manual.frame().projection.xmag < 3);
    near(manual.frame().focus_distance, orthographic.focus_distance);
    near(manual.frame().position, orthographic.position);
    manual.pan(0.1f, 0.2f);
    rstd_assert(dot(manual.frame().position - orthographic.position,
                    manual.frame().position - orthographic.position) > 0);

    CameraFrame pole;
    pole.orientation = { 0.70710678f, 0, 0, 0.70710678f };
    pole.position    = { 1, 2, 3 };
    rstd_assert(manual.restore(pole));
    same_view(pole.view(1).unwrap(), manual.view(1));
    manual.rotate(0.0001f, 0);
    near(manual.frame().position, pole.position);
    pole.orientation    = { 0.7070714831f, 0, 0, 0.7071421742f };
    pole.focus_distance = 100;
    rstd_assert(manual.restore(pole));
    same_view(pole.view(1).unwrap(), manual.view(1));
    auto invalid           = fixed;
    invalid.focus_distance = 0;
    rstd_assert(! invalid.valid() && ! manual.restore(invalid));
    invalid             = fixed;
    invalid.orientation = {};
    rstd_assert(invalid.view(1).is_none());
    rstd_assert(fixed.view(0).is_none());

    auto world  = compose(fixed.position, fixed.orientation, { 2, 3, 4 });
    auto scaled = camera_frame(world, fixed.projection, fixed.focus_distance).unwrap();
    same_view(fixed.view(1).unwrap(), scaled.view(1).unwrap());
    auto shear = Mat4::Identity();
    shear.m[4] = 0.5f;
    rstd_assert(camera_frame(shear, {}).is_none());
    rstd_assert(camera_frame(compose({}, { 0, 0, 0, 1 }, { -1, 1, 1 }), {}).is_none());
    rstd_assert(camera_frame(compose({}, { 0, 0, 0, 1 }, { 0, 1, 1 }), {}).is_none());
    auto pair = camera_frame(compose({}, { 0, 0, 0, 1 }, { -1, -1, 1 }), {}).unwrap();
    near(pair.view(1).unwrap().view.vector({ 1, 0, 0 }), { -1, 0, 0 });

    auto asset        = Arc<Asset>::make();
    asset->generation = u64(7);
    Camera camera;
    camera.near = 0.2f;
    camera.far  = 100;
    asset->cameras.push(rstd::move(camera));
    Node parent;
    parent.translation = { 3, 0, 0 };
    parent.scale       = { 2, 2, 2 };
    parent.children.push(1);
    asset->nodes.push(rstd::move(parent));
    Node child;
    child.parent = 0;
    child.camera = 0;
    asset->nodes.push(rstd::move(child));
    Node matrix;
    matrix.has_matrix = true;
    matrix.camera     = 0;
    matrix.matrix     = compose({ -2, 1, 6 }, { 1, 0, 0, 0 }, { 2, 3, 4 });
    asset->nodes.push(rstd::move(matrix));
    Scene scene;
    scene.roots.push(0);
    scene.roots.push(2);
    asset->scenes.push(rstd::move(scene));
    Animation animation;
    animation.end = 2;
    AnimationSampler position;
    position.components = 3;
    position.times.push(0.0f);
    position.times.push(2.0f);
    float translations[] { 0, 0, 2, 2, 0, 2 };
    for (float value : translations) position.values.push(float(value));
    animation.samplers.push(rstd::move(position));
    animation.channels.push({ 1, 0, AnimationPath::Translation });
    AnimationSampler rotation;
    rotation.components = 4;
    rotation.times.push(0.0f);
    rotation.times.push(2.0f);
    float rotations[] { 0, 0, 0, 1, 0, 0, 1, 0 };
    for (float value : rotations) rotation.values.push(float(value));
    animation.samplers.push(rstd::move(rotation));
    animation.channels.push({ 0, 1, AnimationPath::Rotation });
    asset->animations.push(rstd::move(animation));
    SceneInstance instance(asset.clone());
    instance.set_animation(0);
    instance.set_time(1);
    auto child_frame = instance.camera_frame(asset->node_id(1), 4).unwrap();
    near(child_frame.position, { 3, 2, 4 });
    near(child_frame.view(1).unwrap().view.vector({ 1, 0, 0 }), { 0, -1, 0 });
    near(child_frame.focus_distance, 4);
    same_view(child_frame.view(1).unwrap(), instance.camera_view(asset->node_id(1), 1).unwrap());
    SceneCameraSource source { &instance, asset->node_id(1), 4 };
    auto              scene_source = dyn<CameraSource>::from_ref(source);
    same_view(child_frame.view(1).unwrap(), scene_source->Snapshot(1).unwrap().view(1).unwrap());
    rstd_assert(instance.camera_frame({ 1, u64(8) }).is_none());
    rstd_assert(instance.camera_frame(asset->node_id(0)).is_none());
    source.instance = nullptr;
    rstd_assert(scene_source->Snapshot(1).is_none());
    auto matrix_frame = instance.camera_frame(asset->node_id(2)).unwrap();
    near(matrix_frame.position, { -2, 1, 6 });
    near(matrix_frame.view(1).unwrap().view.vector({ 0, 1, 0 }), { 0, -1, 0 });
    at(asset->nodes, 0).scale = { 2, 3, 4 };
    instance.set_time(1);
    rstd_assert(instance.camera_frame(asset->node_id(1)).is_some());
    at(asset->nodes, 1).rotation = { 0, 0.38268343f, 0, 0.92387953f };
    instance.set_time(1);
    rstd_assert(instance.camera_frame(asset->node_id(1)).is_none());
    at(asset->nodes, 2).matrix = compose({}, { 0, 1, 0, 0 }, { -2, 3, 4 });
    instance.set_time(1);
    rstd_assert(instance.camera_frame(asset->node_id(2)).is_none());
}
TEST(Scene, CameraReference) {
    auto asset        = Arc<Asset>::make();
    asset->generation = u64(11);
    Node root;
    root.translation = { 10, 20, 30 };
    root.rotation    = { -0.70710678f, 0, 0, 0.70710678f };
    root.scale       = { 2, 2, 2 };
    root.children.push(1);
    asset->nodes.push(rstd::move(root));
    Node child;
    child.parent      = 0;
    child.translation = { 1, 0, 0 };
    asset->nodes.push(rstd::move(child));
    asset->nodes.push(Node {});
    Scene scene;
    scene.roots.push(0);
    asset->scenes.push(rstd::move(scene));
    Scene multiple;
    multiple.roots.push(0);
    multiple.roots.push(2);
    asset->scenes.push(rstd::move(multiple));
    asset->scenes.push(Scene {});
    SceneInstance instance(asset.clone());
    auto          reference = instance.scene_root().unwrap();
    rstd_assert(reference.index == 0 && reference.generation == asset->generation);
    CameraLookAt     look { { 1, 2, 3 }, { 1, 4, 3 } };
    CameraProjection projection;
    projection.yfov   = 1.1f;
    projection.aspect = 1.7f;
    projection.near   = 0.25f;
    projection.far    = 40;
    auto world        = instance.camera_frame(look, projection, { 0, 0, 1 }).unwrap();
    same_view(world.view(1).unwrap(),
              camera_frame(look, projection, { 0, 0, 1 }).unwrap().view(1).unwrap());
    auto frame = instance.camera_frame(look, projection, { 0, 0, 1 }, Some(reference)).unwrap();
    auto expected_projection = projection;
    expected_projection.near = 0.5f;
    expected_projection.far  = 80;
    auto expected            = camera_frame(CameraLookAt { { 12, 26, 26 }, { 12, 26, 22 } },
                                            expected_projection,
                                            { 0, 1, 0 })
                                   .unwrap();
    same_view(frame.view(1).unwrap(), expected.view(1).unwrap());
    near(frame.focus_distance, 4);
    near(frame.projection.yfov, projection.yfov);
    near(frame.projection.aspect, projection.aspect);
    auto child_frame =
        instance.camera_frame(look, projection, { 0, 0, 1 }, Some(asset->node_id(1))).unwrap();
    near(child_frame.position, { 14, 26, 26 });
    near(child_frame.focus_distance, 4);

    projection.far = 0;
    frame          = instance.camera_frame(look, projection, { 0, 0, 1 }, Some(reference)).unwrap();
    near(frame.projection.far, 0);
    projection.orthographic = true;
    projection.near         = 0;
    projection.far          = 20;
    projection.xmag         = -3;
    projection.ymag         = 2;
    frame = instance.camera_frame(look, projection, { 0, 0, 1 }, Some(reference)).unwrap();
    near(frame.projection.near, 0);
    near(frame.projection.far, 40);
    near(frame.projection.xmag, -6);
    near(frame.projection.ymag, 4);

    rstd_assert(
        instance.camera_frame(look, {}, { 0, 0, 1 }, Some(AssetId { 0, u64(12) })).is_none());
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, Some(asset->node_id(3))).is_none());
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, Some(asset->node_id(2))).is_none());
    rstd_assert(instance.camera_frame(CameraLookAt {}, {}, { 0, 0, 1 }, Some(reference)).is_none());
    rstd_assert(instance.set_scene(1));
    rstd_assert(instance.scene_root().is_none());
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, Some(asset->node_id(2))).is_some());
    rstd_assert(instance.set_scene(2));
    rstd_assert(instance.scene_root().is_none());
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, Some(reference)).is_none());
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }).is_some());
}
TEST(Scene, CameraReferenceTransform) {
    auto asset        = Arc<Asset>::make();
    asset->generation = u64(13);
    Node root;
    root.has_matrix = true;
    root.matrix     = compose({ 0, 0, 0 },
                              { -0.70710678f, 0, 0, 0.70710678f },
                              { 0.0034276119f, 0.0034276119f, 0.0034276119f });
    asset->nodes.push(rstd::move(root));
    Scene scene;
    scene.roots.push(0);
    asset->scenes.push(rstd::move(scene));
    SceneInstance instance(asset.clone());
    auto          reference = instance.scene_root();
    CameraLookAt  look { { 100, 200, 300 }, { 100, 400, 300 } };
    auto          frame = instance.camera_frame(look, {}, { 0, 0, 1 }, reference).unwrap();
    near(frame.position, { 0.34276119f, 1.02828357f, -0.68552238f });
    near(frame.focus_distance, 0.68552238f);
    near(frame.projection.near, 0.000034276119f);

    auto& matrix = at(asset->nodes, 0).matrix;
    matrix       = compose({}, { 0, 0, 0, 1 }, { 2, 3, 2 });
    instance.set_time(0);
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, reference).is_none());
    matrix = compose({}, { 0, 0, 0, 1 }, { -2, 2, 2 });
    instance.set_time(0);
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, reference).is_none());
    matrix      = Mat4::Identity();
    matrix.m[4] = 0.5f;
    instance.set_time(0);
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, reference).is_none());
    matrix      = Mat4::Identity();
    matrix.m[3] = 0.1f;
    instance.set_time(0);
    rstd_assert(instance.camera_frame(look, {}, { 0, 0, 1 }, reference).is_none());

    matrix = compose({ 1e10f, 0, 0 }, { 0, 0, 0, 1 }, { 2, 2, 2 });
    instance.set_time(0);
    frame =
        instance.camera_frame(CameraLookAt { { 0, 0, 0 }, { 1, 0, 0 } }, {}, { 0, 1, 0 }, reference)
            .unwrap();
    near(frame.position, { 1e10f, 0, 0 });
    near(frame.focus_distance, 2);
    near(frame.view(1).unwrap().view.vector({ 1, 0, 0 }), { 0, 0, -1 });
}
} // namespace vecgltf_test::scene
