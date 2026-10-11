module;
#include <SDL.h>
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:window;
import vecgltf;
import vecgltf.render;
import vui.sdl;
import rstd.test;
import :output;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace vecgltf_test::window {
TEST(Viewer, WindowLifecycle) {
  const bool require_restore =
      rstd::env::var("VECGLTF_TEST_REQUIRE_RESTORE"_str).is_ok();
  vui::sdl::Window window;
  window.open("vecgltf lifecycle", 640, 480).unwrap();
  vui::gpu::Renderer gpu;
  gpu.initialize(window, true).unwrap();
  auto device = gpu.device_session().unwrap();
  auto renderer = vecgltf::Renderer::create(device, {640, 480}).unwrap();
  vecgltf_test::Output output(device);
  output.resize({64, 64});
  rstd::uint64_t output_version{};
  auto asset = vecgltf::LoadAsset("tests/data/triangle.glb"_str).unwrap();
  auto prepared = renderer.prepare(asset.clone()).unwrap();
  vecgltf::SceneInstance scene(asset.clone());
  vecgltf::OrbitCamera camera;
  camera.fit(scene.bounds());
  auto source = vui::ContentSource::create(37).unwrap();
  vui::UiSession session;
  auto &ui = session.document();
  vui::NodeStyle style;
  style.layout.width = Some(640.0);
  style.layout.height = Some(480.0);
  auto root = ui.create<vui::SceneViewport>(style).unwrap();
  ui.set_scene_source(root, Some(vui::ContentSource(source))).unwrap();
  ui.mount(root).unwrap();
  auto draw = [&] {
    auto events = window.poll();
    auto extent = window.pixel_extent();
    rstd_assert(extent.width && extent.height);
    gpu.resize(extent).unwrap();
    renderer.resize(extent).unwrap();
    output.resize(extent);
    ui.set_layout(root, {.width = Some(events.surface.logical_size.width),
                         .height = Some(events.surface.logical_size.height)})
        .unwrap();
    auto frame = output
                     .render(renderer, prepared, scene,
                             camera.view(float(extent.width) / extent.height))
                     .unwrap();
    source
        .publish(vui::gpu::NativeTextureFrame::wrap(
                     rstd::move(frame), {.producer_session = 37,
                                         .frame_version = ++output_version})
                     .unwrap())
        .unwrap();
    session.advance(events.input.as_slice(), window.timing(), events.surface)
        .unwrap();
    gpu.prepare(session.plan_frame().unwrap()).unwrap();
    bool submitted{};
    for (unsigned n = 0; n < 200; ++n) {
      auto result = gpu.submit();
      if (result.is_ok()) {
        submitted = true;
        break;
      }
      auto error = result.unwrap_err();
      rstd_assert(error.result == VK_NOT_READY || error.result == VK_TIMEOUT);
      SDL_Delay(5);
    }
    rstd_assert(submitted);
    gpu.wait().unwrap();
    gpu.present().unwrap();
  };
  draw();
  for (unsigned i = 0; i < 4; ++i) {
    window.resize(640 + i * 24, 480 + i * 16).unwrap();
    for (unsigned n = 0; n < 20; ++n) {
      window.wait(10);
      if (window.pixel_extent().width == 640 + i * 24)
        break;
    }
    draw();
  }
  window.minimize();
  bool minimized{};
  for (unsigned n = 0; n < 100; ++n) {
    auto events = window.wait(10);
    session.advance(events.input.as_slice(), window.timing(), events.surface)
        .unwrap();
    if (events.minimized) {
      minimized = true;
      break;
    }
  }
  rstd_assert(minimized && window.pixel_extent().width == 0);
  auto before = renderer.statistics().recorded_frames;
  gpu.resize({0, 0}).unwrap();
  rstd::io::println("window: awaiting activation");
  window.restore();
  bool restored{};
  for (unsigned n = 0; n < (require_restore ? 1000u : 100u); ++n) {
    auto events = window.wait(10);
    if (!events.minimized) {
      restored = true;
      break;
    }
  }
  rstd_assert(renderer.statistics().recorded_frames == before);
  rstd_assert(!require_restore || restored);
  if (restored) {
    draw();
    rstd::io::println("window: native minimize/restore verified");
  } else
    rstd::io::println(
        "window: native minimized; restore awaits compositor activation");
  source.close().unwrap();
  session.close().unwrap();
  renderer.close();
  gpu.shutdown().unwrap();
  rstd_assert(device.validation_errors() == 0);
  rstd::io::println("window: repeated resize and shutdown passed");
}
} // namespace vecgltf_test::window
