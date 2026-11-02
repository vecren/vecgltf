module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:controls;
import vecgltf;
import vecgltf.render;
import vecgltf.viewer.camera;
import vecgltf.viewer.panel;
import vecgltf.viewer.model;
import vui.sdl;
import rstd.test;
import :output;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;
namespace vecgltf_test::controls
{
TEST(Viewer, Controls) {
    auto                asset = gltf::LoadAsset("data/triangle.gltf"_str).unwrap();
    gltf::SceneInstance instance(asset.clone());
    vui::UiSession      session;
    auto&               ui    = session.document();
    auto                fonts = Box<vectext::SystemTextService>::make();
    ui.set_text_service(Box<vui::TextService>::from_raw(
                            rstd::move(fonts).into_raw().template cast<vui::TextService>()))
        .unwrap();
    vui::NodeStyle style;
    style.layout.width  = Some(960.0);
    style.layout.height = Some(720.0);
    auto     root       = ui.create<vui::Stack>(style).unwrap();
    auto     viewport   = ui.append<vui::SceneViewport>(root, style).unwrap();
    unsigned viewport_events {}, root_keys {};
    session.input()
        .on(viewport,
            vui::EventType::PointerDown,
            [&](auto&, const auto&) {
                ++viewport_events;
            })
        .unwrap();
    session.input()
        .on(
            root,
            vui::EventType::KeyDown,
            [&](auto&, const auto&) {
                ++root_keys;
            },
            vui::RoutePhase::Bubble)
        .unwrap();
    viewer::Panel panel(session, root);
    panel.update(instance, gltf::invalid_index, true, {}, ""_str);
    ui.mount(root).unwrap();
    rstd::uint64_t time {};
    double         height { 720 };
    auto           advance = [&](vui::InputEvent event) {
        session
            .advance(slice<vui::InputEvent>::from_raw_parts(&event, usize(1)),
                     { ++time, {} },
                     { { 960, height }, 1, 1 })
            .unwrap();
    };
    session.advance({}, { ++time, {} }, { { 960, 720 }, 1, 1 }).unwrap();
    auto click = [&](double x, double y) {
        vui::InputEvent event;
        event.type     = vui::EventType::PointerDown;
        event.button   = vui::PointerButton::Primary;
        event.position = { x, y };
        advance(event);
        event.type = vui::EventType::PointerUp;
        advance(event);
    };
    auto command = [&](viewer::Action action) {
        auto commands = panel.take_commands();
        rstd_assert(commands.len() == usize(1) && gltf::at(commands, 0).action == action);
        return gltf::at(commands, 0).value;
    };
    auto scroll = [&](double pixels) {
        vui::InputEvent event;
        event.type         = vui::EventType::Wheel;
        event.position     = { 120, 300 };
        event.scroll_delta = { 0, pixels };
        event.scroll_unit  = vui::ScrollUnit::Pixels;
        advance(event);
    };
    click(40, 180);
    command(viewer::Action::Scene);
    rstd_assert(viewport_events == 0);
    click(40, 344);
    command(viewer::Action::CameraRecord);
    viewer::CameraTrackStatus camera_track;
    camera_track.keyframes = usize(2);
    camera_track.time      = 1;
    camera_track.duration  = 2;
    camera_track.active    = true;
    camera_track.dirty     = true;
    panel.update(instance, gltf::invalid_index, true, {}, ""_str, camera_track);
    click(40, 434);
    command(viewer::Action::CameraPlay);
    click(140, 434);
    command(viewer::Action::CameraLoop);
    click(124, 476);
    rstd_assert(gltf::absolute(command(viewer::Action::CameraSeek) - 0.5f) < 0.001f);
    vui::InputEvent key;
    key.type = vui::EventType::KeyDown;
    key.key  = vui::Key::End;
    advance(key);
    rstd_assert(command(viewer::Action::CameraSeek) == 1);
    key.key = vui::Key::Home;
    advance(key);
    rstd_assert(command(viewer::Action::CameraSeek) == 0);
    key.key = vui::Key::Right;
    advance(key);
    rstd_assert(gltf::absolute(command(viewer::Action::CameraSeek) - 0.01f) < 0.001f);
    key.key = vui::Key::Left;
    advance(key);
    rstd_assert(command(viewer::Action::CameraSeek) == 0);
    rstd_assert(root_keys == 0 && viewport_events == 0);
    click(40, 672);
    command(viewer::Action::Play);
    scroll(100);
    click(124, 644);
    rstd_assert(gltf::absolute(command(viewer::Action::Seek) - 0.5f) < 0.001f);
    key.key = vui::Key::End;
    advance(key);
    rstd_assert(command(viewer::Action::Seek) == 1);
    key.key = vui::Key::Home;
    advance(key);
    rstd_assert(command(viewer::Action::Seek) == 0);
    rstd_assert(root_keys == 0 && viewport_events == 0);
    click(600, 200);
    rstd_assert(viewport_events == 1);
    height = 400;
    panel.resize(height);
    session.advance({}, { ++time, {} }, { { 960, height }, 1, 1 }).unwrap();
    scroll(1000);
    click(40, 248);
    command(viewer::Action::Reload);
    scroll(-1000);
    scroll(300);
    click(40, 44);
    command(viewer::Action::CameraRecord);
    click(140, 44);
    command(viewer::Action::CameraRemove);
    click(40, 134);
    command(viewer::Action::CameraPlay);
    click(140, 134);
    command(viewer::Action::CameraLoop);
    click(124, 176);
    rstd_assert(gltf::absolute(command(viewer::Action::CameraSeek) - 0.5f) < 0.001f);
    click(40, 220);
    command(viewer::Action::CameraIntervalDown);
    click(208, 220);
    command(viewer::Action::CameraIntervalUp);
    click(40, 260);
    command(viewer::Action::CameraClear);
    click(140, 260);
    command(viewer::Action::CameraSave);
    rstd_assert(viewport_events == 1);
    vui::gpu::Renderer gpu;
    gpu.initialize(true).unwrap();
    auto                 device   = gpu.device_session().unwrap();
    auto                 renderer = gltf::Renderer::create(device, { 64, 64 }).unwrap();
    vecgltf_test::Output output(device);
    output.resize({ 64, 64 });
    rstd::uint64_t output_version {};
    auto           prepared   = renderer.prepare(asset.clone()).unwrap();
    auto           generation = asset->generation;
    rstd_assert(
        viewer::reload(renderer, "data/missing.gltf"_str, asset, prepared, instance).is_err());
    rstd_assert(asset->generation == generation && prepared.generation() == generation &&
                instance.asset().generation == generation);
    viewer::reload(renderer, "data/triangle.glb"_str, asset, prepared, instance).unwrap();
    rstd_assert(asset->generation != generation && prepared.generation() == asset->generation);
    gltf::OrbitCamera camera;
    camera.fit(instance.bounds());
    auto frame = output.render(renderer, prepared, instance, camera.view(1)).unwrap();
    output.drain();
    renderer.close();
    session.close().unwrap();
    gpu.shutdown().unwrap();
    rstd_assert(device.validation_errors() == 0);
    rstd::io::println("controls: routing, scrolling, seeking and transactional reload passed");
}
} // namespace vecgltf_test::controls
