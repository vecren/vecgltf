module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:camera;
import vecgltf;
import vecgltf.viewer.camera;
import rstd.json;
import rstd.test;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;

namespace vecgltf_test::camera
{
void near(float a, float b) {
    rstd_assert(gltf::absolute(a - b) < 0.0001f);
}
void same_view(const gltf::CameraFrame& a, const gltf::CameraFrame& b, float aspect = 1.5f) {
    auto av = a.view(aspect).unwrap(), bv = b.view(aspect).unwrap();
    near(av.eye.x, bv.eye.x);
    near(av.eye.y, bv.eye.y);
    near(av.eye.z, bv.eye.z);
    for (unsigned i = 0; i < 16; ++i) {
        near(av.view.m[i], bv.view.m[i]);
        near(av.projection.m[i], bv.projection.m[i]);
    }
    near(a.focus_distance, b.focus_distance);
}

void projection_step(const gltf::CameraFrame& a, const gltf::CameraFrame& b) {
    viewer::CameraTrack track;
    rstd_assert(track.append(a) && track.append(b));
    auto middle = track.sample(1).unwrap();
    rstd_assert(middle.valid());
    auto expected           = a;
    expected.position       = middle.position;
    expected.orientation    = middle.orientation;
    expected.focus_distance = middle.focus_distance;
    same_view(middle, expected);
    same_view(track.sample(2).unwrap(), b);
}

void looks_at(const gltf::CameraFrame& frame, gltf::Vec3 target) {
    auto local = frame.view(1.5f).unwrap().view.point(target);
    near(local.x, 0);
    near(local.y, 0);
    near(local.z, -frame.focus_distance);
}

TEST(Camera, LookAt) {
    gltf::CameraLookAt camera { { 0, 0, 10 }, {} };
    auto               frame = gltf::camera_frame(camera).unwrap();
    near(frame.position.z, 10);
    near(frame.focus_distance, 10);
    near(frame.orientation.w, 1);
    looks_at(frame, camera.target);
    auto restored = frame.look_at().unwrap();
    near(restored.position.z, camera.position.z);
    near(restored.target.x, camera.target.x);
    near(restored.target.y, camera.target.y);
    near(restored.target.z, camera.target.z);
    auto tilted = gltf::camera_frame(camera, {}, { 1, 1, 0 }).unwrap();
    looks_at(tilted, camera.target);
    rstd_assert(gltf::absolute(tilted.orientation.z) > 0.1f);
    rstd_assert(gltf::camera_frame(gltf::CameraLookAt { {}, {} }).is_none());
    rstd_assert(gltf::camera_frame(camera, {}, {}).is_none());
    rstd_assert(gltf::camera_frame(camera, {}, { 0, 0, 1 }).is_none());
    auto invalid       = camera;
    invalid.position.x = rstd::f32::INFINITY_.to_primitive();
    rstd_assert(gltf::camera_frame(invalid).is_none());
    gltf::CameraProjection projection;
    projection.yfov = 0;
    rstd_assert(gltf::camera_frame(camera, projection).is_none());
}

TEST(Camera, Transition) {
    gltf::CameraLookAt     from { { 10, 4, 10 }, { 1, 2, 0 } };
    gltf::CameraLookAt     to { { 0, 0, 2 }, from.target };
    gltf::CameraTransition transition;
    rstd_assert(! transition.playing() && transition.sample(0).is_none());
    rstd_assert(! transition.update(1));
    gltf::CameraProjection projection;
    projection.yfov = 0.7f;
    projection.near = 0.01f;
    projection.far  = 0;
    rstd_assert(transition.start(from, to, projection));
    near(transition.duration(), 2.5f);
    near(transition.time(), 0);
    rstd_assert(transition.playing());
    auto source = dyn<gltf::CameraSource>::from_ref(transition);
    same_view(source->Snapshot(1.5f).unwrap(), gltf::camera_frame(from, projection).unwrap());
    rstd_assert(source->Snapshot(0).is_none());
    rstd_assert(source->Snapshot(rstd::f32::NAN_.to_primitive()).is_none());
    auto middle = transition.sample(1.25f).unwrap();
    near(middle.position.x, 0.625f);
    near(middle.position.y, 0.25f);
    near(middle.position.z, 2.5f);
    looks_at(middle, to.target);
    near(middle.projection.yfov, projection.yfov);
    near(middle.projection.far, 0);
    near(transition.time(), 0);
    rstd_assert(! transition.update(1.25f));
    same_view(source->Snapshot(1.5f).unwrap(), middle);
    auto portrait  = source->Snapshot(0.5f).unwrap().view(0.5f).unwrap();
    auto landscape = middle.view(1.5f).unwrap();
    near(portrait.projection.m[0], landscape.projection.m[0] * 3);
    rstd_assert(! transition.update(-1));
    rstd_assert(! transition.update(rstd::f32::NAN_.to_primitive()));
    rstd_assert(! transition.update(rstd::f32::INFINITY_.to_primitive()));
    near(transition.time(), 1.25f);
    rstd_assert(transition.update(100));
    rstd_assert(! transition.playing() && ! transition.update(1));
    near(transition.time(), 2.5f);
    same_view(source->Snapshot(1.5f).unwrap(), gltf::camera_frame(to, projection).unwrap());
    same_view(transition.sample(-10).unwrap(), gltf::camera_frame(from, projection).unwrap());
    same_view(transition.sample(100).unwrap(), gltf::camera_frame(to, projection).unwrap());
    rstd_assert(transition.sample(rstd::f32::NAN_.to_primitive()).is_none());

    gltf::CameraTransitionOptions options;
    options.duration = 2;
    options.easing   = gltf::CameraEasing::Linear;
    rstd_assert(transition.start(from, to, projection, options));
    rstd_assert(! transition.update(1));
    middle = source->Snapshot(1.5f).unwrap();
    near(middle.position.x, 5);
    near(middle.position.y, 2);
    near(middle.position.z, 6);
    looks_at(middle, to.target);
    transition.stop();
    rstd_assert(! transition.playing() && ! transition.update(100));
    same_view(source->Snapshot(1.5f).unwrap(), middle);

    auto current = middle.look_at().unwrap();
    rstd_assert(transition.start(current, from, projection, options));
    same_view(source->Snapshot(1.5f).unwrap(), middle);
    auto invalid     = options;
    invalid.duration = -1;
    rstd_assert(! transition.start(from, to, projection, invalid));
    invalid.duration = rstd::f32::INFINITY_.to_primitive();
    rstd_assert(! transition.start(from, to, projection, invalid));
    invalid    = options;
    invalid.up = {};
    rstd_assert(! transition.start(from, to, projection, invalid));
    gltf::CameraLookAt opposite_a { { 0, 0, 1 }, {} }, opposite_b { { 0, 0, -1 }, {} };
    rstd_assert(gltf::camera_frame(opposite_a).is_some());
    rstd_assert(gltf::camera_frame(opposite_b).is_some());
    rstd_assert(! transition.start(opposite_a, opposite_b));
    gltf::CameraLookAt pole_a { { -1, 2, 0 }, {} }, pole_b { { 1, 2, 0 }, {} };
    rstd_assert(! transition.start(pole_a, pole_b));
    gltf::CameraLookAt crossing_a { { 1, 0, 1 }, {} }, crossing_b { { -7.3f, 0, -7.3f }, {} };
    rstd_assert(! transition.start(crossing_a, crossing_b));
    rstd_assert(transition.playing());
    near(transition.time(), 0);
    same_view(source->Snapshot(1.5f).unwrap(), middle);

    auto moving   = to;
    moving.target = { 2, 3, -1 };
    rstd_assert(transition.start(from, moving, projection, options));
    looks_at(transition.sample(1).unwrap(), { 1.5f, 2.5f, -0.5f });
    options.duration = 0;
    rstd_assert(transition.start(from, to, projection, options));
    rstd_assert(! transition.playing() && ! transition.update(1));
    same_view(source->Snapshot(1.5f).unwrap(), gltf::camera_frame(to, projection).unwrap());
    rstd_assert(transition.start(opposite_a, opposite_b, projection, options));
    same_view(source->Snapshot(1.5f).unwrap(), gltf::camera_frame(opposite_b, projection).unwrap());

    options.up              = { 0, 0, 1 };
    projection.orthographic = true;
    projection.near         = 0;
    projection.far          = 20;
    projection.xmag         = 3;
    projection.ymag         = 2;
    to.position             = { 3, 4, 5 };
    rstd_assert(transition.start(from, to, projection, options));
    auto z_up = source->Snapshot(1.5f).unwrap();
    same_view(z_up, gltf::camera_frame(to, projection, options.up).unwrap());
    looks_at(z_up, to.target);
    gltf::OrbitCamera orbit;
    rstd_assert(orbit.restore(z_up, 1.5f));
    same_view(orbit.frame(1.5f), z_up);

    gltf::CameraLookAt far_from { { 100000000, 0, 0 }, { 99999992, 0, 0 } };
    gltf::CameraLookAt far_to { { 100000008, 0, 0 }, { 100000000, 0, 0 } };
    options.duration = 2;
    options.up       = { 0, 1, 0 };
    rstd_assert(transition.start(far_from, far_to, {}, options));
    auto distant = transition.sample(1).unwrap();
    rstd_assert(distant.valid());
    near(distant.focus_distance, 8);
    near(distant.view(1.5f)->view.vector({ 1, 0, 0 }).z, 1);

    options.duration = 1;
    rstd_assert(transition.start(from, to, {}, options));
    rstd_assert(! transition.update(0.5f));
    rstd_assert(transition.update(0.49999997f));
    rstd_assert(! transition.playing() && ! transition.update(1));
    near(transition.time(), 1);
}

TEST(Viewer, CameraTrack) {
    viewer::CameraTrack track;
    rstd_assert(track.keys().is_empty() && track.sample(0).is_none());
    rstd_assert(! track.play() && ! track.seek(0) && ! track.remove_last());
    rstd_assert(! track.set_interval(0) && ! track.set_interval(-1));
    gltf::CameraFrame a;
    a.position        = { 0, 0, 4 };
    a.orientation     = gltf::normalized(gltf::Vec4 { 0.2f, 0.3f, -0.4f, 0.8f });
    a.focus_distance  = 2;
    a.projection.yfov = 0.8f;
    a.projection.near = 0.1f;
    rstd_assert(track.append(a));
    rstd_assert(! track.play() && track.duration() == 0);
    same_view(track.sample(-100).unwrap(), a);
    same_view(track.sample(100).unwrap(), a);
    auto b            = a;
    b.position        = { 8, 4, -2 };
    b.orientation     = a.orientation * -1;
    b.focus_distance  = 8;
    b.projection.yfov = 1.2f;
    b.projection.near = 0.3f;
    rstd_assert(track.append(b));
    near(track.duration(), 2);
    near(track.time(), 2);
    rstd_assert(! track.playing() && track.dirty());
    auto middle = track.sample(1).unwrap();
    near(middle.position.x, 4);
    near(middle.position.y, 2);
    near(middle.position.z, 1);
    near(middle.focus_distance, 5);
    near(middle.projection.yfov, 1);
    near(middle.projection.near, 0.2f);
    float orientation_dot =
        middle.orientation.x * a.orientation.x + middle.orientation.y * a.orientation.y +
        middle.orientation.z * a.orientation.z + middle.orientation.w * a.orientation.w;
    near(gltf::absolute(orientation_dot), 1);
    auto invalid        = a;
    invalid.orientation = {};
    rstd_assert(! track.append(invalid) && track.keys().len() == usize(2));
    Vec<viewer::CameraKeyframe> unordered;
    unordered.push({ 0, a });
    unordered.push({ 0, b });
    rstd_assert(! track.replace(rstd::move(unordered)));
    near(track.time(), 2);
    rstd_assert(track.play());
    near(track.time(), 0);
    track.update(0.5f);
    near(track.time(), 0.5f);
    track.pause();
    track.update(1);
    near(track.time(), 0.5f);
    rstd_assert(track.seek(1.5f) && track.play());
    track.update(1000001);
    near(track.time(), 0.5f);
    track.update(-1);
    near(track.time(), 0.5f);
    rstd_assert(track.seek(100));
    near(track.time(), 2);
    rstd_assert(! track.playing());
    rstd_assert(track.seek(-1));
    near(track.time(), 0);
    track.set_looping(false);
    rstd_assert(track.play());
    track.update(100);
    near(track.time(), 2);
    rstd_assert(! track.playing());
    rstd_assert(track.play());
    near(track.time(), 0);
    rstd_assert(rstd::as<gltf::CameraSource>(track).Snapshot(1.5f).is_some());

    auto fixed_aspect              = b;
    fixed_aspect.projection.aspect = 1.5f;
    projection_step(a, fixed_aspect);
    auto finite_far           = b;
    finite_far.projection.far = 100;
    projection_step(a, finite_far);
    auto orthographic                    = b;
    orthographic.projection.orthographic = true;
    orthographic.projection.near         = 0;
    orthographic.projection.far          = 50;
    orthographic.projection.xmag         = 3;
    orthographic.projection.ymag         = 2;
    projection_step(a, orthographic);
    auto mirrored            = orthographic;
    mirrored.projection.xmag = -3;
    projection_step(orthographic, mirrored);

    auto         asset = gltf::LoadAsset("data/triangle.gltf"_str).unwrap();
    gltf::Camera camera;
    static_cast<gltf::CameraProjection&>(camera) = orthographic.projection;
    asset->cameras.push(rstd::move(camera));
    gltf::at(asset->nodes, 0).camera = 0;
    gltf::SceneInstance      instance(asset.clone());
    auto                     revision = instance.revision();
    viewer::CameraController fitted;
    fitted.fit(instance);
    gltf::OrbitCamera expected;
    expected.fit(instance.bounds());
    float initial_far = fitted.frame(instance, 1.5f)->projection.far;
    rstd_assert(fitted.rotate(instance, 1.5f, 0, 0));
    expected.rotate(0, 0);
    rstd_assert(fitted.zoom(instance, 1.5f, -50));
    expected.zoom(-50);
    same_view(fitted.frame(instance, 1.5f).unwrap(), expected.frame(1.5f));
    same_view(fitted.frame(instance, 0.5f).unwrap(), expected.frame(0.5f), 0.5f);
    rstd_assert(fitted.frame(instance, 1.5f)->projection.far > initial_far);
    viewer::CameraController controller;
    controller.fit(instance);
    rstd_assert(controller.record(instance, 1.5f));
    rstd_assert(controller.mode() == viewer::CameraMode::Orbit);
    controller.orbit().rotate(0.4f, -0.1f);
    rstd_assert(controller.record(instance, 1.5f));
    rstd_assert(controller.mode() == viewer::CameraMode::Orbit);
    rstd_assert(controller.toggle_track() && controller.animating());
    controller.update(0.5f);
    near(controller.track().time(), 0.5f);
    auto displayed = controller.frame(instance, 1.5f).unwrap();
    rstd_assert(controller.rotate(instance, 1.5f, 0, 0));
    same_view(displayed, controller.frame(instance, 1.5f).unwrap());
    rstd_assert(controller.mode() == viewer::CameraMode::Orbit && ! controller.animating() &&
                ! controller.track().playing());
    rstd_assert(controller.select_asset(instance, 0, 1.5f));
    displayed = controller.frame(instance, 1.5f).unwrap();
    rstd_assert(controller.record(instance, 1.5f));
    rstd_assert(controller.mode() == viewer::CameraMode::Asset && controller.asset_camera() == 0);
    rstd_assert(controller.pan(instance, 1.5f, 0, 0));
    same_view(displayed, controller.frame(instance, 1.5f).unwrap());
    rstd_assert(controller.seek_track(controller.track().duration()));
    displayed = controller.frame(instance, 1.5f).unwrap();
    rstd_assert(controller.remove_last(instance, 1.5f));
    rstd_assert(controller.mode() == viewer::CameraMode::Orbit);
    same_view(displayed, controller.frame(instance, 1.5f).unwrap());
    rstd_assert(controller.track().keys().len() == usize(2));
    controller.fit(instance);
    rstd_assert(controller.track().keys().len() == usize(2));
    rstd_assert(controller.seek_track(1));
    displayed = controller.frame(instance, 1.5f).unwrap();
    controller.clear_track(instance, 1.5f);
    same_view(displayed, controller.frame(instance, 1.5f).unwrap());
    rstd_assert(controller.track().keys().is_empty() &&
                controller.mode() == viewer::CameraMode::Orbit);
    rstd_assert(instance.revision() == revision);

    auto directory = rstd::path::PathBuf::from("../extools"_str);
    rstd::fs::create_dir_all(directory.as_path()).unwrap();
    auto path = "../extools/camera-track-test.json"_str;
    auto file = rstd::path::PathBuf::from(path);
    track.pause();
    rstd_assert(track.set_interval(1.25f));
    viewer::SaveCameraTrack(path, track).unwrap();
    auto loaded = viewer::LoadCameraTrack(path).unwrap();
    near(loaded.interval(), 1.25f);
    rstd_assert(! loaded.looping() && ! loaded.playing() && ! loaded.dirty());
    rstd_assert(loaded.keys().len() == usize(2));
    same_view(loaded.sample(1).unwrap(), track.sample(1).unwrap());
    controller.replace_track(rstd::move(loaded));
    rstd_assert(controller.mode() == viewer::CameraMode::Track);
    auto saved     = rstd::fs::read_to_string(file.as_path()).unwrap();
    auto corrupted = rstd::json::from_str(saved.as_str()).unwrap();
    corrupted["keyframes"_str][usize(1)]["time"_str] =
        rstd::json::Value::Number(rstd::json::Number::from_i64(i64(-1)));
    auto text = rstd::json::to_string(corrupted);
    rstd::fs::write(file.as_path(), text.as_str().as_bytes()).unwrap();
    auto failed = viewer::LoadCameraTrack(path);
    rstd_assert(failed.is_err());
    if (failed.is_ok()) controller.replace_track(failed.unwrap());
    rstd_assert(controller.track().keys().len() == usize(2));
    same_view(controller.track().sample(1).unwrap(), track.sample(1).unwrap());
    rstd::fs::write(file.as_path(), "{"_str.as_bytes()).unwrap();
    rstd_assert(viewer::LoadCameraTrack(path).is_err());
    rstd::fs::write(file.as_path(), "{\"version\":1e999}"_str.as_bytes()).unwrap();
    rstd_assert(viewer::LoadCameraTrack(path).is_err());
    rstd::fs::remove_file(file.as_path()).unwrap();
    rstd_assert(viewer::LoadCameraTrack(path).is_err());
    rstd_assert(instance.revision() == revision);
    rstd::io::println("camera: independent track, source takeover and JSON persistence passed");
}
} // namespace vecgltf_test::camera
