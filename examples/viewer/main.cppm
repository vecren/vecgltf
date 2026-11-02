export module vecgltf.viewer;
import vecgltf;
import vecgltf.render;
import vui.sdl;
import vecgltf.viewer.panel;
import vecgltf.viewer.model;
import vecgltf.viewer.camera;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;

struct Options {
    String   path, capture, environment, camera_path;
    unsigned width { 960 }, height { 720 }, frames {}, scene { gltf::invalid_index },
        animation { gltf::invalid_index }, camera { gltf::invalid_index };
    float               time {}, camera_time {}, zoom {}, roll {};
    gltf::Vec2          rotate {}, pan {};
    gltf::RenderOptions render;
    bool validation {}, headless {}, fixed_time {}, fixed_camera_time {}, camera_play {};
};
auto argument(const char* value) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(value).to_str().unwrap_or(""_str);
}
bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        auto arg  = argument(argv[i]);
        auto next = [&]() -> ref<str> {
            return ++i < argc ? argument(argv[i]) : ""_str;
        };
        auto number = [&](unsigned& target) {
            auto value = rstd::from_str<u32>(next());
            if (value.is_err()) return false;
            target = value.unwrap().to_primitive();
            return true;
        };
        auto real = [&](float& target) {
            auto value = rstd::from_str<f32>(next());
            if (value.is_err() || ! value.unwrap().is_finite()) return false;
            target = value.unwrap().to_primitive();
            return true;
        };
        if (arg == "--validation"_str)
            options.validation = true;
        else if (arg == "--background"_str)
            options.render.background = true;
        else if (arg == "--environment"_str) {
            options.environment = String::make(next());
            if (options.environment.is_empty()) return false;
        } else if (arg == "--headless"_str)
            options.headless = true;
        else if (arg == "--capture"_str) {
            options.capture = String::make(next());
            if (options.capture.is_empty()) return false;
        } else if (arg == "--frames"_str) {
            if (! number(options.frames) || options.frames > 100000) return false;
        } else if (arg == "--scene"_str) {
            if (! number(options.scene)) return false;
        } else if (arg == "--camera"_str) {
            if (! number(options.camera)) return false;
        } else if (arg == "--camera-path"_str) {
            options.camera_path = String::make(next());
            if (options.camera_path.is_empty()) return false;
        } else if (arg == "--camera-time"_str) {
            if (! real(options.camera_time) || options.camera_time < 0) return false;
            options.fixed_camera_time = true;
        } else if (arg == "--camera-play"_str) {
            options.camera_play = true;
        } else if (arg == "--animation"_str) {
            if (! number(options.animation)) return false;
        } else if (arg == "--debug"_str) {
            unsigned value {};
            if (! number(value) || value > 5) return false;
            options.render.debug = gltf::DebugView(value);
        } else if (arg == "--exposure"_str) {
            auto value = rstd::from_str<f32>(next());
            if (value.is_err() || ! value.unwrap().is_finite() || value.unwrap() <= f32(0))
                return false;
            options.render.exposure = value.unwrap().to_primitive();
        } else if (arg == "--size"_str) {
            if (! number(options.width) || ! number(options.height)) return false;
        } else if (arg == "--time"_str) {
            auto value = rstd::from_str<f32>(next());
            if (value.is_err() || ! value.unwrap().is_finite()) return false;
            options.time       = value.unwrap().to_primitive();
            options.fixed_time = true;
        } else if (arg == "--zoom"_str) {
            if (! real(options.zoom)) return false;
        } else if (arg == "--rotate"_str) {
            if (! real(options.rotate.x) || ! real(options.rotate.y)) return false;
        } else if (arg == "--pan"_str) {
            if (! real(options.pan.x) || ! real(options.pan.y)) return false;
        } else if (arg == "--roll"_str) {
            if (! real(options.roll)) return false;
        } else if (arg.starts_with("-"_str) || ! options.path.is_empty())
            return false;
        else
            options.path = String::make(arg);
    }
    if (options.headless && ! options.frames) options.frames = 1;
    return ! options.path.is_empty() && options.width && options.height && options.width <= 8192 &&
           options.height <= 8192 && ! (options.camera_play && options.fixed_camera_time);
}
void report(gltf::Error error) {
    rstd::io::eprintln(
        "{} [{}] {}: {}", error.source, unsigned(error.kind), error.location, error.message);
}
bool capture(vui::gpu::Renderer& renderer, ref<str> path) {
    auto result = renderer.readback();
    if (result.is_err()) {
        rstd::io::eprintln("capture failed: {}", unsigned(result.unwrap_err().stage));
        return false;
    }
    auto image  = result.unwrap();
    auto header = rstd::format("P6\n{} {}\n255\n", image.width, image.height);
    auto data   = Vec<u8>::from(header.as_str().as_bytes());
    data.reserve(usize(image.width) * usize(image.height) * usize(3));
    for (unsigned i = 0; i < image.width * image.height; ++i)
        for (unsigned c = 0; c < 3; ++c) data.push(u8(image.rgba[usize(i * 4 + c)]));
    auto destination = rstd::path::PathBuf::from(path);
    auto written     = rstd::fs::write(destination.as_path(), data.as_slice());
    if (written.is_err()) {
        rstd::io::eprintln("capture {}: {}", path, written.unwrap_err());
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    Options options;
    if (! parse(argc, argv, options)) {
        rstd::io::eprintln("usage: vecgltf-viewer model.gltf|model.glb [--headless] [--size W H] "
                           "[--frames N] [--capture image.ppm] [--scene N] [--camera node] "
                           "[--camera-path track.json] [--camera-time seconds | --camera-play] "
                           "[--animation N] "
                           "[--time seconds] [--zoom steps] [--rotate yaw pitch] [--pan x y] "
                           "[--roll radians] "
                           "[--environment image.hdr] [--background] [--exposure scale] "
                           "[--debug 0..5] [--validation]");
        return 2;
    }
    auto loaded = gltf::LoadAsset(options.path.as_str());
    if (loaded.is_err()) {
        report(loaded.unwrap_err());
        return 1;
    }
    auto asset = loaded.unwrap();
    for (const auto& diagnostic : asset->diagnostics)
        rstd::io::eprintln("{}: {}", diagnostic.location, diagnostic.message);
    gltf::SceneInstance instance(asset.clone());
    if ((options.scene != gltf::invalid_index && ! instance.set_scene(options.scene)) ||
        (options.animation != gltf::invalid_index && ! instance.set_animation(options.animation))) {
        rstd::io::eprintln("scene or animation index is out of range");
        return 2;
    }
    instance.set_time(options.time);
    instance.set_playing(! options.fixed_time);
    viewer::CameraController cameras;
    cameras.fit(instance);
    cameras.orbit().zoom(options.zoom);
    cameras.orbit().rotate(options.rotate.x, options.rotate.y);
    cameras.orbit().pan(options.pan.x, options.pan.y);
    cameras.orbit().roll(options.roll);
    if (options.camera_path.is_empty())
        options.camera_path = rstd::format("{}.camera.json", options.path);
    auto camera_file = rstd::path::PathBuf::from(options.camera_path.as_str());
    auto exists      = rstd::fs::exists(camera_file.as_path());
    if (exists.is_err()) {
        rstd::io::eprintln("{}: {}", options.camera_path, exists.unwrap_err());
        return 1;
    }
    if (exists.unwrap()) {
        auto track = viewer::LoadCameraTrack(options.camera_path.as_str());
        if (track.is_err()) {
            auto error = track.unwrap_err();
            rstd::io::eprintln("{}: {}: {}", error.source, error.location, error.message);
            return 1;
        }
        cameras.replace_track(track.unwrap());
    }
    if (options.camera != gltf::invalid_index &&
        ! cameras.select_asset(instance, options.camera, 1)) {
        rstd::io::eprintln("camera node is invalid");
        return 2;
    }
    if ((options.fixed_camera_time && ! cameras.seek_track(options.camera_time)) ||
        (options.camera_play && ! cameras.toggle_track())) {
        rstd::io::eprintln("camera path has insufficient keyframes");
        return 2;
    }
    vui::sdl::Window   window;
    vui::gpu::Renderer ui_renderer;
    auto               gpu_error = [&](vui::gpu::Error error) {
        rstd::io::eprintln("renderer stage={} Vulkan={} SDL={}",
                           unsigned(error.stage),
                           int(error.result),
                           argument(window.error()));
        return 1;
    };
    if (! options.headless) {
        auto opened = window.open("vecgltf", options.width, options.height);
        if (opened.is_err()) return gpu_error(opened.unwrap_err());
    }
    auto initialized =
        options.headless
            ? ui_renderer.initialize(options.validation)
            : ui_renderer.initialize(window, options.validation, ! options.capture.is_empty());
    if (initialized.is_err()) return gpu_error(initialized.unwrap_err());
    auto                     device   = ui_renderer.device_session().unwrap();
    constexpr rstd::uint64_t producer = 1;
    VkExtent2D               extent =
        options.headless ? VkExtent2D { options.width, options.height } : window.pixel_extent();
    auto created = gltf::Renderer::create(device, extent);
    if (created.is_err()) {
        report(created.unwrap_err());
        return 1;
    }
    auto renderer      = created.unwrap();
    auto output_queue  = vrento::vulkan::TextureContext::create(device).unwrap();
    auto output_device = dyn<vrento::texture::TextureDevice>::from_ref(output_queue);
    Option<vrento::texture::TexturePool> output_pool;
    VkExtent2D                           output_extent {};
    rstd::uint64_t                       output_version {};
    if (! options.environment.is_empty()) {
        auto environment = gltf::LoadEnvironment(options.environment.as_str());
        if (environment.is_err()) {
            report(environment.unwrap_err());
            return 1;
        }
        auto prepared_environment = renderer.set_environment(environment.unwrap());
        if (prepared_environment.is_err()) {
            report(prepared_environment.unwrap_err());
            return 1;
        }
    }
    auto preparing = renderer.prepare(asset.clone());
    if (preparing.is_err()) {
        report(preparing.unwrap_err());
        return 1;
    }
    auto           prepared = preparing.unwrap();
    auto           source   = vui::ContentSource::create(producer).unwrap();
    vui::UiSession session;
    auto&          document = session.document();
    if (! options.headless) {
        auto text_service = Box<vectext::SystemTextService>::make();
        document
            .set_text_service(Box<vui::TextService>::from_raw(
                rstd::move(text_service).into_raw().template cast<vui::TextService>()))
            .unwrap();
    }
    vui::NodeStyle style;
    style.focusable     = true;
    style.layout.width  = Some(vui::Scalar(extent.width));
    style.layout.height = Some(vui::Scalar(extent.height));
    auto root           = document.create<vui::Stack>(style).unwrap();
    auto viewport       = document.append<vui::SceneViewport>(root, style).unwrap();
    document.set_scene_source(viewport, Some(vui::ContentSource(source))).unwrap();
    document.set_scene_presentation(viewport, vui::ImageFit::Fill, vui::ImageSampling::Linear)
        .unwrap();
    Option<Box<viewer::Panel>> panel;
    if (! options.headless) panel = Some(Box<viewer::Panel>::make(session, root));
    String status =
        asset->diagnostics.is_empty() ? ""_Str : asset->diagnostics[usize(0)].message.clone();
    bool  looping       = true;
    float camera_aspect = float(extent.width) / float(extent.height);
    if (panel)
        (*panel)->update(instance,
                         cameras.asset_camera(),
                         looping,
                         options.render,
                         status.as_str(),
                         cameras.status());
    document.mount(root).unwrap();
    vui::Point         last {};
    vui::PointerButton drag {};
    session.input()
        .on(viewport,
            vui::EventType::PointerDown,
            [&](auto& context, const auto& event) {
                if (! cameras.take_control(instance, camera_aspect)) return;
                drag = event.input.button;
                last = event.input.position;
                context.capture_pointer().unwrap();
                context.focus().unwrap();
            })
        .unwrap();
    session.input()
        .on(viewport,
            vui::EventType::PointerMove,
            [&](auto&, const auto& event) {
                if (session.input().captured(event.input.pointer) != Some(viewport)) return;
                auto  p = event.input.position;
                float x = float(p.x - last.x), y = float(p.y - last.y);
                last = p;
                if (drag == vui::PointerButton::Primary)
                    cameras.rotate(instance, camera_aspect, -x * 0.008f, -y * 0.008f);
                else
                    cameras.pan(instance,
                                camera_aspect,
                                x / float(extent.height),
                                y / float(extent.height));
            })
        .unwrap();
    session.input()
        .on(viewport,
            vui::EventType::PointerUp,
            [&](auto& context, const auto&) {
                context.release_pointer().unwrap();
                drag = {};
            })
        .unwrap();
    session.input()
        .on(viewport,
            vui::EventType::Wheel,
            [&](auto& context, const auto& event) {
                cameras.zoom(instance,
                             camera_aspect,
                             float(event.input.scroll_delta.y) *
                                 (event.input.scroll_unit == vui::ScrollUnit::Pixels ? 0.025f : 1));
                context.prevent_default();
            })
        .unwrap();
    session.input()
        .on(viewport,
            vui::EventType::KeyDown,
            [&](auto&, const auto& event) {
                if (event.input.key == vui::Key::Home) {
                    cameras.fit(instance);
                }
                if (event.input.key == vui::Key::Space) {
                    if (cameras.mode() == viewer::CameraMode::Track) {
                        cameras.toggle_track();
                        options.fixed_camera_time = false;
                    } else {
                        instance.set_playing(! instance.playing());
                        options.fixed_time = false;
                    }
                }
            })
        .unwrap();
    rstd::uint64_t previous {}, serial {};
    unsigned       completed {};
    bool           close {}, refresh_surface {}, redraw_pending { true };
    while (! close) {
        vui::sdl::Events events;
        vui::FrameTiming timing;
        if (options.headless) {
            events.surface = { { double(extent.width), double(extent.height) }, 1, 1 };
            timing.now_ns  = ++serial * 16'666'667;
        } else {
            events = window.wait(
                ! completed
                    ? 0
                    : (options.frames || cameras.animating() ||
                               (instance.playing() && instance.animation() != gltf::invalid_index)
                           ? 16
                           : 1000));
            timing = window.timing();
            close  = events.close_requested;
            for (const auto& event : events.input)
                if (event.type == vui::EventType::KeyDown && event.key == vui::Key::Escape)
                    close = true;
            if (close) break;
            if (events.minimized) {
                session.advance(events.input.as_slice(), timing, events.surface).unwrap();
                refresh_surface = true;
                previous        = timing.now_ns;
                continue;
            }
            auto size = window.pixel_extent();
            if (! size.width || ! size.height) continue;
            if (refresh_surface || size.width != extent.width || size.height != extent.height) {
                auto resized = ui_renderer.resize(size);
                if (resized.is_err()) return gpu_error(resized.unwrap_err());
                extent          = size;
                refresh_surface = false;
                events.redraw   = true;
            }
        }
        vui::LayoutStyle layout;
        layout.width  = Some(events.surface.logical_size.width);
        layout.height = Some(events.surface.logical_size.height);
        document.set_layout(root, layout).unwrap();
        double panel_width = options.headless ? 0 : 248;
        if (! options.headless && events.surface.logical_size.width < 480) panel_width = 0;
        if (panel) {
            (*panel)->set_visible(panel_width > 0);
            (*panel)->resize(events.surface.logical_size.height);
        }
        double model_width = events.surface.logical_size.width - panel_width;
        layout.width       = Some(model_width);
        document.set_layout(viewport, layout).unwrap();
        document.set_transform(viewport, { 1, 0, 0, 1, panel_width, 0 }).unwrap();
        VkExtent2D model_extent { unsigned(model_width * events.surface.scale + 0.5),
                                  extent.height };
        if (! model_extent.width) model_extent.width = 1;
        camera_aspect      = float(model_extent.width) / float(model_extent.height);
        auto model_resized = renderer.resize(model_extent);
        if (model_resized.is_err()) {
            report(model_resized.unwrap_err());
            return 1;
        }
        bool camera_was_playing = cameras.animating();
        auto advance            = session.advance(events.input.as_slice(), timing, events.surface);
        if (advance.is_err()) {
            rstd::io::eprintln("input/session stage={}", unsigned(advance.unwrap_err().stage));
            return 1;
        }
        bool changed = ! events.input.is_empty();
        if (panel) {
            auto commands = (*panel)->take_commands();
            changed       = changed || ! commands.is_empty();
            for (const auto& command : commands) {
                switch (command.action) {
                case viewer::Action::Scene:
                    instance.set_scene((instance.scene() + 1) % gltf::count(asset->scenes));
                    cameras.fit(instance);
                    break;
                case viewer::Action::Camera: {
                    auto     current = cameras.asset_camera();
                    unsigned next    = current == gltf::invalid_index ? 0 : current + 1;
                    while (next < gltf::count(asset->nodes) &&
                           (gltf::at(asset->nodes, next).camera == gltf::invalid_index ||
                            ! instance.visible(next)))
                        ++next;
                    if (next == gltf::count(asset->nodes))
                        cameras.take_control(instance, camera_aspect);
                    else if (! cameras.select_asset(instance, next, camera_aspect))
                        status = "相机变换无效"_Str;
                    break;
                }
                case viewer::Action::Animation: {
                    unsigned next =
                        instance.animation() == gltf::invalid_index ? 0 : instance.animation() + 1;
                    instance.set_animation(
                        next < gltf::count(asset->animations) ? next : gltf::invalid_index);
                    options.fixed_time = false;
                    break;
                }
                case viewer::Action::Play:
                    instance.set_playing(! instance.playing());
                    options.fixed_time = false;
                    break;
                case viewer::Action::Loop:
                    looping = ! looping;
                    instance.set_looping(looping);
                    break;
                case viewer::Action::Seek:
                    if (instance.animation() != gltf::invalid_index) {
                        const auto& clip = gltf::at(asset->animations, instance.animation());
                        instance.set_time(clip.start + command.value * (clip.end - clip.start));
                        instance.set_playing(false);
                    }
                    break;
                case viewer::Action::Fit: cameras.fit(instance); break;
                case viewer::Action::CameraRecord:
                    if (cameras.record(instance, camera_aspect))
                        status = rstd::format("已记录 {} 个视角", cameras.status().keyframes);
                    else
                        status = "当前视角无法记录"_Str;
                    break;
                case viewer::Action::CameraRemove:
                    cameras.remove_last(instance, camera_aspect);
                    break;
                case viewer::Action::CameraPlay:
                    if (! cameras.toggle_track())
                        status = "请先记录至少两个视角"_Str;
                    else {
                        options.fixed_camera_time = false;
                        status                    = ""_Str;
                    }
                    break;
                case viewer::Action::CameraLoop:
                    cameras.track().set_looping(! cameras.track().looping());
                    break;
                case viewer::Action::CameraSeek:
                    cameras.seek_track(command.value * cameras.track().duration());
                    break;
                case viewer::Action::CameraIntervalDown:
                    cameras.track().set_interval(
                        gltf::maximum(0.25f, cameras.track().interval() - 0.25f));
                    break;
                case viewer::Action::CameraIntervalUp:
                    cameras.track().set_interval(
                        gltf::minimum(60, cameras.track().interval() + 0.25f));
                    break;
                case viewer::Action::CameraClear:
                    cameras.clear_track(instance, camera_aspect);
                    break;
                case viewer::Action::CameraSave: {
                    auto saved =
                        viewer::SaveCameraTrack(options.camera_path.as_str(), cameras.track());
                    if (saved.is_err()) {
                        auto error = saved.unwrap_err();
                        status = rstd::format("保存失败：{}\n{}", error.location, error.message);
                    } else {
                        cameras.track().mark_saved();
                        status = "轨迹已保存"_Str;
                        rstd::io::println("camera path saved: {}", options.camera_path);
                    }
                    break;
                }
                case viewer::Action::Background:
                    options.render.background = ! options.render.background;
                    break;
                case viewer::Action::ExposureDown:
                    options.render.exposure = gltf::maximum(0.01f, options.render.exposure / 1.25f);
                    break;
                case viewer::Action::ExposureUp:
                    options.render.exposure = gltf::minimum(100, options.render.exposure * 1.25f);
                    break;
                case viewer::Action::Debug:
                    options.render.debug =
                        gltf::DebugView((unsigned(options.render.debug) + 1) % 6);
                    break;
                case viewer::Action::Reload: {
                    auto replaced =
                        viewer::reload(renderer, options.path.as_str(), asset, prepared, instance);
                    if (replaced.is_err()) {
                        auto error = replaced.unwrap_err();
                        status = rstd::format("加载失败：{}\n{}", error.location, error.message);
                        break;
                    }
                    instance.set_looping(looping);
                    options.fixed_time = false;
                    cameras.fit(instance);
                    status = "模型已重新加载"_Str;
                    break;
                }
                }
            }
        }
        float camera_before = cameras.track().time();
        if (previous) {
            float delta = float(timing.now_ns - previous) * 1e-9f;
            if (! options.fixed_time) instance.update(delta);
            if (camera_was_playing && ! options.fixed_camera_time) cameras.update(delta);
        }
        changed        = changed || camera_before != cameras.track().time();
        previous       = timing.now_ns;
        bool animating = instance.playing() && instance.animation() != gltf::invalid_index &&
                         ! options.fixed_time;
        animating      = animating || cameras.animating();
        if (! options.headless && ! options.frames && completed && ! redraw_pending && ! changed &&
            ! events.redraw && ! advance.unwrap().frame_needed && ! animating)
            continue;
        auto camera_frame = cameras.frame(instance, camera_aspect);
        if (camera_frame.is_none()) {
            status = "相机变换无效，已返回手动视角"_Str;
            if (! cameras.take_control(instance, camera_aspect)) cameras.fit(instance);
            camera_frame = cameras.frame(instance, camera_aspect);
        }
        auto view = camera_frame.unwrap().view(camera_aspect).unwrap();
        if (panel)
            (*panel)->update(instance,
                             cameras.asset_camera(),
                             looping,
                             options.render,
                             status.as_str(),
                             cameras.status());
        if (! output_pool || output_extent.width != model_extent.width ||
            output_extent.height != model_extent.height) {
            vrento::texture::TextureDesc description;
            description.extent = { model_extent.width, model_extent.height };
            description.usage =
                vrento::texture::Usage::Sampled | vrento::texture::Usage::ColorAttachment;
            auto next =
                vrento::texture::TexturePool::create(output_device.as_mut_ref(), description);
            if (next.is_err()) {
                rstd::io::eprintln("output allocation kind={}", unsigned(next.unwrap_err().kind));
                return 1;
            }
            output_pool   = Some(next.unwrap());
            output_extent = model_extent;
        }
        auto work  = output_queue.Begin().unwrap();
        auto write = output_pool->acquire().unwrap();
        auto frame = renderer.record(work, write.view(), prepared, instance, view, options.render);
        if (frame.is_err()) {
            report(frame.unwrap_err());
            return 1;
        }
        auto submitted = output_queue.Submit(rstd::move(work));
        if (submitted.is_err()) {
            rstd::io::eprintln("model submit kind={}", unsigned(submitted.unwrap_err().kind));
            return 1;
        }
        auto published = output_pool->publish(rstd::move(write), submitted.unwrap()).unwrap();
        auto native    = vui::gpu::NativeTextureFrame::wrap(
            rstd::move(published),
            { .producer_session = producer, .frame_version = ++output_version });
        if (native.is_err() || source.publish(native.unwrap()).is_err()) {
            rstd::io::eprintln("could not publish model frame");
            return 1;
        }
        auto update = session.advance({}, timing, events.surface);
        if (update.is_err()) {
            rstd::io::eprintln("frame/session stage={}", unsigned(update.unwrap_err().stage));
            return 1;
        }
        auto plan = session.plan_frame();
        if (plan.is_err()) {
            rstd::io::eprintln("frame planning failed");
            return 1;
        }
        auto ready = ui_renderer.prepare(plan.unwrap());
        if (ready.is_err()) {
            auto error = ready.unwrap_err();
            if (! options.headless && error.result == VK_ERROR_OUT_OF_DATE_KHR) {
                refresh_surface = true;
                redraw_pending  = true;
                continue;
            }
            return gpu_error(error);
        }
        auto submit = ui_renderer.submit();
        if (submit.is_err()) {
            auto error = submit.unwrap_err();
            if (! options.headless &&
                (error.stage == vui::gpu::Stage::Acquire ||
                 error.stage == vui::gpu::Stage::Timeout) &&
                (error.result == VK_NOT_READY || error.result == VK_TIMEOUT ||
                 error.result == VK_ERROR_OUT_OF_DATE_KHR)) {
                auto discarded = ui_renderer.discard_prepared(5'000'000'000ull);
                if (discarded.is_err()) return gpu_error(discarded.unwrap_err());
                refresh_surface = error.result == VK_ERROR_OUT_OF_DATE_KHR;
                redraw_pending  = true;
                continue;
            }
            return gpu_error(error);
        }
        auto done = ui_renderer.wait(5'000'000'000ull);
        if (done.is_err()) return gpu_error(done.unwrap_err());
        if (! options.headless) {
            auto present = ui_renderer.present();
            if (present.is_err()) return gpu_error(present.unwrap_err());
            refresh_surface = present.unwrap().status != vui::gpu::PresentStatus::Queued;
        }
        redraw_pending = false;
        ++completed;
        if (options.frames && completed >= options.frames) close = true;
    }
    if (completed && ! options.capture.is_empty() &&
        ! capture(ui_renderer, options.capture.as_str()))
        return 1;
    source.close().unwrap();
    session.close().unwrap();
    renderer.close();
    auto shutdown = ui_renderer.shutdown(5'000'000'000ull);
    if (shutdown.is_err()) return gpu_error(shutdown.unwrap_err());
    rstd::io::println(
        "GPU completed frames={}, validation errors={}", completed, device.validation_errors());
    return device.validation_errors() ? 1 : 0;
}
