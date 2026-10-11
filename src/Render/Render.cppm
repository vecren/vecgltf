module;
#include <vulkan/vulkan_core.h>

export module vecgltf.render;
export import vecgltf.scene;
export import vecgltf.load;
export import vrento;
import vecgltf.shaders;
import vecgltf.textures;
import vecgltf.environment;

namespace vecgltf {
namespace vk = vrento::vulkan;
namespace res = vrento::resource;
namespace registry = vrento::resource_registry;
using namespace rstd::literals;

auto render_error(ref<str> location, ref<str> message,
                  ErrorKind kind = ErrorKind::Device) -> Error {
  return {kind, String(), String::make(location), String::make(message)};
}
auto texture_error(vrento::texture::Error error) -> Error {
  return render_error(
      "texture"_str,
      rstd::format("kind={} native={}", unsigned(error.kind), error.native)
          .as_str(),
      error.kind == vrento::texture::ErrorKind::Busy ? ErrorKind::Busy
                                                     : ErrorKind::Device);
}
struct RenderCore {
  vk::DeviceSession session;
  vk::BufferManager buffers;
  Option<vk::TextureContext> image_device;
  registry::DescriptorLayoutRegistry descriptor_layouts;
  registry::DescriptorSystem descriptors;
  res::DescriptorLayoutHandle frame_layout, material_layout, storage_layout;
  Option<vk::BufferAllocation> frame_buffer, zero_buffer;
  vvk::RenderPass pass;
  vvk::PipelineLayout layout;
  vk::PipelineParameters pipeline;
  vk::PipelineParameters transparent, background;
  bool recording{};
  explicit RenderCore(vk::DeviceSession device)
      : session(rstd::move(device)), buffers(session.device()),
        image_device(None()) {}
};
struct PreparedPrimitive {
  vk::BufferAllocation vertices;
  Option<vk::BufferAllocation> indices;
  unsigned vertex_count{}, index_count{};
  Option<vk::BufferAllocation> morphs;
  Option<registry::PreparedDescriptorBinding> morph_binding;
};
struct PreparedMesh {
  Vec<PreparedPrimitive> primitives;
};
struct PreparedMaterial {
  vk::BufferAllocation parameters;
  registry::PreparedDescriptorBinding descriptor;
};
struct PreparedTexture {
  unsigned texture{};
  bool srgb{};
  vrento::texture::TextureViewRef view;
  vrento::texture::SamplerRef sampler;
};
struct PreparedImageEntry {
  unsigned image{};
  bool srgb{};
  Arc<vrento::texture::PreparedImage> source;
};
struct MaterialParameters {
  Vec4 base, emissive, factors, flags;
  Vec4 offset_scale[5], rotation_uv[5];
};
struct FrameParameters {
  Vec4 eye, options;
  Mat4 inverse_view_projection;
};
struct PreparedEnvironment {
  Arc<RenderCore> core;
  Vec<Arc<vrento::texture::PreparedImage>> textures;
  Option<registry::PreparedDescriptorBinding> descriptor;
  explicit PreparedEnvironment(Arc<RenderCore> owner)
      : core(rstd::move(owner)) {}
};
struct PreparedData {
  Arc<RenderCore> core;
  Arc<Asset> asset;
  Vec<PreparedMesh> meshes;
  Vec<PreparedTexture> textures;
  Vec<PreparedImageEntry> images;
  Vec<PreparedMaterial> materials;
  PreparedData(Arc<RenderCore> owner, Arc<Asset> source)
      : core(rstd::move(owner)), asset(rstd::move(source)) {}
};
struct RenderInputs {
  Arc<PreparedData> prepared;
  vrento::texture::TextureViewRef depth;
  vrento::texture::SubmissionReceipt receipt;
  Option<Arc<PreparedEnvironment>> environment;
  Option<vk::BufferUploadBatchLease> uploads;
  Option<vk::BufferAllocation> poses;
  Option<registry::PreparedDescriptorBinding> pose_binding;
  vvk::Framebuffer framebuffer;
  RenderInputs(Arc<PreparedData> source,
               vrento::texture::TextureViewRef depth_view,
               vrento::texture::SubmissionReceipt submission)
      : prepared(rstd::move(source)), depth(rstd::move(depth_view)),
        receipt(rstd::move(submission)) {
    prepared->core->recording = true;
  }
  ~RenderInputs() {
    if (uploads && !receipt.submitted() && !prepared->core->session.lost()) {
      if (!prepared->core->buffers.RestoreUploads(uploads.take().unwrap()))
        rstd::panic("cannot restore cancelled model uploads");
    }
    prepared->core->recording = false;
  }
};
struct DrawConstants {
  Mat4 mvp;
  Vec4 model[3];
  unsigned skin_offset{}, weight_offset{}, morph_count{}, vertex_count{};
};
static_assert(sizeof(DrawConstants) == 128);

template <rstd::size_t N>
auto shader(const unsigned (&code)[N], vrento::ShaderType stage)
    -> vk::Uni_ShaderSpv {
  auto result = Box<vk::ShaderSpv>::make();
  result->stage = stage;
  for (auto word : code)
    result->spirv.push(unsigned(word));
  return result;
}

bool initialize(RenderCore &core) {
  if (!core.buffers.init())
    return false;
  auto image_device = vk::TextureContext::create(core.session);
  if (image_device.is_err())
    return false;
  core.image_device = Some(image_device.unwrap());
  const auto &device = core.session.device().handle();
  VkAttachmentDescription attachments[2]{};
  attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
  attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].initialLayout = attachments[0].finalLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[1].format = VK_FORMAT_D32_SFLOAT;
  attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  for (auto &attachment : attachments) {
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth{1,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color;
  subpass.pDepthStencilAttachment = &depth;
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass.attachmentCount = 2;
  pass.pAttachments = attachments;
  pass.subpassCount = 1;
  pass.pSubpasses = &subpass;
  pass.dependencyCount = 1;
  pass.pDependencies = &dependency;
  if (core.session.observe_result(device.CreateRenderPass(pass, core.pass)) !=
      VK_SUCCESS)
    return false;
  vk::DescriptorSetInfo frame_info, material_info;
  frame_info.bindings.push({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  for (unsigned i = 0; i < 5; ++i)
    material_info.bindings.push({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  material_info.bindings.push({5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  for (unsigned binding = 1; binding <= 4; ++binding)
    frame_info.bindings.push({binding,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                              VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  auto frame_layout =
      core.descriptor_layouts.Ensure(core.session.device(), frame_info);
  auto material_layout =
      core.descriptor_layouts.Ensure(core.session.device(), material_info);
  if (frame_layout.is_err() || material_layout.is_err())
    return false;
  core.frame_layout = frame_layout.unwrap();
  core.material_layout = material_layout.unwrap();
  vk::DescriptorSetInfo storage;
  storage.bindings.push({0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                         VK_SHADER_STAGE_VERTEX_BIT, nullptr});
  auto storage_layout =
      core.descriptor_layouts.Ensure(core.session.device(), storage);
  if (storage_layout.is_err())
    return false;
  core.storage_layout = storage_layout.unwrap();
  VkDescriptorSetLayout layouts[]{
      *(**core.descriptor_layouts.Resolve(core.frame_layout)).layout,
      *(**core.descriptor_layouts.Resolve(core.material_layout)).layout,
      *(**core.descriptor_layouts.Resolve(core.storage_layout)).layout,
      *(**core.descriptor_layouts.Resolve(core.storage_layout)).layout};
  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(DrawConstants)};
  VkPipelineLayoutCreateInfo layout{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  layout.setLayoutCount = 4;
  layout.pSetLayouts = layouts;
  if (core.session.observe_result(
          device.CreatePipelineLayout(layout, core.layout)) != VK_SUCCESS)
    return false;
  vk::GraphicsPipeline pipeline;
  pipeline.setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  pipeline.depth.depthTestEnable = VK_TRUE;
  pipeline.depth.depthWriteEnable = VK_TRUE;
  pipeline.depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkVertexInputBindingDescription binding{0, sizeof(Vertex),
                                          VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[]{
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, __builtin_offsetof(Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, __builtin_offsetof(Vertex, normal)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, __builtin_offsetof(Vertex, color)},
      {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
       __builtin_offsetof(Vertex, tangent)},
      {4, 0, VK_FORMAT_R32G32_SFLOAT, __builtin_offsetof(Vertex, uv0)},
      {5, 0, VK_FORMAT_R32G32_SFLOAT, __builtin_offsetof(Vertex, uv1)},
      {6, 0, VK_FORMAT_R32G32B32A32_UINT, __builtin_offsetof(Vertex, joints)},
      {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
       __builtin_offsetof(Vertex, weights)}};
  pipeline.addInputBindingDescription(
      slice<VkVertexInputBindingDescription>::from_raw_parts(&binding,
                                                             usize(1)));
  pipeline.addInputAttributeDescription(
      slice<VkVertexInputAttributeDescription>::from_raw_parts(attributes,
                                                               usize(8)));
  pipeline.addStage(
      shader(shaders::model_vertex.words, vrento::ShaderType::VERTEX));
  pipeline.addStage(
      shader(shaders::model_fragment.words, vrento::ShaderType::FRAGMENT));
  if (!pipeline.create(core.session.device(), *core.pass, *core.layout,
                       core.pipeline))
    return false;
  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = 15;
  pipeline.setColorBlendStates(
      slice<VkPipelineColorBlendAttachmentState>::from_raw_parts(&blend,
                                                                 usize(1)));
  pipeline.depth.depthWriteEnable = VK_FALSE;
  if (!pipeline.create(core.session.device(), *core.pass, *core.layout,
                       core.transparent))
    return false;
  core.frame_buffer = core.buffers.Allocate(
      {sizeof(FrameParameters),
       core.session.device().limits().minUniformBufferOffsetAlignment,
       vk::BufferUploadClass::Uniform});
  if (core.frame_buffer.is_none())
    return false;
  core.zero_buffer = core.buffers.Allocate(
      {16, core.session.device().limits().minStorageBufferOffsetAlignment,
       vk::BufferUploadClass::Storage});
  Vec4 zero{};
  if (core.zero_buffer.is_none() ||
      core.buffers
          .QueueWrite(*core.zero_buffer,
                      slice<rstd::uint8_t>::from_raw_parts(
                          reinterpret_cast<const rstd::uint8_t *>(&zero),
                          usize(sizeof(zero))))
          .is_none())
    return false;
  vk::GraphicsPipeline background;
  background.setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  background.addStage(
      shader(shaders::background_vertex.words, vrento::ShaderType::VERTEX));
  background.addStage(
      shader(shaders::background_fragment.words, vrento::ShaderType::FRAGMENT));
  if (!background.create(core.session.device(), *core.pass, *core.layout,
                         core.background))
    return false;
  return true;
}
auto bind_storage(RenderCore &core, const vk::BufferAllocation &buffer,
                  unsigned set)
    -> Result<registry::PreparedDescriptorBinding, Error> {
  registry::DescriptorBufferBinding binding{0, buffer.buffer(), buffer.offset(),
                                            buffer.size(),
                                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
  auto result = core.descriptors.Prepare(
      core.session.device(), set,
      **core.descriptor_layouts.Resolve(core.storage_layout), {},
      slice<registry::DescriptorBufferBinding>::from_raw_parts(&binding,
                                                               usize(1)),
      registry::DescriptorBindingReuse::Shared);
  if (result.is_err())
    return Err(render_error("storage binding"_str,
                            result.unwrap_err().message.as_str()));
  return Ok(result.unwrap());
}

} // namespace vecgltf

export namespace vecgltf {
enum class DebugView { None, BaseColor, Normal, Metallic, Roughness, Emissive };
struct RenderOptions {
  float exposure{1};
  DebugView debug{};
  bool background{};
};
class PreparedAsset {
  friend class Renderer;
  Arc<PreparedData> data_;
  explicit PreparedAsset(Arc<PreparedData> data) : data_(rstd::move(data)) {}

public:
  PreparedAsset(const PreparedAsset &other) : data_(other.data_.clone()) {}
  PreparedAsset(PreparedAsset &&) = default;
  auto operator=(PreparedAsset &&) -> PreparedAsset & = default;
  u64 generation() const { return data_->asset->generation; }
};
struct RendererStatistics {
  rstd::uint64_t prepared_assets{}, asset_buffer_bytes{},
      environment_preparations{}, target_rebuilds{}, recorded_frames{};
};
class Renderer {
  Arc<RenderCore> core_;
  Option<Arc<PreparedEnvironment>> environment_;
  Option<vrento::texture::TextureViewRef> depth_;
  VkExtent2D extent_{};
  RendererStatistics statistics_;
  explicit Renderer(Arc<RenderCore> core) : core_(rstd::move(core)) {}

public:
  Renderer(const Renderer &) = delete;
  Renderer(Renderer &&) = default;
  auto operator=(Renderer &&) -> Renderer & = delete;
  ~Renderer() { close(); }
  static auto create(vk::DeviceSession session, VkExtent2D extent)
      -> Result<Renderer, Error> {
    if (!session.valid())
      return Err(render_error("create"_str, "invalid device"_str));
    auto core = Arc<RenderCore>::make(rstd::move(session));
    if (!initialize(*core))
      return Err(
          render_error("create"_str, "could not create model pipeline"_str));
    Renderer result(rstd::move(core));
    auto environment = result.set_environment(default_environment());
    if (environment.is_err())
      return Err(environment.unwrap_err());
    auto resized = result.resize(extent);
    if (resized.is_err())
      return Err(resized.unwrap_err());
    return Ok(rstd::move(result));
  }
  const RendererStatistics &statistics() const { return statistics_; }
  auto set_environment(HdrImage source) -> Result<empty, Error> {
    if (!source.width || !source.height ||
        count(source.rgba) != rstd::size_t(source.width) * source.height * 4)
      return Err(render_error("environment"_str,
                              "invalid HDR image dimensions"_str,
                              ErrorKind::InvalidAsset));
    for (auto value : source.rgba)
      if (!rstd::f32(value).is_finite() || value < 0)
        return Err(render_error("environment"_str, "invalid HDR radiance"_str,
                                ErrorKind::InvalidAsset));
    auto processed = preprocess_environment(rstd::move(source));
    auto environment = Arc<PreparedEnvironment>::make(core_.clone());
    auto add = [&](Vec<HdrImage> images, bool wrap = true) {
      auto result = upload_environment_image(
          dyn<vrento::texture::TextureDevice>::from_ref(*core_->image_device)
              .as_mut_ref(),
          rstd::move(images), wrap);
      if (result.is_err())
        return false;
      environment->textures.push(Arc<vrento::texture::PreparedImage>::make(
          rstd::move(result).unwrap()));
      return true;
    };
    if (!add(rstd::move(processed.specular)))
      return Err(render_error("environment"_str, "specular upload failed"_str));
    Vec<HdrImage> diffuse;
    diffuse.push(rstd::move(processed.diffuse));
    Vec<HdrImage> brdf;
    brdf.push(rstd::move(processed.brdf));
    Vec<HdrImage> background;
    background.push(rstd::move(processed.background));
    if (!add(rstd::move(diffuse)) || !add(rstd::move(brdf), false) ||
        !add(rstd::move(background)))
      return Err(render_error("environment"_str, "image upload failed"_str));
    registry::DescriptorImageBinding images[4];
    for (unsigned i = 0; i < 4; ++i)
      images[i] = {i + 1,
                   vk::sampled_image(at(environment->textures, i)->view,
                                     at(environment->textures, i)->sampler)
                       .unwrap()};
    registry::DescriptorBufferBinding frame{0, core_->frame_buffer->buffer(),
                                            core_->frame_buffer->offset(),
                                            sizeof(FrameParameters)};
    auto descriptor = core_->descriptors.Prepare(
        core_->session.device(), 0,
        **core_->descriptor_layouts.Resolve(core_->frame_layout),
        slice<registry::DescriptorImageBinding>::from_raw_parts(images,
                                                                usize(4)),
        slice<registry::DescriptorBufferBinding>::from_raw_parts(&frame,
                                                                 usize(1)),
        registry::DescriptorBindingReuse::Shared);
    if (descriptor.is_err())
      return Err(render_error("environment"_str,
                              descriptor.unwrap_err().message.as_str()));
    environment->descriptor = Some(descriptor.unwrap());
    environment_ = Some(rstd::move(environment));
    ++statistics_.environment_preparations;
    return Ok(empty{});
  }
  auto prepare(Arc<Asset> asset) -> Result<PreparedAsset, Error> {
    auto data = Arc<PreparedData>::make(core_.clone(), rstd::move(asset));
    auto upload =
        [&](const void *bytes, rstd::size_t size,
            vk::BufferUploadClass usage) -> Option<vk::BufferAllocation> {
      auto alignment =
          usage == vk::BufferUploadClass::Uniform
              ? core_->session.device().limits().minUniformBufferOffsetAlignment
          : usage == vk::BufferUploadClass::Storage
              ? core_->session.device().limits().minStorageBufferOffsetAlignment
              : 4;
      auto allocation = core_->buffers.Allocate({size, alignment, usage});
      if (allocation.is_none())
        return None();
      auto source = slice<rstd::uint8_t>::from_raw_parts(
          static_cast<const rstd::uint8_t *>(bytes), usize(size));
      if (core_->buffers.QueueWrite(*allocation, source).is_none())
        return None();
      statistics_.asset_buffer_bytes += size;
      return allocation;
    };
    auto texture = [&](unsigned index, bool srgb) -> Option<PreparedTexture> {
      for (const auto &existing : data->textures)
        if (existing.texture == index && existing.srgb == srgb)
          return Some(PreparedTexture(existing));
      unsigned image_index = index == invalid_index
                                 ? invalid_index
                                 : at(data->asset->textures, index).image;
      Option<Arc<vrento::texture::PreparedImage>> image;
      auto device =
          dyn<vrento::texture::TextureDevice>::from_ref(*core_->image_device);
      for (auto &existing : data->images)
        if (existing.image == image_index && existing.srgb == srgb)
          image = Some(existing.source.clone());
      if (!image) {
        Image white;
        white.width = white.height = 1;
        white.rgba.resize(usize(4), u8(255));
        auto prepared = prepare_texture(
            device.as_mut_ref(),
            image_index == invalid_index ? white
                                         : at(data->asset->images, image_index),
            srgb);
        if (prepared.is_err())
          return None();
        image =
            Some(Arc<vrento::texture::PreparedImage>::make(prepared.unwrap()));
        data->images.push({image_index, srgb, image->clone()});
      }
      auto sampler = device->CreateSampler({texture_sample(
          index == invalid_index ? Sampler{}
                                 : at(data->asset->textures, index).sampler)});
      if (sampler.is_err())
        return None();
      PreparedTexture result{index, srgb, (*image)->view, sampler.unwrap()};
      data->textures.push(PreparedTexture(result));
      return Some(rstd::move(result));
    };
    for (const auto &material : data->asset->materials) {
      const TextureBinding *bindings[]{&material.base, &material.physical,
                                       &material.normal, &material.occlusion,
                                       &material.emission};
      MaterialParameters parameters{};
      parameters.base = material.base_color;
      parameters.emissive = {material.emissive.x, material.emissive.y,
                             material.emissive.z, 0};
      parameters.factors = {material.metallic, material.roughness,
                            material.normal.strength,
                            material.occlusion.strength};
      parameters.flags = {
          float(unsigned(material.alpha)), material.alpha_cutoff,
          material.double_sided ? 1.0f : 0.0f, material.unlit ? 1.0f : 0.0f};
      registry::DescriptorImageBinding images[5];
      for (unsigned i = 0; i < 5; ++i) {
        const auto &binding = *bindings[i];
        parameters.offset_scale[i] = {binding.offset.x, binding.offset.y,
                                      binding.scale.x, binding.scale.y};
        parameters.rotation_uv[i] = {
            cosine(binding.rotation), sine(binding.rotation), float(binding.uv),
            binding.texture == invalid_index ? 0.0f : 1.0f};
        auto image = texture(binding.texture, i == 0 || i == 4);
        if (image.is_none())
          return Err(render_error("prepare texture"_str,
                                  "image allocation or upload failed"_str));
        images[i] = {i,
                     vk::sampled_image(image->view, image->sampler).unwrap()};
      }
      auto buffer = upload(&parameters, sizeof(parameters),
                           vk::BufferUploadClass::Uniform);
      if (buffer.is_none())
        return Err(render_error("prepare material"_str,
                                "parameter allocation failed"_str));
      registry::DescriptorBufferBinding buffer_binding{
          5, buffer->buffer(), buffer->offset(), sizeof(parameters)};
      auto descriptor = core_->descriptors.Prepare(
          core_->session.device(), 1,
          **core_->descriptor_layouts.Resolve(core_->material_layout),
          slice<registry::DescriptorImageBinding>::from_raw_parts(images,
                                                                  usize(5)),
          slice<registry::DescriptorBufferBinding>::from_raw_parts(
              &buffer_binding, usize(1)),
          registry::DescriptorBindingReuse::Shared);
      if (descriptor.is_err())
        return Err(render_error("prepare material"_str,
                                descriptor.unwrap_err().message.as_str()));
      data->materials.push({rstd::move(buffer).unwrap(), descriptor.unwrap()});
    }
    for (const auto &mesh : data->asset->meshes) {
      PreparedMesh prepared;
      for (const auto &primitive : mesh.primitives) {
        auto vertices = upload(primitive.vertices.data(),
                               count(primitive.vertices) * sizeof(Vertex),
                               vk::BufferUploadClass::Vertex);
        if (vertices.is_none())
          return Err(render_error("prepare vertices"_str,
                                  "buffer allocation or upload failed"_str));
        PreparedPrimitive p{rstd::move(vertices).unwrap(), None(),
                            unsigned(count(primitive.vertices)),
                            unsigned(count(primitive.indices))};
        if (primitive.indexed) {
          p.indices = upload(primitive.indices.data(),
                             count(primitive.indices) * sizeof(unsigned),
                             vk::BufferUploadClass::Index);
          if (p.indices.is_none())
            return Err(render_error("prepare indices"_str,
                                    "buffer allocation or upload failed"_str));
        }
        if (!primitive.morphs.is_empty()) {
          Vec<Vec4> deltas;
          for (const auto &morph : primitive.morphs)
            for (unsigned v = 0; v < count(primitive.vertices); ++v) {
              auto position = at(morph.positions, v),
                   normal = at(morph.normals, v),
                   tangent = at(morph.tangents, v);
              deltas.push({position.x, position.y, position.z, 0});
              deltas.push({normal.x, normal.y, normal.z, 0});
              deltas.push({tangent.x, tangent.y, tangent.z, 0});
            }
          auto bytes = count(deltas) * sizeof(Vec4);
          if (bytes > core_->session.device().limits().maxStorageBufferRange)
            return Err(render_error(
                "morph"_str, "morph data exceeds device storage range"_str,
                ErrorKind::Limit));
          p.morphs =
              upload(deltas.data(), bytes, vk::BufferUploadClass::Storage);
          if (p.morphs.is_none())
            return Err(render_error("morph"_str, "morph upload failed"_str));
        }
        auto morph_binding = bind_storage(
            *core_, p.morphs.is_some() ? *p.morphs : *core_->zero_buffer, 3);
        if (morph_binding.is_err())
          return Err(morph_binding.unwrap_err());
        p.morph_binding = Some(morph_binding.unwrap());
        prepared.primitives.push(rstd::move(p));
      }
      data->meshes.push(rstd::move(prepared));
    }
    ++statistics_.prepared_assets;
    return Ok(PreparedAsset(rstd::move(data)));
  }
  auto resize(VkExtent2D extent) -> Result<empty, Error> {
    if (!extent.width || !extent.height)
      return Err(render_error("resize"_str, "zero extent"_str));
    if (extent.width == extent_.width && extent.height == extent_.height)
      return Ok(empty{});
    vrento::texture::TextureDesc desc;
    desc.extent = {extent.width, extent.height};
    desc.format = vrento::TextureFormat::D32F;
    desc.usage = vrento::texture::Usage::DepthAttachment;
    auto depth = core_->image_device->CreateTexture(desc);
    if (depth.is_err())
      return Err(texture_error(depth.unwrap_err()));
    auto depth_view = core_->image_device->CreateView(
        depth.unwrap(),
        {vrento::texture::Dimension::D2, {vrento::texture::Aspect::Depth}});
    if (depth_view.is_err())
      return Err(texture_error(depth_view.unwrap_err()));
    depth_ = Some(depth_view.unwrap());
    extent_ = extent;
    ++statistics_.target_rebuilds;
    return Ok(empty{});
  }
  auto record(vrento::texture::Submission &work,
              vrento::texture::TextureViewRef target,
              const PreparedAsset &prepared, const SceneInstance &instance,
              const View &view, RenderOptions options = {})
      -> Result<empty, Error> {
    if (!target.valid() || &*prepared.data_->core != &*core_ ||
        prepared.generation() != instance.asset().generation)
      return Err(render_error("render"_str,
                              "asset or renderer identity mismatch"_str));
    if (core_->recording)
      return Err(render_error("record"_str,
                              "previous model submission is still in use"_str,
                              ErrorKind::Busy));
    const auto &target_desc = target.texture().description();
    auto range = target.description().range;
    if (target_desc.format != vrento::TextureFormat::RGBA8 ||
        range.mip_count != 1 || range.layer_count != 1)
      return Err(render_error("record"_str, "unsupported target view"_str));
    auto size = vrento::texture::mip_extent(target_desc, range.first_mip);
    auto resized = resize({size.width, size.height});
    if (resized.is_err())
      return resized;
    auto access = work.commands()->UseTexture(
        {target, vrento::texture::Access::ColorAttachment,
         vrento::texture::Stages::Color, true});
    if (access.is_err())
      return Err(texture_error(access.unwrap_err()));
    auto native_command = vk::native_recording(work, core_->session);
    if (native_command.is_err())
      return Err(texture_error(native_command.unwrap_err()));
    auto command_buffer = native_command.unwrap();
    auto target_view = vk::native_view(target);
    if (target_view.is_err())
      return Err(texture_error(target_view.unwrap_err()));
    auto inputs = Box<RenderInputs>::make(prepared.data_.clone(), *depth_,
                                          work.receipt());
    inputs->environment = Some(environment_->clone());
    struct PoseOffsets {
      unsigned skin{invalid_index}, weights{};
    };
    Vec<PoseOffsets> pose_offsets;
    Vec<Vec4> pose_data;
    for (unsigned n = 0; n < count(instance.asset().nodes); ++n) {
      const auto &pose = instance.pose(n);
      PoseOffsets offsets;
      if (!pose.skin_palette.is_empty()) {
        offsets.skin = unsigned(count(pose_data));
        for (const auto &matrix : pose.skin_palette)
          for (unsigned c = 0; c < 4; ++c)
            pose_data.push({matrix.m[c * 4], matrix.m[c * 4 + 1],
                            matrix.m[c * 4 + 2], matrix.m[c * 4 + 3]});
      }
      offsets.weights = unsigned(count(pose_data));
      for (unsigned i = 0; i < count(pose.weights); i += 4) {
        Vec4 weights{};
        float *components = &weights.x;
        for (unsigned c = 0; c < 4 && i + c < count(pose.weights); ++c)
          components[c] = at(pose.weights, i + c);
        pose_data.push(rstd::move(weights));
      }
      pose_offsets.push(rstd::move(offsets));
    }
    if (pose_data.is_empty())
      pose_data.push(Vec4{});
    auto pose_bytes = count(pose_data) * sizeof(Vec4);
    if (pose_bytes > core_->session.device().limits().maxStorageBufferRange)
      return Err(render_error("pose"_str,
                              "pose data exceeds device storage range"_str,
                              ErrorKind::Limit));
    inputs->poses = core_->buffers.Allocate(
        {pose_bytes,
         core_->session.device().limits().minStorageBufferOffsetAlignment,
         vk::BufferUploadClass::Storage});
    if (inputs->poses.is_none() ||
        core_->buffers
            .QueueWrite(
                *inputs->poses,
                slice<rstd::uint8_t>::from_raw_parts(
                    reinterpret_cast<const rstd::uint8_t *>(pose_data.data()),
                    usize(pose_bytes)))
            .is_none())
      return Err(render_error("pose"_str, "pose upload failed"_str));
    auto pose_binding = bind_storage(*core_, *inputs->poses, 2);
    if (pose_binding.is_err())
      return Err(pose_binding.unwrap_err());
    inputs->pose_binding = Some(pose_binding.unwrap());
    auto depth_access = work.commands()->UseTexture(
        {*depth_, vrento::texture::Access::DepthAttachment,
         vrento::texture::Stages::Depth, true});
    if (depth_access.is_err())
      return Err(texture_error(depth_access.unwrap_err()));
    VkImageView attachments[]{target_view.unwrap(),
                              vk::native_view(*depth_).unwrap()};
    VkFramebufferCreateInfo framebuffer{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = *core_->pass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = attachments;
    framebuffer.width = extent_.width;
    framebuffer.height = extent_.height;
    framebuffer.layers = 1;
    const auto &device = core_->session.device().handle();
    const auto &d = device.Dispatch();
    if (core_->session.observe_result(device.CreateFramebuffer(
            framebuffer, inputs->framebuffer)) != VK_SUCCESS)
      return Err(render_error("render"_str, "framebuffer creation failed"_str));
    vvk::CommandBuffer command(command_buffer, d);
    FrameParameters frame_parameters{
        {view.eye.x, view.eye.y, view.eye.z, 1},
        {options.exposure, float(unsigned(options.debug)),
         options.background ? 1.0f : 0.0f, 7},
        (view.projection * view.view).inverse().unwrap_or(Mat4::Identity())};
    if (core_->buffers
            .QueueWrite(
                *core_->frame_buffer,
                slice<rstd::uint8_t>::from_raw_parts(
                    reinterpret_cast<const rstd::uint8_t *>(&frame_parameters),
                    usize(sizeof(frame_parameters))))
            .is_none())
      return Err(render_error("frame"_str, "view parameter upload failed"_str));
    vk::RecordedBufferUploads recorded;
    if (core_->buffers.HasPendingUploads()) {
      if (!core_->buffers.RecordPendingUploads(command, recorded))
        return Err(render_error("upload"_str, "buffer recording failed"_str));
      inputs->uploads =
          core_->buffers.CommitRecordedUploads(rstd::move(recorded));
      if (!inputs->uploads)
        return Err(
            render_error("upload"_str, "buffer upload lease failed"_str));
    }
    auto prepare_image = [&](const vrento::texture::PreparedImage &source)
        -> Result<empty, Error> {
      if (!source.view.texture().ready().valid()) {
        for (auto &upload : source.uploads) {
          auto queued = work.commands()->Upload(upload.clone());
          if (queued.is_err())
            return Err(texture_error(queued.unwrap_err()));
        }
      }
      auto used = work.commands()->UseSampledView(
          source.view, source.sampler, vrento::texture::Stages::Fragment);
      if (used.is_err())
        return Err(texture_error(used.unwrap_err()));
      return Ok(empty{});
    };
    for (auto &source : prepared.data_->images) {
      auto result = prepare_image(*source.source);
      if (result.is_err())
        return result;
    }
    for (auto &source : (*environment_)->textures) {
      auto result = prepare_image(*source);
      if (result.is_err())
        return result;
    }
    VkClearValue clear[2]{};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = *core_->pass;
    begin.framebuffer = *inputs->framebuffer;
    begin.renderArea = {{0, 0}, extent_};
    begin.clearValueCount = 2;
    begin.pClearValues = clear;
    auto cmd = command_buffer;
    d.vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(extent_.width), float(extent_.height),
                        0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    d.vkCmdSetViewport(cmd, 0, 1, &viewport);
    d.vkCmdSetScissor(cmd, 0, 1, &scissor);
    registry::DescriptorBindingRecordState binding_state;
    (*environment_)->descriptor->Record(command, *core_->layout, binding_state);
    if (options.background) {
      d.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          *core_->background.handle);
      d.vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    inputs->pose_binding->Record(command, *core_->layout, binding_state);
    const auto &asset = instance.asset();
    struct Draw {
      unsigned node{}, primitive{};
      float depth{};
      bool transparent{};
    };
    Vec<Draw> draws;
    for (unsigned n = 0; n < count(asset.nodes); ++n) {
      auto mesh_index = at(asset.nodes, n).mesh;
      if (!instance.visible(n) || mesh_index == invalid_index)
        continue;
      const auto &mesh = at(asset.meshes, mesh_index);
      for (unsigned p = 0; p < count(mesh.primitives); ++p) {
        const auto &primitive = at(mesh.primitives, p);
        auto position = at(instance.pose(n).primitive_bounds, p).center();
        draws.push({n, p, view.view.point(position).z,
                    at(asset.materials, primitive.material).alpha ==
                        AlphaMode::Blend});
      }
    }
    rstd::slice_::sort_unstable_by(
        draws.as_mut_slice().as_mut_ref(), [](const Draw &a, const Draw &b) {
          if (a.transparent != b.transparent)
            return !a.transparent;
          return a.transparent ? a.depth < b.depth : a.depth > b.depth;
        });
    for (const auto &draw : draws) {
      auto n = draw.node, p = draw.primitive,
           mesh_index = at(asset.nodes, n).mesh;
      const auto &mesh = at(asset.meshes, mesh_index);
      const auto &gpu_mesh = at(prepared.data_->meshes, mesh_index);
      const auto &world = instance.pose(n).world;
      if (instance.pose(n).skin_palette.is_empty() &&
          absolute(world.determinant3()) < 1e-12f)
        continue;
      DrawConstants constants{};
      constants.mvp = view.projection * view.view;
      for (unsigned row = 0; row < 3; ++row)
        constants.model[row] = {world.m[row], world.m[4 + row],
                                world.m[8 + row], world.m[12 + row]};
      const auto &primitive = at(mesh.primitives, p);
      const auto &gpu = at(gpu_mesh.primitives, p);
      d.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          draw.transparent ? *core_->transparent.handle
                                           : *core_->pipeline.handle);
      at(prepared.data_->materials, primitive.material)
          .descriptor.Record(command, *core_->layout, binding_state);
      gpu.morph_binding->Record(command, *core_->layout, binding_state);
      constants.skin_offset = at(pose_offsets, n).skin;
      constants.weight_offset = at(pose_offsets, n).weights;
      constants.morph_count = unsigned(count(primitive.morphs));
      constants.vertex_count = gpu.vertex_count;
      d.vkCmdPushConstants(cmd, *core_->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(constants), &constants);
      auto buffer = gpu.vertices.buffer();
      auto offset = gpu.vertices.offset();
      d.vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
      if (gpu.indices) {
        d.vkCmdBindIndexBuffer(cmd, gpu.indices->buffer(),
                               gpu.indices->offset(), VK_INDEX_TYPE_UINT32);
        d.vkCmdDrawIndexed(cmd, gpu.index_count, 1, 0, 0, 0);
      } else
        d.vkCmdDraw(cmd, gpu.vertex_count, 1, 0, 0);
    }
    d.vkCmdEndRenderPass(cmd);
    auto sampled =
        work.commands()->UseTexture({target, vrento::texture::Access::Sampled,
                                     vrento::texture::Stages::Fragment});
    if (sampled.is_err())
      return Err(texture_error(sampled.unwrap_err()));
    work.retain(rstd::move(inputs));
    ++statistics_.recorded_frames;
    return Ok(empty{});
  }
  void close() { depth_ = None(); }
};
} // namespace vecgltf
