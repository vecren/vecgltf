module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:controls;
import vecgltf;
import vecgltf.render;
import vecgltf.viewer.panel;
import vecgltf.viewer.model;
import vui.sdl;
import rstd.test;
import :output;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;
namespace vecgltf_test::controls {
TEST(Viewer, Controls) {
  auto asset = gltf::LoadAsset("tests/data/triangle.gltf"_str).unwrap();
  gltf::SceneInstance instance(asset.clone());
  vui::UiSession session;
  auto &ui = session.document();
  auto fonts = Box<vectext::SystemTextService>::make();
  ui
      .set_text_service(Box<vui::TextService>::from_raw(
          rstd::move(fonts).into_raw().template cast<vui::TextService>()))
      .unwrap();
  vui::NodeStyle style;
  style.layout.width = Some(960.0);
  style.layout.height = Some(720.0);
  auto root = ui.create<vui::Stack>(style).unwrap();
  auto viewport = ui.append<vui::SceneViewport>(root, style).unwrap();
  unsigned viewport_events{};
  session.input()
      .on(viewport, vui::EventType::PointerDown,
          [&](auto &, const auto &) { ++viewport_events; })
      .unwrap();
  viewer::Panel panel(session, root);
  panel.update(instance, gltf::invalid_index, true, {}, ""_str);
  ui.mount(root).unwrap();
  rstd::uint64_t time{};
  auto advance = [&](vui::InputEvent event) {
    session
        .advance(slice<vui::InputEvent>::from_raw_parts(&event, usize(1)),
                 {++time, {}}, {{960, 720}, 1, 1})
        .unwrap();
  };
  session.advance({}, {++time, {}}, {{960, 720}, 1, 1}).unwrap();
  auto click = [&](double x, double y) {
    vui::InputEvent event;
    event.type = vui::EventType::PointerDown;
    event.button = vui::PointerButton::Primary;
    event.position = {x, y};
    advance(event);
    event.type = vui::EventType::PointerUp;
    advance(event);
  };
  click(40, 180);
  auto commands = panel.take_commands();
  rstd_assert(commands.len() == usize(1) &&
              gltf::at(commands, 0).action == viewer::Action::Scene);
  rstd_assert(viewport_events == 0);
  click(40, 384);
  commands = panel.take_commands();
  rstd_assert(commands.len() == usize(1) &&
              gltf::at(commands, 0).action == viewer::Action::Play);
  click(124, 456);
  commands = panel.take_commands();
  rstd_assert(commands.len() == usize(1) &&
              gltf::at(commands, 0).action == viewer::Action::Seek);
  rstd_assert(gltf::absolute(gltf::at(commands, 0).value - 0.5f) < 0.001f);
  vui::InputEvent key;
  key.type = vui::EventType::KeyDown;
  key.key = vui::Key::End;
  advance(key);
  commands = panel.take_commands();
  rstd_assert(commands.len() == usize(1));
  rstd_assert(gltf::at(commands, 0).value == 1);
  click(600, 200);
  rstd_assert(viewport_events == 1);
  panel.resize(400);
  session.advance({}, {++time, {}}, {{960, 400}, 1, 1}).unwrap();
  vui::InputEvent wheel;
  wheel.type = vui::EventType::Wheel;
  wheel.position = {120, 300};
  wheel.scroll_delta = {0, 1000};
  advance(wheel);
  click(40, 248);
  commands = panel.take_commands();
  rstd_assert(commands.len() == usize(1) &&
              gltf::at(commands, 0).action == viewer::Action::Reload);
  vui::gpu::Renderer gpu;
  gpu.initialize(true).unwrap();
  auto device = gpu.device_session().unwrap();
  auto renderer = gltf::Renderer::create(device, {64, 64}).unwrap();
  vecgltf_test::Output output(device);
  output.resize({64, 64});
  rstd::uint64_t output_version{};
  auto prepared = renderer.prepare(asset.clone()).unwrap();
  auto generation = asset->generation;
  rstd_assert(viewer::reload(renderer, "tests/data/missing.gltf"_str, asset,
                             prepared, instance)
                  .is_err());
  rstd_assert(asset->generation == generation &&
              prepared.generation() == generation &&
              instance.asset().generation == generation);
  viewer::reload(renderer, "tests/data/triangle.glb"_str, asset, prepared,
                 instance)
      .unwrap();
  rstd_assert(asset->generation != generation &&
              prepared.generation() == asset->generation);
  gltf::OrbitCamera camera;
  camera.fit(instance.bounds());
  auto frame =
      output.render(renderer, prepared, instance, camera.view(1)).unwrap();
  output.drain();
  renderer.close();
  session.close().unwrap();
  gpu.shutdown().unwrap();
  rstd_assert(device.validation_errors() == 0);
  rstd::io::println(
      "controls: routing, scrolling, seeking and transactional reload passed");
}
} // namespace vecgltf_test::controls
