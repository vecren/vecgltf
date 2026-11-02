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

namespace vecgltf_test::camera {
void near(float a, float b) { rstd_assert(gltf::absolute(a - b) < 0.0001f); }
void same_view(const gltf::CameraFrame &a, const gltf::CameraFrame &b,
               float aspect = 1.5f) {
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

void projection_step(const gltf::CameraFrame &a,
                     const gltf::CameraFrame &b) {
  viewer::CameraTrack track;
  rstd_assert(track.append(a) && track.append(b));
  auto middle = track.sample(1).unwrap();
  rstd_assert(middle.valid());
  auto expected = a;
  expected.position = middle.position;
  expected.orientation = middle.orientation;
  expected.focus_distance = middle.focus_distance;
  same_view(middle, expected);
  same_view(track.sample(2).unwrap(), b);
}

TEST(Viewer, CameraTrack) {
  viewer::CameraTrack track;
  rstd_assert(track.keys().is_empty() && track.sample(0).is_none());
  rstd_assert(!track.play() && !track.seek(0) && !track.remove_last());
  rstd_assert(!track.set_interval(0) && !track.set_interval(-1));
  gltf::CameraFrame a;
  a.position = {0, 0, 4};
  a.orientation = gltf::normalized(gltf::Vec4{0.2f, 0.3f, -0.4f, 0.8f});
  a.focus_distance = 2;
  a.projection.yfov = 0.8f;
  a.projection.near = 0.1f;
  rstd_assert(track.append(a));
  rstd_assert(!track.play() && track.duration() == 0);
  same_view(track.sample(-100).unwrap(), a);
  same_view(track.sample(100).unwrap(), a);
  auto b = a;
  b.position = {8, 4, -2};
  b.orientation = a.orientation * -1;
  b.focus_distance = 8;
  b.projection.yfov = 1.2f;
  b.projection.near = 0.3f;
  rstd_assert(track.append(b));
  near(track.duration(), 2);
  near(track.time(), 2);
  rstd_assert(!track.playing() && track.dirty());
  auto middle = track.sample(1).unwrap();
  near(middle.position.x, 4);
  near(middle.position.y, 2);
  near(middle.position.z, 1);
  near(middle.focus_distance, 5);
  near(middle.projection.yfov, 1);
  near(middle.projection.near, 0.2f);
  float orientation_dot = middle.orientation.x * a.orientation.x +
                          middle.orientation.y * a.orientation.y +
                          middle.orientation.z * a.orientation.z +
                          middle.orientation.w * a.orientation.w;
  near(gltf::absolute(orientation_dot), 1);
  auto invalid = a;
  invalid.orientation = {};
  rstd_assert(!track.append(invalid) && track.keys().len() == usize(2));
  Vec<viewer::CameraKeyframe> unordered;
  unordered.push({0, a});
  unordered.push({0, b});
  rstd_assert(!track.replace(rstd::move(unordered)));
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
  rstd_assert(!track.playing());
  rstd_assert(track.seek(-1));
  near(track.time(), 0);
  track.set_looping(false);
  rstd_assert(track.play());
  track.update(100);
  near(track.time(), 2);
  rstd_assert(!track.playing());
  rstd_assert(track.play());
  near(track.time(), 0);
  rstd_assert(rstd::as<gltf::CameraSource>(track).Snapshot(1.5f).is_some());

  auto fixed_aspect = b;
  fixed_aspect.projection.aspect = 1.5f;
  projection_step(a, fixed_aspect);
  auto finite_far = b;
  finite_far.projection.far = 100;
  projection_step(a, finite_far);
  auto orthographic = b;
  orthographic.projection.orthographic = true;
  orthographic.projection.near = 0;
  orthographic.projection.far = 50;
  orthographic.projection.xmag = 3;
  orthographic.projection.ymag = 2;
  projection_step(a, orthographic);
  auto mirrored = orthographic;
  mirrored.projection.xmag = -3;
  projection_step(orthographic, mirrored);

  auto asset = gltf::LoadAsset("tests/data/triangle.gltf"_str).unwrap();
  gltf::Camera camera;
  static_cast<gltf::CameraProjection &>(camera) = orthographic.projection;
  asset->cameras.push(rstd::move(camera));
  gltf::at(asset->nodes, 0).camera = 0;
  gltf::SceneInstance instance(asset.clone());
  auto revision = instance.revision();
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
  rstd_assert(controller.mode() == viewer::CameraMode::Orbit &&
              !controller.animating() && !controller.track().playing());
  rstd_assert(controller.select_asset(instance, 0, 1.5f));
  displayed = controller.frame(instance, 1.5f).unwrap();
  rstd_assert(controller.record(instance, 1.5f));
  rstd_assert(controller.mode() == viewer::CameraMode::Asset &&
              controller.asset_camera() == 0);
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

  auto directory = rstd::path::PathBuf::from("extools"_str);
  rstd::fs::create_dir_all(directory.as_path()).unwrap();
  auto path = "extools/camera-track-test.json"_str;
  auto file = rstd::path::PathBuf::from(path);
  track.pause();
  rstd_assert(track.set_interval(1.25f));
  viewer::SaveCameraTrack(path, track).unwrap();
  auto loaded = viewer::LoadCameraTrack(path).unwrap();
  near(loaded.interval(), 1.25f);
  rstd_assert(!loaded.looping() && !loaded.playing() && !loaded.dirty());
  rstd_assert(loaded.keys().len() == usize(2));
  same_view(loaded.sample(1).unwrap(), track.sample(1).unwrap());
  controller.replace_track(rstd::move(loaded));
  rstd_assert(controller.mode() == viewer::CameraMode::Track);
  auto saved = rstd::fs::read_to_string(file.as_path()).unwrap();
  auto corrupted = rstd::json::from_str(saved.as_str()).unwrap();
  corrupted["keyframes"_str][usize(1)]["time"_str] =
      rstd::json::Value::Number(rstd::json::Number::from_i64(i64(-1)));
  auto text = rstd::json::to_string(corrupted);
  rstd::fs::write(file.as_path(), text.as_str().as_bytes()).unwrap();
  auto failed = viewer::LoadCameraTrack(path);
  rstd_assert(failed.is_err());
  if (failed.is_ok())
    controller.replace_track(failed.unwrap());
  rstd_assert(controller.track().keys().len() == usize(2));
  same_view(controller.track().sample(1).unwrap(), track.sample(1).unwrap());
  rstd::fs::write(file.as_path(), "{"_str.as_bytes()).unwrap();
  rstd_assert(viewer::LoadCameraTrack(path).is_err());
  rstd::fs::write(file.as_path(), "{\"version\":1e999}"_str.as_bytes()).unwrap();
  rstd_assert(viewer::LoadCameraTrack(path).is_err());
  rstd::fs::remove_file(file.as_path()).unwrap();
  rstd_assert(viewer::LoadCameraTrack(path).is_err());
  rstd_assert(instance.revision() == revision);
  rstd::io::println(
      "camera: independent track, source takeover and JSON persistence passed");
}
} // namespace vecgltf_test::camera
