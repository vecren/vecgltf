module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
#include <cstdio>

module vecgltf.tests:render;
import vecgltf;
import vecgltf.render;
import vecgltf.environment;
import vui.render;
import rstd.test;
import rstd.log;
import :output;
using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::render
{
Arc<Asset> triangle(Vec4 color = { 1, 0, 0, 1 }, bool reverse = false) {
    static unsigned generation {};
    auto            asset = Arc<Asset>::make();
    asset->generation     = u64(++generation);
    Material material;
    material.base_color = color;
    material.unlit      = true;
    asset->materials.push(rstd::move(material));
    Primitive primitive;
    Vec3      positions[] { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    for (unsigned i = 0; i < 3; ++i) {
        Vertex v;
        v.position = positions[reverse ? 2 - i : i];
        v.normal   = { 0, 0, 1 };
        v.uv0      = { 0.25f, 1.75f };
        primitive.bounds.include(v.position);
        primitive.vertices.push(rstd::move(v));
    }
    Mesh mesh;
    mesh.primitives.push(rstd::move(primitive));
    asset->meshes.push(rstd::move(mesh));
    Node node;
    node.mesh = 0;
    asset->nodes.push(rstd::move(node));
    Scene scene;
    scene.roots.push(0);
    asset->scenes.push(rstd::move(scene));
    return asset;
}
void near(unsigned actual, unsigned expected) {
    rstd_assert(actual + 2 >= expected && actual <= expected + 2);
}

auto read_pixels(vrento::vulkan::DeviceSession   session,
                 vrento::vulkan::TextureContext& queue,
                 vrento::texture::TextureViewRef target) -> Vec<u8> {
    auto               size = vrento::texture::mip_extent(target.texture().description(),
                                                          target.description().range.first_mip);
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size   = size.width * size.height * 4;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto buffer = session.device()
                      .memory_allocator()
                      .create_buffer(info, vvk::MemoryRequest::Readback())
                      .unwrap();
    auto work   = queue.Begin().unwrap();
    work.commands()
        ->UseTexture(
            { target, vrento::texture::Access::TransferSource, vrento::texture::Stages::Transfer })
        .unwrap();
    VkBufferImageCopy copy {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { size.width, size.height, 1 };
    session.device().handle().Dispatch().vkCmdCopyImageToBuffer(
        vrento::vulkan::native_recording(work, session).unwrap(),
        vrento::vulkan::native_texture(target.texture()).unwrap(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffer.handle(),
        1,
        &copy);
    VkBufferMemoryBarrier host { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    host.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer                                         = buffer.handle();
    host.size                                           = VK_WHOLE_SIZE;
    session.device().handle().Dispatch().vkCmdPipelineBarrier(
        vrento::vulkan::native_recording(work, session).unwrap(),
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &host,
        0,
        nullptr);
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto memory = buffer.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    Vec<u8> pixels;
    for (unsigned i = 0; i < info.size; ++i)
        pixels.push(u8(static_cast<unsigned char*>(mapped.data())[i]));
    return pixels;
}
TEST(Render, PreparationTimings) {
    namespace vk = vrento::vulkan;
    using vrento::resource_registry::PipelineReadiness;
    double             cold {}, warm {}, hot {}, asynchronous {};
    constexpr unsigned samples = 3, queries = 128;
    for (unsigned sample = 0; sample < samples; ++sample) {
        auto session  = vk::DeviceSession::create().unwrap();
        auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
        auto stats    = session.graphics_resources().pipelines.stats();
        rstd_assert(stats.creations == u64(4) && stats.peak_entries == usize(4));
        cold += stats.creation_nanoseconds.to_primitive() / 1e6;
        auto encoded =
            session.graphics_resources().pipelines.driver_cache().encode(session.device()).unwrap();
        auto loaded =
            vk::DeviceSession::create({ .pipeline_cache_data = encoded.as_slice() }).unwrap();
        auto warmed       = Renderer::create(loaded, { 64, 64 }).unwrap();
        auto warmed_stats = loaded.graphics_resources().pipelines.stats();
        rstd_assert(warmed_stats.creations == u64(4));
        warm += warmed_stats.creation_nanoseconds.to_primitive() / 1e6;
        auto                         queue = vk::TextureContext::create(loaded).unwrap();
        vrento::texture::TextureDesc desc;
        desc.extent  = { 64, 64 };
        desc.usage   = vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::Sampled;
        auto target  = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
        auto started = rstd::time::Instant::now();
        for (unsigned i = 0; i < queries; ++i)
            rstd_assert(warmed.prepare_target(target).unwrap() == PipelineReadiness::Ready);
        hot += started.elapsed().as_secs_f64() * 1000;
        rstd_assert(loaded.graphics_resources().pipelines.stats().creations ==
                    warmed_stats.creations);
        auto deferred       = vk::DeviceSession::create().unwrap();
        auto deferred_queue = vk::TextureContext::create(deferred).unwrap();
        auto deferred_target =
            deferred_queue.CreateView(deferred_queue.CreateTexture(desc).unwrap(), {}).unwrap();
        started = rstd::time::Instant::now();
        auto pending =
            Renderer::create(deferred, { 64, 64 }, vk::PipelinePreparationMode::Deferred).unwrap();
        while (pending.prepare_target(deferred_target).unwrap() == PipelineReadiness::Pending) {
            rstd_assert(started.elapsed().as_secs() < u64(5));
            rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
        }
        asynchronous += started.elapsed().as_secs_f64() * 1000;
        rstd_assert(deferred.graphics_resources().pipelines.stats().creations == u64(4));
    }
    std::printf("model pipelines: %u samples, 4 recipes; empty driver native "
                "preparation %.3f ms; loaded driver %.3f ms; %u target queries "
                "%.3f ms; deferred init and ready %.3f ms; peak entries 4\n",
                samples,
                cold / samples,
                warm / samples,
                queries,
                hot / samples,
                asynchronous / samples);
}
TEST(Render, DeferredPreparation) {
    namespace vk  = vrento::vulkan;
    namespace tex = vrento::texture;
    using vrento::resource_registry::PipelineReadiness;
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto renderer =
        Renderer::create(session, { 64, 64 }, vk::PipelinePreparationMode::Deferred).unwrap();
    auto          asset    = triangle();
    auto          prepared = renderer.prepare(asset.clone()).unwrap();
    SceneInstance instance(asset.clone());
    View camera { look_at({ 0, 0, 2 }, { 0, 0, 0 }), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    tex::TextureDesc desc;
    desc.extent = { 64, 64 };
    desc.usage  = tex::Usage::ColorAttachment | tex::Usage::Sampled | tex::Usage::TransferSource;
    auto old    = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    auto wait_ready = [&](tex::TextureViewRef view) {
        auto              start = rstd::time::Instant::now();
        PipelineReadiness status;
        do {
            status = renderer.prepare_target(view).unwrap();
            rstd_assert(status != PipelineReadiness::Failed);
            if (status == PipelineReadiness::Pending)
                rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
        } while (status == PipelineReadiness::Pending && start.elapsed().as_millis() < u64(5000));
        rstd_assert(status == PipelineReadiness::Ready);
    };
    wait_ready(old);
    auto first = queue.Begin().unwrap();
    renderer.record(first, old, prepared, instance, camera, { 1, DebugView::BaseColor }).unwrap();
    queue.wait(queue.Submit(rstd::move(first)).unwrap()).unwrap();
    auto revision = old.texture().content_revision();
    auto baseline = renderer.upload_statistics();
    auto frames   = renderer.statistics().recorded_frames;
    desc.format   = vrento::TextureFormat::RGBA8_SRGB;
    auto next     = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    auto work     = queue.Begin().unwrap();
    auto pending =
        renderer.record(work, next, prepared, instance, camera, { 1, DebugView::BaseColor });
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
    rstd_assert(next.texture().content_revision() == 0);
    rstd_assert(old.texture().content_revision() == revision);
    rstd_assert(renderer.statistics().recorded_frames == frames);
    for (unsigned i = 0; i < 5; ++i) {
        rstd_assert(renderer.upload_statistics().queued_copies[i] == baseline.queued_copies[i]);
        rstd_assert(renderer.upload_statistics().recorded_copies[i] == baseline.recorded_copies[i]);
    }
    wait_ready(next);
    renderer.record(work, next, prepared, instance, camera, { 1, DebugView::BaseColor }).unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto old_pixels  = read_pixels(session, queue, old);
    auto next_pixels = read_pixels(session, queue, next);
    near(u8(old_pixels[usize((32 * 64 + 32) * 4)]).to_primitive(), 255);
    near(u8(next_pixels[usize((32 * 64 + 32) * 4)]).to_primitive(), 255);
    rstd_assert(renderer.graph_statistics().graph_compiles == 2);
    renderer.close();
    auto closed = renderer.prepare_target(next).unwrap_err();
    rstd_assert(closed.cause.is_some());
    rstd_assert(closed.message.as_str().contains("pipeline preparation closed"_str));
    auto cause = closed.cause.take().unwrap();
    rstd_assert(rstd::format("{}", cause.as_ptr().as_ptr().as_ref())
                    .as_str()
                    .contains("pipeline preparation closed"_str));
    rstd_assert(session.validation_errors() == 0);
}
TEST(Render, QueuedDynamicInputs) {
    auto session  = vrento::vulkan::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vrento::vulkan::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto asset    = triangle();
    at(asset->materials, 0).metallic = 0;
    MorphTarget morph;
    for (unsigned i = 0; i < 3; ++i) {
        morph.positions.push({ 3, 0, 0 });
        morph.normals.push({});
        morph.tangents.push({});
    }
    at(at(asset->meshes, 0).primitives, 0).morphs.push(rstd::move(morph));
    at(asset->nodes, 0).weights.push(0.0f);
    Animation animation;
    animation.end = 1;
    AnimationSampler weights;
    weights.components = 1;
    weights.times.push(0.0f);
    weights.times.push(1.0f);
    weights.values.push(0.0f);
    weights.values.push(1.0f);
    animation.samplers.push(rstd::move(weights));
    animation.channels.push({ 0, 0, AnimationPath::Weights });
    asset->animations.push(rstd::move(animation));
    auto& primitive           = at(at(asset->meshes, 0).primitives, 0);
    primitive.skin_attributes = true;
    for (auto& vertex : primitive.vertices) vertex.weights = { 1, 0, 0, 0 };
    at(asset->nodes, 0).skin = 0;
    asset->nodes.push(Node {});
    at(asset->scenes, 0).roots.push(1);
    Skin skin;
    skin.joints.push(1);
    skin.inverse_bind.push(Mat4::Identity());
    asset->skins.push(rstd::move(skin));
    AnimationSampler joint;
    joint.components = 3;
    joint.times.push(0.0f);
    joint.times.push(1.0f);
    float joint_values[] { 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f };
    for (float value : joint_values) joint.values.push(float(value));
    at(asset->animations, 0).samplers.push(rstd::move(joint));
    at(asset->animations, 0).channels.push({ 1, 1, AnimationPath::Translation });
    auto          prepared = renderer.prepare(asset.clone()).unwrap();
    SceneInstance instance(asset.clone());
    instance.set_animation(0);
    View camera { look_at({ 0, 0, 2 }, { 0, 0, 0 }), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    vrento::texture::TextureDesc desc;
    desc.extent = { 64, 64 };
    desc.usage  = vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::Sampled |
                  vrento::texture::Usage::TransferSource;
    auto target = [&] {
        return queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    };
    auto warm  = target();
    auto setup = queue.Begin().unwrap();
    renderer.record(setup, warm, prepared, instance, camera, { 1, DebugView::BaseColor }).unwrap();
    queue.wait(queue.Submit(rstd::move(setup)).unwrap()).unwrap();
    queue.Poll().unwrap();
    auto                      baseline  = renderer.upload_statistics();
    auto                      pipelines = session.graphics_resources().pipelines.stats().creations;
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &type;
    vvk::Semaphore semaphore;
    rstd_assert(session.device().handle().CreateSemaphore(info, semaphore) == VK_SUCCESS);
    auto handle = *semaphore;
    auto gate =
        queue.ImportDependency(handle, 1, Arc<dyn<rstd::any::Any>>::make(rstd::move(semaphore)))
            .unwrap();
    Vec<vrento::texture::TextureViewRef> outputs;
    vrento::texture::SubmissionToken     last;
    for (unsigned frame = 0; frame < 12; ++frame) {
        instance.set_time(float(frame & 1));
        outputs.push(target());
        auto work = queue.Begin().unwrap();
        work.commands()->Wait(gate, vrento::texture::Stages::All).unwrap();
        auto debug = frame & 2 ? DebugView::Metallic : DebugView::BaseColor;
        renderer.record(work, outputs[usize(frame)], prepared, instance, camera, { 1, debug })
            .unwrap();
        last = queue.Submit(rstd::move(work)).unwrap();
        rstd_assert(! last.poll().unwrap());
        rstd_assert(renderer.graph_statistics().graph_compiles == 1);
        rstd_assert(renderer.graph_statistics().native_completed == 1);
        rstd_assert(session.graphics_resources().pipelines.stats().creations == pipelines);
    }
    renderer.close();
    auto uploaded = renderer.upload_statistics();
    rstd_assert(uploaded.recorded_copies[0] == baseline.recorded_copies[0]);
    rstd_assert(uploaded.recorded_copies[1] == baseline.recorded_copies[1]);
    auto signal = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        session.device().handle().Dispatch().vkGetDeviceProcAddr(*session.device().handle(),
                                                                 "vkSignalSemaphoreKHR"));
    VkSemaphoreSignalInfo release { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    release.semaphore = handle;
    release.value     = 1;
    rstd_assert(signal && signal(*session.device().handle(), &release) == VK_SUCCESS);
    queue.wait(last).unwrap();
    queue.Poll().unwrap();
    for (unsigned frame = 0; frame < 12; ++frame) {
        auto pixels = read_pixels(session, queue, outputs[usize(frame)]);
        near(u8(pixels[usize((32 * 64 + 32) * 4)]).to_primitive(), frame % 4 == 0 ? 255 : 0);
    }
    rstd_assert(session.validation_errors() == 0);
}
TEST(Render, SharedDepth) {
    auto session = vrento::vulkan::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vrento::vulkan::TextureContext::create(session).unwrap();
    auto first   = Renderer::create(session, { 64, 64 }).unwrap();
    auto second  = Renderer::create(session, { 64, 64 }).unwrap();
    auto front = triangle(), back = triangle({ 0, 0, 1, 1 });
    for (auto& vertex : at(at(back->meshes, 0).primitives, 0).vertices) vertex.position.z = -0.5f;
    auto          front_gpu = first.prepare(front.clone()).unwrap();
    auto          back_gpu  = second.prepare(back.clone()).unwrap();
    SceneInstance front_scene(front.clone()), back_scene(back.clone());
    View camera { look_at({ 0, 0, 2 }, { 0, 0, 0 }), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    vrento::texture::TextureDesc color_desc;
    color_desc.extent = { 64, 64 };
    color_desc.format = vrento::TextureFormat::RGBA8_SRGB;
    color_desc.usage  = vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::Sampled |
                        vrento::texture::Usage::TransferSource;
    auto color        = queue.CreateView(queue.CreateTexture(color_desc).unwrap(), {}).unwrap();
    auto depth_desc   = color_desc;
    depth_desc.format = vrento::TextureFormat::D32F;
    depth_desc.usage  = vrento::texture::Usage::DepthAttachment;
    auto depth =
        queue
            .CreateView(queue.CreateTexture(depth_desc).unwrap(),
                        { vrento::texture::Dimension::D2, { vrento::texture::Aspect::Depth } })
            .unwrap();
    auto work = queue.Begin().unwrap();
    first
        .record(work,
                RenderTarget { color, Some(vrento::texture::TextureViewRef(depth)) },
                front_gpu,
                front_scene,
                camera,
                { 1, DebugView::BaseColor })
        .unwrap();
    auto created = session.graphics_resources().pipelines.stats().creations;
    second
        .record(work,
                RenderTarget { color,
                               Some(vrento::texture::TextureViewRef(depth)),
                               vrento::vulkan::AttachmentLoad::Load,
                               vrento::vulkan::AttachmentLoad::Load },
                back_gpu,
                back_scene,
                camera,
                { 1, DebugView::BaseColor })
        .unwrap();
    rstd_assert(session.graphics_resources().pipelines.stats().creations == created);
    {
        vui::gpu::Renderer ui;
        ui.initialize(session, false).unwrap();
        vui::UiSession document;
        vui::NodeStyle style;
        style.layout.width = style.layout.height = Some(vui::Scalar(16));
        style.background                         = { 0, 0.5f, 0, 0.5f };
        auto node = document.document().create<vui::Rectangle>(style).unwrap();
        document.document().mount(node).unwrap();
        document.advance({}, { 1, {} }, { { 64, 64 }, 1, 1 }).unwrap();
        ui.record(work, document.plan_frame().unwrap(), color).unwrap();
        ui.shutdown().unwrap();
    }
    work.commands()
        ->UseTexture(
            { color, vrento::texture::Access::TransferSource, vrento::texture::Stages::Transfer })
        .unwrap();
    VkBufferCreateInfo buffer_info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size         = 64 * 64 * 4;
    buffer_info.usage        = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto              buffer = session.device()
                                   .memory_allocator()
                                   .create_buffer(buffer_info, vvk::MemoryRequest::Readback())
                                   .unwrap();
    VkBufferImageCopy copy {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { 64, 64, 1 };
    const auto& dispatch  = session.device().handle().Dispatch();
    auto        command   = vrento::vulkan::native_recording(work, session).unwrap();
    dispatch.vkCmdCopyImageToBuffer(command,
                                    vrento::vulkan::native_texture(color.texture()).unwrap(),
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    buffer.handle(),
                                    1,
                                    &copy);
    VkBufferMemoryBarrier host { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    host.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer                                         = buffer.handle();
    host.size                                           = VK_WHOLE_SIZE;
    dispatch.vkCmdPipelineBarrier(command,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &host,
                                  0,
                                  nullptr);
    auto done = queue.Submit(rstd::move(work)).unwrap();
    queue.wait(done).unwrap();
    queue.Poll().unwrap();
    auto memory = buffer.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    auto* pixels = static_cast<unsigned char*>(mapped.data());
    rstd_assert(pixels[(32 * 64 + 32) * 4] == 255 && pixels[(32 * 64 + 32) * 4 + 2] == 0);
    rstd_assert(pixels[(8 * 64 + 8) * 4] == 0 && pixels[(8 * 64 + 8) * 4 + 1] > 185 &&
                pixels[(8 * 64 + 8) * 4 + 1] < 191);
    near(pixels[(8 * 64 + 15) * 4 + 1], 188);
    near(pixels[(8 * 64 + 16) * 4 + 1], 0);
    auto warm_creations = session.graphics_resources().pipelines.stats().creations;
    color_desc.extent   = { 32, 96 };
    depth_desc.extent   = color_desc.extent;
    auto resized_color  = queue.CreateView(queue.CreateTexture(color_desc).unwrap(), {}).unwrap();
    auto resized_depth =
        queue
            .CreateView(queue.CreateTexture(depth_desc).unwrap(),
                        { vrento::texture::Dimension::D2, { vrento::texture::Aspect::Depth } })
            .unwrap();
    auto resized_work = queue.Begin().unwrap();
    first
        .record(
            resized_work,
            RenderTarget { resized_color, Some(vrento::texture::TextureViewRef(resized_depth)) },
            front_gpu,
            front_scene,
            camera,
            { 1, DebugView::BaseColor })
        .unwrap();
    second
        .record(resized_work,
                RenderTarget { resized_color,
                               Some(vrento::texture::TextureViewRef(resized_depth)),
                               vrento::vulkan::AttachmentLoad::Load,
                               vrento::vulkan::AttachmentLoad::Load },
                back_gpu,
                back_scene,
                camera,
                { 1, DebugView::BaseColor })
        .unwrap();
    queue.wait(queue.Submit(rstd::move(resized_work)).unwrap()).unwrap();
    queue.Poll().unwrap();
    auto resized_pixels = read_pixels(session, queue, resized_color);
    near(u8(resized_pixels[usize((48 * 32 + 16) * 4)]).to_primitive(), 255);
    rstd_assert(session.graphics_resources().pipelines.stats().creations == warm_creations);
    rstd_assert(first.graph_statistics().graph_compiles == 2);
    rstd_assert(second.graph_statistics().graph_compiles == 2);
    auto cleared = queue.Begin().unwrap();
    second
        .record(
            cleared,
            RenderTarget { resized_color, Some(vrento::texture::TextureViewRef(resized_depth)) },
            back_gpu,
            back_scene,
            camera,
            { 1, DebugView::BaseColor })
        .unwrap();
    rstd_assert(second.graph_statistics().graph_compiles == 3);
    queue.wait(queue.Submit(rstd::move(cleared)).unwrap()).unwrap();
    auto cleared_pixels = read_pixels(session, queue, resized_color);
    near(u8(cleared_pixels[usize((48 * 32 + 16) * 4)]).to_primitive(), 0);
    near(u8(cleared_pixels[usize((48 * 32 + 16) * 4 + 2)]).to_primitive(), 255);
    rstd_assert(session.validation_errors() == 0);
}
TEST(Render, MaterialWorkflows) {
    auto session  = vrento::vulkan::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vrento::vulkan::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    vrento::texture::TextureDesc desc;
    desc.extent = { 64, 64 };
    desc.format = vrento::TextureFormat::RGBA8_SRGB;
    desc.usage  = vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::Sampled |
                  vrento::texture::Usage::TransferSource;
    auto target = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    View view { look_at({ 0, 0, 2 }, {}), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    auto draw = [&](Arc<Asset> asset, DebugView debug = DebugView::None, float exposure = 1) {
        auto          prepared = renderer.prepare(asset.clone()).unwrap();
        SceneInstance instance(asset.clone());
        auto          work = queue.Begin().unwrap();
        auto          recorded =
            renderer.record(work, target, prepared, instance, view, { exposure, debug });
        if (recorded.is_err()) {
            auto error = recorded.unwrap_err();
            rstd::io::eprintln("{}: {}", error.location, error.message);
            rstd_assert(false);
        }
        recorded.unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        return read_pixels(session, queue, target);
    };
    auto  asset               = triangle({ 0.5f, 0.5f, 0.5f, 1 });
    auto& material            = at(asset->materials, 0);
    material.unlit            = false;
    material.workflow         = MaterialWorkflow::SpecularGlossiness;
    material.specular         = { 0.8f, 0.6f, 0.4f };
    material.glossiness       = 0.5f;
    material.physical.texture = 0;
    Image image;
    image.width = image.height = 1;
    unsigned physical_pixels[] { 128, 64, 32, 128 };
    for (unsigned value : physical_pixels) image.rgba.push(u8(value));
    asset->images.push(rstd::move(image));
    Texture texture;
    texture.image = 0;
    asset->textures.push(rstd::move(texture));
    auto sampled   = draw(asset.clone());
    auto roughness = draw(asset.clone(), DebugView::Roughness);
    auto center    = [](const Vec<u8>& pixels, unsigned channel) {
        return pixels[usize((32 * 64 + 32) * 4 + channel)].to_primitive();
    };
    near(center(roughness, 0), 224);

    auto  decoded       = triangle({ 0.5f, 0.5f, 0.5f, 1 });
    auto& expected      = at(decoded->materials, 0);
    expected.unlit      = false;
    expected.workflow   = MaterialWorkflow::SpecularGlossiness;
    expected.specular   = { 0.8f * 0.2158605f, 0.6f * 0.05126946f, 0.4f * 0.01444384f };
    expected.glossiness = 0.5f * (128.0f / 255);
    auto factored       = draw(decoded.clone());
    for (unsigned channel = 0; channel < 3; ++channel)
        near(center(sampled, channel), center(factored, channel));

    auto metallic_sample = [&](float metallic) {
        auto  sample    = triangle({ 0.8f, 0.4f, 0.1f, 1 });
        auto& value     = at(sample->materials, 0);
        value.unlit     = false;
        value.metallic  = metallic;
        value.roughness = 0.8f;
        return draw(rstd::move(sample), DebugView::None, 0.1f);
    };
    auto dielectric = metallic_sample(0), metal = metallic_sample(1), mixed = metallic_sample(0.5f);
    auto hdr = [](unsigned pixel) {
        float value = float(pixel) / 255;
        float y     = value <= 0.04045f
                          ? value / 12.92f
                          : rstd::f32((value + 0.055f) / 1.055f).powf(f32(2.4f)).to_primitive();
        float a = 2.51f - 2.43f * y, b = 0.03f - 0.59f * y;
        return (-b + square_root(b * b + 0.56f * y * a)) / (2 * a);
    };
    for (unsigned channel = 0; channel < 3; ++channel) {
        float value = (hdr(center(dielectric, channel)) + hdr(center(metal, channel))) * 0.5f;
        float mapped =
            (value * (2.51f * value + 0.03f)) / (value * (2.43f * value + 0.59f) + 0.14f);
        float encoded =
            mapped <= 0.0031308f
                ? mapped * 12.92f
                : 1.055f * rstd::f32(mapped).powf(f32(1.0f / 2.4f)).to_primitive() - 0.055f;
        near(center(mixed, channel), unsigned(encoded * 255 + 0.5f));
    }
    auto  coordinates     = triangle({ 1, 1, 1, 1 });
    auto& mapped          = at(coordinates->materials, 0);
    mapped.base.texture   = 0;
    mapped.base.uv        = 2;
    mapped.normal.texture = 1;
    mapped.normal.uv      = 3;
    auto&       primitive = at(at(coordinates->meshes, 0).primitives, 0);
    TexcoordSet color_uv, normal_uv;
    color_uv.index  = 2;
    normal_uv.index = 3;
    for (unsigned i = 0; i < 3; ++i) {
        color_uv.coordinates.push({ 0.75f, 0.5f });
        normal_uv.coordinates.push({ 0.25f, 0.5f });
    }
    primitive.extra_texcoords.push(rstd::move(color_uv));
    primitive.extra_texcoords.push(rstd::move(normal_uv));
    unsigned        colors[] { 255, 0, 0, 255, 0, 0, 255, 255 };
    unsigned        normals[] { 128, 128, 255, 255, 255, 128, 128, 255 };
    slice<unsigned> maps[] { slice<unsigned>::from_raw_parts(colors, usize(8)),
                             slice<unsigned>::from_raw_parts(normals, usize(8)) };
    for (auto pixels : maps) {
        Image values;
        values.width  = 2;
        values.height = 1;
        for (unsigned value : pixels) values.rgba.push(u8(value));
        Texture sampler;
        sampler.image = count(coordinates->images);
        coordinates->images.push(rstd::move(values));
        coordinates->textures.push(rstd::move(sampler));
    }
    auto base_color = draw(coordinates.clone(), DebugView::BaseColor);
    near(center(base_color, 0), 0);
    near(center(base_color, 2), 255);
    auto normal_color = draw(coordinates.clone(), DebugView::Normal);
    near(center(normal_color, 0), 188);
    near(center(normal_color, 2), 255);
    rstd_assert(session.validation_errors() == 0);
}

TEST(Render, BlendOcclusion) {
    namespace vk              = vrento::vulkan;
    namespace tex             = vrento::texture;
    auto             session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             queue    = vk::TextureContext::create(session).unwrap();
    auto             renderer = Renderer::create(session, { 64, 64 }).unwrap();
    tex::TextureDesc desc;
    desc.extent = { 64, 64 };
    desc.format = vrento::TextureFormat::RGBA8_SRGB;
    desc.usage  = tex::Usage::ColorAttachment | tex::Usage::Sampled | tex::Usage::TransferSource;
    auto target = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    View view { look_at({ 0, 0, 2 }, {}), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    struct Sample {
        float    factor, vertex;
        unsigned texture, red, blue;
        bool     split {}, opaque_rear {};
    };
    Sample samples[] { { 1, 1, 255, 255, 0 },       { 1, 0.5f, 255, 188, 188 },
                       { 1, 0, 255, 0, 255 },       { 1, 1, 128, 188, 187 },
                       { 1, 0.5f, 128, 137, 224 },  { 0.5f, 0.5f, 128, 99, 240, true, true },
                       { 1, 1, 255, 255, 0, true }, { 0, 1, 255, 0, 255, true, true } };
    for (const auto& sample : samples) {
        auto  asset           = triangle({ 1, 1, 1, sample.factor });
        auto& material        = at(asset->materials, 0);
        material.alpha        = AlphaMode::Blend;
        material.base.texture = 0;
        Image image;
        image.width  = 2;
        image.height = 1;
        unsigned pixels[] { 255, 255, 255, sample.texture, 255, 255, 255, 255 };
        for (unsigned value : pixels) image.rgba.push(u8(value));
        asset->images.push(rstd::move(image));
        Texture texture;
        texture.image       = 0;
        texture.sampler.min = texture.sampler.mag = Filter::Nearest;
        texture.sampler.mip                       = MipFilter::None;
        asset->textures.push(rstd::move(texture));
        auto& mesh       = at(asset->meshes, 0);
        auto& primitive  = at(mesh.primitives, 0);
        primitive.bounds = {};
        for (auto& vertex : primitive.vertices) {
            vertex.position.z = 0.25f;
            vertex.color      = { 1, 0, 0, sample.vertex };
            vertex.uv0        = { 0.25f, 0.5f };
            primitive.bounds.include(vertex.position);
        }
        Primitive rear;
        for (unsigned i = 0; i < 3; ++i) {
            auto vertex       = Vertex(at(primitive.vertices, i));
            vertex.position.z = -0.25f;
            vertex.color      = { 0, 0, 1, 1 };
            vertex.uv0        = { 0.75f, 0.5f };
            if (sample.split) {
                rear.bounds.include(vertex.position);
                rear.vertices.push(rstd::move(vertex));
            } else {
                primitive.bounds.include(vertex.position);
                primitive.vertices.push(rstd::move(vertex));
            }
        }
        if (sample.split) {
            Material background;
            background.alpha = sample.opaque_rear ? AlphaMode::Opaque : AlphaMode::Blend;
            rear.material    = count(asset->materials);
            asset->materials.push(rstd::move(background));
            mesh.primitives.push(rstd::move(rear));
        }
        auto          prepared = renderer.prepare(asset.clone()).unwrap();
        SceneInstance instance(asset.clone());
        auto          work = queue.Begin().unwrap();
        renderer.record(work, target, prepared, instance, view, { 1, DebugView::BaseColor })
            .unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        auto result = read_pixels(session, queue, target);
        auto center = [&](unsigned channel) {
            return u8(result[usize((32 * 64 + 32) * 4 + channel)]).to_primitive();
        };
        near(center(0), sample.red);
        near(center(1), 0);
        near(center(2), sample.blue);
    }
    rstd_assert(session.validation_errors() == 0);
}

TEST(Render, TranslatedCoordinates) {
    auto session  = vrento::vulkan::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vrento::vulkan::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    vrento::texture::TextureDesc desc;
    desc.extent = { 64, 64 };
    desc.usage  = vrento::texture::Usage::ColorAttachment | vrento::texture::Usage::Sampled |
                  vrento::texture::Usage::TransferSource;
    auto target = queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
    Vec3 offset { 600, -50, 50 }, eye { 1, 0.5f, 2 };
    auto camera = [&](Vec3 origin) {
        return View { look_at(origin + eye, origin),
                      perspective(0.8f, 1, 0.0004f, 500),
                      origin + eye };
    };
    auto make = [&](Vec3 origin, bool skinned) {
        auto     asset = triangle();
        Material back;
        back.base_color = { 0, 1, 0, 1 };
        back.unlit      = true;
        asset->materials.push(rstd::move(back));
        auto&     mesh = at(asset->meshes, 0);
        Primitive rear;
        rear.material = 1;
        for (const auto& vertex : at(mesh.primitives, 0).vertices) {
            auto value = vertex;
            value.position.z -= 0.02f;
            rear.bounds.include(value.position);
            rear.vertices.push(rstd::move(value));
        }
        mesh.primitives.push(rstd::move(rear));
        if (skinned) {
            for (auto& primitive : mesh.primitives) {
                primitive.skin_attributes = true;
                for (auto& vertex : primitive.vertices) vertex.weights = { 1, 0, 0, 0 };
            }
            at(asset->nodes, 0).skin = 0;
            Node joint;
            joint.translation = origin;
            asset->nodes.push(rstd::move(joint));
            at(asset->scenes, 0).roots.push(1);
            Skin skin;
            skin.joints.push(1);
            skin.inverse_bind.push(Mat4::Identity());
            asset->skins.push(rstd::move(skin));
        } else {
            at(asset->nodes, 0).translation = origin;
        }
        return asset;
    };
    auto record = [&](const PreparedAsset& prepared,
                      const SceneInstance& instance,
                      View                 view,
                      RenderOptions        options = { 1, DebugView::BaseColor }) {
        auto work = queue.Begin().unwrap();
        renderer.record(work, target, prepared, instance, view, options).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        return read_pixels(session, queue, target);
    };
    auto          baseline     = make({}, false);
    auto          baseline_gpu = renderer.prepare(baseline.clone()).unwrap();
    SceneInstance baseline_scene(baseline.clone());
    auto          expected = record(baseline_gpu, baseline_scene, camera({}));
    auto          compare  = [&](const Vec<u8>& pixels) {
        unsigned changed {};
        for (unsigned i = 0; i < 64 * 64; ++i)
            for (unsigned channel = 0; channel < 3; ++channel) {
                auto index = usize(i * 4 + channel);
                if (absolute(float(u8(pixels[index]).to_primitive()) -
                             float(u8(expected[index]).to_primitive())) > 2) {
                    ++changed;
                    break;
                }
            }
        rstd_assert(changed <= 4);
    };
    bool variants[] { false, true };
    for (bool skinned : variants) {
        auto          asset    = make(offset, skinned);
        auto          prepared = renderer.prepare(asset.clone()).unwrap();
        SceneInstance instance(asset.clone());
        compare(record(prepared, instance, camera(offset)));
        auto lighting_eye = camera(offset);
        lighting_eye.eye  = {};
        compare(record(prepared, instance, lighting_eye));
        compare(record(prepared, instance, camera(offset)));
    }
    HdrImage environment { 16, 8 };
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            environment.rgba.push(float(x + 1) / 16);
            environment.rgba.push(float(y + 1) / 8);
            environment.rgba.push(0.25f);
            environment.rgba.push(1.0f);
        }
    renderer.set_environment(rstd::move(environment)).unwrap();
    at(baseline->materials, 0).unlit    = false;
    at(baseline->materials, 0).metallic = 0.4f;
    baseline_gpu                        = renderer.prepare(baseline.clone()).unwrap();
    auto origin_view                    = camera({});
    origin_view.projection              = perspective(0.8f, 1, 0.0004f);
    expected   = record(baseline_gpu, baseline_scene, origin_view, { 1, DebugView::None, true });
    auto moved = make(offset, false);
    at(moved->materials, 0).unlit    = false;
    at(moved->materials, 0).metallic = 0.4f;
    auto          moved_gpu          = renderer.prepare(moved.clone()).unwrap();
    SceneInstance moved_scene(moved.clone());
    auto          moved_view = camera(offset);
    moved_view.projection    = origin_view.projection;
    compare(record(moved_gpu, moved_scene, moved_view, { 1, DebugView::None, true }));
    rstd_assert(session.validation_errors() == 0);
}

TEST(Render, Integration) {
    static rstd::log::EnvLogger logger("info"_str);
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(rstd::log::LevelFilter::Info);
    auto constant = [] {
        HdrImage image { 4, 2 };
        for (unsigned i = 0; i < 8; ++i) {
            image.rgba.push(1.0f);
            image.rgba.push(0.5f);
            image.rgba.push(0.25f);
            image.rgba.push(1.0f);
        }
        return image;
    };
    auto environment    = preprocess_environment(constant());
    auto check_constant = [](const HdrImage& image) {
        for (unsigned i = 0; i < count(image.rgba); i += 4) {
            rstd_assert(absolute(at(image.rgba, i) - 1) < 0.0001f);
            rstd_assert(absolute(at(image.rgba, i + 1) - 0.5f) < 0.0001f);
            rstd_assert(absolute(at(image.rgba, i + 2) - 0.25f) < 0.0001f);
        }
    };
    check_constant(environment.diffuse);
    for (const auto& mip : environment.specular) check_constant(mip);
    for (auto value : environment.brdf.rgba)
        rstd_assert(rstd::f32(value).is_finite() && value >= 0 && value < 2);
    vui::gpu::Renderer ui;
    rstd_assert(ui.initialize(true).is_ok());
    auto                 session  = ui.device_session().unwrap();
    auto                 renderer = Renderer::create(session, { 64, 64 }).unwrap();
    vecgltf_test::Output output(session);
    output.resize({ 64, 64 });
    rstd::uint64_t output_version {};
    auto           source = vui::ContentSource::create(19).unwrap();
    vui::UiSession document;
    vui::NodeStyle style;
    style.layout.width = style.layout.height = Some(vui::Scalar(64));
    auto root     = document.document().create<vui::Stack>(style).unwrap();
    auto viewport = document.document().append<vui::SceneViewport>(root, style).unwrap();
    document.document().set_scene_source(viewport, Some(vui::ContentSource(source))).unwrap();
    document.document()
        .set_scene_presentation(viewport, vui::ImageFit::Fill, vui::ImageSampling::Nearest)
        .unwrap();
    document.document().mount(root).unwrap();
    unsigned frame {};
    auto     draw = [&](Arc<Asset> asset,
                        unsigned   size       = 64,
                        bool       background = false,
                        DebugView  debug      = DebugView::BaseColor) {
        rstd_assert(renderer.resize({ size, size }).is_ok());
        output.resize({ size, size });
        auto          prepared = renderer.prepare(asset.clone()).unwrap();
        SceneInstance instance(asset.clone());
        View          camera { look_at({ 0, 0, 2 }, { 0, 0, 0 }),
                               perspective(0.8f, 1, 0.1f, 10),
                               { 0, 0, 2 } };
        auto          image =
            output.render(renderer, prepared, instance, camera, { 1, debug, background }).unwrap();
        source
            .publish(vui::gpu::NativeTextureFrame::wrap(
                         rstd::move(image),
                         { .producer_session = 19, .frame_version = ++output_version })
                         .unwrap())
            .unwrap();
        document.advance({}, { ++frame, {} }, { { 64, 64 }, 1, 1 }).unwrap();
        ui.prepare(document.plan_frame().unwrap()).unwrap();
        ui.submit().unwrap();
        ui.wait(5'000'000'000ull).unwrap();
        return ui.readback().unwrap();
    };
    auto center = [](const vui::gpu::Pixels& image, unsigned channel) {
        return image.rgba[usize((32 * image.width + 32) * 4 + channel)];
    };
    auto          cancelled_asset    = triangle();
    auto          cancelled_prepared = renderer.prepare(cancelled_asset.clone()).unwrap();
    SceneInstance cancelled_scene(cancelled_asset.clone());
    View          cancelled_view { look_at({ 0, 0, 2 }, { 0, 0, 0 }),
                                   perspective(0.8f, 1, 0.1f, 10),
                                   { 0, 0, 2 } };
    {
        auto cancelled = output.queue.Begin().unwrap();
        auto write     = output.pool->acquire().unwrap();
        renderer
            .record(cancelled,
                    write.view(),
                    cancelled_prepared,
                    cancelled_scene,
                    cancelled_view,
                    { 1, DebugView::BaseColor })
            .unwrap();
    }
    auto retried = output
                       .render(renderer,
                               cancelled_prepared,
                               cancelled_scene,
                               cancelled_view,
                               { 1, DebugView::BaseColor })
                       .unwrap();
    source
        .publish(
            vui::gpu::NativeTextureFrame::wrap(
                rstd::move(retried), { .producer_session = 19, .frame_version = ++output_version })
                .unwrap())
        .unwrap();
    document.advance({}, { ++frame, {} }, { { 64, 64 }, 1, 1 }).unwrap();
    ui.prepare(document.plan_frame().unwrap()).unwrap();
    ui.submit().unwrap();
    ui.wait().unwrap();
    near(center(ui.readback().unwrap(), 0), 255);
    auto red = draw(triangle());
    near(center(red, 0), 255);
    near(center(red, 1), 0);
    auto back = draw(triangle({ 1, 0, 0, 1 }, true));
    rstd_assert(center(back, 0) < 60);
    auto double_sided                           = triangle({ 1, 0, 0, 1 }, true);
    at(double_sided->materials, 0).double_sided = true;
    auto visible                                = draw(double_sided.clone());
    near(center(visible, 0), 255);
    auto mirrored                  = triangle();
    at(mirrored->nodes, 0).scale.x = -1;
    auto negative                  = draw(mirrored.clone());
    near(center(negative, 0), 255);

    auto  textured = triangle({ 1, 1, 1, 1 });
    Image image;
    image.width = image.height = 2;
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 2; ++x) {
            image.rgba.push(u8(y ? 0 : 255));
            image.rgba.push(u8(0));
            image.rgba.push(u8(y ? 255 : 0));
            image.rgba.push(u8(255));
        }
    textured->images.push(rstd::move(image));
    Texture texture;
    texture.image   = 0;
    texture.sampler = {
        Wrap::Clamp, Wrap::Mirror, Filter::Nearest, Filter::Nearest, MipFilter::None
    };
    textured->textures.push(rstd::move(texture));
    at(textured->materials, 0).base.texture = 0;
    auto sampled                            = draw(textured.clone());
    near(center(sampled, 0), 255);
    near(center(sampled, 2), 0);
    auto  shared_image = triangle({ 1, 1, 1, 1 });
    Image shared_pixels;
    shared_pixels.width = shared_pixels.height = 2;
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 2; ++x) {
            shared_pixels.rgba.push(u8(y ? 0 : 255));
            shared_pixels.rgba.push(u8(0));
            shared_pixels.rgba.push(u8(y ? 255 : 0));
            shared_pixels.rgba.push(u8(255));
        }
    shared_image->images.push(rstd::move(shared_pixels));
    Texture mirrored_sampler;
    mirrored_sampler.image   = 0;
    mirrored_sampler.sampler = {
        Wrap::Clamp, Wrap::Mirror, Filter::Nearest, Filter::Nearest, MipFilter::None
    };
    Texture clamped_sampler   = mirrored_sampler;
    clamped_sampler.sampler.t = Wrap::Clamp;
    shared_image->textures.push(rstd::move(mirrored_sampler));
    shared_image->textures.push(rstd::move(clamped_sampler));
    at(shared_image->materials, 0).base.texture = 0;
    Material right_material;
    right_material.unlit        = true;
    right_material.base.texture = 1;
    shared_image->materials.push(rstd::move(right_material));
    auto&     left_primitive = at(at(shared_image->meshes, 0).primitives, 0);
    Primitive right_primitive;
    right_primitive.material = 1;
    for (auto& vertex : left_primitive.vertices) {
        Vertex right_vertex     = vertex;
        right_vertex.position.x = vertex.position.x * 0.5f + 0.5f;
        right_primitive.bounds.include(right_vertex.position);
        right_primitive.vertices.push(rstd::move(right_vertex));
        vertex.position.x = vertex.position.x * 0.5f - 0.5f;
    }
    at(shared_image->meshes, 0).primitives.push(rstd::move(right_primitive));
    auto shared_sample = draw(shared_image.clone());
    near(shared_sample.rgba[usize((32 * 64 + 16) * 4)], 255);
    near(shared_sample.rgba[usize((32 * 64 + 48) * 4 + 2)], 255);
    at(textured->textures, 0).sampler.t = Wrap::Clamp;
    auto clamp_sample                   = draw(textured.clone());
    near(center(clamp_sample, 2), 255);
    at(textured->materials, 0).base.offset.y = -1.5f;
    auto transformed                         = draw(textured.clone());
    near(center(transformed, 0), 255);
    for (auto pixel : at(textured->images, 0).rgba) pixel = u8(128);
    auto srgb = draw(textured.clone());
    near(center(srgb, 0), 128);
    near(center(srgb, 1), 128);

    auto material_checks = triangle({ 1, 1, 1, 1 });
    auto solid_texture   = [&](unsigned r, unsigned g, unsigned b) {
        Image image;
        image.width = image.height = 1;
        image.rgba.push(u8(r));
        image.rgba.push(u8(g));
        image.rgba.push(u8(b));
        image.rgba.push(u8(255));
        unsigned index = unsigned(count(material_checks->images));
        material_checks->images.push(rstd::move(image));
        Texture texture;
        texture.image = index;
        material_checks->textures.push(rstd::move(texture));
        return index;
    };
    auto& checked_material            = at(material_checks->materials, 0);
    checked_material.physical.texture = solid_texture(0, 64, 128);
    checked_material.metallic         = 0.5f;
    checked_material.roughness        = 0.8f;
    auto metal = draw(material_checks.clone(), 64, false, DebugView::Metallic);
    near(center(metal, 0), 137);
    auto rough = draw(material_checks.clone(), 64, false, DebugView::Roughness);
    near(center(rough, 0), 124);
    checked_material.emission.texture = solid_texture(128, 128, 128);
    checked_material.emissive         = { 0.5f, 0.5f, 0.5f };
    auto emissive = draw(material_checks.clone(), 64, false, DebugView::Emissive);
    near(center(emissive, 0), 92);
    checked_material.emissive = { 2.5f, 5, 10 };
    auto strong_emissive      = draw(material_checks.clone(), 64, false, DebugView::Emissive);
    near(center(strong_emissive, 0), 194);
    near(center(strong_emissive, 1), 255);
    near(center(strong_emissive, 2), 255);
    checked_material.emissive       = { 0.5f, 0.5f, 0.5f };
    checked_material.normal.texture = solid_texture(128, 255, 128);
    auto normal                     = draw(material_checks.clone(), 64, false, DebugView::Normal);
    near(center(normal, 0), 188);
    near(center(normal, 1), 255);
    near(center(normal, 2), 188);

    auto transparency  = triangle({ 0, 0, 1, 1 });
    auto overlay       = triangle({ 1, 0, 0, 0.5f });
    auto primitive     = rstd::move(at(at(overlay->meshes, 0).primitives, 0));
    primitive.material = 1;
    for (auto& vertex : primitive.vertices) vertex.position.z = 0.2f;
    at(transparency->meshes, 0).primitives.push(rstd::move(primitive));
    auto material  = rstd::move(at(overlay->materials, 0));
    material.alpha = AlphaMode::Blend;
    transparency->materials.push(rstd::move(material));
    auto blend = draw(transparency.clone());
    near(center(blend, 0), 188);
    near(center(blend, 2), 188);
    at(transparency->materials, 1).alpha        = AlphaMode::Mask;
    at(transparency->materials, 1).base_color.w = 0.25f;
    auto mask                                   = draw(transparency.clone());
    near(center(mask, 0), 0);
    near(center(mask, 2), 255);
    auto resized = draw(triangle(), 96);
    near(center(resized, 0), 255);
    auto        morphed = triangle();
    MorphTarget target;
    for (unsigned i = 0; i < 3; ++i) {
        target.positions.push({ 3, 0, 0 });
        target.normals.push({});
        target.tangents.push({});
    }
    at(at(morphed->meshes, 0).primitives, 0).morphs.push(rstd::move(target));
    at(morphed->nodes, 0).weights.push(1.0f);
    auto displaced = draw(morphed.clone());
    rstd_assert(center(displaced, 0) < 60);
    at(at(morphed->nodes, 0).weights, 0) = 0;
    auto restored                        = draw(morphed.clone());
    near(center(restored, 0), 255);
    auto  skinned                     = triangle();
    auto& skinned_primitive           = at(at(skinned->meshes, 0).primitives, 0);
    skinned_primitive.skin_attributes = true;
    for (auto& vertex : skinned_primitive.vertices) vertex.weights = { 1, 0, 0, 0 };
    at(skinned->nodes, 0).skin        = 0;
    at(skinned->nodes, 0).translation = { -3, 0, 0 };
    Node joint;
    joint.translation = { 3, 0, 0 };
    skinned->nodes.push(rstd::move(joint));
    Skin skin;
    skin.joints.push(1);
    skin.inverse_bind.push(Mat4::Identity());
    skinned->skins.push(rstd::move(skin));
    auto shifted_skin = draw(skinned.clone());
    rstd_assert(center(shifted_skin, 0) < 60);
    at(skinned->nodes, 1).translation = { 0, 0, 0 };
    auto restored_skin                = draw(skinned.clone());
    near(center(restored_skin, 0), 255);
    renderer.set_environment(constant()).unwrap();
    rstd_assert(renderer.set_environment(HdrImage {}).is_err());
    auto background = draw(triangle(), 64, true);
    near(background.rgba[usize(0)], 232);
    near(background.rgba[usize(1)], 206);
    near(background.rgba[usize(2)], 165);
    auto          persistent = triangle();
    auto          prepared   = renderer.prepare(persistent.clone()).unwrap();
    SceneInstance instance(persistent.clone());
    OrbitCamera   orbit;
    orbit.fit(instance.bounds());
    output.render(renderer, prepared, instance, orbit.view(1)).unwrap();
    output.drain();
    const auto baseline          = renderer.statistics();
    const auto upload_baseline   = renderer.upload_statistics();
    const auto pipeline_baseline = session.graphics_resources().pipelines.stats().creations;
    for (unsigned i = 0; i < 8; ++i) {
        orbit.rotate(0.05f, 0);
        instance.set_time(float(i));
        auto frame = output.render(renderer, prepared, instance, orbit.view(1)).unwrap();
        output.drain();
        rstd_assert(session.graphics_resources().pipelines.stats().creations == pipeline_baseline);
    }
    const auto upload_stats = renderer.upload_statistics();
    rstd_assert(upload_stats.recorded_copies[0] == upload_baseline.recorded_copies[0]);
    rstd_assert(upload_stats.recorded_copies[1] == upload_baseline.recorded_copies[1]);
    const auto stats = renderer.statistics();
    rstd_assert(stats.recorded_frames == baseline.recorded_frames + 8);
    rstd_assert(stats.prepared_assets == baseline.prepared_assets);
    rstd_assert(stats.asset_buffer_bytes == baseline.asset_buffer_bytes);
    rstd_assert(stats.environment_preparations == baseline.environment_preparations);
    rstd_assert(stats.target_rebuilds == baseline.target_rebuilds);
    auto held =
        output.render(renderer, prepared, instance, orbit.view(1), { 1, DebugView::BaseColor })
            .unwrap();
    auto replacement = renderer.prepare(triangle({ 0, 1, 0, 1 })).unwrap();
    prepared         = rstd::move(replacement);
    renderer.resize({ 80, 48 }).unwrap();
    renderer.close();
    source
        .publish(
            vui::gpu::NativeTextureFrame::wrap(
                rstd::move(held), { .producer_session = 19, .frame_version = ++output_version })
                .unwrap())
        .unwrap();
    document.advance({}, { ++frame, {} }, { { 64, 64 }, 1, 1 }).unwrap();
    ui.prepare(document.plan_frame().unwrap()).unwrap();
    ui.submit().unwrap();
    ui.wait(5'000'000'000ull).unwrap();
    near(center(ui.readback().unwrap(), 0), 255);
    source.close().unwrap();
    document.close().unwrap();
    renderer.close();
    ui.shutdown(5'000'000'000ull).unwrap();
    rstd_assert(session.validation_errors() == 0);
    rstd::io::println("render: color encoding, sampler axes, texture transform, "
                      "faces, alpha, environment, skin/morph, resize and frame leases "
                      "passed");
}
} // namespace vecgltf_test::render
