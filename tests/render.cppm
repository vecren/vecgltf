module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

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

namespace vecgltf_test::render {
Arc<Asset> triangle(Vec4 color = {1, 0, 0, 1}, bool reverse = false) {
  static unsigned generation{};
  auto asset = Arc<Asset>::make();
  asset->generation = u64(++generation);
  Material material;
  material.base_color = color;
  material.unlit = true;
  asset->materials.push(rstd::move(material));
  Primitive primitive;
  Vec3 positions[]{{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
  for (unsigned i = 0; i < 3; ++i) {
    Vertex v;
    v.position = positions[reverse ? 2 - i : i];
    v.normal = {0, 0, 1};
    v.uv0 = {0.25f, 1.75f};
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

TEST(Render, SharedDepth) {
  auto session =
      vrento::vulkan::DeviceSession::create({.validation = true}).unwrap();
  auto queue = vrento::vulkan::TextureContext::create(session).unwrap();
  auto first = Renderer::create(session, {64, 64}).unwrap();
  auto second = Renderer::create(session, {64, 64}).unwrap();
  auto front = triangle(), back = triangle({0, 0, 1, 1});
  for (auto &vertex : at(at(back->meshes, 0).primitives, 0).vertices)
    vertex.position.z = -0.5f;
  auto front_gpu = first.prepare(front.clone()).unwrap();
  auto back_gpu = second.prepare(back.clone()).unwrap();
  SceneInstance front_scene(front.clone()), back_scene(back.clone());
  View camera{
      look_at({0, 0, 2}, {0, 0, 0}), perspective(0.8f, 1, 0.1f, 10), {0, 0, 2}};
  vrento::texture::TextureDesc color_desc;
  color_desc.extent = {64, 64};
  color_desc.format = vrento::TextureFormat::RGBA8_SRGB;
  color_desc.usage = vrento::texture::Usage::ColorAttachment |
                     vrento::texture::Usage::Sampled |
                     vrento::texture::Usage::TransferSource;
  auto color =
      queue.CreateView(queue.CreateTexture(color_desc).unwrap(), {}).unwrap();
  auto depth_desc = color_desc;
  depth_desc.format = vrento::TextureFormat::D32F;
  depth_desc.usage = vrento::texture::Usage::DepthAttachment;
  auto depth = queue
                   .CreateView(queue.CreateTexture(depth_desc).unwrap(),
                               {vrento::texture::Dimension::D2,
                                {vrento::texture::Aspect::Depth}})
                   .unwrap();
  auto work = queue.Begin().unwrap();
  first
      .record(work,
              RenderTarget{color, Some(vrento::texture::TextureViewRef(depth))},
              front_gpu, front_scene, camera, {1, DebugView::BaseColor})
      .unwrap();
  auto created = session.graphics_resources().pipelines.stats().creations;
  second
      .record(work,
              RenderTarget{color, Some(vrento::texture::TextureViewRef(depth)),
                           vrento::rg::AttachmentLoad::Load,
                           vrento::rg::AttachmentLoad::Load},
              back_gpu, back_scene, camera, {1, DebugView::BaseColor})
      .unwrap();
  rstd_assert(session.graphics_resources().pipelines.stats().creations ==
              created);
  {
    vui::gpu::Renderer ui;
    ui.initialize(session, false).unwrap();
    vui::UiSession document;
    vui::NodeStyle style;
    style.layout.width = style.layout.height = Some(vui::Scalar(16));
    style.background = {0, 1, 0, 1};
    auto node = document.document().create<vui::Rectangle>(style).unwrap();
    document.document().mount(node).unwrap();
    document.advance({}, {1, {}}, {{64, 64}, 1, 1}).unwrap();
    ui.record(work, document.plan_frame().unwrap(), color).unwrap();
    ui.shutdown().unwrap();
  }
  work.commands()
      ->UseTexture({color, vrento::texture::Access::TransferSource,
                    vrento::texture::Stages::Transfer})
      .unwrap();
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = 64 * 64 * 4;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  auto buffer = session.device()
                    .memory_allocator()
                    .create_buffer(buffer_info, vvk::MemoryRequest::Readback())
                    .unwrap();
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {64, 64, 1};
  const auto &dispatch = session.device().handle().Dispatch();
  auto command = vrento::vulkan::native_recording(work, session).unwrap();
  dispatch.vkCmdCopyImageToBuffer(
      command, vrento::vulkan::native_texture(color.texture()).unwrap(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle(), 1, &copy);
  VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host.buffer = buffer.handle();
  host.size = VK_WHOLE_SIZE;
  dispatch.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                &host, 0, nullptr);
  auto done = queue.Submit(rstd::move(work)).unwrap();
  queue.wait(done).unwrap();
  queue.Poll().unwrap();
  auto memory = buffer.allocation();
  auto mapped = memory.map().unwrap();
  memory.invalidate().unwrap();
  auto *pixels = static_cast<unsigned char *>(mapped.data());
  rstd_assert(pixels[(32 * 64 + 32) * 4] == 255 &&
              pixels[(32 * 64 + 32) * 4 + 2] == 0);
  rstd_assert(pixels[(8 * 64 + 8) * 4] == 0 &&
              pixels[(8 * 64 + 8) * 4 + 1] == 255);
  rstd_assert(session.validation_errors() == 0);
}
TEST(Render, Integration) {
  static rstd::log::EnvLogger logger("info"_str);
  rstd::log::set_logger(logger);
  rstd::log::set_max_level(rstd::log::LevelFilter::Info);
  auto constant = [] {
    HdrImage image{4, 2};
    for (unsigned i = 0; i < 8; ++i) {
      image.rgba.push(1.0f);
      image.rgba.push(0.5f);
      image.rgba.push(0.25f);
      image.rgba.push(1.0f);
    }
    return image;
  };
  auto environment = preprocess_environment(constant());
  auto check_constant = [](const HdrImage &image) {
    for (unsigned i = 0; i < count(image.rgba); i += 4) {
      rstd_assert(absolute(at(image.rgba, i) - 1) < 0.0001f);
      rstd_assert(absolute(at(image.rgba, i + 1) - 0.5f) < 0.0001f);
      rstd_assert(absolute(at(image.rgba, i + 2) - 0.25f) < 0.0001f);
    }
  };
  check_constant(environment.diffuse);
  for (const auto &mip : environment.specular)
    check_constant(mip);
  for (auto value : environment.brdf.rgba)
    rstd_assert(rstd::f32(value).is_finite() && value >= 0 && value < 2);
  vui::gpu::Renderer ui;
  rstd_assert(ui.initialize(true).is_ok());
  auto session = ui.device_session().unwrap();
  auto renderer = Renderer::create(session, {64, 64}).unwrap();
  vecgltf_test::Output output(session);
  output.resize({64, 64});
  rstd::uint64_t output_version{};
  auto source = vui::ContentSource::create(19).unwrap();
  vui::UiSession document;
  vui::NodeStyle style;
  style.layout.width = style.layout.height = Some(vui::Scalar(64));
  auto root = document.document().create<vui::Stack>(style).unwrap();
  auto viewport =
      document.document().append<vui::SceneViewport>(root, style).unwrap();
  document.document()
      .set_scene_source(viewport, Some(vui::ContentSource(source)))
      .unwrap();
  document.document()
      .set_scene_presentation(viewport, vui::ImageFit::Fill,
                              vui::ImageSampling::Nearest)
      .unwrap();
  document.document().mount(root).unwrap();
  unsigned frame{};
  auto draw = [&](Arc<Asset> asset, unsigned size = 64, bool background = false,
                  DebugView debug = DebugView::BaseColor) {
    rstd_assert(renderer.resize({size, size}).is_ok());
    output.resize({size, size});
    auto prepared = renderer.prepare(asset.clone()).unwrap();
    SceneInstance instance(asset.clone());
    View camera{look_at({0, 0, 2}, {0, 0, 0}),
                perspective(0.8f, 1, 0.1f, 10),
                {0, 0, 2}};
    auto image = output
                     .render(renderer, prepared, instance, camera,
                             {1, debug, background})
                     .unwrap();
    source
        .publish(vui::gpu::NativeTextureFrame::wrap(
                     rstd::move(image), {.producer_session = 19,
                                         .frame_version = ++output_version})
                     .unwrap())
        .unwrap();
    document.advance({}, {++frame, {}}, {{64, 64}, 1, 1}).unwrap();
    ui.prepare(document.plan_frame().unwrap()).unwrap();
    ui.submit().unwrap();
    ui.wait(5'000'000'000ull).unwrap();
    return ui.readback().unwrap();
  };
  auto center = [](const vui::gpu::Pixels &image, unsigned channel) {
    return image.rgba[usize((32 * image.width + 32) * 4 + channel)];
  };
  auto cancelled_asset = triangle();
  auto cancelled_prepared = renderer.prepare(cancelled_asset.clone()).unwrap();
  SceneInstance cancelled_scene(cancelled_asset.clone());
  View cancelled_view{
      look_at({0, 0, 2}, {0, 0, 0}), perspective(0.8f, 1, 0.1f, 10), {0, 0, 2}};
  {
    auto cancelled = output.queue.Begin().unwrap();
    auto write = output.pool->acquire().unwrap();
    renderer
        .record(cancelled, write.view(), cancelled_prepared, cancelled_scene,
                cancelled_view, {1, DebugView::BaseColor})
        .unwrap();
  }
  auto retried = output
                     .render(renderer, cancelled_prepared, cancelled_scene,
                             cancelled_view, {1, DebugView::BaseColor})
                     .unwrap();
  source
      .publish(vui::gpu::NativeTextureFrame::wrap(
                   rstd::move(retried),
                   {.producer_session = 19, .frame_version = ++output_version})
                   .unwrap())
      .unwrap();
  document.advance({}, {++frame, {}}, {{64, 64}, 1, 1}).unwrap();
  ui.prepare(document.plan_frame().unwrap()).unwrap();
  ui.submit().unwrap();
  ui.wait().unwrap();
  near(center(ui.readback().unwrap(), 0), 255);
  auto red = draw(triangle());
  near(center(red, 0), 255);
  near(center(red, 1), 0);
  auto back = draw(triangle({1, 0, 0, 1}, true));
  rstd_assert(center(back, 0) < 60);
  auto double_sided = triangle({1, 0, 0, 1}, true);
  at(double_sided->materials, 0).double_sided = true;
  auto visible = draw(double_sided.clone());
  near(center(visible, 0), 255);
  auto mirrored = triangle();
  at(mirrored->nodes, 0).scale.x = -1;
  auto negative = draw(mirrored.clone());
  near(center(negative, 0), 255);

  auto textured = triangle({1, 1, 1, 1});
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
  texture.image = 0;
  texture.sampler = {Wrap::Clamp, Wrap::Mirror, Filter::Nearest,
                     Filter::Nearest, MipFilter::None};
  textured->textures.push(rstd::move(texture));
  at(textured->materials, 0).base.texture = 0;
  auto sampled = draw(textured.clone());
  near(center(sampled, 0), 255);
  near(center(sampled, 2), 0);
  auto shared_image = triangle({1, 1, 1, 1});
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
  mirrored_sampler.image = 0;
  mirrored_sampler.sampler = {Wrap::Clamp, Wrap::Mirror, Filter::Nearest,
                              Filter::Nearest, MipFilter::None};
  Texture clamped_sampler = mirrored_sampler;
  clamped_sampler.sampler.t = Wrap::Clamp;
  shared_image->textures.push(rstd::move(mirrored_sampler));
  shared_image->textures.push(rstd::move(clamped_sampler));
  at(shared_image->materials, 0).base.texture = 0;
  Material right_material;
  right_material.unlit = true;
  right_material.base.texture = 1;
  shared_image->materials.push(rstd::move(right_material));
  auto &left_primitive = at(at(shared_image->meshes, 0).primitives, 0);
  Primitive right_primitive;
  right_primitive.material = 1;
  for (auto &vertex : left_primitive.vertices) {
    Vertex right_vertex = vertex;
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
  auto clamp_sample = draw(textured.clone());
  near(center(clamp_sample, 2), 255);
  at(textured->materials, 0).base.offset.y = -1.5f;
  auto transformed = draw(textured.clone());
  near(center(transformed, 0), 255);
  for (auto pixel : at(textured->images, 0).rgba)
    pixel = u8(128);
  auto srgb = draw(textured.clone());
  near(center(srgb, 0), 128);
  near(center(srgb, 1), 128);

  auto material_checks = triangle({1, 1, 1, 1});
  auto solid_texture = [&](unsigned r, unsigned g, unsigned b) {
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
  auto &checked_material = at(material_checks->materials, 0);
  checked_material.physical.texture = solid_texture(0, 64, 128);
  checked_material.metallic = 0.5f;
  checked_material.roughness = 0.8f;
  auto metal = draw(material_checks.clone(), 64, false, DebugView::Metallic);
  near(center(metal, 0), 137);
  auto rough = draw(material_checks.clone(), 64, false, DebugView::Roughness);
  near(center(rough, 0), 124);
  checked_material.emission.texture = solid_texture(128, 128, 128);
  checked_material.emissive = {0.5f, 0.5f, 0.5f};
  auto emissive = draw(material_checks.clone(), 64, false, DebugView::Emissive);
  near(center(emissive, 0), 92);
  checked_material.normal.texture = solid_texture(128, 255, 128);
  auto normal = draw(material_checks.clone(), 64, false, DebugView::Normal);
  near(center(normal, 0), 188);
  near(center(normal, 1), 255);
  near(center(normal, 2), 188);

  auto transparency = triangle({0, 0, 1, 1});
  auto overlay = triangle({1, 0, 0, 0.5f});
  auto primitive = rstd::move(at(at(overlay->meshes, 0).primitives, 0));
  primitive.material = 1;
  for (auto &vertex : primitive.vertices)
    vertex.position.z = 0.2f;
  at(transparency->meshes, 0).primitives.push(rstd::move(primitive));
  auto material = rstd::move(at(overlay->materials, 0));
  material.alpha = AlphaMode::Blend;
  transparency->materials.push(rstd::move(material));
  auto blend = draw(transparency.clone());
  near(center(blend, 0), 188);
  near(center(blend, 2), 188);
  at(transparency->materials, 1).alpha = AlphaMode::Mask;
  at(transparency->materials, 1).base_color.w = 0.25f;
  auto mask = draw(transparency.clone());
  near(center(mask, 0), 0);
  near(center(mask, 2), 255);
  auto resized = draw(triangle(), 96);
  near(center(resized, 0), 255);
  auto morphed = triangle();
  MorphTarget target;
  for (unsigned i = 0; i < 3; ++i) {
    target.positions.push({3, 0, 0});
    target.normals.push({});
    target.tangents.push({});
  }
  at(at(morphed->meshes, 0).primitives, 0).morphs.push(rstd::move(target));
  at(morphed->nodes, 0).weights.push(1.0f);
  auto displaced = draw(morphed.clone());
  rstd_assert(center(displaced, 0) < 60);
  at(at(morphed->nodes, 0).weights, 0) = 0;
  auto restored = draw(morphed.clone());
  near(center(restored, 0), 255);
  auto skinned = triangle();
  auto &skinned_primitive = at(at(skinned->meshes, 0).primitives, 0);
  skinned_primitive.skin_attributes = true;
  for (auto &vertex : skinned_primitive.vertices)
    vertex.weights = {1, 0, 0, 0};
  at(skinned->nodes, 0).skin = 0;
  at(skinned->nodes, 0).translation = {-3, 0, 0};
  Node joint;
  joint.translation = {3, 0, 0};
  skinned->nodes.push(rstd::move(joint));
  Skin skin;
  skin.joints.push(1);
  skin.inverse_bind.push(Mat4::Identity());
  skinned->skins.push(rstd::move(skin));
  auto shifted_skin = draw(skinned.clone());
  rstd_assert(center(shifted_skin, 0) < 60);
  at(skinned->nodes, 1).translation = {0, 0, 0};
  auto restored_skin = draw(skinned.clone());
  near(center(restored_skin, 0), 255);
  renderer.set_environment(constant()).unwrap();
  rstd_assert(renderer.set_environment(HdrImage{}).is_err());
  auto background = draw(triangle(), 64, true);
  near(background.rgba[usize(0)], 232);
  near(background.rgba[usize(1)], 206);
  near(background.rgba[usize(2)], 165);
  auto persistent = triangle();
  auto prepared = renderer.prepare(persistent.clone()).unwrap();
  SceneInstance instance(persistent.clone());
  OrbitCamera orbit;
  orbit.fit(instance.bounds());
  const auto baseline = renderer.statistics();
  for (unsigned i = 0; i < 8; ++i) {
    orbit.rotate(0.05f, 0);
    instance.set_time(float(i));
    auto frame =
        output.render(renderer, prepared, instance, orbit.view(1)).unwrap();
    output.drain();
  }
  const auto stats = renderer.statistics();
  rstd_assert(stats.recorded_frames == baseline.recorded_frames + 8);
  rstd_assert(stats.prepared_assets == baseline.prepared_assets);
  rstd_assert(stats.asset_buffer_bytes == baseline.asset_buffer_bytes);
  rstd_assert(stats.environment_preparations ==
              baseline.environment_preparations);
  rstd_assert(stats.target_rebuilds == baseline.target_rebuilds);
  auto held = output
                  .render(renderer, prepared, instance, orbit.view(1),
                          {1, DebugView::BaseColor})
                  .unwrap();
  auto replacement = renderer.prepare(triangle({0, 1, 0, 1})).unwrap();
  prepared = rstd::move(replacement);
  renderer.resize({80, 48}).unwrap();
  renderer.close();
  source
      .publish(vui::gpu::NativeTextureFrame::wrap(
                   rstd::move(held),
                   {.producer_session = 19, .frame_version = ++output_version})
                   .unwrap())
      .unwrap();
  document.advance({}, {++frame, {}}, {{64, 64}, 1, 1}).unwrap();
  ui.prepare(document.plan_frame().unwrap()).unwrap();
  ui.submit().unwrap();
  ui.wait(5'000'000'000ull).unwrap();
  near(center(ui.readback().unwrap(), 0), 255);
  source.close().unwrap();
  document.close().unwrap();
  renderer.close();
  ui.shutdown(5'000'000'000ull).unwrap();
  rstd_assert(session.validation_errors() == 0);
  rstd::io::println(
      "render: color encoding, sampler axes, texture transform, "
      "faces, alpha, environment, skin/morph, resize and frame leases passed");
}
} // namespace vecgltf_test::render
