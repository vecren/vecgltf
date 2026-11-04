export module vecgltf.viewer;
import :options;
import :settings;
import vecgltf;
import vecgltf.render;
import vecgltf.display;
import vui.sdl;
import vecgltf.viewer.panel;
import vecgltf.viewer.model;
import vecgltf.viewer.camera;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;

using viewer::CameraReference;

auto argument(const char* value) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(value).to_str().unwrap_or(""_str);
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
    auto parsed = viewer::ParseOptions(argc, argv);
    if (parsed.is_err()) {
        rstd::io::eprintln("{}", parsed.unwrap_err());
        return 2;
    }
    auto selected = parsed.unwrap();
    if (selected.is_none()) return 0;
    auto                         options = rstd::move(selected).unwrap();
    Option<viewer::ViewSettings> settings;
    if (! options.settings_file.is_empty()) {
        auto loaded_settings = viewer::LoadViewSettings(options.settings_file.as_str());
        if (loaded_settings.is_err()) {
            report(loaded_settings.unwrap_err());
            return 1;
        }
        settings          = Some(loaded_settings.unwrap());
        const auto& scene = settings->scene;
        if (! options.render_overrides.exposure)
            options.render.exposure = settings->display.exposure;
        if (! options.render_overrides.tone_mapping)
            options.render.tone_mapping = settings->display.tone_mapping;
        if (! options.render_overrides.background) options.render.background = scene.background;
        if (! options.render_overrides.ibl_intensity)
            options.render.ibl_intensity = scene.ibl_intensity;
        options.render.background_intensity = scene.background_intensity;
        options.render.world_to_environment = scene.world_to_environment;
        options.render.directional_lights   = scene.directional_lights;
        if (options.render_overrides.direct_light)
            options.render.directional_lights =
                gltf::DirectionalLights(Vec<gltf::DirectionalLight> {});
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
    if (options.external_camera) {
        Option<gltf::AssetId> reference;
        if (options.camera_reference == CameraReference::SceneRoot) {
            reference = instance.scene_root();
            if (reference.is_none()) {
                rstd::io::eprintln("camera scene reference requires exactly one scene root");
                return 2;
            }
        } else if (options.camera_reference == CameraReference::Node)
            reference = Some(asset->node_id(options.camera_reference_node));
        gltf::CameraProjection projection;
        projection.yfov = options.camera_fov * (3.14159265358979323846f / 180);
        auto frame      = instance.camera_frame(
            { options.camera_position.unwrap(), options.camera_target.unwrap() },
            projection,
            options.camera_up,
            reference);
        if (frame.is_none() ||
            ! cameras.orbit().restore(*frame, float(options.width) / float(options.height))) {
            rstd::io::eprintln("camera preset or reference transform is invalid");
            return 2;
        }
    }
    cameras.orbit().zoom(options.zoom);
    cameras.orbit().rotate(options.rotate.x, options.rotate.y);
    cameras.orbit().pan(options.pan.x, options.pan.y);
    cameras.orbit().roll(options.roll);
    if (options.camera_path.is_empty())
        options.camera_path = rstd::format("{}.camera.json", options.path);
    if (! options.external_camera) {
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
    auto                          renderer = created.unwrap();
    Option<gltf::DisplayRenderer> display;
    if (settings) {
        auto next = gltf::DisplayRenderer::create(device);
        if (next.is_err()) {
            report(next.unwrap_err());
            return 1;
        }
        display = Some(next.unwrap());
    }
    auto output_queue  = vrento::vulkan::TextureContext::create(device).unwrap();
    auto output_device = dyn<vrento::texture::TextureDevice>::from_ref(output_queue);
    Option<vrento::texture::TexturePool> output_pool;
    Option<vrento::texture::TexturePool> radiance_pool;
    Option<vrento::texture::TexturePool> coverage_pool;
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
    } else if (settings && settings->environment_images) {
        auto prepared_environment =
            renderer.set_environment(settings->environment_images.take().unwrap());
        if (prepared_environment.is_err()) {
            report(prepared_environment.unwrap_err());
            return 1;
        }
    }
    auto preparing = renderer.prepare(asset.clone(), options.prepare);
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
                    auto replaced = viewer::reload(renderer,
                                                   options.path.as_str(),
                                                   asset,
                                                   prepared,
                                                   instance,
                                                   options.prepare);
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
        gltf::DisplayOptions display_options;
        if (display) {
            display_options              = settings->display;
            display_options.exposure     = options.render.exposure;
            display_options.tone_mapping = options.render.tone_mapping;
            if (options.render.debug != gltf::DebugView::None) {
                display_options              = gltf::DisplayOptions {};
                display_options.tone_mapping = gltf::ToneMapping::Linear;
            }
        }
        bool needs_coverage =
            display && ((display_options.bloom && display_options.bloom->intensity > 0) ||
                        display_options.sharpen > 0);
        if (! output_pool || output_extent.width != model_extent.width ||
            output_extent.height != model_extent.height) {
            vrento::texture::TextureDesc description;
            description.extent = { model_extent.width, model_extent.height };
            if (display) description.format = vrento::TextureFormat::RGBA8_SRGB;
            description.usage =
                vrento::texture::Usage::Sampled | vrento::texture::Usage::ColorAttachment;
            auto next =
                vrento::texture::TexturePool::create(output_device.as_mut_ref(), description);
            if (next.is_err()) {
                rstd::io::eprintln("output allocation kind={}", unsigned(next.unwrap_err().kind));
                return 1;
            }
            output_pool   = Some(next.unwrap());
            coverage_pool = None();
            if (display) {
                description.format = vrento::TextureFormat::RGBA16F;
                auto hdr =
                    vrento::texture::TexturePool::create(output_device.as_mut_ref(), description);
                if (hdr.is_err()) {
                    rstd::io::eprintln("radiance allocation kind={}",
                                       unsigned(hdr.unwrap_err().kind));
                    return 1;
                }
                radiance_pool = Some(hdr.unwrap());
            }
            output_extent = model_extent;
        }
        if (needs_coverage && ! coverage_pool) {
            vrento::texture::TextureDesc description;
            description.extent = { model_extent.width, model_extent.height };
            description.format = vrento::TextureFormat::RGBA16F;
            description.usage =
                vrento::texture::Usage::Sampled | vrento::texture::Usage::ColorAttachment;
            auto coverage =
                vrento::texture::TexturePool::create(output_device.as_mut_ref(), description);
            if (coverage.is_err()) {
                rstd::io::eprintln("coverage allocation kind={}",
                                   unsigned(coverage.unwrap_err().kind));
                return 1;
            }
            coverage_pool = Some(coverage.unwrap());
        }
        auto                                        write = output_pool->acquire().unwrap();
        Option<vrento::texture::TexturePool::Write> radiance_write;
        Option<vrento::texture::TexturePool::Write> coverage_write;
        Option<gltf::SceneOptions>                  scene_options;
        Option<gltf::RenderTarget>                  scene_target;
        if (display) {
            auto resolved = viewer::ResolveSceneOptions(*settings, instance, options.render);
            if (resolved.is_err()) {
                report(resolved.unwrap_err());
                return 1;
            }
            scene_options  = Some(resolved.unwrap());
            radiance_write = Some(radiance_pool->acquire().unwrap());
            scene_target   = Some(gltf::RenderTarget { radiance_write->view() });
            if (needs_coverage) {
                coverage_write         = Some(coverage_pool->acquire().unwrap());
                scene_target->coverage = Some(coverage_write->view());
            }
            auto scene_ready = renderer.prepare_scene_target(*scene_target);
            auto display_ready =
                display->prepare_target(gltf::DisplayTarget { write.view() }, display_options);
            if (scene_ready.is_err()) {
                report(scene_ready.unwrap_err());
                return 1;
            }
            if (display_ready.is_err()) {
                report(display_ready.unwrap_err());
                return 1;
            }
            if (scene_ready.unwrap() != vrento::resource_registry::PipelineReadiness::Ready ||
                display_ready.unwrap() != vrento::resource_registry::PipelineReadiness::Ready) {
                rstd::io::eprintln("scene or display pipelines are not ready");
                return 1;
            }
        }
        auto work = output_queue.Begin().unwrap();
        auto frame =
            display
                ? renderer.record_scene(
                      work, scene_target.take().unwrap(), prepared, instance, view, *scene_options)
                : renderer.record(work, write.view(), prepared, instance, view, options.render);
        if (frame.is_err()) {
            report(frame.unwrap_err());
            return 1;
        }
        if (display) {
            auto displayed = display->record(
                work,
                gltf::DisplayInput { radiance_write->view(),
                                     coverage_write ? Some(coverage_write->view()) : None() },
                gltf::DisplayTarget { write.view() },
                display_options);
            if (displayed.is_err()) {
                report(displayed.unwrap_err());
                return 1;
            }
        }
        auto submitted = output_queue.Submit(rstd::move(work));
        if (submitted.is_err()) {
            rstd::io::eprintln("model submit kind={}", unsigned(submitted.unwrap_err().kind));
            return 1;
        }
        auto submission = submitted.unwrap();
        if (radiance_write)
            radiance_pool->publish(radiance_write.take().unwrap(), submission).unwrap();
        if (coverage_write)
            coverage_pool->publish(coverage_write.take().unwrap(), submission).unwrap();
        auto published = output_pool->publish(rstd::move(write), submission).unwrap();
        auto native    = vui::gpu::NativeTextureFrame::wrap(
            rstd::move(published),
            { .producer_session = producer,
              .frame_version    = ++output_version,
              .color_space =
                  display ? vui::ContentColorSpace::Srgb : vui::ContentColorSpace::LinearSrgb });
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
    if (display) display->close();
    auto shutdown = ui_renderer.shutdown(5'000'000'000ull);
    if (shutdown.is_err()) return gpu_error(shutdown.unwrap_err());
    rstd::io::println(
        "GPU completed frames={}, validation errors={}", completed, device.validation_errors());
    return device.validation_errors() ? 1 : 0;
}
