module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:window;
import vecgltf;
import vecgltf.render;
import vui.host;
import rstd.test;
import :output;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace vecgltf_test::window
{
void lifecycle(bool verify_minimize) {
    vui::host::Window window;
    window.open("vecgltf lifecycle", 640, 480,
                verify_minimize ? vui::host::WindowBackend::X11 : vui::host::WindowBackend::Automatic).unwrap();
    auto expected_backend = rstd::env::var("VECGLTF_TEST_BACKEND"_str);
    if (expected_backend.is_ok() && expected_backend->as_str() == "x11"_str)
        rstd_assert(window.backend() == vui::host::WindowBackend::X11);
    if (expected_backend.is_ok() && expected_backend->as_str() == "wayland"_str)
        rstd_assert(window.backend() == vui::host::WindowBackend::Wayland);
    for (unsigned n {}; n < 200 && ! window.configured(); ++n) window.wait(10).unwrap();
    rstd_assert(window.configured());
    vui::gpu::Renderer gpu;
    gpu.initialize(window, true, true).unwrap();
    auto                 device   = gpu.device_session().unwrap();
    auto                 renderer = vecgltf::Renderer::create(device, { 640, 480 }).unwrap();
    vecgltf_test::Output output(device);
    output.resize({ 64, 64 });
    rstd::uint64_t         output_version {};
    auto                   asset    = vecgltf::LoadAsset("data/triangle.glb"_str).unwrap();
    auto                   prepared = renderer.prepare(asset.clone()).unwrap();
    vecgltf::SceneInstance scene(asset.clone());
    vecgltf::OrbitCamera   camera;
    camera.fit(scene.bounds());
    auto           source = vui::ContentSource::create(37).unwrap();
    vui::UiSession session;
    auto&          ui = session.document();
    auto style = vui::NodeStyleBuilder{}.width(640).height(480).build();
    auto root           = ui.create<vui::SceneViewport>(style).unwrap();
    ui.set_scene_source(root, Some(vui::ContentSource(source))).unwrap();
    ui.mount(root).unwrap();
    auto draw = [&] {
        auto events = window.poll().unwrap();
        auto extent = window.pixel_extent();
        rstd_assert(extent.width && extent.height);
        gpu.resize(extent).unwrap();
        renderer.resize(extent).unwrap();
        output.resize(extent);
        ui.set_layout(root,
                      vui::LayoutStyleBuilder{}.width(events.surface.logical_size.width)
                          .height(events.surface.logical_size.height).build())
            .unwrap();
        auto frame =
            output
                .render(renderer, prepared, scene, camera.view(float(extent.width) / extent.height))
                .unwrap();
        source
            .publish(vui::gpu::NativeTextureFrame::wrap(
                         rstd::move(frame),
                         { .producer_session = 37, .frame_version = ++output_version })
                         .unwrap())
            .unwrap();
        session.advance(events.input.as_slice(), window.timing(), events.surface).unwrap();
        gpu.prepare(session.plan_frame().unwrap()).unwrap();
        bool submitted {};
        for (unsigned n = 0; n < 200; ++n) {
            auto result = gpu.submit();
            if (result.is_ok()) {
                submitted = true;
                break;
            }
            auto error = result.unwrap_err();
            rstd_assert(error.result == VK_NOT_READY || error.result == VK_TIMEOUT);
            rstd::thread::sleep(rstd::time::Duration::from_millis(u64(5)));
        }
        rstd_assert(submitted);
        gpu.wait().unwrap();
        gpu.present().unwrap();
        auto pixels = gpu.readback().unwrap();
        rstd_assert(pixels.width == extent.width && pixels.height == extent.height);
        bool varied {};
        for (usize i {}; i + usize(3) < pixels.rgba.len(); i += usize(4)) {
            if (pixels.rgba[i] != pixels.rgba[usize(0)] || pixels.rgba[i + usize(1)] != pixels.rgba[usize(1)] ||
                pixels.rgba[i + usize(2)] != pixels.rgba[usize(2)]) { varied = true; break; }
        }
        rstd_assert(varied);
    };
    draw();
    for (unsigned i = 0; i < 4; ++i) {
        window.resize(640 + i * 24, 480 + i * 16).unwrap();
        for (unsigned n = 0; n < 20; ++n) {
            window.wait(10).unwrap();
            if (window.logical_size() == Some(vui::Size{double(640 + i * 24), double(480 + i * 16)})) break;
        }
        rstd_assert((window.logical_size() == Some(vui::Size{double(640 + i * 24), double(480 + i * 16)})));
        draw();
    }
    if (verify_minimize) {
        for (unsigned n {}; n < 200 && window.minimized() != Some(false); ++n)
            window.wait(10).unwrap();
        rstd_assert(window.minimized() == Some(false));
        auto extent = window.pixel_extent();
        window.minimize().unwrap();
        bool minimized {};
        for (unsigned n {}; n < 300; ++n) {
            auto events = window.wait(10).unwrap();
            session.advance(events.input.as_slice(), window.timing(), events.surface).unwrap();
            if (events.minimized == Some(true)) {
                rstd_assert(events.suspended == Some(true));
                minimized = true;
                break;
            }
        }
        rstd_assert(minimized && window.pixel_extent().width == extent.width &&
                    window.pixel_extent().height == extent.height);
        auto before = renderer.statistics().recorded_frames;
        window.restore().unwrap();
        bool restored {};
        for (unsigned n {}; n < 300; ++n) {
            auto events = window.wait(10).unwrap();
            session.advance(events.input.as_slice(), window.timing(), events.surface).unwrap();
            if (events.minimized == Some(false)) {
                rstd_assert(events.suspended == Some(false));
                restored = true;
                break;
            }
        }
        rstd_assert(renderer.statistics().recorded_frames == before && restored);
        draw();
        rstd::io::println("window: native minimize/restore verified");
    }
    source.close().unwrap();
    session.close().unwrap();
    renderer.close();
    gpu.shutdown().unwrap();
    rstd_assert(device.validation_errors() == 0);
    rstd::io::println("window: repeated resize and shutdown passed");
}
TEST(Viewer, WindowLifecycle) {
    if (rstd::env::var_os("VECGLTF_TEST_REQUIRE_RESTORE"_str).is_some()) {
        auto session_type = rstd::env::var("XDG_SESSION_TYPE"_str);
        if (session_type.is_ok() && session_type->as_str() == "x11"_str) {
            lifecycle(true);
            return;
        }
        GTEST_SKIP() << "legacy restore requires an explicit X11 session; Wayland cannot confirm or request restore";
    }
    lifecycle(false);
}
TEST(Viewer, X11MinimizeRestore) {
    if (rstd::env::var_os("VECGLTF_TEST_X11_WM"_str).is_none())
        GTEST_SKIP() << "requires an isolated X11 window manager";
    lifecycle(true);
}
TEST(Viewer, WaylandMinimizeCapabilities) {
    if (rstd::env::var_os("VECGLTF_TEST_WAYLAND"_str).is_none())
        GTEST_SKIP() << "requires an isolated Wayland compositor";
    vui::host::Window window;
    window.open("vecgltf wayland capabilities", 640, 480, vui::host::WindowBackend::Wayland).unwrap();
    for (unsigned n {}; n < 200 && ! window.configured(); ++n) window.wait(10).unwrap();
    rstd_assert(window.configured() && window.minimized().is_none());
    auto result = window.minimize();
    rstd_assert(result.is_ok() || result.unwrap_err() == vui::host::WindowActionError::Unsupported);
    rstd_assert(window.restore().unwrap_err() == vui::host::WindowActionError::Unsupported);
    window.wait(0).unwrap();
    rstd_assert(window.minimized().is_none());
}
} // namespace vecgltf_test::window
