module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:display;
import vecgltf;
import vecgltf.render;
import vecgltf.display;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::display
{
namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;
auto texture(vk::TextureContext&   queue,
             vrento::TextureFormat format,
             unsigned              width  = 64,
             unsigned              height = 64) -> tex::TextureViewRef {
    tex::TextureDesc desc;
    desc.extent = { width, height };
    desc.format = format;
    desc.usage  = tex::Usage::ColorAttachment | tex::Usage::Sampled | tex::Usage::TransferSource |
                  tex::Usage::TransferDestination;
    return queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
}
void clear(vk::DeviceSession   session,
           tex::Submission&    work,
           tex::TextureViewRef target,
           Vec4                color) {
    work.commands()
        ->UseTexture({ target, tex::Access::TransferDestination, tex::Stages::Transfer })
        .unwrap();
    VkClearColorValue       value { .float32 = { color.x, color.y, color.z, color.w } };
    VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    session.device().handle().Dispatch().vkCmdClearColorImage(
        vk::native_recording(work, session).unwrap(),
        vk::native_texture(target.texture()).unwrap(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &value,
        1,
        &range);
}
auto pixels(vk::DeviceSession session, vk::TextureContext& queue, tex::TextureViewRef target)
    -> Vec<u8> {
    auto size =
        tex::mip_extent(target.texture().description(), target.description().range.first_mip);
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size   = size.width * size.height * 4;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto buffer = session.device()
                      .memory_allocator()
                      .create_buffer(info, vvk::MemoryRequest::Readback())
                      .unwrap();
    auto work   = queue.Begin().unwrap();
    work.commands()
        ->UseTexture({ target, tex::Access::TransferSource, tex::Stages::Transfer })
        .unwrap();
    auto              command = vk::native_recording(work, session).unwrap();
    VkBufferImageCopy copy {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { size.width, size.height, 1 };
    auto& dispatch        = session.device().handle().Dispatch();
    dispatch.vkCmdCopyImageToBuffer(command,
                                    vk::native_texture(target.texture()).unwrap(),
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
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto memory = buffer.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    return Vec<u8>::from(
        slice<u8>::from_raw_parts(static_cast<const rstd::byte*>(mapped.data()), usize(info.size)));
}
unsigned channel(const Vec<u8>& image,
                 unsigned       component,
                 unsigned       x     = 32,
                 unsigned       y     = 32,
                 unsigned       width = 64) {
    return image[usize((y * width + x) * 4 + component)].to_primitive();
}
void near(unsigned actual, unsigned expected) {
    rstd_assert(actual + 2 >= expected && actual <= expected + 2);
}
auto triangle(Vec3 emission, float alpha, float z) -> Arc<Asset> {
    static unsigned generation {};
    auto            asset = Arc<Asset>::make();
    asset->generation     = u64(++generation);
    Material material;
    material.base_color = { 0, 0, 0, alpha };
    material.emissive   = emission;
    material.alpha      = AlphaMode::Blend;
    asset->materials.push(rstd::move(material));
    Primitive primitive;
    Vec3      positions[] { { -1, -1, z }, { 1, -1, z }, { 0, 1, z } };
    for (const auto& position : positions) {
        Vertex vertex;
        vertex.position = position;
        vertex.normal   = { 0, 0, 1 };
        primitive.vertices.push(rstd::move(vertex));
        primitive.bounds.include(position);
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
TEST(Display, LinearComposition) {
    auto             session        = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             queue          = vk::TextureContext::create(session).unwrap();
    auto             back_renderer  = Renderer::create(session, { 64, 64 }).unwrap();
    auto             front_renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto             display        = DisplayRenderer::create(session).unwrap();
    auto             hdr            = texture(queue, vrento::TextureFormat::RGBA16F);
    auto             output         = texture(queue, vrento::TextureFormat::RGBA8);
    tex::TextureDesc depth_desc;
    depth_desc.extent       = { 64, 64 };
    depth_desc.format       = vrento::TextureFormat::D32F;
    depth_desc.usage        = tex::Usage::DepthAttachment;
    auto          depth     = queue
                                  .CreateView(queue.CreateTexture(depth_desc).unwrap(),
                                              { tex::Dimension::D2, { tex::Aspect::Depth } })
                                  .unwrap();
    auto          back      = triangle({ 0, 0, 2 }, 0.5f, -0.5f);
    auto          front     = triangle({ 4, 0, 0 }, 0.5f, 0);
    auto          back_gpu  = back_renderer.prepare(back.clone()).unwrap();
    auto          front_gpu = front_renderer.prepare(front.clone()).unwrap();
    SceneInstance back_scene(back.clone()), front_scene(front.clone());
    View          view { look_at({ 0, 0, 2 }, {}), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    SceneOptions  scene;
    scene.direct_light  = None();
    scene.ibl_intensity = 0;
    RenderTarget target { hdr, Some(tex::TextureViewRef(depth)) };
    back_renderer.prepare_scene_target(target).unwrap();
    front_renderer.prepare_scene_target(target).unwrap();
    display.prepare_target({ output }).unwrap();
    auto work = queue.Begin().unwrap();
    back_renderer
        .record_scene(work,
                      RenderTarget { hdr, Some(tex::TextureViewRef(depth)) },
                      back_gpu,
                      back_scene,
                      view,
                      scene)
        .unwrap();
    front_renderer
        .record_scene(work,
                      RenderTarget { hdr,
                                     Some(tex::TextureViewRef(depth)),
                                     vk::AttachmentLoad::Load,
                                     vk::AttachmentLoad::Load },
                      front_gpu,
                      front_scene,
                      view,
                      scene)
        .unwrap();
    DisplayOptions options;
    options.tone_mapping = ToneMapping::ReinhardMax;
    display.record(work, hdr, { output }, options).unwrap();
    back_renderer.close();
    front_renderer.close();
    display.close();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto image = pixels(session, queue, output);
    near(channel(image, 0), 139);
    near(channel(image, 1), 0);
    near(channel(image, 2), 35);
    near(channel(image, 3), 191);
    rstd_assert(session.validation_errors() == 0);
}
TEST(Display, SrgbExportToUnorm) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto display = DisplayRenderer::create(session).unwrap();
    auto hdr     = texture(queue, vrento::TextureFormat::RGBA16F);
    auto output  = texture(queue, vrento::TextureFormat::RGBA8);
    const bool post_modes[] { false, true };
    const float alphas[] { 0.0f, 0.5f, 1.0f };
    for (bool post : post_modes) {
        DisplayOptions options;
        options.tone_mapping = ToneMapping::Linear;
        options.fxaa = post;
        for (float alpha : alphas) {
            auto work = queue.Begin().unwrap();
            clear(session, work, hdr, { 0.25f * alpha, 0.5f * alpha, 0.75f * alpha, alpha });
            display.record(work, hdr, { output, vk::AttachmentLoad::Clear, DisplayEncoding::Srgb }, options)
                .unwrap();
            queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
            auto image = pixels(session, queue, output);
            near(channel(image, 0), unsigned(137 * alpha));
            near(channel(image, 1), unsigned(188 * alpha));
            near(channel(image, 2), unsigned(225 * alpha));
            near(channel(image, 3), unsigned(255 * alpha));
        }
    }
    auto srgb = texture(queue, vrento::TextureFormat::RGBA8_SRGB);
    rstd_assert(display.prepare_target({ srgb, vk::AttachmentLoad::Clear, DisplayEncoding::Srgb }).is_err());
    rstd_assert(display.prepare_target({ output, vk::AttachmentLoad::Load, DisplayEncoding::Srgb }).is_err());
    rstd_assert(session.validation_errors() == 0);
}
TEST(Display, GradingAndAlpha) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto display = DisplayRenderer::create(session).unwrap();
    auto hdr     = texture(queue, vrento::TextureFormat::RGBA16F);
    auto output  = texture(queue, vrento::TextureFormat::RGBA8);
    auto srgb    = texture(queue, vrento::TextureFormat::RGBA8_SRGB);
    auto draw    = [&](Vec4               color,
                       DisplayOptions     options = {},
                       bool               encoded = false,
                       vk::AttachmentLoad load    = vk::AttachmentLoad::Clear) {
        auto target = encoded ? srgb : output;
        auto work   = queue.Begin().unwrap();
        clear(session, work, hdr, color);
        display.record(work, hdr, { target, load }, options).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        return pixels(session, queue, target);
    };
    DisplayOptions linear;
    linear.tone_mapping = ToneMapping::Linear;
    auto straight       = draw({ 0.125f, 0.25f, 0.375f, 0.5f }, linear);
    near(channel(straight, 0), 32);
    near(channel(straight, 1), 64);
    near(channel(straight, 2), 96);
    near(channel(straight, 3), 128);
    auto encoded = draw({ 0.25f, 0.25f, 0.25f, 1 }, linear, true);
    near(channel(encoded, 0), 137);
    auto zero = draw({ 4, 2, 1, 0 }, linear);
    for (unsigned c = 0; c < 4; ++c) near(channel(zero, c), 0);
    auto compiles      = display.graph_statistics().graph_compiles;
    linear.exposure    = 2;
    linear.grading     = { 0.5f, 0, 0.5f };
    auto     graded    = draw({ 0.25f, 0.5f, 0.75f, 1 }, linear);
    float    luminance = 0.25f * 0.2126f + 0.5f * 0.7152f + 0.75f * 0.0722f;
    unsigned expected  = unsigned(maximum(luminance * 1.5f - 0.25f, 0) * 255 + 0.5f);
    for (unsigned c = 0; c < 3; ++c) near(channel(graded, c), expected);
    rstd_assert(display.graph_statistics().graph_compiles == compiles);
    linear.exposure = 1;
    linear.grading  = {};
    linear.vignette = Some(Vignette { 1, 0.1f, 0.5f });
    auto vignette   = draw({ 0.5f, 0.5f, 0.5f, 0.5f }, linear);
    near(channel(vignette, 0), 128);
    near(channel(vignette, 0, 0, 0), 0);
    near(channel(vignette, 3, 0, 0), 128);
    rstd_assert(display.graph_statistics().graph_compiles == compiles);
    linear.vignette = None();
    auto loaded     = draw({ 0.125f, 0, 0, 0.5f }, linear, false, vk::AttachmentLoad::Load);
    near(channel(loaded, 0), 96);
    near(channel(loaded, 3), 191);
    auto smaller = texture(queue, vrento::TextureFormat::RGBA8, 32, 24);
    auto work    = queue.Begin().unwrap();
    clear(session, work, hdr, { 0.25f, 0.25f, 0.25f, 1 });
    display.record(work, hdr, { smaller }, linear).unwrap();
    rstd_assert(display.graph_statistics().graph_compiles == compiles + 2);
    display.close();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto resized = pixels(session, queue, smaller);
    near(channel(resized, 0, 16, 12, 32), 64);
    rstd_assert(session.validation_errors() == 0);
}
TEST(Display, PreparationAndLifetime) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto display = DisplayRenderer::create(session, vk::PipelinePreparationMode::Deferred).unwrap();
    auto hdr     = texture(queue, vrento::TextureFormat::RGBA16F);
    auto output  = texture(queue, vrento::TextureFormat::RGBA8);
    auto work    = queue.Begin().unwrap();
    auto initial = display.upload_statistics();
    auto rejected = [&](DisplayOptions options) {
        auto result = display.record(work, hdr, { output }, options);
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
        rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
        rstd_assert(output.texture().content_revision() == 0);
        rstd_assert(display.statistics().recorded_frames == 0);
    };
    DisplayOptions invalid;
    invalid.exposure = -1;
    rejected(invalid);
    invalid.exposure = rstd::f32::INFINITY_.to_primitive();
    rejected(invalid);
    invalid                    = {};
    invalid.grading.saturation = -1;
    rejected(invalid);
    invalid                  = {};
    invalid.grading.contrast = -1.5f;
    rejected(invalid);
    invalid          = {};
    invalid.vignette = Some(Vignette { 1, 0.5f, 0.5f });
    rejected(invalid);
    auto wrong = display.record(work, output, { output });
    rstd_assert(wrong.is_err() && wrong.unwrap_err().kind == ErrorKind::InvalidAsset);
    auto pending = display.record(work, hdr, { output });
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
    rstd_assert(output.texture().content_revision() == 0 &&
                display.statistics().recorded_frames == 0);
    for (unsigned i = 0; i < 5; ++i) {
        rstd_assert(display.upload_statistics().queued_copies[i] == initial.queued_copies[i]);
        rstd_assert(display.upload_statistics().recorded_copies[i] == initial.recorded_copies[i]);
    }
    auto start = rstd::time::Instant::now();
    while (display.prepare_target({ output }).unwrap() ==
           vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    clear(session, work, hdr, { 4, 0, 0, 1 });
    DisplayOptions options;
    options.tone_mapping = ToneMapping::ReinhardMax;
    display.record(work, hdr, { output }, options).unwrap();
    display.close();
    auto closed = display.prepare_target({ output });
    rstd_assert(closed.is_err());
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto image = pixels(session, queue, output);
    near(channel(image, 0), 204);
    near(channel(image, 3), 255);
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::display
