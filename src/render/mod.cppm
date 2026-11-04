module;
#include <vulkan/vulkan_core.h>

export module vecgltf.render;
export import vecgltf.scene;
export import vecgltf.load;
export import vecgltf.render.options;
export import vecgltf.environment;
export import vrento;
import vecgltf.shaders;
import vecgltf.textures;
import vecgltf.render.shadow;

using namespace rstd::literals;

namespace vecgltf
{
namespace vk       = vrento::vulkan;
namespace res      = vrento::resource;
namespace registry = vrento::resource_registry;

auto render_error(ref<str> location, ref<str> message, ErrorKind kind = ErrorKind::Device)
    -> Error {
    return { kind, String(), String::make(location), String::make(message) };
}
template<class T>
auto backend_error(ref<str> location, T error, ErrorKind kind = ErrorKind::Device) -> Error {
    auto message = rstd::format("{}", error);
    return { kind,
             String(),
             String::make(location),
             rstd::move(message),
             Some(Arc<dyn<rstd::error::Error>>::make(rstd::move(error))) };
}
auto texture_error(vrento::texture::Error error) -> Error {
    auto kind =
        error.kind == vrento::texture::ErrorKind::Busy ? ErrorKind::Busy : ErrorKind::Device;
    return backend_error("texture"_str, rstd::move(error), kind);
}
auto graph_error(vk::GraphExecutionError error) -> Error {
    auto texture = error.texture_error();
    auto kind = texture && (*texture)->kind == vrento::texture::ErrorKind::Busy ? ErrorKind::Busy
                                                                                : ErrorKind::Device;
    return backend_error("model pass"_str, rstd::move(error), kind);
}
auto graph_error(vrento::fg::Error error) -> Error {
    return graph_error(vk::GraphExecutionError { vk::GraphExecutionStage::Graph,
                                                 error.pass,
                                                 error.resource,
                                                 vk::GraphExecutionCause::Graph(error) });
}
bool valid_environment_image(const HdrImage& image) {
    auto pixels = rstd::uint64_t(image.width) * image.height;
    if (! image.width || ! image.height || pixels > (~rstd::size_t(0)) / 4 ||
        count(image.rgba) != pixels * 4)
        return false;
    for (auto value : image.rgba)
        if (! rstd::f32(value).is_finite() || value < 0) return false;
    return true;
}
struct LightingParameters {
    Vec4                     directions[max_directional_lights], radiances[max_directional_lights];
    unsigned                 count {};
    Vec<shadow::ShadowLight> shadows;
};
auto lower_lighting(const RenderOptions& options, const View& view, unsigned max_extent)
    -> Result<LightingParameters, Error> {
    auto invalid = []() {
        return render_error("render options"_str,
                            "invalid lighting or environment rotation"_str,
                            ErrorKind::InvalidAsset);
    };
    auto non_negative = [](float value) {
        return rstd::f32(value).is_finite() && value >= 0;
    };
    if (! non_negative(options.ibl_intensity) || ! non_negative(options.background_intensity) ||
        unsigned(options.debug) > unsigned(DebugView::Emissive))
        return Err(invalid());
    const auto& matrix = options.world_to_environment;
    for (auto value : matrix.m)
        if (! rstd::f32(value).is_finite()) return Err(invalid());
    if (matrix.m[3] != 0 || matrix.m[7] != 0 || matrix.m[11] != 0 || matrix.m[12] != 0 ||
        matrix.m[13] != 0 || matrix.m[14] != 0 || matrix.m[15] != 1)
        return Err(invalid());
    Vec3 columns[] { { matrix.m[0], matrix.m[1], matrix.m[2] },
                     { matrix.m[4], matrix.m[5], matrix.m[6] },
                     { matrix.m[8], matrix.m[9], matrix.m[10] } };
    for (unsigned i = 0; i < 3; ++i) {
        if (absolute(dot(columns[i], columns[i]) - 1) > 0.0001f) return Err(invalid());
        for (unsigned j = 0; j < i; ++j)
            if (absolute(dot(columns[i], columns[j])) > 0.0001f) return Err(invalid());
    }
    if (absolute(matrix.determinant3() - 1) > 0.0001f) return Err(invalid());
    LightingParameters result {};
    Option<Mat4>       inverse_view;
    auto               append = [&](const DirectionalLight& light) -> Result<empty, Error> {
        if (! rstd::f32(light.direction.x).is_finite() ||
            ! rstd::f32(light.direction.y).is_finite() ||
            ! rstd::f32(light.direction.z).is_finite() || ! non_negative(light.color.x) ||
            ! non_negative(light.color.y) || ! non_negative(light.color.z) ||
            ! non_negative(light.intensity) || ! non_negative(light.color.x * light.intensity) ||
            ! non_negative(light.color.y * light.intensity) ||
            ! non_negative(light.color.z * light.intensity) ||
            (light.space != LightSpace::World && light.space != LightSpace::View))
            return Err(invalid());
        auto normalize = [](Vec3 value) -> Option<Vec3> {
            if (! rstd::f32(value.x).is_finite() || ! rstd::f32(value.y).is_finite() ||
                ! rstd::f32(value.z).is_finite())
                return None();
            float scale = maximum(absolute(value.x), maximum(absolute(value.y), absolute(value.z)));
            if (scale == 0) return None();
            return Some(normalized(Vec3 { value.x / scale, value.y / scale, value.z / scale }));
        };
        auto direction = normalize(light.direction);
        if (! direction) return Err(invalid());
        if (light.space == LightSpace::View) {
            if (! inverse_view) inverse_view = view.view.inverse();
            if (! inverse_view) return Err(invalid());
            direction = normalize(inverse_view->vector(direction.unwrap()));
            if (! direction) return Err(invalid());
        }
        auto value = direction.unwrap();
        if (light.shadow) {
            auto valid = shadow::ValidateShadow(*light.shadow, max_extent);
            if (valid.is_err()) return Err(valid.unwrap_err());
            result.shadows.push({ result.count, value, *light.shadow });
        }
        result.directions[result.count]  = { value.x, value.y, value.z, 0 };
        result.radiances[result.count++] = { light.color.x * light.intensity,
                                             light.color.y * light.intensity,
                                             light.color.z * light.intensity,
                                             0 };
        return Ok(empty {});
    };
    if (options.directional_lights) {
        const auto& lights = options.directional_lights.values();
        if (count(lights) > max_directional_lights)
            return Err(render_error(
                "render options"_str, "too many directional lights"_str, ErrorKind::Limit));
        for (const auto& light : lights) {
            auto added = append(light);
            if (added.is_err()) return Err(added.unwrap_err());
        }
    } else if (options.direct_light) {
        auto added = append(*options.direct_light);
        if (added.is_err()) return Err(added.unwrap_err());
    }
    return Ok(rstd::move(result));
}
auto camera_relative_model(const Mat4& model, Vec3 origin) -> Mat4 {
    auto         result = model;
    const double offset[] { origin.x, origin.y, origin.z };
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 3; ++row)
            result.m[column * 4 + row] = float(double(model.m[column * 4 + row]) -
                                               offset[row] * double(model.m[column * 4 + 3]));
    return result;
}
auto camera_relative_view(const Mat4& view, Vec3 origin) -> Mat4 {
    auto         result = view;
    const double offset[] { origin.x, origin.y, origin.z };
    for (unsigned row = 0; row < 4; ++row) {
        double translation = view.m[12 + row];
        for (unsigned column = 0; column < 3; ++column)
            translation += double(view.m[column * 4 + row]) * offset[column];
        result.m[12 + row] = float(translation);
    }
    return result;
}
float view_depth(const Mat4& view, Vec3 point) {
    double z = double(view.m[2]) * point.x + double(view.m[6]) * point.y +
               double(view.m[10]) * point.z + view.m[14];
    double w = double(view.m[3]) * point.x + double(view.m[7]) * point.y +
               double(view.m[11]) * point.z + view.m[15];
    return float(z / w);
}
auto reversed_projection(const Mat4& projection) -> Mat4 {
    auto result = projection;
    for (unsigned column = 0; column < 4; ++column)
        result.m[column * 4 + 2] =
            float(double(projection.m[column * 4 + 3]) - projection.m[column * 4 + 2]);
    return result;
}
enum class ModelPipeline : unsigned
{
    Opaque,
    BlendOpaque,
    Blend,
    ReversedOpaque,
    ReversedBlendOpaque,
    ReversedBlend,
    Background,
    Shadow
};
struct RenderCore {
    vk::DeviceSession                          session;
    LoadTrace                                  trace;
    vk::BufferManager                          buffers;
    Option<vk::TextureContext>                 image_device;
    registry::DescriptorLayoutRegistry&        descriptor_layouts;
    registry::DescriptorSystem                 descriptors;
    res::DescriptorLayoutHandle                frame_layout, material_layout, storage_layout;
    Option<vk::BufferAllocation>               zero_buffer;
    Option<registry::PipelineLayoutResult>     layout;
    Vec<res::PipelineRecipeHandle>             recipes;
    Option<vk::PipelinePreparationQueue>       preparation;
    vk::GraphicsScopeCache                     scopes;
    Vec<registry::PipelineResourceResult>      warmed;
    Option<vrento::texture::SubmissionReceipt> latest;
    Option<vrento::texture::SamplerRef>        shadow_sampler;
    explicit RenderCore(vk::DeviceSession device, LoadTrace observer)
        : session(rstd::move(device)),
          trace(rstd::move(observer)),
          buffers(session.device()),
          image_device(None()),
          descriptor_layouts(session.graphics_resources().descriptors) {}
    ~RenderCore() {
        for (const auto& recipe : recipes)
            session.graphics_resources().pipelines.ReleaseRecipe(recipe);
    }
};
struct PreparedPrimitive {
    vk::BufferAllocation                        vertices;
    Option<vk::BufferAllocation>                indices;
    unsigned                                    vertex_count {}, index_count {};
    Option<vk::BufferAllocation>                morphs;
    Option<registry::PreparedDescriptorBinding> morph_binding;
};
struct ModelVertex {
    Vec3     position, normal;
    Vec4     color, tangent;
    Vec2     texcoords[5];
    unsigned joints[4] {};
    Vec4     weights {};
};
struct PreparedMesh {
    Vec<PreparedPrimitive> primitives;
};
struct PreparedMaterial {
    vk::BufferAllocation                parameters;
    registry::PreparedDescriptorBinding descriptor;
};
struct PreparedTexture {
    unsigned                        texture {};
    bool                            srgb {};
    vrento::texture::TextureViewRef view;
    vrento::texture::SamplerRef     sampler;
};
struct PreparedImageEntry {
    unsigned                  image {};
    bool                      srgb {};
    Arc<PreparedTextureImage> source;
};
struct MaterialParameters {
    Vec4 base, emissive, factors, flags, specular;
    Vec4 offset_scale[5], rotation_uv[5];
};
struct FrameParameters {
    Vec4 eye, options;
    Mat4 inverse_view_projection, projection;
    Vec4 lighting;
    Mat4 world_to_environment;
    Vec4 light_directions[max_directional_lights], light_radiances[max_directional_lights];
    Vec4 environment_lod;
};
static_assert(sizeof(FrameParameters) == 512);
struct ShadowParameters {
    Mat4 matrices[max_directional_lights];
    Vec4 rectangles[max_directional_lights], biases[max_directional_lights];
    Vec4 atlas;
};
static_assert(sizeof(ShadowParameters) == 784);
struct PreparedEnvironment {
    Arc<RenderCore>                          core;
    Vec<Arc<vrento::texture::PreparedImage>> textures;
    float                                    max_lod {}, lod_scale {}, lod_exponent { 1 };
    explicit PreparedEnvironment(Arc<RenderCore> owner): core(rstd::move(owner)) {}
};
struct PreparedData {
    Arc<RenderCore>         core;
    Arc<Asset>              asset;
    Vec<PreparedMesh>       meshes;
    Vec<PreparedTexture>    textures;
    Vec<PreparedImageEntry> images;
    Vec<PreparedMaterial>   materials;
    PreparedData(Arc<RenderCore> owner, Arc<Asset> source)
        : core(rstd::move(owner)), asset(rstd::move(source)) {}
};
struct DrawConstants {
    Mat4     view;
    Vec4     model[3];
    unsigned skin_offset {}, weight_offset {}, morph_count {}, vertex_count {};
};
static_assert(sizeof(DrawConstants) == 128);

struct ModelDraw {
    unsigned      mesh {}, primitive {}, material {};
    DrawConstants constants;
    bool          transparent {};
};
struct ShadowFrame {
    shadow::ShadowView                          view;
    Option<vk::BufferAllocation>                buffer;
    Option<registry::PreparedDescriptorBinding> binding;
};
struct ModelFrameResources {
    Arc<PreparedData>                           prepared;
    vrento::texture::TextureViewRef             depth;
    vrento::texture::SubmissionReceipt          receipt;
    Option<Arc<PreparedEnvironment>>            environment;
    Option<vk::BufferUploadBatchLease>          uploads;
    Option<vk::BufferAllocation>                poses, frame_buffer;
    Option<vk::BufferAllocation>                coverage_frame_buffer;
    Option<registry::PreparedDescriptorBinding> frame_binding;
    Option<registry::PreparedDescriptorBinding> coverage_frame_binding;
    Option<registry::PreparedDescriptorBinding> pose_binding;
    Option<vrento::texture::TextureViewRef>     coverage;
    Option<vrento::texture::TextureViewRef>     shadow_atlas;
    Option<vk::BufferAllocation>                shadow_parameters;
    Vec<ShadowFrame>                            shadows;
    ModelFrameResources(Arc<PreparedData>                  source,
                        vrento::texture::TextureViewRef    depth_view,
                        vrento::texture::SubmissionReceipt submission)
        : prepared(rstd::move(source)),
          depth(rstd::move(depth_view)),
          receipt(rstd::move(submission)) {
        prepared->core->latest = Some(vrento::texture::SubmissionReceipt(receipt));
    }
    ~ModelFrameResources() {
        if (uploads && ! receipt.submitted() && ! prepared->core->session.lost()) {
            if (! prepared->core->buffers.RestoreUploads(uploads.take().unwrap()))
                rstd::panic("cannot restore cancelled model uploads");
        }
    }
};
struct ModelFrameData {
    Arc<ModelFrameResources> resources;
    Arc<Vec<ModelDraw>>      draws;
    bool                     background {};
    bool                     reversed_depth {};
    bool                     coverage {};
};
template<rstd::size_t N>
auto shader(const unsigned (&code)[N], vrento::ShaderType stage) -> vk::Uni_ShaderSpv {
    auto result   = Box<vk::ShaderSpv>::make();
    result->stage = stage;
    result->spirv = vrento::ShaderCode::from(slice<rstd::uint32_t>::from_raw_parts(code, usize(N)));
    return result;
}

auto initialize(RenderCore& core, vk::PipelinePreparationMode mode) -> Result<empty, Error> {
    LoadScope scope(core.trace, LoadEvent::RendererInitialize);
    auto      preparation = vk::PipelinePreparationQueue::create(core.session, mode);
    if (preparation.is_err())
        return Err(backend_error("pipeline worker"_str, preparation.unwrap_err()));
    core.preparation = Some(preparation.unwrap());
    if (! core.buffers.init())
        return Err(render_error("buffer manager"_str, "initialization failed"_str));
    auto image_device = vk::TextureContext::create(core.session);
    if (image_device.is_err()) return Err(texture_error(image_device.unwrap_err()));
    core.image_device = Some(image_device.unwrap());
    vrento::texture::SamplerDesc shadow_sampler;
    shadow_sampler.sample.minFilter = shadow_sampler.sample.magFilter =
        vrento::TextureFilter::NEAREST;
    shadow_sampler.sample.mipFilter = vrento::TextureMipmapFilter::None;
    shadow_sampler.sample.wrapS = shadow_sampler.sample.wrapT = vrento::TextureWrap::CLAMP_TO_EDGE;
    auto sampled = core.image_device->CreateSampler(shadow_sampler);
    if (sampled.is_err()) return Err(texture_error(sampled.unwrap_err()));
    core.shadow_sampler = Some(sampled.unwrap());
    vk::DescriptorSetInfo frame_info, material_info;
    frame_info.bindings.push({ 0,
                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                               1,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               nullptr });
    for (unsigned i = 0; i < 5; ++i)
        material_info.bindings.push({ i,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      1,
                                      VK_SHADER_STAGE_FRAGMENT_BIT,
                                      nullptr });
    material_info.bindings.push(
        { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    for (unsigned binding = 1; binding <= 4; ++binding)
        frame_info.bindings.push({ binding,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   1,
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                                   nullptr });
    frame_info.bindings.push(
        { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    frame_info.bindings.push(
        { 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    auto frame_layout    = core.descriptor_layouts.Ensure(core.session.device(), frame_info);
    auto material_layout = core.descriptor_layouts.Ensure(core.session.device(), material_info);
    if (frame_layout.is_err())
        return Err(backend_error("frame layout"_str, frame_layout.unwrap_err()));
    if (material_layout.is_err())
        return Err(backend_error("material layout"_str, material_layout.unwrap_err()));
    core.frame_layout    = frame_layout.unwrap();
    core.material_layout = material_layout.unwrap();
    vk::DescriptorSetInfo storage;
    storage.bindings.push(
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr });
    auto storage_layout = core.descriptor_layouts.Ensure(core.session.device(), storage);
    if (storage_layout.is_err())
        return Err(backend_error("pipeline layout"_str, storage_layout.unwrap_err()));
    core.storage_layout = storage_layout.unwrap();
    registry::PipelineLayoutRequest layout_request;
    layout_request.descriptor_sets.push(frame_info.clone());
    layout_request.descriptor_sets.push(material_info.clone());
    layout_request.descriptor_sets.push(storage.clone());
    layout_request.descriptor_sets.push(storage.clone());
    layout_request.push_constants.push({ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(DrawConstants) });
    auto& graphics = core.session.graphics_resources();
    auto  layout =
        graphics.layouts.Ensure(core.session.device(), layout_request, core.descriptor_layouts);
    if (layout.is_err()) return Err(backend_error("pipeline layout"_str, layout.unwrap_err()));
    core.layout = Some(layout.unwrap());
    vk::GraphicsPipeline        defaults;
    vk::PipelineResourceRequest pipeline;
    pipeline.pipeline_layout        = core.layout->handle;
    pipeline.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline.depth                  = defaults.depth;
    pipeline.raster                 = defaults.raster;
    pipeline.multisample            = defaults.multisample;
    pipeline.depth.depthTestEnable  = VK_TRUE;
    pipeline.depth.depthWriteEnable = VK_TRUE;
    pipeline.depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipeline.has_depth_attachment   = true;
    pipeline.color_initial_layout   = pipeline.color_final_layout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    pipeline.depth_initial_layout = pipeline.depth_final_layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pipeline.color_load_op = pipeline.depth_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pipeline.color_blend.colorWriteMask             = 15;
    auto add = [&](vk::PipelineResourceRequest value, bool warm = true) -> Result<empty, Error> {
        auto signature = vk::MakeRenderPassResourceDesc(value);
        auto recipe    = graphics.pipelines.RegisterRecipe(rstd::move(value), graphics.layouts);
        if (recipe.is_none())
            return Err(render_error("pipeline recipe"_str, "registration failed"_str));
        auto handle = recipe.unwrap();
        core.recipes.push(res::PipelineRecipeHandle(handle));
        if (! warm) return Ok(empty {});
        auto ready = core.preparation->prepare(handle, signature);
        if (ready.is_err()) return Err(backend_error("pipeline"_str, ready.unwrap_err()));
        auto prepared = ready.unwrap();
        if (prepared.error)
            return Err(backend_error("pipeline"_str, prepared.error.take().unwrap()));
        if (prepared.status == registry::PipelineReadiness::Failed)
            return Err(render_error("pipeline"_str, "preparation failed"_str));
        if (prepared.pipeline) core.warmed.push(prepared.pipeline.take().unwrap());
        return Ok(empty {});
    };
    VkVertexInputBindingDescription binding { 0, sizeof(ModelVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[] {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, __builtin_offsetof(ModelVertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, __builtin_offsetof(ModelVertex, normal) },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, __builtin_offsetof(ModelVertex, color) },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, __builtin_offsetof(ModelVertex, tangent) },
        { 4, 0, VK_FORMAT_R32G32_SFLOAT, __builtin_offsetof(ModelVertex, texcoords) },
        { 5,
          0,
          VK_FORMAT_R32G32_SFLOAT,
          __builtin_offsetof(ModelVertex, texcoords) + sizeof(Vec2) },
        { 6,
          0,
          VK_FORMAT_R32G32_SFLOAT,
          __builtin_offsetof(ModelVertex, texcoords) + sizeof(Vec2) * 2 },
        { 7,
          0,
          VK_FORMAT_R32G32_SFLOAT,
          __builtin_offsetof(ModelVertex, texcoords) + sizeof(Vec2) * 3 },
        { 8,
          0,
          VK_FORMAT_R32G32_SFLOAT,
          __builtin_offsetof(ModelVertex, texcoords) + sizeof(Vec2) * 4 },
        { 9, 0, VK_FORMAT_R32G32B32A32_UINT, __builtin_offsetof(ModelVertex, joints) },
        { 10, 0, VK_FORMAT_R32G32B32A32_SFLOAT, __builtin_offsetof(ModelVertex, weights) }
    };
    pipeline.vertex_bindings.push(VkVertexInputBindingDescription(binding));
    for (const auto& attribute : attributes)
        pipeline.vertex_attrs.push(VkVertexInputAttributeDescription(attribute));
    pipeline.shader_stages.push(shader(shaders::model_vertex.words, vrento::ShaderType::VERTEX));
    pipeline.shader_stages.push(
        shader(shaders::model_fragment.words, vrento::ShaderType::FRAGMENT));
    VkPipelineColorBlendAttachmentState blend {};
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = 15;
    const bool directions[] { false, true };
    for (bool reversed : directions) {
        auto model = pipeline.clone();
        model.depth.depthCompareOp =
            reversed ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        auto opaque = add(model.clone());
        if (opaque.is_err()) return opaque;
        auto coverage = model.clone();
        coverage.shader_stages[usize(1)] =
            shader(shaders::model_opaque_fragment.words, vrento::ShaderType::FRAGMENT);
        auto blend_opaque = add(rstd::move(coverage));
        if (blend_opaque.is_err()) return blend_opaque;
        model.color_blend            = blend;
        model.depth.depthWriteEnable = VK_FALSE;
        auto transparent             = add(rstd::move(model));
        if (transparent.is_err()) return transparent;
    }
    core.zero_buffer =
        core.buffers.Allocate({ 16,
                                core.session.device().limits().minStorageBufferOffsetAlignment,
                                vk::BufferUploadClass::Storage });
    Vec4 zero {};
    if (core.zero_buffer.is_none() ||
        core.buffers
            .QueueWrite(*core.zero_buffer,
                        slice<rstd::uint8_t>::from_raw_parts(
                            reinterpret_cast<const rstd::uint8_t*>(&zero), usize(sizeof(zero))))
            .is_none())
        return Err(render_error("zero buffer"_str, "allocation or upload failed"_str));
    vk::PipelineResourceRequest background;
    background.pipeline_layout            = core.layout->handle;
    background.topology                   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    background.depth                      = defaults.depth;
    background.raster                     = defaults.raster;
    background.multisample                = defaults.multisample;
    background.has_depth_attachment       = true;
    background.color_blend.colorWriteMask = 15;
    background.color_initial_layout       = background.color_final_layout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    background.depth_initial_layout = background.depth_final_layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    background.shader_stages.push(
        shader(shaders::background_vertex.words, vrento::ShaderType::VERTEX));
    background.shader_stages.push(
        shader(shaders::background_fragment.words, vrento::ShaderType::FRAGMENT));
    auto added = add(rstd::move(background));
    if (added.is_err()) return added;
    auto caster                 = pipeline.clone();
    caster.has_color_attachment = false;
    caster.shader_stages[usize(1)] =
        shader(shaders::model_shadow_fragment.words, vrento::ShaderType::FRAGMENT);
    return add(rstd::move(caster), false);
}
auto bind_storage(RenderCore& core, const vk::BufferAllocation& buffer, unsigned set)
    -> Result<registry::PreparedDescriptorBinding, Error> {
    registry::DescriptorBufferBinding binding {
        0, buffer.buffer(), buffer.offset(), buffer.size(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    };
    auto result = core.descriptors.Prepare(
        core.session.device(),
        set,
        **core.descriptor_layouts.Resolve(core.storage_layout),
        {},
        slice<registry::DescriptorBufferBinding>::from_raw_parts(&binding, usize(1)),
        registry::DescriptorBindingReuse::Shared);
    if (result.is_err()) return Err(backend_error("storage binding"_str, result.unwrap_err()));
    return Ok(result.unwrap());
}

void record_geometry(ModelFrameResources&                    resources,
                     const ModelDraw&                        draw,
                     VkCommandBuffer                         cmd,
                     VkPipeline                              pipeline,
                     registry::DescriptorBindingRecordState& state,
                     const Mat4*                             view = nullptr) {
    auto&              core     = *resources.prepared->core;
    const auto&        dispatch = core.session.device().handle().Dispatch();
    vvk::CommandBuffer command(cmd, dispatch);
    auto               layout = *core.layout->physical->layout;
    const auto& gpu = at(at(resources.prepared->meshes, draw.mesh).primitives, draw.primitive);
    dispatch.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    at(resources.prepared->materials, draw.material).descriptor.Record(command, layout, state);
    gpu.morph_binding->Record(command, layout, state);
    auto constants = draw.constants;
    if (view) constants.view = *view;
    dispatch.vkCmdPushConstants(
        cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
    auto buffer = gpu.vertices.buffer();
    auto offset = gpu.vertices.offset();
    dispatch.vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    if (gpu.indices) {
        dispatch.vkCmdBindIndexBuffer(
            cmd, gpu.indices->buffer(), gpu.indices->offset(), VK_INDEX_TYPE_UINT32);
        dispatch.vkCmdDrawIndexed(cmd, gpu.index_count, 1, 0, 0, 0);
    } else
        dispatch.vkCmdDraw(cmd, gpu.vertex_count, 1, 0, 0);
}
constexpr vrento::vulkan::GraphSerial shadow_kind = 0x5647434c54464402ull;
constexpr vrento::vulkan::GraphSerial model_kind  = 0x5647434c54464401ull;
struct ModelPrepared {
    Arc<ModelFrameData>                        inputs;
    Arc<Vec<registry::PipelineResourceResult>> pipelines;
    auto Record(vk::GraphRecordContext& context) const -> Result<empty, vk::GraphExecutionError> {
        auto& core   = *inputs->resources->prepared->core;
        auto  native = vk::native_recording(context.submission, core.session);
        if (native.is_err())
            return Err(vk::GraphExecutionError {
                vk::GraphExecutionStage::Record,
                {},
                {},
                vrento::vulkan::GraphExecutionCause::Texture(native.unwrap_err()) });
        auto                                   cmd = native.unwrap();
        const auto&                            d   = core.session.device().handle().Dispatch();
        vvk::CommandBuffer                     command(cmd, d);
        auto                                   layout = *core.layout->physical->layout;
        registry::DescriptorBindingRecordState binding_state;
        const auto& frame_binding = inputs->coverage ? inputs->resources->coverage_frame_binding
                                                     : inputs->resources->frame_binding;
        frame_binding->Record(command, layout, binding_state);
        if (inputs->background) {
            d.vkCmdBindPipeline(cmd,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                *(*pipelines)[usize(unsigned(ModelPipeline::Background))]
                                     .pipeline->pipeline.handle);
            d.vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        inputs->resources->pose_binding->Record(command, layout, binding_state);
        auto record = [&](const ModelDraw& draw, ModelPipeline pipeline) {
            record_geometry(*inputs->resources,
                            draw,
                            cmd,
                            *(*pipelines)[usize(unsigned(pipeline))].pipeline->pipeline.handle,
                            binding_state);
        };
        for (const auto& draw : *inputs->draws)
            if (! draw.transparent)
                record(draw,
                       inputs->reversed_depth ? ModelPipeline::ReversedOpaque
                                              : ModelPipeline::Opaque);
        // Opaque texels in BLEND meshes need depth before any fractional-alpha color.
        for (const auto& draw : *inputs->draws)
            if (draw.transparent)
                record(draw,
                       inputs->reversed_depth ? ModelPipeline::ReversedBlendOpaque
                                              : ModelPipeline::BlendOpaque);
        for (const auto& draw : *inputs->draws)
            if (draw.transparent)
                record(draw,
                       inputs->reversed_depth ? ModelPipeline::ReversedBlend
                                              : ModelPipeline::Blend);
        return Ok(empty {});
    }
};
struct ModelPassDesc {
    using PassDescriptor = ModelPassDesc;
    vk::VulkanPassIO           io;
    vk::RenderPassResourceDesc signature;
};
struct ModelPass {
    using FrameData = ModelFrameData;
    Arc<RenderCore>                            core;
    Arc<Vec<registry::PipelineResourceResult>> pipelines;
    auto Instantiate(const vk::GraphPrepareContext& context, Arc<FrameData> frame) const
        -> Result<vrento::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        if (! context.scope)
            return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Schema });
        return Ok(
            Arc<dyn<vk::VulkanPass>>::make(ModelPrepared { rstd::move(frame), pipelines.clone() }));
    }
};
struct ModelPassFactory {
    using Pass = ModelPass;
    Arc<RenderCore> core;
    auto            Compile(const ModelPassDesc&         desc,
                            const vk::PassExecutionInfo& execution,
                            const vk::PassCompileContext&)
        -> Result<vrento::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        Vec<registry::PipelineResourceResult> pipelines;
        for (unsigned i = 0; i < unsigned(ModelPipeline::Shadow); ++i) {
            auto prepared = core->preparation->prepare(core->recipes[usize(i)], desc.signature);
            if (prepared.is_err())
                return Err(vk::GraphExecutionError {
                    vk::GraphExecutionStage::Prepare,
                    execution.id,
                    {},
                    vk::GraphExecutionCause::Resource(rstd::move(prepared).unwrap_err()) });
            auto value = prepared.unwrap();
            if (value.error)
                return Err(vk::GraphExecutionError {
                    vk::GraphExecutionStage::Prepare,
                    execution.id,
                    {},
                    vk::GraphExecutionCause::Resource(value.error.take().unwrap()) });
            if (value.status == registry::PipelineReadiness::Pending)
                return Ok(vrento::PendingPreparation {});
            if (! value.pipeline)
                return Err(
                    vk::GraphExecutionError { vk::GraphExecutionStage::Prepare, execution.id });
            pipelines.push(value.pipeline.take().unwrap());
        }
        return Ok(Arc<dyn<vk::VulkanPass>>::make(
            ModelPass { core.clone(),
                        Arc<Vec<registry::PipelineResourceResult>>::make(rstd::move(pipelines)) }));
    }
};
struct ShadowPrepared {
    Arc<ModelFrameData>                   inputs;
    Arc<registry::PipelineResourceResult> pipeline;
    auto Record(vk::GraphRecordContext& context) const -> Result<empty, vk::GraphExecutionError> {
        auto& resources = *inputs->resources;
        auto& core      = *resources.prepared->core;
        auto  native    = vk::native_recording(context.submission, core.session);
        if (native.is_err())
            return Err(
                vk::GraphExecutionError { vk::GraphExecutionStage::Record,
                                          {},
                                          {},
                                          vk::GraphExecutionCause::Texture(native.unwrap_err()) });
        auto                                   cmd      = native.unwrap();
        const auto&                            dispatch = core.session.device().handle().Dispatch();
        vvk::CommandBuffer                     command(cmd, dispatch);
        auto                                   layout = *core.layout->physical->layout;
        registry::DescriptorBindingRecordState state;
        resources.pose_binding->Record(command, layout, state);
        for (const auto& shadow : resources.shadows) {
            VkViewport viewport { float(shadow.view.x),
                                  float(shadow.view.y),
                                  float(shadow.view.resolution),
                                  float(shadow.view.resolution),
                                  0,
                                  1 };
            VkRect2D   scissor { { int(shadow.view.x), int(shadow.view.y) },
                                 { shadow.view.resolution, shadow.view.resolution } };
            dispatch.vkCmdSetViewport(cmd, 0, 1, &viewport);
            dispatch.vkCmdSetScissor(cmd, 0, 1, &scissor);
            shadow.binding->Record(command, layout, state);
            for (const auto& draw : *inputs->draws)
                record_geometry(resources,
                                draw,
                                cmd,
                                *pipeline->pipeline->pipeline.handle,
                                state,
                                &shadow.view.view);
        }
        return Ok(empty {});
    }
};
struct ShadowPass {
    using FrameData = ModelFrameData;
    Arc<registry::PipelineResourceResult> pipeline;
    auto Instantiate(const vk::GraphPrepareContext& context, Arc<FrameData> frame) const
        -> Result<vrento::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        if (! context.scope)
            return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Schema });
        return Ok(
            Arc<dyn<vk::VulkanPass>>::make(ShadowPrepared { rstd::move(frame), pipeline.clone() }));
    }
};
struct ShadowPassDesc {
    using PassDescriptor = ShadowPassDesc;
    vk::VulkanPassIO           io;
    vk::RenderPassResourceDesc signature;
};
struct ShadowPassFactory {
    using Pass = ShadowPass;
    Arc<RenderCore> core;
    auto            Compile(const ShadowPassDesc&        desc,
                            const vk::PassExecutionInfo& execution,
                            const vk::PassCompileContext&)
        -> Result<vrento::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        auto prepared = core->preparation->prepare(
            core->recipes[usize(unsigned(ModelPipeline::Shadow))], desc.signature);
        if (prepared.is_err())
            return Err(vk::GraphExecutionError {
                vk::GraphExecutionStage::Prepare,
                execution.id,
                {},
                vk::GraphExecutionCause::Resource(prepared.unwrap_err()) });
        auto value = prepared.unwrap();
        if (value.error)
            return Err(vk::GraphExecutionError {
                vk::GraphExecutionStage::Prepare,
                execution.id,
                {},
                vk::GraphExecutionCause::Resource(value.error.take().unwrap()) });
        if (value.status == registry::PipelineReadiness::Pending)
            return Ok(vrento::PendingPreparation {});
        if (! value.pipeline)
            return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Prepare, execution.id });
        return Ok(Arc<dyn<vk::VulkanPass>>::make(ShadowPass {
            Arc<registry::PipelineResourceResult>::make(value.pipeline.take().unwrap()) }));
    }
};
struct ModelScopes {
    Arc<RenderCore> core;
    auto            Prepare(const vrento::vulkan::GraphicsDescription& description,
                            const vk::GraphPrepareContext&             context)
        -> Result<vrento::vulkan::Preparation<Arc<dyn<vk::PreparedGraphicsScope>>>,
                  vk::GraphExecutionError> {
        return core->scopes.Prepare(description, context);
    }
};
} // namespace vecgltf

namespace rstd
{
template<>
struct Impl<vrento::vulkan::GraphicsScopeBackend, vecgltf::ModelScopes>
    : ImplBase<vecgltf::ModelScopes> {
    auto Prepare(const vrento::vulkan::GraphicsDescription& description,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<
            vrento::vulkan::Preparation<sync::Arc<dyn<vrento::vulkan::PreparedGraphicsScope>>>,
            vrento::vulkan::GraphExecutionError> {
        return self().Prepare(description, context);
    }
};
} // namespace rstd
export namespace vecgltf
{
enum class DepthEncoding
{
    Forward,
    Reversed
};
struct RenderTarget {
    vrento::texture::TextureViewRef         color;
    Option<vrento::texture::TextureViewRef> depth;
    vrento::vulkan::AttachmentLoad          color_load { vrento::vulkan::AttachmentLoad::Clear };
    vrento::vulkan::AttachmentLoad          depth_load { vrento::vulkan::AttachmentLoad::Clear };
    // None selects Reversed for owned depth, Forward for supplied depth; Load must match its producer.
    // View.projection always uses the forward [0,1] convention.
    Option<DepthEncoding> depth_encoding;
    // record_scene clears this sampled RGBA16F view and writes geometry coverage to alpha.
    // Its extent matches color; the environment background contributes zero coverage.
    Option<vrento::texture::TextureViewRef> coverage;
};
struct PrepareOptions {
    // Upper bound for mipmapped linear samplers, limited by enabled device capabilities.
    float max_anisotropy { 1 };
};
class PreparedAsset {
    friend class Renderer;
    Arc<PreparedData> data_;
    explicit PreparedAsset(Arc<PreparedData> data): data_(rstd::move(data)) {}

public:
    PreparedAsset(const PreparedAsset& other): data_(other.data_.clone()) {}
    PreparedAsset(PreparedAsset&&)                    = default;
    auto operator=(PreparedAsset&&) -> PreparedAsset& = default;
    u64  generation() const { return data_->asset->generation; }
};
struct RendererStatistics {
    rstd::uint64_t prepared_assets {}, asset_buffer_bytes {}, environment_preparations {},
        target_rebuilds {}, recorded_frames {};
};
class Renderer {
    Arc<RenderCore>                         core_;
    Option<Arc<PreparedEnvironment>>        environment_;
    Option<vrento::texture::TextureViewRef> depth_;
    VkExtent2D                              extent_ {};
    RendererStatistics                      statistics_;
    Option<vk::Renderer>                    compiler_;
    struct GraphShape {
        struct Resource {
            vrento::fg::ResourceId   id;
            vrento::fg::ResourceKind kind;
            rstd::uint64_t           bytes {};
            unsigned                 mips { 1 }, layers { 1 };
            bool                     imported { true };
            VkFormat                 format { VK_FORMAT_UNDEFINED };
            unsigned                 width {}, height {}, aspects { 1 };
            friend bool              operator==(const Resource&, const Resource&) = default;
        };
        Vec<Resource>              resources;
        Vec<vk::PassExecutionInfo> passes;
        friend bool                operator==(const GraphShape&, const GraphShape&) = default;
    };
    Option<GraphShape> graph_shape_;
    explicit Renderer(Arc<RenderCore> core): core_(rstd::move(core)) {}

    auto upload_environment(EnvironmentImages source) -> Result<empty, Error> {
        if (! valid_environment_image(source.background) ||
            ! valid_environment_image(source.diffuse) || ! valid_environment_image(source.brdf) ||
            source.specular.is_empty() || ! rstd::f32(source.specular_lod_scale).is_finite() ||
            (source.specular_lod_scale < 0 && source.specular_lod_scale != -1) ||
            ! rstd::f32(source.specular_lod_exponent).is_finite() ||
            source.specular_lod_exponent <= 0 ||
            (source.brdf_filter != vrento::TextureFilter::LINEAR &&
             source.brdf_filter != vrento::TextureFilter::NEAREST))
            return Err(render_error("environment"_str,
                                    "invalid environment images or sampling"_str,
                                    ErrorKind::InvalidAsset));
        unsigned width = at(source.specular, 0).width, height = at(source.specular, 0).height;
        unsigned levels = 1, dimension = width > height ? width : height;
        for (; dimension > 1; dimension /= 2) ++levels;
        if (count(source.specular) > levels)
            return Err(render_error(
                "environment"_str, "too many specular mip levels"_str, ErrorKind::InvalidAsset));
        for (const auto& mip : source.specular) {
            if (! valid_environment_image(mip) || mip.width != width || mip.height != height)
                return Err(render_error(
                    "environment"_str, "invalid specular mip chain"_str, ErrorKind::InvalidAsset));
            width  = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }
        auto environment     = Arc<PreparedEnvironment>::make(core_.clone());
        environment->max_lod = float(count(source.specular) - 1);
        environment->lod_scale =
            source.specular_lod_scale == -1 ? environment->max_lod : source.specular_lod_scale;
        environment->lod_exponent = source.specular_lod_exponent;
        auto add                  = [&](Vec<HdrImage>         images,
                                        bool                  wrap = true,
                                        vrento::TextureFilter filter =
                                            vrento::TextureFilter::LINEAR) -> Result<empty, Error> {
            auto result = upload_environment_image(
                dyn<vrento::texture::TextureDevice>::from_ref(*core_->image_device).as_mut_ref(),
                rstd::move(images),
                wrap,
                core_->trace,
                filter);
            if (result.is_err()) return Err(texture_error(result.unwrap_err()));
            environment->textures.push(Arc<vrento::texture::PreparedImage>::make(result.unwrap()));
            return Ok(empty {});
        };
        auto specular = add(rstd::move(source.specular));
        if (specular.is_err()) return specular;
        Vec<HdrImage> diffuse;
        diffuse.push(rstd::move(source.diffuse));
        Vec<HdrImage> brdf;
        brdf.push(rstd::move(source.brdf));
        Vec<HdrImage> background;
        background.push(rstd::move(source.background));
        auto diffuse_ready = add(rstd::move(diffuse));
        if (diffuse_ready.is_err()) return diffuse_ready;
        auto brdf_ready = add(rstd::move(brdf), false, source.brdf_filter);
        if (brdf_ready.is_err()) return brdf_ready;
        auto background_ready = add(rstd::move(background));
        if (background_ready.is_err()) return background_ready;
        environment_ = Some(rstd::move(environment));
        ++statistics_.environment_preparations;
        return Ok(empty {});
    }

public:
    Renderer(const Renderer&)               = delete;
    Renderer(Renderer&&)                    = default;
    auto operator=(Renderer&&) -> Renderer& = delete;
    ~Renderer() { close(); }
    static auto create(vk::DeviceSession           session,
                       VkExtent2D                  extent,
                       vk::PipelinePreparationMode mode  = vk::PipelinePreparationMode::Synchronous,
                       LoadTrace                   trace = {}) -> Result<Renderer, Error> {
        LoadScope scope(trace, LoadEvent::RendererCreate);
        if (! session.valid()) return Err(render_error("create"_str, "invalid device"_str));
        auto core        = Arc<RenderCore>::make(rstd::move(session), rstd::move(trace));
        auto initialized = initialize(*core, mode);
        if (initialized.is_err()) return Err(initialized.unwrap_err());
        Renderer result(rstd::move(core));
        auto     environment = result.set_environment(default_environment());
        if (environment.is_err()) return Err(environment.unwrap_err());
        auto resized = result.resize(extent);
        if (resized.is_err()) return Err(resized.unwrap_err());
        return Ok(rstd::move(result));
    }
    const RendererStatistics& statistics() const { return statistics_; }
    auto                      graph_statistics() const -> vk::RendererStatistics {
        return compiler_ ? compiler_->statistics() : vk::RendererStatistics {};
    }
    auto upload_statistics() const -> vk::BufferUploadStatistics {
        return core_->buffers.statistics();
    }
    auto set_environment(HdrImage source) -> Result<empty, Error> {
        LoadScope scope(core_->trace, LoadEvent::Environment);
        if (! valid_environment_image(source))
            return Err(
                render_error("environment"_str, "invalid HDR image"_str, ErrorKind::InvalidAsset));
        return upload_environment(preprocess_environment(rstd::move(source), core_->trace));
    }
    auto set_environment(EnvironmentImages source) -> Result<empty, Error> {
        LoadScope scope(core_->trace, LoadEvent::Environment);
        return upload_environment(rstd::move(source));
    }
    auto prepare(Arc<Asset> asset, PrepareOptions options = {}) -> Result<PreparedAsset, Error> {
        if (! rstd::f32(options.max_anisotropy).is_finite() || options.max_anisotropy < 1)
            return Err(render_error("prepare options"_str,
                                    "anisotropy must be finite and at least one"_str,
                                    ErrorKind::InvalidAsset));
        LoadScope scope(core_->trace, LoadEvent::PrepareAsset);
        auto      data   = Arc<PreparedData>::make(core_.clone(), rstd::move(asset));
        auto      upload = [&](const void*           bytes,
                               rstd::size_t          size,
                               vk::BufferUploadClass usage) -> Option<vk::BufferAllocation> {
            auto alignment  = usage == vk::BufferUploadClass::Uniform
                                  ? core_->session.device().limits().minUniformBufferOffsetAlignment
                              : usage == vk::BufferUploadClass::Storage
                                  ? core_->session.device().limits().minStorageBufferOffsetAlignment
                                  : 4;
            auto allocation = core_->buffers.Allocate({ size, alignment, usage });
            if (allocation.is_none()) return None();
            auto source = slice<rstd::uint8_t>::from_raw_parts(
                static_cast<const rstd::uint8_t*>(bytes), usize(size));
            if (core_->buffers.QueueWrite(*allocation, source).is_none()) return None();
            statistics_.asset_buffer_bytes += size;
            return allocation;
        };
        auto texture = [&](unsigned index, bool srgb) -> Result<PreparedTexture, Error> {
            for (const auto& existing : data->textures)
                if (existing.texture == index && existing.srgb == srgb)
                    return Ok(PreparedTexture(existing));
            unsigned image_index =
                index == invalid_index ? invalid_index : at(data->asset->textures, index).image;
            Option<Arc<PreparedTextureImage>> image;
            auto device = dyn<vrento::texture::TextureDevice>::from_ref(*core_->image_device);
            for (auto& existing : data->images)
                if (existing.image == image_index && existing.srgb == srgb)
                    image = Some(existing.source.clone());
            if (! image) {
                Image white;
                white.width = white.height = 1;
                white.rgba.resize(usize(4), u8(255));
                auto prepared = prepare_texture(
                    core_->session,
                    device.as_mut_ref(),
                    image_index == invalid_index ? white : at(data->asset->images, image_index),
                    srgb,
                    core_->trace);
                if (prepared.is_err()) return Err(texture_error(prepared.unwrap_err()));
                image = Some(Arc<PreparedTextureImage>::make(prepared.unwrap()));
                data->images.push({ image_index, srgb, image->clone() });
            }
            vrento::texture::SamplerDesc description { texture_sample(
                index == invalid_index ? Sampler {} : at(data->asset->textures, index).sampler) };
            if (description.sample.minFilter == vrento::TextureFilter::LINEAR &&
                description.sample.magFilter == vrento::TextureFilter::LINEAR &&
                description.sample.mipFilter == vrento::TextureMipmapFilter::Linear)
                description.max_anisotropy =
                    minimum(options.max_anisotropy,
                            core_->session.device().capabilities().max_sampler_anisotropy);
            auto sampler = device->CreateSampler(description);
            if (sampler.is_err()) return Err(texture_error(sampler.unwrap_err()));
            PreparedTexture result { index, srgb, (*image)->image.view, sampler.unwrap() };
            data->textures.push(PreparedTexture(result));
            return Ok(rstd::move(result));
        };
        LoadScope materials(core_->trace, LoadEvent::PrepareMaterials);
        for (const auto& material : data->asset->materials) {
            bool specular_glossiness = material.workflow == MaterialWorkflow::SpecularGlossiness;
            const TextureBinding* bindings[] { &material.base,
                                               &material.physical,
                                               &material.normal,
                                               &material.occlusion,
                                               &material.emission };
            MaterialParameters    parameters {};
            parameters.base     = material.base_color;
            parameters.emissive = {
                material.emissive.x, material.emissive.y, material.emissive.z, 0
            };
            parameters.factors  = { material.metallic,
                                    material.roughness,
                                    material.normal.strength,
                                    material.occlusion.strength };
            parameters.specular = {
                material.specular.x, material.specular.y, material.specular.z, material.glossiness
            };
            parameters.flags = { float(unsigned(material.alpha)),
                                 material.alpha_cutoff,
                                 material.double_sided ? 1.0f : 0.0f,
                                 float((material.unlit ? 1u : 0u) |
                                       (specular_glossiness ? 2u : 0u)) };
            registry::DescriptorImageBinding images[5];
            for (unsigned i = 0; i < 5; ++i) {
                const auto& binding        = *bindings[i];
                parameters.offset_scale[i] = {
                    binding.offset.x, binding.offset.y, binding.scale.x, binding.scale.y
                };
                parameters.rotation_uv[i] = { cosine(binding.rotation),
                                              sine(binding.rotation),
                                              0,
                                              binding.texture == invalid_index ? 0.0f : 1.0f };
                auto image =
                    texture(binding.texture, i == 0 || i == 4 || (i == 1 && specular_glossiness));
                if (image.is_err()) return Err(image.unwrap_err());
                auto sampled = vk::sampled_image(image->view, image->sampler).unwrap();
                images[i]    = { i, sampled.image, sampled.layout };
            }
            auto buffer = upload(&parameters, sizeof(parameters), vk::BufferUploadClass::Uniform);
            if (buffer.is_none())
                return Err(render_error("prepare material"_str, "parameter allocation failed"_str));
            registry::DescriptorBufferBinding buffer_binding {
                5, buffer->buffer(), buffer->offset(), sizeof(parameters)
            };
            auto descriptor = core_->descriptors.Prepare(
                core_->session.device(),
                1,
                **core_->descriptor_layouts.Resolve(core_->material_layout),
                slice<registry::DescriptorImageBinding>::from_raw_parts(images, usize(5)),
                slice<registry::DescriptorBufferBinding>::from_raw_parts(&buffer_binding, usize(1)),
                registry::DescriptorBindingReuse::Shared);
            if (descriptor.is_err())
                return Err(backend_error("prepare material"_str, descriptor.unwrap_err()));
            data->materials.push({ rstd::move(buffer).unwrap(), descriptor.unwrap() });
        }
        materials.finish();
        LoadScope geometry(core_->trace, LoadEvent::PrepareGeometry);
        for (const auto& mesh : data->asset->meshes) {
            PreparedMesh prepared;
            for (const auto& primitive : mesh.primitives) {
                const auto&           material = at(data->asset->materials, primitive.material);
                const TextureBinding* bindings[] { &material.base,
                                                   &material.physical,
                                                   &material.normal,
                                                   &material.occlusion,
                                                   &material.emission };
                Vec<ModelVertex>      lowered;
                lowered.resize(primitive.vertices.len(), ModelVertex {});
                for (unsigned v = 0; v < count(primitive.vertices); ++v) {
                    const auto& source = at(primitive.vertices, v);
                    auto&       output = at(lowered, v);
                    output.position    = source.position;
                    output.normal      = source.normal;
                    output.color       = source.color;
                    output.tangent     = source.tangent;
                    output.weights     = source.weights;
                    for (unsigned i = 0; i < 4; ++i) output.joints[i] = source.joints[i];
                    for (unsigned slot = 0; slot < 5; ++slot) {
                        const auto& binding = *bindings[slot];
                        if (binding.texture == invalid_index) continue;
                        auto coord = primitive.texcoord(binding.uv, v);
                        if (coord.is_none())
                            return Err(
                                render_error("prepare texcoords"_str,
                                             "material references a missing texcoord set"_str,
                                             ErrorKind::InvalidAsset));
                        output.texcoords[slot] = coord.unwrap();
                    }
                }
                auto vertices = upload(lowered.data(),
                                       count(lowered) * sizeof(ModelVertex),
                                       vk::BufferUploadClass::Vertex);
                if (vertices.is_none())
                    return Err(render_error("prepare vertices"_str,
                                            "buffer allocation or upload failed"_str));
                PreparedPrimitive p { rstd::move(vertices).unwrap(),
                                      None(),
                                      unsigned(count(primitive.vertices)),
                                      unsigned(count(primitive.indices)) };
                if (primitive.indexed) {
                    p.indices = upload(primitive.indices.data(),
                                       count(primitive.indices) * sizeof(unsigned),
                                       vk::BufferUploadClass::Index);
                    if (p.indices.is_none())
                        return Err(render_error("prepare indices"_str,
                                                "buffer allocation or upload failed"_str));
                }
                if (! primitive.morphs.is_empty()) {
                    Vec<Vec4> deltas;
                    for (const auto& morph : primitive.morphs)
                        for (unsigned v = 0; v < count(primitive.vertices); ++v) {
                            auto position = at(morph.positions, v), normal = at(morph.normals, v),
                                 tangent = at(morph.tangents, v);
                            deltas.push({ position.x, position.y, position.z, 0 });
                            deltas.push({ normal.x, normal.y, normal.z, 0 });
                            deltas.push({ tangent.x, tangent.y, tangent.z, 0 });
                        }
                    auto bytes = count(deltas) * sizeof(Vec4);
                    if (bytes > core_->session.device().limits().maxStorageBufferRange)
                        return Err(render_error("morph"_str,
                                                "morph data exceeds device storage range"_str,
                                                ErrorKind::Limit));
                    p.morphs = upload(deltas.data(), bytes, vk::BufferUploadClass::Storage);
                    if (p.morphs.is_none())
                        return Err(render_error("morph"_str, "morph upload failed"_str));
                }
                auto morph_binding =
                    bind_storage(*core_, p.morphs.is_some() ? *p.morphs : *core_->zero_buffer, 3);
                if (morph_binding.is_err()) return Err(morph_binding.unwrap_err());
                p.morph_binding = Some(morph_binding.unwrap());
                prepared.primitives.push(rstd::move(p));
            }
            data->meshes.push(rstd::move(prepared));
        }
        geometry.finish();
        ++statistics_.prepared_assets;
        return Ok(PreparedAsset(rstd::move(data)));
    }
    auto resize(VkExtent2D extent) -> Result<empty, Error> {
        if (! extent.width || ! extent.height)
            return Err(render_error("resize"_str, "zero extent"_str));
        if (extent.width == extent_.width && extent.height == extent_.height) return Ok(empty {});
        vrento::texture::TextureDesc desc;
        desc.extent = { extent.width, extent.height };
        desc.format = vrento::TextureFormat::D32F;
        desc.usage  = vrento::texture::Usage::DepthAttachment;
        auto depth  = core_->image_device->CreateTexture(desc);
        if (depth.is_err()) return Err(texture_error(depth.unwrap_err()));
        auto depth_view = core_->image_device->CreateView(
            depth.unwrap(), { vrento::texture::Dimension::D2, { vrento::texture::Aspect::Depth } });
        if (depth_view.is_err()) return Err(texture_error(depth_view.unwrap_err()));
        depth_  = Some(depth_view.unwrap());
        extent_ = extent;
        ++statistics_.target_rebuilds;
        return Ok(empty {});
    }

private:
    auto validate_coverage(const RenderTarget& destination, bool linear_scene)
        -> Result<empty, Error> {
        if (! destination.coverage) return Ok(empty {});
        auto invalid = []() {
            return render_error(
                "coverage target"_str, "invalid scene coverage view"_str, ErrorKind::InvalidAsset);
        };
        const auto& coverage = *destination.coverage;
        if (! linear_scene || ! destination.color.valid() || ! coverage.valid() ||
            destination.color.texture().identity().epoch != core_->session.epoch() ||
            ! destination.color.permits(true) ||
            coverage.texture().identity().epoch != core_->session.epoch() ||
            ! coverage.permits(true) ||
            coverage.texture().identity() == destination.color.texture().identity())
            return Err(invalid());
        const auto& color_desc  = destination.color.texture().description();
        const auto& color_range = destination.color.description().range;
        if (color_desc.format != vrento::TextureFormat::RGBA16F || color_desc.samples != 1 ||
            destination.color.description().dimension != vrento::texture::Dimension::D2 ||
            color_range.aspect != vrento::texture::Aspect::Color || color_range.mip_count != 1 ||
            color_range.layer_count != 1 || color_range.first_mip >= color_desc.mip_levels ||
            color_range.first_layer >= color_desc.layers ||
            ! vrento::texture::has(color_desc.usage,
                                   vrento::texture::Usage::ColorAttachment |
                                       vrento::texture::Usage::Sampled))
            return Err(invalid());
        const auto& desc  = coverage.texture().description();
        const auto& range = coverage.description().range;
        if (desc.format != vrento::TextureFormat::RGBA16F || desc.samples != 1 ||
            coverage.description().dimension != vrento::texture::Dimension::D2 ||
            range.aspect != vrento::texture::Aspect::Color || range.mip_count != 1 ||
            range.layer_count != 1 || range.first_mip >= desc.mip_levels ||
            range.first_layer >= desc.layers ||
            ! vrento::texture::has(desc.usage,
                                   vrento::texture::Usage::ColorAttachment |
                                       vrento::texture::Usage::Sampled))
            return Err(invalid());
        auto size = vrento::texture::mip_extent(destination.color.texture().description(),
                                                destination.color.description().range.first_mip);
        if (vrento::texture::mip_extent(desc, range.first_mip) != size) return Err(invalid());
        if (destination.depth) {
            const auto& depth = *destination.depth;
            if (! depth.valid() || ! depth.permits(true) ||
                depth.texture().identity().epoch != core_->session.epoch() ||
                depth.texture().identity() == coverage.texture().identity())
                return Err(invalid());
            const auto& depth_desc  = depth.texture().description();
            const auto& depth_range = depth.description().range;
            if (depth.description().dimension != vrento::texture::Dimension::D2 ||
                depth_desc.samples != 1 || depth_range.mip_count != 1 ||
                depth_range.layer_count != 1 || depth_range.first_mip >= depth_desc.mip_levels ||
                depth_range.first_layer >= depth_desc.layers ||
                depth_range.aspect != vrento::texture::Aspect::Depth ||
                ! vrento::texture::has(depth_desc.usage, vrento::texture::Usage::DepthAttachment) ||
                vrento::texture::mip_extent(depth_desc, depth_range.first_mip) != size)
                return Err(invalid());
        }
        return Ok(empty {});
    }
    auto prepare_shadows(const LightingParameters& lights, bool retry)
        -> Result<registry::PipelineReadiness, Error> {
        if (lights.shadows.is_empty()) return Ok(registry::PipelineReadiness::Ready);
        Bounds bounds;
        bounds.include({});
        auto plan = shadow::BuildShadowPlan(lights.shadows.as_slice(),
                                            bounds,
                                            core_->session.device().limits().maxImageDimension2D);
        if (plan.is_err()) return Err(plan.unwrap_err());
        vrento::texture::TextureDesc description;
        description.extent = { plan->width, plan->height };
        description.format = vrento::TextureFormat::D32F;
        description.usage =
            vrento::texture::Usage::DepthAttachment | vrento::texture::Usage::Sampled;
        auto supported = core_->image_device->QuerySupport(description);
        if (supported.is_err()) return Err(texture_error(supported.unwrap_err()));
        vk::RenderPassResourceDesc signature;
        signature.has_color_attachment = false;
        signature.has_depth_attachment = true;
        signature.depth_format         = VK_FORMAT_D32_SFLOAT;
        auto prepared                  = core_->preparation->prepare(
            core_->recipes[usize(unsigned(ModelPipeline::Shadow))], signature, retry);
        if (prepared.is_err())
            return Err(backend_error("shadow preparation"_str, prepared.unwrap_err()));
        auto value = prepared.unwrap();
        if (value.error)
            return Err(backend_error("shadow preparation"_str, value.error.take().unwrap()));
        if (value.status == registry::PipelineReadiness::Failed)
            return Err(render_error("shadow preparation"_str, "pipeline preparation failed"_str));
        if (value.pipeline) {
            bool retained {};
            for (const auto& cached : core_->warmed)
                retained |= cached.handle == value.pipeline->handle;
            if (! retained) core_->warmed.push(value.pipeline.take().unwrap());
        }
        return Ok(value.status);
    }
    auto prepare_target_impl(const RenderTarget&       destination,
                             bool                      retry,
                             bool                      linear_scene,
                             const LightingParameters* lighting = nullptr)
        -> Result<registry::PipelineReadiness, Error> {
        auto coverage_valid = validate_coverage(destination, linear_scene);
        if (coverage_valid.is_err()) return Err(coverage_valid.unwrap_err());
        auto target = destination.color;
        if (! target.valid() || target.texture().identity().epoch != core_->session.epoch())
            return Err(render_error("prepare target"_str, "invalid target"_str));
        const auto& desc = target.texture().description();
        if ((linear_scene ? desc.format != vrento::TextureFormat::RGBA16F
                          : (desc.format != vrento::TextureFormat::RGBA8 &&
                             desc.format != vrento::TextureFormat::RGBA8_SRGB)) ||
            desc.samples != 1)
            return Err(render_error("prepare target"_str, "unsupported target"_str));
        vk::RenderPassResourceDesc signature;
        signature.color_format         = vk::ToVkType(desc.format);
        signature.has_depth_attachment = true;
        if (destination.depth)
            signature.depth_format =
                vk::ToVkType(destination.depth->texture().description().format);
        auto status = registry::PipelineReadiness::Ready;
        if (lighting) {
            auto ready = prepare_shadows(*lighting, retry);
            if (ready.is_err()) return Err(ready.unwrap_err());
            status = ready.unwrap();
        }
        for (unsigned i = 0; i < unsigned(ModelPipeline::Shadow); ++i) {
            auto result = core_->preparation->prepare(core_->recipes[usize(i)], signature, retry);
            if (result.is_err())
                return Err(backend_error("prepare target"_str, result.unwrap_err()));
            auto value = result.unwrap();
            if (value.error)
                return Err(backend_error("prepare target"_str, value.error.take().unwrap()));
            if (value.status == registry::PipelineReadiness::Failed)
                return Err(render_error("prepare target"_str, "pipeline preparation failed"_str));
            if (value.pipeline) {
                bool retained {};
                for (const auto& cached : core_->warmed)
                    retained |= cached.handle == value.pipeline->handle;
                if (! retained) core_->warmed.push(value.pipeline.take().unwrap());
            }
            if (value.status == registry::PipelineReadiness::Pending) status = value.status;
        }
        return Ok(status);
    }

public:
    auto prepare_target(const RenderTarget& destination, bool retry = false)
        -> Result<registry::PipelineReadiness, Error> {
        return prepare_target_impl(destination, retry, false);
    }
    auto prepare_scene_target(const RenderTarget& destination, bool retry = false)
        -> Result<registry::PipelineReadiness, Error> {
        return prepare_target_impl(destination, retry, true);
    }
    auto prepare_scene_target(vrento::texture::TextureViewRef target, bool retry = false)
        -> Result<registry::PipelineReadiness, Error> {
        return prepare_scene_target(RenderTarget { rstd::move(target) }, retry);
    }
    auto prepare_target(vrento::texture::TextureViewRef target, bool retry = false)
        -> Result<registry::PipelineReadiness, Error> {
        return prepare_target(RenderTarget { rstd::move(target) }, retry);
    }
    auto prepare_target(const RenderTarget& target, RenderOptions options, bool retry = false)
        -> Result<registry::PipelineReadiness, Error> {
        auto lights = lower_lighting(options,
                                     { Mat4::Identity(), Mat4::Identity(), {} },
                                     core_->session.device().limits().maxImageDimension2D);
        if (lights.is_err()) return Err(lights.unwrap_err());
        return prepare_target_impl(target, retry, false, &*lights);
    }
    auto prepare_scene_target(const RenderTarget& target, SceneOptions options, bool retry = false)
        -> Result<registry::PipelineReadiness, Error> {
        RenderOptions values { 1,
                               options.debug,
                               options.background,
                               options.direct_light,
                               options.ibl_intensity,
                               ToneMapping::Linear,
                               options.directional_lights,
                               options.world_to_environment,
                               options.background_intensity };
        auto          lights = lower_lighting(values,
                                              { Mat4::Identity(), Mat4::Identity(), {} },
                                              core_->session.device().limits().maxImageDimension2D);
        if (lights.is_err()) return Err(lights.unwrap_err());
        return prepare_target_impl(target, retry, true, &*lights);
    }
    auto prepare_scene_target(vrento::texture::TextureViewRef target,
                              SceneOptions                    options,
                              bool retry = false) -> Result<registry::PipelineReadiness, Error> {
        return prepare_scene_target(RenderTarget { rstd::move(target) }, options, retry);
    }
    auto prepare_target(vrento::texture::TextureViewRef target,
                        RenderOptions                   options,
                        bool retry = false) -> Result<registry::PipelineReadiness, Error> {
        return prepare_target(RenderTarget { rstd::move(target) }, options, retry);
    }
    auto record(vrento::texture::Submission&    work,
                vrento::texture::TextureViewRef target,
                const PreparedAsset&            prepared,
                const SceneInstance&            instance,
                const View&                     view,
                RenderOptions                   options = {}) -> Result<empty, Error> {
        return record(work, RenderTarget { rstd::move(target) }, prepared, instance, view, options);
    }
    auto record(vrento::texture::Submission& work,
                RenderTarget                 destination,
                const PreparedAsset&         prepared,
                const SceneInstance&         instance,
                const View&                  view,
                RenderOptions                options = {}) -> Result<empty, Error> {
        return record_impl(work, rstd::move(destination), prepared, instance, view, options, false);
    }
    auto record_scene(vrento::texture::Submission& work,
                      RenderTarget                 destination,
                      const PreparedAsset&         prepared,
                      const SceneInstance&         instance,
                      const View&                  view,
                      SceneOptions                 options = {}) -> Result<empty, Error> {
        RenderOptions values { 1,
                               options.debug,
                               options.background,
                               options.direct_light,
                               options.ibl_intensity,
                               ToneMapping::Linear,
                               options.directional_lights,
                               options.world_to_environment,
                               options.background_intensity };
        return record_impl(work, rstd::move(destination), prepared, instance, view, values, true);
    }
    auto record_scene(vrento::texture::Submission&    work,
                      vrento::texture::TextureViewRef target,
                      const PreparedAsset&            prepared,
                      const SceneInstance&            instance,
                      const View&                     view,
                      SceneOptions                    options = {}) -> Result<empty, Error> {
        return record_scene(
            work, RenderTarget { rstd::move(target) }, prepared, instance, view, options);
    }

private:
    auto record_impl(vrento::texture::Submission& work,
                     RenderTarget                 destination,
                     const PreparedAsset&         prepared,
                     const SceneInstance&         instance,
                     const View&                  view,
                     RenderOptions                options,
                     bool                         linear_scene) -> Result<empty, Error> {
        auto target = destination.color;
        if (! target.valid() || &*prepared.data_->core != &*core_ ||
            prepared.generation() != instance.asset().generation)
            return Err(render_error("render"_str, "asset or renderer identity mismatch"_str));
        auto coverage_valid = validate_coverage(destination, linear_scene);
        if (coverage_valid.is_err()) return coverage_valid;
        if (! work.valid() ||
            work.receipt().status() != vrento::texture::SubmissionStatus::Recording)
            return Err(
                render_error("record"_str, "submission is not recording"_str, ErrorKind::Busy));
        auto queue = vk::native_queue(work, core_->session);
        if (queue.is_err()) return Err(texture_error(queue.unwrap_err()));
        if (core_->latest &&
            (core_->latest->status() == vrento::texture::SubmissionStatus::Recording ||
             core_->latest->uncertain()))
            return Err(render_error("record"_str,
                                    "previous model submission has not been accepted"_str,
                                    ErrorKind::Busy));
        auto non_negative = [](float value) {
            return rstd::f32(value).is_finite() && value >= 0;
        };
        if (! non_negative(options.exposure) || ! non_negative(options.ibl_intensity) ||
            (options.tone_mapping != ToneMapping::ACES &&
             options.tone_mapping != ToneMapping::ReinhardMax &&
             options.tone_mapping != ToneMapping::Linear))
            return Err(render_error("render options"_str,
                                    "invalid exposure, IBL intensity or tone mapping"_str,
                                    ErrorKind::InvalidAsset));
        auto lowered =
            lower_lighting(options, view, core_->session.device().limits().maxImageDimension2D);
        if (lowered.is_err()) return Err(lowered.unwrap_err());
        auto        lights      = lowered.unwrap();
        const auto& target_desc = target.texture().description();
        auto        range       = target.description().range;
        if ((linear_scene ? target_desc.format != vrento::TextureFormat::RGBA16F
                          : (target_desc.format != vrento::TextureFormat::RGBA8 &&
                             target_desc.format != vrento::TextureFormat::RGBA8_SRGB)) ||
            target_desc.samples != 1 || range.mip_count != 1 || range.layer_count != 1)
            return Err(render_error("record"_str, "unsupported target view"_str));
        if (destination.coverage) {
            auto identity = destination.coverage->texture().identity();
            for (const auto& texture : prepared.data_->textures)
                if (texture.view.texture().identity() == identity)
                    return Err(render_error("coverage target"_str,
                                            "coverage aliases an asset texture"_str,
                                            ErrorKind::InvalidAsset));
            for (const auto& image : prepared.data_->images)
                if (image.source->image.view.texture().identity() == identity)
                    return Err(render_error("coverage target"_str,
                                            "coverage aliases an asset image"_str,
                                            ErrorKind::InvalidAsset));
            for (const auto& texture : (*environment_)->textures)
                if (texture->view.texture().identity() == identity)
                    return Err(render_error("coverage target"_str,
                                            "coverage aliases an environment texture"_str,
                                            ErrorKind::InvalidAsset));
        }
        const auto& asset = instance.asset();
        struct Draw {
            unsigned node {}, primitive {};
            float    depth {};
            bool     transparent {};
        };
        Vec<Draw>  draws;
        Bounds     caster_bounds;
        const auto origin = view.eye;
        for (unsigned n = 0; n < count(asset.nodes); ++n) {
            auto mesh_index = at(asset.nodes, n).mesh;
            if (! instance.visible(n) || mesh_index == invalid_index) continue;
            const auto& pose = instance.pose(n);
            if (pose.skin_palette.is_empty() && absolute(pose.world.determinant3()) < 1e-12f)
                continue;
            const auto& mesh = at(asset.meshes, mesh_index);
            for (unsigned p = 0; p < count(mesh.primitives); ++p) {
                const auto& primitive = at(mesh.primitives, p);
                auto        position  = at(instance.pose(n).primitive_bounds, p).center();
                if (! lights.shadows.is_empty() &&
                    (at(asset.materials, primitive.material).alpha != AlphaMode::Blend ||
                     at(asset.materials, primitive.material).base_color.w >= 1)) {
                    auto bounds = instance.relative_primitive_bounds(n, p, origin);
                    if (bounds.valid) {
                        caster_bounds.include(bounds.min);
                        caster_bounds.include(bounds.max);
                    }
                }
                draws.push({ n,
                             p,
                             view_depth(view.view, position),
                             at(asset.materials, primitive.material).alpha == AlphaMode::Blend });
            }
        }
        rstd::slice_::sort_unstable_by(
            draws.as_mut_slice().as_mut_ref(), [](const Draw& a, const Draw& b) {
                if (a.transparent != b.transparent) return ! a.transparent;
                return a.transparent ? a.depth < b.depth : a.depth > b.depth;
            });
        auto planned =
            shadow::BuildShadowPlan(lights.shadows.as_slice(),
                                    caster_bounds,
                                    core_->session.device().limits().maxImageDimension2D);
        if (planned.is_err()) return Err(planned.unwrap_err());
        auto shadow_plan = planned.unwrap();
        auto readiness   = prepare_target_impl(destination, false, linear_scene, &lights);
        if (readiness.is_err()) return Err(readiness.unwrap_err());
        auto status = readiness.unwrap();
        if (status != registry::PipelineReadiness::Ready)
            return Err(render_error("record"_str,
                                    "target pipelines are not ready"_str,
                                    status == registry::PipelineReadiness::Pending
                                        ? ErrorKind::Pending
                                        : ErrorKind::Device));
        auto size = vrento::texture::mip_extent(target_desc, range.first_mip);
        if (destination.depth.is_none()) {
            auto resized = resize({ size.width, size.height });
            if (resized.is_err()) return resized;
        }
        auto depth = destination.depth ? *destination.depth : *depth_;
        bool reversed_depth =
            destination.depth_encoding.unwrap_or(destination.depth ? DepthEncoding::Forward
                                                                   : DepthEncoding::Reversed) ==
            DepthEncoding::Reversed;
        struct RecordingGuard {
            vrento::texture::Submission& work;
            bool                         accepted {};
            ~RecordingGuard() {
                if (! accepted) work.discard();
            }
        } recording { work };
        auto native_command = vk::native_recording(work, core_->session);
        if (native_command.is_err()) return Err(texture_error(native_command.unwrap_err()));
        auto command_buffer = native_command.unwrap();
        auto resources =
            Arc<ModelFrameResources>::make(prepared.data_.clone(), depth, work.receipt());
        resources->environment = Some(environment_->clone());
        resources->coverage    = rstd::move(destination.coverage);
        ModelFrameData inputs {
            resources.clone(), Arc<Vec<ModelDraw>>::make(), options.background, reversed_depth
        };
        const auto relative_view = camera_relative_view(view.view, origin);
        struct PoseOffsets {
            unsigned skin { invalid_index }, weights {};
        };
        Vec<PoseOffsets> pose_offsets;
        Vec<Vec4>        pose_data;
        for (unsigned n = 0; n < count(instance.asset().nodes); ++n) {
            const auto& pose = instance.pose(n);
            PoseOffsets offsets;
            if (! pose.skin_palette.is_empty()) {
                offsets.skin = unsigned(count(pose_data));
                for (const auto& world : pose.skin_palette) {
                    auto matrix = camera_relative_model(world, origin);
                    for (unsigned c = 0; c < 4; ++c)
                        pose_data.push({ matrix.m[c * 4],
                                         matrix.m[c * 4 + 1],
                                         matrix.m[c * 4 + 2],
                                         matrix.m[c * 4 + 3] });
                }
            }
            offsets.weights = unsigned(count(pose_data));
            for (unsigned i = 0; i < count(pose.weights); i += 4) {
                Vec4   weights {};
                float* components = &weights.x;
                for (unsigned c = 0; c < 4 && i + c < count(pose.weights); ++c)
                    components[c] = at(pose.weights, i + c);
                pose_data.push(rstd::move(weights));
            }
            pose_offsets.push(rstd::move(offsets));
        }
        if (pose_data.is_empty()) pose_data.push(Vec4 {});
        auto pose_bytes = count(pose_data) * sizeof(Vec4);
        if (pose_bytes > core_->session.device().limits().maxStorageBufferRange)
            return Err(render_error(
                "pose"_str, "pose data exceeds device storage range"_str, ErrorKind::Limit));
        resources->poses = core_->buffers.Allocate(
            { pose_bytes,
              core_->session.device().limits().minStorageBufferOffsetAlignment,
              vk::BufferUploadClass::Storage });
        if (resources->poses.is_none() ||
            core_->buffers
                .QueueWrite(*resources->poses,
                            slice<rstd::uint8_t>::from_raw_parts(
                                reinterpret_cast<const rstd::uint8_t*>(pose_data.data()),
                                usize(pose_bytes)))
                .is_none())
            return Err(render_error("pose"_str, "pose upload failed"_str));
        auto pose_binding = bind_storage(*core_, *resources->poses, 2);
        if (pose_binding.is_err()) return Err(pose_binding.unwrap_err());
        resources->pose_binding = Some(pose_binding.unwrap());
        const auto&        d    = core_->session.device().handle().Dispatch();
        vvk::CommandBuffer command(command_buffer, d);
        FrameParameters    frame_parameters {};
        frame_parameters.eye     = { 0, 0, 0, 1 };
        frame_parameters.options = { options.exposure,
                                     float(unsigned(options.debug)),
                                     options.background ? 1.0f : 0.0f,
                                     (*environment_)->max_lod };
        frame_parameters.inverse_view_projection =
            (view.projection * relative_view).inverse().unwrap_or(Mat4::Identity());
        frame_parameters.projection =
            reversed_depth ? reversed_projection(view.projection) : view.projection;
        frame_parameters.lighting             = { options.ibl_intensity,
                                                  float(unsigned(options.tone_mapping)),
                                                  float(lights.count),
                                                  options.background_intensity };
        frame_parameters.world_to_environment = options.world_to_environment;
        for (unsigned i = 0; i < lights.count; ++i) {
            frame_parameters.light_directions[i] = lights.directions[i];
            frame_parameters.light_radiances[i]  = lights.radiances[i];
        }
        frame_parameters.environment_lod = {
            (*environment_)->lod_scale, (*environment_)->lod_exponent, 0, 0
        };
        resources->frame_buffer = core_->buffers.Allocate(
            { sizeof(FrameParameters),
              core_->session.device().limits().minUniformBufferOffsetAlignment,
              vk::BufferUploadClass::Uniform });
        if (! resources->frame_buffer)
            return Err(render_error("frame"_str, "frame allocation failed"_str));
        if (core_->buffers
                .QueueWrite(*resources->frame_buffer,
                            slice<rstd::uint8_t>::from_raw_parts(
                                reinterpret_cast<const rstd::uint8_t*>(&frame_parameters),
                                usize(sizeof(frame_parameters))))
                .is_none())
            return Err(render_error("frame"_str, "view parameter upload failed"_str));
        ShadowParameters shadow_parameters {};
        if (! shadow_plan.views.is_empty()) {
            vrento::texture::TextureDesc description;
            description.extent = { shadow_plan.width, shadow_plan.height };
            description.format = vrento::TextureFormat::D32F;
            description.usage =
                vrento::texture::Usage::DepthAttachment | vrento::texture::Usage::Sampled;
            auto texture = core_->image_device->CreateTexture(description);
            if (texture.is_err()) return Err(texture_error(texture.unwrap_err()));
            auto atlas = core_->image_device->CreateView(
                texture.unwrap(),
                { vrento::texture::Dimension::D2, { vrento::texture::Aspect::Depth } });
            if (atlas.is_err()) return Err(texture_error(atlas.unwrap_err()));
            resources->shadow_atlas = Some(atlas.unwrap());
            shadow_parameters.atlas = {
                1.0f / float(shadow_plan.width), 1.0f / float(shadow_plan.height), 0, 0
            };
            for (auto& shadow : shadow_plan.views) {
                unsigned index                      = shadow.light_index;
                shadow_parameters.matrices[index]   = shadow.matrix;
                shadow_parameters.rectangles[index] = {
                    float(shadow.x),
                    float(shadow.y),
                    float(shadow.resolution),
                    shadow.options.filter == ShadowFilter::Bilinear ? 1.0f : 0.0f
                };
                shadow_parameters.biases[index] = { shadow.options.bias,
                                                    shadow.options.slope_bias,
                                                    shadow.options.max_bias,
                                                    shadow.options.normal_bias };
                resources->shadows.push(ShadowFrame { rstd::move(shadow) });
            }
        }
        auto upload_uniform = [&](const auto& values) -> Result<vk::BufferAllocation, Error> {
            auto buffer = core_->buffers.Allocate(
                { sizeof(values),
                  core_->session.device().limits().minUniformBufferOffsetAlignment,
                  vk::BufferUploadClass::Uniform });
            if (! buffer || core_->buffers
                                .QueueWrite(*buffer,
                                            slice<rstd::uint8_t>::from_raw_parts(
                                                reinterpret_cast<const rstd::uint8_t*>(&values),
                                                usize(sizeof(values))))
                                .is_none())
                return Err(render_error("shadow parameters"_str, "uniform upload failed"_str));
            return Ok(buffer.take().unwrap());
        };
        auto shadow_uniform = upload_uniform(shadow_parameters);
        if (shadow_uniform.is_err()) return Err(shadow_uniform.unwrap_err());
        resources->shadow_parameters = Some(shadow_uniform.unwrap());
        registry::DescriptorImageBinding images[5];
        for (unsigned i = 0; i < 4; ++i) {
            auto sampled = vk::sampled_image(at((*resources->environment)->textures, i)->view,
                                             at((*resources->environment)->textures, i)->sampler)
                               .unwrap();
            images[i]    = { i + 1, sampled.image, sampled.layout };
        }
        auto atlas_sampled = vk::sampled_image(
            resources->shadow_atlas ? *resources->shadow_atlas
                                    : at((*resources->environment)->textures, 2)->view,
            *core_->shadow_sampler);
        if (atlas_sampled.is_err()) return Err(texture_error(atlas_sampled.unwrap_err()));
        images[4]       = { 6, atlas_sampled->image, atlas_sampled->layout };
        auto bind_frame = [&](const vk::BufferAllocation& buffer)
            -> Result<registry::PreparedDescriptorBinding, Error> {
            registry::DescriptorBufferBinding bindings[] {
                { 0, buffer.buffer(), buffer.offset(), sizeof(FrameParameters) },
                { 5,
                  resources->shadow_parameters->buffer(),
                  resources->shadow_parameters->offset(),
                  sizeof(ShadowParameters) }
            };
            auto descriptor = core_->descriptors.Prepare(
                core_->session.device(),
                0,
                **core_->descriptor_layouts.Resolve(core_->frame_layout),
                slice<registry::DescriptorImageBinding>::from_raw_parts(images, usize(5)),
                slice<registry::DescriptorBufferBinding>::from_raw_parts(bindings, usize(2)),
                registry::DescriptorBindingReuse::Shared);
            if (descriptor.is_err())
                return Err(backend_error("frame descriptor"_str, descriptor.unwrap_err()));
            return Ok(descriptor.unwrap());
        };
        auto descriptor = bind_frame(*resources->frame_buffer);
        if (descriptor.is_err()) return Err(descriptor.unwrap_err());
        resources->frame_binding = Some(descriptor.unwrap());
        if (resources->coverage) {
            auto values      = frame_parameters;
            values.options.z = 2;
            auto buffer      = upload_uniform(values);
            if (buffer.is_err()) return Err(buffer.unwrap_err());
            resources->coverage_frame_buffer = Some(buffer.unwrap());
            auto binding                     = bind_frame(*resources->coverage_frame_buffer);
            if (binding.is_err()) return Err(binding.unwrap_err());
            resources->coverage_frame_binding = Some(binding.unwrap());
        }
        for (auto& shadow : resources->shadows) {
            auto values       = frame_parameters;
            values.projection = shadow.view.projection;
            auto buffer       = upload_uniform(values);
            if (buffer.is_err()) return Err(buffer.unwrap_err());
            shadow.buffer = Some(buffer.unwrap());
            auto binding  = bind_frame(*shadow.buffer);
            if (binding.is_err()) return Err(binding.unwrap_err());
            shadow.binding = Some(binding.unwrap());
        }
        LoadScope                 uploads(core_->trace, LoadEvent::Uploads);
        vk::RecordedBufferUploads recorded;
        if (core_->buffers.HasPendingUploads()) {
            if (! core_->buffers.RecordPendingUploads(command, recorded))
                return Err(render_error("upload"_str, "buffer recording failed"_str));
            resources->uploads = core_->buffers.CommitRecordedUploads(rstd::move(recorded));
            if (! resources->uploads)
                return Err(render_error("upload"_str, "buffer upload lease failed"_str));
        }
        for (auto& source : prepared.data_->images) {
            auto result =
                record_texture(work, source.source->image, source.source->mipmaps, core_->trace);
            if (result.is_err()) return Err(texture_error(result.unwrap_err()));
        }
        for (auto& source : (*environment_)->textures) {
            auto result = record_texture(work, *source, {}, core_->trace);
            if (result.is_err()) return Err(texture_error(result.unwrap_err()));
        }
        uploads.finish();
        for (const auto& draw : draws) {
            auto        n = draw.node, p = draw.primitive, mesh_index = at(asset.nodes, n).mesh;
            const auto& mesh     = at(asset.meshes, mesh_index);
            const auto& gpu_mesh = at(prepared.data_->meshes, mesh_index);
            const auto& world    = instance.pose(n).world;
            if (instance.pose(n).skin_palette.is_empty() && absolute(world.determinant3()) < 1e-12f)
                continue;
            DrawConstants constants {};
            constants.view      = relative_view;
            auto relative_world = camera_relative_model(world, origin);
            for (unsigned row = 0; row < 3; ++row)
                constants.model[row] = { relative_world.m[row],
                                         relative_world.m[4 + row],
                                         relative_world.m[8 + row],
                                         relative_world.m[12 + row] };
            const auto& primitive   = at(mesh.primitives, p);
            const auto& gpu         = at(gpu_mesh.primitives, p);
            constants.skin_offset   = at(pose_offsets, n).skin;
            constants.weight_offset = at(pose_offsets, n).weights;
            constants.morph_count   = unsigned(count(primitive.morphs));
            constants.vertex_count  = gpu.vertex_count;
            inputs.draws->push({ mesh_index, p, primitive.material, constants, draw.transparent });
        }
        GraphShape shape;
        bool       has_shadows = resources->shadow_atlas.is_some();
        if (has_shadows) shape.passes.push(vk::PassExecutionInfo {});
        shape.passes.push(vk::PassExecutionInfo {});
        if (resources->coverage) shape.passes.push(vk::PassExecutionInfo {});
        unsigned first     = has_shadows ? 1 : 0;
        auto&    pass      = shape.passes[usize(first)];
        pass.id            = { 1 };
        pass.kind          = model_kind;
        auto coverage_pass = resources->coverage ? &shape.passes[usize(first + 1)] : nullptr;
        auto shadow_pass   = has_shadows ? &shape.passes[usize()] : nullptr;
        if (shadow_pass) {
            shadow_pass->id   = { 3 };
            shadow_pass->kind = shadow_kind;
        }
        if (coverage_pass) {
            coverage_pass->id   = { 2 };
            coverage_pass->kind = model_kind;
        }
        vk::GraphBindings                    bindings(1);
        Vec<vrento::texture::TextureViewRef> references;
        auto                                 bind = [&](vrento::texture::TextureViewRef view) {
            for (usize i {}; i < references.len(); ++i)
                if (references[i].texture().identity() == view.texture().identity() &&
                    references[i].description() == view.description())
                    return vrento::vulkan::ResourceId { i.to_primitive() + 1, 1 };
            vrento::vulkan::ResourceId id { references.len().to_primitive() + 1, 1 };
            auto binding = Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view));
            auto info    = binding->Info();
            bindings.bind(id, rstd::move(binding)).unwrap();
            const auto& desc   = view.texture().description();
            const auto  range  = view.description().range;
            const auto  extent = vrento::texture::mip_extent(desc, range.first_mip);
            shape.resources.push({ id,
                                   vrento::fg::ResourceKind::Texture,
                                   0,
                                   range.mip_count,
                                   range.layer_count,
                                   true,
                                   vk::ToVkType(desc.format),
                                   extent.width,
                                   extent.height,
                                   info.aspects });
            references.push(rstd::move(view));
            return id;
        };
        auto color_id = bind(target), depth_id = bind(depth);
        shape.resources[usize()].imported  = destination.color_load == vk::AttachmentLoad::Load;
        shape.resources[usize(1)].imported = destination.depth_load == vk::AttachmentLoad::Load;
        auto attachment                    = [&](vk::PassExecutionInfo&         execution,
                                                 vrento::vulkan::ResourceId     id,
                                                 vrento::vulkan::PassUsage      usage,
                                                 vrento::vulkan::PassStages     stages,
                                                 vrento::vulkan::AttachmentLoad load,
                                                 unsigned                       aspect,
                                                 unsigned                       version = 1) {
            vrento::vulkan::PassResourceUse use {
                id, version, vrento::vulkan::GraphAccess::Write, usage, stages
            };
            use.range.aspect = aspect;
            if (load == vrento::vulkan::AttachmentLoad::Load) {
                auto read    = use;
                read.version = version - 1;
                read.access  = vrento::vulkan::GraphAccess::Read;
                execution.uses.push(rstd::move(read));
            }
            execution.uses.push(rstd::move(use));
        };
        attachment(pass,
                   color_id,
                   vrento::vulkan::PassUsage::ColorAttachment,
                   vrento::vulkan::PassStages::Color,
                   destination.color_load,
                   1);
        attachment(pass,
                   depth_id,
                   vrento::vulkan::PassUsage::DepthAttachment,
                   vrento::vulkan::PassStages::Depth,
                   destination.depth_load,
                   2);
        vrento::vulkan::GraphicsDescription graphics;
        graphics.color =
            Some(vrento::vulkan::GraphAttachment { color_id, 1, destination.color_load });
        graphics.depth =
            Some(vrento::vulkan::GraphAttachment { depth_id, 1, destination.depth_load });
        graphics.depth->clear[usize()] = reversed_depth ? 0 : 1;
        pass.graphics                  = Some(graphics);
        if (coverage_pass) {
            auto coverage_id                   = bind(*resources->coverage);
            shape.resources[usize(2)].imported = false;
            attachment(*coverage_pass,
                       coverage_id,
                       vk::PassUsage::ColorAttachment,
                       vk::PassStages::Color,
                       vk::AttachmentLoad::Clear,
                       1);
            attachment(*coverage_pass,
                       depth_id,
                       vk::PassUsage::DepthAttachment,
                       vk::PassStages::Depth,
                       vk::AttachmentLoad::Load,
                       2,
                       2);
            vk::GraphicsDescription coverage_graphics;
            coverage_graphics.color =
                Some(vk::GraphAttachment { coverage_id, 1, vk::AttachmentLoad::Clear });
            coverage_graphics.depth =
                Some(vk::GraphAttachment { depth_id, 2, vk::AttachmentLoad::Load });
            coverage_pass->graphics = Some(coverage_graphics);
        }
        if (shadow_pass) {
            auto atlas_id                                       = bind(*resources->shadow_atlas);
            shape.resources[usize(atlas_id.value - 1)].imported = false;
            attachment(*shadow_pass,
                       atlas_id,
                       vk::PassUsage::DepthAttachment,
                       vk::PassStages::Depth,
                       vk::AttachmentLoad::Clear,
                       2);
            vk::GraphicsDescription shadow_graphics;
            shadow_graphics.depth =
                Some(vk::GraphAttachment { atlas_id, 1, vk::AttachmentLoad::Clear });
            shadow_graphics.depth->clear[usize()] = 1;
            shadow_pass->graphics                 = Some(shadow_graphics);
        }
        auto sampled_use = [&](vk::PassExecutionInfo&                 execution,
                               const vrento::texture::TextureViewRef& view,
                               unsigned                               version = 0) {
            auto id = bind(view);
            for (const auto& use : execution.uses)
                if (use.resource == id) return;
            vrento::vulkan::PassResourceUse use { id,
                                                  version,
                                                  vrento::vulkan::GraphAccess::Read,
                                                  vrento::vulkan::PassUsage::Sampled,
                                                  vrento::vulkan::PassStages::Fragment };
            const auto                      range = view.description().range;
            use.range.mip_count                   = range.mip_count;
            use.range.layer_count                 = range.layer_count;
            use.range.aspect = range.aspect == vrento::texture::Aspect::Depth ? 2 : 1;
            execution.uses.push(rstd::move(use));
        };
        for (const auto& source : prepared.data_->images) {
            sampled_use(pass, source.source->image.view);
            if (shadow_pass) sampled_use(*shadow_pass, source.source->image.view);
            if (coverage_pass) sampled_use(*coverage_pass, source.source->image.view);
        }
        for (const auto& source : (*environment_)->textures) {
            sampled_use(pass, source->view);
            if (shadow_pass) sampled_use(*shadow_pass, source->view);
            if (coverage_pass) sampled_use(*coverage_pass, source->view);
        }
        if (shadow_pass) {
            sampled_use(pass, *resources->shadow_atlas, 1);
            if (coverage_pass) sampled_use(*coverage_pass, *resources->shadow_atlas, 1);
        }
        struct BufferResource {
            rstd::uint64_t identity;
            vk::ResourceId id;
        };
        Vec<BufferResource> allocations;
        auto                next_binding = references.len().to_primitive() + 1;
        auto                buffer_use   = [&](vk::PassExecutionInfo&      execution,
                                               const vk::BufferAllocation& allocation,
                                               vrento::vulkan::PassUsage   usage,
                                               vrento::vulkan::PassStages  stages) {
            vk::ResourceId id {};
            for (const auto& resource : allocations)
                if (resource.identity == allocation.identity()) id = resource.id;
            if (! id.value) {
                id = { next_binding++, 1 };
                allocations.push({ allocation.identity(), id });
                bindings
                    .bind(id,
                          Arc<dyn<vk::GraphBinding>>::make(
                              vk::GraphUploadedBufferBinding(core_->session, allocation.clone())))
                    .unwrap();
                shape.resources.push({ id, vrento::fg::ResourceKind::Buffer, allocation.size() });
            }
            for (const auto& use : execution.uses)
                if (use.resource == id) return;
            vrento::vulkan::PassResourceUse use {
                id, 0, vrento::vulkan::GraphAccess::Read, usage, stages
            };
            use.range.size = allocation.size();
            execution.uses.push(rstd::move(use));
        };
        buffer_use(pass,
                   *resources->frame_buffer,
                   vrento::vulkan::PassUsage::Uniform,
                   vrento::vulkan::PassStages::Vertex | vrento::vulkan::PassStages::Fragment);
        if (coverage_pass)
            buffer_use(*coverage_pass,
                       *resources->coverage_frame_buffer,
                       vk::PassUsage::Uniform,
                       vk::PassStages::Vertex | vk::PassStages::Fragment);
        buffer_use(
            pass, *resources->shadow_parameters, vk::PassUsage::Uniform, vk::PassStages::Fragment);
        if (coverage_pass)
            buffer_use(*coverage_pass,
                       *resources->shadow_parameters,
                       vk::PassUsage::Uniform,
                       vk::PassStages::Fragment);
        if (shadow_pass)
            for (const auto& shadow : resources->shadows)
                buffer_use(
                    *shadow_pass, *shadow.buffer, vk::PassUsage::Uniform, vk::PassStages::Vertex);
        auto shared_buffer_use = [&](const vk::BufferAllocation& allocation,
                                     vk::PassUsage               usage,
                                     vk::PassStages              stages) {
            buffer_use(pass, allocation, usage, stages);
            if (coverage_pass) buffer_use(*coverage_pass, allocation, usage, stages);
            if (shadow_pass) buffer_use(*shadow_pass, allocation, usage, stages);
        };
        shared_buffer_use(*core_->zero_buffer,
                          vrento::vulkan::PassUsage::Storage,
                          vrento::vulkan::PassStages::Vertex);
        shared_buffer_use(*resources->poses,
                          vrento::vulkan::PassUsage::Storage,
                          vrento::vulkan::PassStages::Vertex);
        for (const auto& material : prepared.data_->materials)
            shared_buffer_use(material.parameters,
                              vrento::vulkan::PassUsage::Uniform,
                              vrento::vulkan::PassStages::Fragment);
        for (const auto& mesh : prepared.data_->meshes)
            for (const auto& primitive : mesh.primitives) {
                shared_buffer_use(primitive.vertices,
                                  vrento::vulkan::PassUsage::Vertex,
                                  vrento::vulkan::PassStages::Vertex);
                if (primitive.indices)
                    shared_buffer_use(*primitive.indices,
                                      vrento::vulkan::PassUsage::Index,
                                      vrento::vulkan::PassStages::Vertex);
                if (primitive.morphs)
                    shared_buffer_use(*primitive.morphs,
                                      vrento::vulkan::PassUsage::Storage,
                                      vrento::vulkan::PassStages::Vertex);
            }
        core_->scopes.collect();
        if (! compiler_) {
            compiler_ = Some(vk::Renderer(core_->session));
            compiler_
                ->bind(Arc<dyn<vk::PassFactory>>::make(
                    vk::TypedPassFactory<ModelPassDesc, ModelPassFactory> {
                        ModelPassFactory { core_.clone() } }))
                .unwrap();
            compiler_
                ->bind(Arc<dyn<vk::PassFactory>>::make(
                    vk::TypedPassFactory<ShadowPassDesc, ShadowPassFactory> {
                        ShadowPassFactory { core_.clone() } }))
                .unwrap();
        }
        if (! graph_shape_ || *graph_shape_ != shape) {
            vrento::fg::FrameGraphBuilder builder(1);
            for (const auto& resource : shape.resources) {
                vrento::fg::ResourceDescription desc { .id         = resource.id,
                                                       .kind       = resource.kind,
                                                       .bytes      = resource.bytes,
                                                       .mip_levels = resource.mips,
                                                       .layers     = resource.layers,
                                                       .aspects    = resource.aspects };
                auto declared = resource.imported ? builder.import_resource(rstd::move(desc))
                                                  : builder.create_resource(rstd::move(desc));
                if (declared.is_err()) return Err(graph_error(declared.unwrap_err()));
            }
            Vec<vk::PassInput> descriptions;
            for (const auto& execution : shape.passes) {
                auto added = builder.add_pass(execution.id);
                if (added.is_err()) return Err(graph_error(added.unwrap_err()));
                ModelPassDesc desc;
                desc.io.kind                        = execution.kind;
                desc.io.graphics                    = execution.graphics;
                desc.signature.color_format         = vk::ToVkType(target_desc.format);
                desc.signature.has_depth_attachment = true;
                desc.signature.depth_format = vk::ToVkType(depth.texture().description().format);
                if (execution.kind == shadow_kind) {
                    desc.signature.has_color_attachment = false;
                    desc.signature.depth_format         = VK_FORMAT_D32_SFLOAT;
                }
                for (const auto& use : execution.uses) {
                    if (use.access != vk::GraphAccess::Write) continue;
                    bool inherits {};
                    for (const auto& read : execution.uses)
                        if (read.resource == use.resource && read.access == vk::GraphAccess::Read &&
                            read.version + 1 == use.version)
                            inherits = true;
                    auto previous = vrento::fg::ResourceVersion { use.resource, use.version - 1 };
                    auto output = builder.reserve_write(execution.id, previous, inherits, inherits);
                    if (output.is_err()) return Err(graph_error(output.unwrap_err()));
                    if (inherits)
                        desc.io.storage.push(
                            { vrento::pass::ContentRef::bind(builder, { use.resource, use.version })
                                  .unwrap(),
                              vrento::pass::ContentRef::bind(builder, previous).unwrap() });
                }
                for (const auto& use : execution.uses) {
                    auto content =
                        vrento::pass::ContentRef::bind(builder, { use.resource, use.version });
                    if (content.is_err()) return Err(graph_error(content.unwrap_err()));
                    vk::VulkanResourceUse value {
                        content.unwrap(), use.usage, use.stages, use.range
                    };
                    if (use.access == vk::GraphAccess::Read)
                        desc.io.inputs.push(rstd::move(value));
                    else
                        desc.io.outputs.push(rstd::move(value));
                }
                auto published = [&]() {
                    if (execution.kind != shadow_kind)
                        return vk::declare_input(builder, execution.id, rstd::move(desc));
                    ShadowPassDesc shadow_desc;
                    shadow_desc.io        = rstd::move(desc.io);
                    shadow_desc.signature = desc.signature;
                    return vk::declare_input(builder, execution.id, rstd::move(shadow_desc));
                }();
                if (published.is_err()) return Err(graph_error(published.unwrap_err()));
                descriptions.push(published.unwrap());
            }
            auto graph = rstd::move(builder).finish();
            if (graph.is_err()) return Err(graph_error(graph.unwrap_err()));
            auto compiled = compiler_->compile(graph.unwrap(), rstd::move(descriptions));
            if (compiled.is_err()) return Err(graph_error(compiled.unwrap_err()));
            graph_shape_ = Some(rstd::move(shape));
        }
        if (compiler_->pending()) {
            auto continued = compiler_->continue_compile(compiler_->status().id);
            if (continued.is_err()) return Err(graph_error(continued.unwrap_err()));
            if (*continued == vk::CompileProgress::Pending)
                return Err(render_error("model pass"_str, "prewarmed pipeline unavailable"_str));
        }
        auto compiled      = compiler_->compiled().unwrap();
        auto shared_draws  = inputs.draws.clone();
        auto frame_binding = compiled.frame<ModelPass>({ 1 }, 1, rstd::move(inputs));
        if (frame_binding.is_err()) return Err(graph_error(frame_binding.unwrap_err()));
        Vec<vk::PassFrameBinding> frames;
        frames.push(frame_binding.unwrap());
        if (has_shadows) {
            auto shadow_binding = compiled.frame<ShadowPass>(
                { 3 }, 1, ModelFrameData { resources.clone(), shared_draws.clone() });
            if (shadow_binding.is_err()) return Err(graph_error(shadow_binding.unwrap_err()));
            frames.push(shadow_binding.unwrap());
        }
        if (resources->coverage) {
            auto coverage_binding = compiled.frame<ModelPass>(
                { 2 },
                1,
                ModelFrameData {
                    resources.clone(), rstd::move(shared_draws), false, reversed_depth, true });
            if (coverage_binding.is_err()) return Err(graph_error(coverage_binding.unwrap_err()));
            frames.push(coverage_binding.unwrap());
        }
        auto prepared_frame = compiled.instantiate(
            bindings,
            frames.as_slice(),
            Box<dyn<vk::GraphicsScopeBackend>>::make(ModelScopes { core_.clone() }),
            true,
            &work);
        if (prepared_frame.is_err()) return Err(graph_error(prepared_frame.unwrap_err()));
        if (! prepared_frame->ready())
            return Err(render_error("model pass"_str, "unexpected frame preparation pending"_str));
        auto program        = prepared_frame->take_ready();
        auto recorded_frame = program.record(work);
        if (recorded_frame.is_err()) return Err(graph_error(recorded_frame.unwrap_err()));
        auto sampled = work.commands()->UseTexture(
            { target, vrento::texture::Access::Sampled, vrento::texture::Stages::Fragment });
        if (sampled.is_err()) return Err(texture_error(sampled.unwrap_err()));
        if (resources->coverage) {
            auto coverage_sampled =
                work.commands()->UseTexture({ *resources->coverage,
                                              vrento::texture::Access::Sampled,
                                              vrento::texture::Stages::Fragment });
            if (coverage_sampled.is_err()) return Err(texture_error(coverage_sampled.unwrap_err()));
        }
        recording.accepted = true;
        ++statistics_.recorded_frames;
        return Ok(empty {});
    }

public:
    void close() {
        compiler_    = None();
        graph_shape_ = None();
        depth_       = None();
        if (core_ && core_->preparation) {
            core_->preparation->close();
            core_->warmed.clear();
        }
    }
};
} // namespace vecgltf
