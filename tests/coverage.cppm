module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:coverage;
import vecgltf;
import vecgltf.render;
import vecgltf.environment;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::coverage
{
namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;

auto texture(vk::TextureContext&   queue,
             unsigned              width  = 64,
             unsigned              height = 64,
             vrento::TextureFormat format = vrento::TextureFormat::RGBA16F,
             tex::Usage            usage  = tex::Usage::ColorAttachment | tex::Usage::Sampled |
                                            tex::Usage::TransferSource) -> tex::TextureViewRef {
    tex::TextureDesc desc;
    desc.extent = { width, height };
    desc.format = format;
    desc.usage  = usage;
    return queue.CreateView(queue.CreateTexture(desc).unwrap(), {}).unwrap();
}
auto pixels(vk::DeviceSession session, vk::TextureContext& queue, tex::TextureViewRef target)
    -> Vec<u8> {
    auto size =
        tex::mip_extent(target.texture().description(), target.description().range.first_mip);
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size   = size.width * size.height * 8;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto buffer = session.device()
                      .memory_allocator()
                      .create_buffer(info, vvk::MemoryRequest::Readback())
                      .unwrap();
    auto work   = queue.Begin().unwrap();
    work.commands()
        ->UseTexture({ target, tex::Access::TransferSource, tex::Stages::Transfer })
        .unwrap();
    auto&             dispatch = session.device().handle().Dispatch();
    auto              command  = vk::native_recording(work, session).unwrap();
    VkBufferImageCopy copy {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { size.width, size.height, 1 };
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
float channel(const Vec<u8>& image,
              unsigned       component,
              unsigned       x     = 32,
              unsigned       y     = 32,
              unsigned       width = 64) {
    auto     offset = usize(((y * width + x) * 4 + component) * 2);
    unsigned bits   = image[offset].to_primitive() | (image[offset + usize(1)].to_primitive() << 8);
    unsigned exponent = (bits >> 10) & 31, mantissa = bits & 1023;
    rstd_assert(exponent != 31);
    float value = exponent ? 1 + float(mantissa) / 1024 : float(mantissa) / 1024;
    int   power = exponent ? int(exponent) - 15 : -14;
    for (; power < 0; ++power) value *= 0.5f;
    for (; power > 0; --power) value *= 2;
    return bits & 32768 ? -value : value;
}
void near(float actual, float expected) {
    rstd_assert(absolute(actual - expected) < 0.002f);
}
auto asset() -> Arc<Asset> {
    static unsigned generation {};
    auto            value = Arc<Asset>::make();
    value->generation     = u64(++generation);
    value->scenes.push(Scene {});
    return value;
}
void triangle(Asset&    value,
              AlphaMode mode,
              float     alpha,
              float     z            = 0,
              bool      reverse      = false,
              bool      double_sided = false) {
    Material material;
    material.base_color   = { 1, 0.5f, 0.25f, alpha };
    material.alpha        = mode;
    material.unlit        = true;
    material.double_sided = double_sided;
    Primitive primitive;
    primitive.material = unsigned(count(value.materials));
    value.materials.push(rstd::move(material));
    Vec3 positions[] { { -1, -1, z }, { 1, -1, z }, { 0, 1, z } };
    for (unsigned i = 0; i < 3; ++i) {
        Vertex vertex;
        vertex.position = positions[reverse ? 2 - i : i];
        vertex.normal   = { 0, 0, 1 };
        vertex.uv0      = { 0.5f, 0.5f };
        primitive.bounds.include(vertex.position);
        primitive.vertices.push(rstd::move(vertex));
    }
    Mesh mesh;
    mesh.primitives.push(rstd::move(primitive));
    Node node;
    node.mesh = unsigned(count(value.meshes));
    value.meshes.push(rstd::move(mesh));
    at(value.scenes, 0).roots.push(unsigned(count(value.nodes)));
    value.nodes.push(rstd::move(node));
}
auto view(float offset = 0, float aspect = 1) -> View {
    return { look_at({ offset, 0, 2 }, { offset, 0, 0 }),
             perspective(0.8f, aspect, 0.1f, 10),
             { offset, 0, 2 } };
}
auto options(bool background = false) -> SceneOptions {
    SceneOptions result;
    result.background    = background;
    result.direct_light  = None();
    result.ibl_intensity = 0;
    return result;
}
void bright_background(Renderer& renderer) {
    auto image = []() {
        HdrImage result { 1, 1 };
        float    values[] { 4, 4, 4, 1 };
        for (float value : values) result.rgba.push(float(value));
        return result;
    };
    EnvironmentImages environment;
    environment.background = image();
    environment.diffuse    = image();
    environment.brdf       = image();
    environment.specular.push(image());
    renderer.set_environment(rstd::move(environment)).unwrap();
}
auto destination(tex::TextureViewRef color, tex::TextureViewRef coverage) -> RenderTarget {
    RenderTarget result { rstd::move(color) };
    result.coverage = Some(rstd::move(coverage));
    return result;
}

TEST(Coverage, GeometryAlphaAndBackground) {
    auto session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vk::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    bright_background(renderer);
    auto hdr = texture(queue), coverage = texture(queue);
    struct Case {
        AlphaMode mode;
        float     alpha;
        bool      reverse, double_sided;
        float     expected;
    };
    Case                   cases[] { { AlphaMode::Opaque, 0.2f, false, false, 1 },
                                     { AlphaMode::Mask, 0.2f, false, false, 0 },
                                     { AlphaMode::Mask, 0.75f, false, false, 1 },
                                     { AlphaMode::Blend, 0.25f, false, false, 0.25f },
                                     { AlphaMode::Blend, 1, false, false, 1 },
                                     { AlphaMode::Blend, 0, false, false, 0 },
                                     { AlphaMode::Blend, 0.25f, true, false, 0 },
                                     { AlphaMode::Blend, 0.25f, true, true, 0.25f } };
    Option<rstd::uint64_t> compiles;
    for (const auto& test : cases) {
        auto source = asset();
        triangle(*source, test.mode, test.alpha, 0, test.reverse, test.double_sided);
        auto          prepared = renderer.prepare(source.clone()).unwrap();
        SceneInstance scene(source.clone());
        auto          work = queue.Begin().unwrap();
        renderer
            .record_scene(work, destination(hdr, coverage), prepared, scene, view(), options(true))
            .unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        auto values = pixels(session, queue, coverage);
        for (unsigned c = 0; c < 3; ++c) near(channel(values, c), 0);
        near(channel(values, 3), test.expected);
        near(channel(values, 3, 0, 0), 0);
        auto color = pixels(session, queue, hdr);
        near(channel(color, 0, 0, 0), 4);
        near(channel(color, 3, 0, 0), 1);
        if (compiles) {
            rstd_assert(renderer.graph_statistics().graph_compiles == *compiles);
        } else {
            compiles = Some(renderer.graph_statistics().graph_compiles);
        }
    }
    auto source = asset();
    triangle(*source, AlphaMode::Blend, 0.5f);
    Image image;
    image.width = image.height = 1;
    unsigned texel[] { 255, 255, 255, 128 };
    for (unsigned value : texel) image.rgba.push(u8(value));
    source->images.push(rstd::move(image));
    Texture sampler;
    sampler.image = 0;
    source->textures.push(rstd::move(sampler));
    at(source->materials, 0).base.texture = 0;
    auto          prepared                = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          work = queue.Begin().unwrap();
    renderer.record_scene(work, destination(hdr, coverage), prepared, scene, view(), options())
        .unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    near(channel(pixels(session, queue, coverage), 3), 0.5f * 128 / 255);
    auto with_coverage = pixels(session, queue, hdr);
    auto without       = queue.Begin().unwrap();
    renderer.record_scene(without, hdr, prepared, scene, view(), options()).unwrap();
    queue.wait(queue.Submit(rstd::move(without)).unwrap()).unwrap();
    auto without_coverage = pixels(session, queue, hdr);
    rstd_assert(without_coverage.len() == with_coverage.len());
    for (usize i {}; i < with_coverage.len(); ++i)
        rstd_assert(without_coverage[i].get() == with_coverage[i].get());
    rstd_assert(session.validation_errors() == 0);
}

TEST(Coverage, SharedDepthAndBlend) {
    auto             session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto             queue    = vk::TextureContext::create(session).unwrap();
    auto             occluder = Renderer::create(session, { 64, 64 }).unwrap();
    auto             subject  = Renderer::create(session, { 64, 64 }).unwrap();
    auto             hdr = texture(queue), coverage = texture(queue);
    tex::TextureDesc depth_desc;
    depth_desc.extent = { 64, 64 };
    depth_desc.format = vrento::TextureFormat::D32F;
    depth_desc.usage  = tex::Usage::DepthAttachment;
    auto depth        = queue
                            .CreateView(queue.CreateTexture(depth_desc).unwrap(),
                                        { tex::Dimension::D2, { tex::Aspect::Depth } })
                            .unwrap();
    auto opaque       = asset();
    triangle(*opaque, AlphaMode::Opaque, 1, 0.4f);
    auto blends = asset();
    triangle(*blends, AlphaMode::Blend, 0.5f, 0);
    triangle(*blends, AlphaMode::Blend, 0.5f, 0.8f);
    auto          opaque_gpu = occluder.prepare(opaque.clone()).unwrap();
    auto          blend_gpu  = subject.prepare(blends.clone()).unwrap();
    SceneInstance opaque_scene(opaque.clone()), blend_scene(blends.clone());
    DepthEncoding encodings[] { DepthEncoding::Forward, DepthEncoding::Reversed };
    for (auto encoding : encodings) {
        RenderTarget back { hdr, Some(tex::TextureViewRef(depth)) };
        back.depth_encoding = Some(encoding);
        auto front          = destination(hdr, coverage);
        front.depth         = Some(tex::TextureViewRef(depth));
        front.color_load = front.depth_load = vk::AttachmentLoad::Load;
        front.depth_encoding                = Some(encoding);
        auto work                           = queue.Begin().unwrap();
        occluder.record_scene(work, rstd::move(back), opaque_gpu, opaque_scene, view(), options())
            .unwrap();
        subject.record_scene(work, rstd::move(front), blend_gpu, blend_scene, view(), options())
            .unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        near(channel(pixels(session, queue, coverage), 3), 0.5f);
    }
    auto work = queue.Begin().unwrap();
    subject
        .record_scene(work, destination(hdr, coverage), blend_gpu, blend_scene, view(), options())
        .unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    near(channel(pixels(session, queue, coverage), 3), 0.75f);
    rstd_assert(session.validation_errors() == 0);
}

TEST(Coverage, PreparationAndLifetime) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto renderer =
        Renderer::create(session, { 64, 64 }, vk::PipelinePreparationMode::Deferred).unwrap();
    auto source = asset();
    triangle(*source, AlphaMode::Blend, 0.5f);
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          hdr = texture(queue), coverage = texture(queue);
    auto          target    = destination(hdr, coverage);
    auto          work      = queue.Begin().unwrap();
    auto          initial   = renderer.upload_statistics();
    auto          unchanged = [&]() {
        rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
        rstd_assert(hdr.texture().content_revision() == 0 &&
                    coverage.texture().content_revision() == 0);
        rstd_assert(renderer.statistics().recorded_frames == 0);
        auto current = renderer.upload_statistics();
        for (unsigned i = 0; i < 5; ++i) {
            rstd_assert(current.queued_copies[i] == initial.queued_copies[i]);
            rstd_assert(current.recorded_copies[i] == initial.recorded_copies[i]);
        }
    };
    auto rejected = [&](tex::TextureViewRef invalid) {
        auto result = renderer.record_scene(
            work, destination(hdr, invalid), prepared, scene, view(), options());
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
        unchanged();
    };
    rejected({});
    rejected(hdr);
    rejected(texture(queue, 32, 64));
    rejected(texture(queue, 64, 64, vrento::TextureFormat::RGBA8));
    rejected(texture(queue, 64, 64, vrento::TextureFormat::RGBA16F, tex::Usage::ColorAttachment));
    rejected(texture(queue, 64, 64, vrento::TextureFormat::RGBA16F, tex::Usage::Sampled));
    auto unsampled =
        texture(queue, 64, 64, vrento::TextureFormat::RGBA16F, tex::Usage::ColorAttachment);
    auto invalid_color = renderer.record_scene(
        work, destination(unsampled, coverage), prepared, scene, view(), options());
    rstd_assert(invalid_color.is_err() &&
                invalid_color.unwrap_err().kind == ErrorKind::InvalidAsset);
    unchanged();
    auto foreign_session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto foreign_queue   = vk::TextureContext::create(foreign_session).unwrap();
    rejected(texture(foreign_queue));
    auto foreign_work = foreign_queue.Begin().unwrap();
    auto wrong_queue  = renderer.record_scene(
        foreign_work, destination(hdr, coverage), prepared, scene, view(), options());
    rstd_assert(wrong_queue.is_err());
    rstd_assert(foreign_work.receipt().status() == tex::SubmissionStatus::Recording);
    unchanged();
    auto ldr    = texture(queue, 64, 64, vrento::TextureFormat::RGBA8);
    auto legacy = renderer.record(work, destination(ldr, coverage), prepared, scene, view());
    rstd_assert(legacy.is_err() && legacy.unwrap_err().kind == ErrorKind::InvalidAsset);
    unchanged();
    auto pending =
        renderer.record_scene(work, destination(hdr, coverage), prepared, scene, view(), options());
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    unchanged();
    auto start = rstd::time::Instant::now();
    while (renderer.prepare_scene_target(target).unwrap() ==
           vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    renderer.record_scene(work, destination(hdr, coverage), prepared, scene, view(), options())
        .unwrap();
    auto first       = queue.Submit(rstd::move(work)).unwrap();
    auto compiles    = renderer.graph_statistics().graph_compiles;
    auto smaller_hdr = texture(queue, 32, 24), smaller_coverage = texture(queue, 32, 24);
    auto second_work = queue.Begin().unwrap();
    renderer
        .record_scene(second_work,
                      destination(smaller_hdr, smaller_coverage),
                      prepared,
                      scene,
                      view(10, 32.0f / 24),
                      options())
        .unwrap();
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles + 1);
    renderer.close();
    auto second = queue.Submit(rstd::move(second_work)).unwrap();
    queue.wait(first).unwrap();
    queue.wait(second).unwrap();
    near(channel(pixels(session, queue, coverage), 3), 0.5f);
    near(channel(pixels(session, queue, smaller_coverage), 3, 16, 12, 32), 0);
    rstd_assert(session.validation_errors() == 0 && foreign_session.validation_errors() == 0);
}

TEST(Coverage, WriteLease) {
    auto session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vk::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto source   = asset();
    triangle(*source, AlphaMode::Blend, 0.5f);
    auto             prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance    scene(source.clone());
    auto             hdr = texture(queue);
    tex::TextureDesc desc;
    desc.extent = { 64, 64 };
    desc.format = vrento::TextureFormat::RGBA16F;
    desc.usage  = tex::Usage::ColorAttachment | tex::Usage::Sampled | tex::Usage::TransferSource;
    auto pool =
        tex::TexturePool::create(dyn<tex::TextureDevice>::from_ref(queue).as_mut_ref(), desc, 1)
            .unwrap();
    auto write = pool.acquire().unwrap();
    auto work  = queue.Begin().unwrap();
    renderer.record_scene(work, destination(hdr, write.view()), prepared, scene, view(), options())
        .unwrap();
    auto submitted = queue.Submit(rstd::move(work)).unwrap();
    auto read      = pool.publish(rstd::move(write), submitted).unwrap();
    queue.wait(submitted).unwrap();
    auto next   = queue.Begin().unwrap();
    auto frames = renderer.statistics().recorded_frames;
    auto result =
        renderer.record_scene(next, destination(hdr, read), prepared, scene, view(), options());
    rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
    rstd_assert(next.receipt().status() == tex::SubmissionStatus::Recording);
    rstd_assert(renderer.statistics().recorded_frames == frames);
    near(channel(pixels(session, queue, read), 3), 0.5f);
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::coverage
