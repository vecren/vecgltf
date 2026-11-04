module;
#include <vulkan/vulkan_core.h>

export module vecgltf.display;
export import vecgltf.render.options;
import vecgltf.asset;
import vecgltf.shaders;
export import vrento;

using namespace rstd::literals;

export namespace vecgltf
{
struct ColorGrading {
    // Contrast is at least -1 and applies max(color * (1 + contrast) - 0.5 * contrast, 0).
    float brightness { 1 }, saturation { 1 }, contrast {};
};
struct Vignette {
    // Radii measure distance in normalized UV coordinates, without aspect correction.
    float intensity { 1 }, inner_radius {}, outer_radius { 1 };
    Vec2  center { 0.5f, 0.5f };
};
struct Bloom {
    // Radius blends the five level weights in [0,1].
    float intensity { 1 }, threshold { 1 }, radius {};
};
struct Grain {
    // A fixed application-supplied phase yields stable screen noise.
    float strength {}, phase {};
};
struct DisplayOptions {
    float            exposure { 1 };
    ToneMapping      tone_mapping { ToneMapping::ACES };
    ColorGrading     grading;
    Option<Vignette> vignette;
    Option<Bloom>    bloom;
    bool             fxaa {};
    float            sharpen {};
    Option<Grain>    grain;
    // Scales Bloom and sharpen taps; FXAA follows the input pixel grid.
    float sampling_scale { 1 };
};
struct DisplayInput {
    vrento::texture::TextureViewRef color;
    // Geometry coverage is independent of an opaque environment background.
    Option<vrento::texture::TextureViewRef> coverage;
};
struct DisplayTarget {
    vrento::texture::TextureViewRef color;
    vrento::vulkan::AttachmentLoad  load { vrento::vulkan::AttachmentLoad::Clear };
};
struct DisplayStatistics {
    rstd::uint64_t recorded_frames {};
};
} // namespace vecgltf

namespace vecgltf::display
{
namespace vk       = vrento::vulkan;
namespace tex      = vrento::texture;
namespace registry = vrento::resource_registry;
namespace res      = vrento::resource;

auto error(ref<str> location, ref<str> message, ErrorKind kind = ErrorKind::Device) -> Error {
    return { kind, {}, String::make(location), String::make(message) };
}
template<class T>
auto cause(ref<str> location, T value, ErrorKind kind = ErrorKind::Device) -> Error {
    auto message = rstd::format("{}", value);
    return { kind,
             {},
             String::make(location),
             rstd::move(message),
             Some(Arc<dyn<rstd::error::Error>>::make(rstd::move(value))) };
}
auto texture_error(tex::Error value) -> Error {
    auto kind = value.kind == tex::ErrorKind::Busy ? ErrorKind::Busy : ErrorKind::Device;
    return cause("display texture"_str, rstd::move(value), kind);
}
auto graph_error(vk::GraphExecutionError value) -> Error {
    return cause("display pass"_str, rstd::move(value));
}
auto graph_error(vrento::fg::Error value) -> Error {
    return graph_error(vk::GraphExecutionError { vk::GraphExecutionStage::Graph,
                                                 value.pass,
                                                 value.resource,
                                                 vk::GraphExecutionCause::Graph(value) });
}
bool nonnegative(float value) {
    return rstd::f32(value).is_finite() && value >= 0;
}
auto options_valid(const DisplayOptions& options) -> Result<empty, Error> {
    if (! nonnegative(options.exposure) || ! nonnegative(options.grading.brightness) ||
        ! nonnegative(options.exposure * options.grading.brightness) ||
        ! nonnegative(options.grading.saturation) ||
        ! rstd::f32(options.grading.contrast).is_finite() || options.grading.contrast < -1 ||
        (options.tone_mapping != ToneMapping::ACES &&
         options.tone_mapping != ToneMapping::ReinhardMax &&
         options.tone_mapping != ToneMapping::Linear))
        return Err(error("display options"_str,
                         "invalid exposure, grading or tone mapping"_str,
                         ErrorKind::InvalidAsset));
    if (options.vignette) {
        const auto& value = *options.vignette;
        if (! nonnegative(value.intensity) || value.intensity > 1 ||
            ! nonnegative(value.inner_radius) || ! nonnegative(value.outer_radius) ||
            value.outer_radius <= value.inner_radius || ! rstd::f32(value.center.x).is_finite() ||
            ! rstd::f32(value.center.y).is_finite())
            return Err(
                error("display options"_str, "invalid vignette"_str, ErrorKind::InvalidAsset));
    }
    if (options.bloom &&
        (! nonnegative(options.bloom->intensity) || ! nonnegative(options.bloom->threshold) ||
         ! nonnegative(options.bloom->radius) || options.bloom->radius > 1))
        return Err(error("display options"_str, "invalid bloom"_str, ErrorKind::InvalidAsset));
    if (! nonnegative(options.sharpen) || ! nonnegative(options.sampling_scale) ||
        options.sampling_scale == 0 ||
        (options.grain && (! nonnegative(options.grain->strength) || options.grain->strength > 1 ||
                           ! rstd::f32(options.grain->phase).is_finite())))
        return Err(error(
            "display options"_str, "invalid postprocess options"_str, ErrorKind::InvalidAsset));
    return Ok(empty {});
}
bool bloom_enabled(const DisplayOptions& options) {
    return options.bloom && options.bloom->intensity > 0;
}
bool post_enabled(const DisplayOptions& options) {
    return options.fxaa || options.sharpen > 0 || (options.grain && options.grain->strength > 0);
}
enum class Stage : unsigned
{
    Display,
    Encoded,
    Bloom,
    Post
};
constexpr unsigned sampler_count = 7;
struct Parameters {
    Vec4 mapping, grading, vignette, bloom, post, screen, blur;
};
static_assert(sizeof(Parameters) == 112);
struct Core {
    vk::DeviceSession                      session;
    vk::BufferManager                      buffers;
    Option<vk::TextureContext>             images;
    registry::DescriptorSystem             descriptors;
    res::DescriptorLayoutHandle            descriptor_layout;
    Option<registry::PipelineLayoutResult> layout;
    Vec<res::PipelineRecipeHandle>         recipes;
    Option<vk::PipelinePreparationQueue>   preparation;
    Vec<registry::PipelineResourceResult>  warmed;
    vk::GraphicsScopeCache                 scopes;
    Option<tex::SamplerRef>                sampler;
    Option<tex::SubmissionReceipt>         latest;
    explicit Core(vk::DeviceSession value): session(rstd::move(value)), buffers(session.device()) {}
    ~Core() {
        for (const auto& recipe : recipes)
            session.graphics_resources().pipelines.ReleaseRecipe(recipe);
    }
};
template<rstd::size_t N>
auto shader(const unsigned (&code)[N], vrento::ShaderType stage) -> vk::Uni_ShaderSpv {
    auto result   = Box<vk::ShaderSpv>::make();
    result->stage = stage;
    result->spirv = vrento::ShaderCode::from(slice<rstd::uint32_t>::from_raw_parts(code, usize(N)));
    return result;
}
auto initialize(Core& core, vk::PipelinePreparationMode mode) -> Result<empty, Error> {
    auto preparation = vk::PipelinePreparationQueue::create(core.session, mode);
    if (preparation.is_err())
        return Err(cause("display preparation"_str, preparation.unwrap_err()));
    core.preparation = Some(preparation.unwrap());
    if (! core.buffers.init())
        return Err(error("display buffers"_str, "initialization failed"_str));
    auto images = vk::TextureContext::create(core.session);
    if (images.is_err()) return Err(texture_error(images.unwrap_err()));
    core.images = Some(images.unwrap());
    tex::SamplerDesc sampler;
    sampler.sample.minFilter = sampler.sample.magFilter = vrento::TextureFilter::LINEAR;
    sampler.sample.mipFilter                            = vrento::TextureMipmapFilter::None;
    sampler.sample.wrapS = sampler.sample.wrapT = vrento::TextureWrap::CLAMP_TO_EDGE;
    auto sampled                                = core.images->CreateSampler(sampler);
    if (sampled.is_err()) return Err(texture_error(sampled.unwrap_err()));
    core.sampler = Some(sampled.unwrap());
    vk::DescriptorSetInfo set;
    for (unsigned i = 0; i < sampler_count; ++i)
        set.bindings.push({ i == 0 ? 0 : i + 1,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                            1,
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr });
    set.bindings.push(
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    auto& graphics   = core.session.graphics_resources();
    auto  descriptor = graphics.descriptors.Ensure(core.session.device(), set);
    if (descriptor.is_err()) return Err(cause("display layout"_str, descriptor.unwrap_err()));
    core.descriptor_layout = descriptor.unwrap();
    registry::PipelineLayoutRequest layout_request;
    layout_request.descriptor_sets.push(set.clone());
    auto layout =
        graphics.layouts.Ensure(core.session.device(), layout_request, graphics.descriptors);
    if (layout.is_err()) return Err(cause("display layout"_str, layout.unwrap_err()));
    core.layout = Some(layout.unwrap());
    for (unsigned i = 0; i < 4; ++i) {
        Stage                       stage = Stage(i);
        vk::GraphicsPipeline        defaults;
        vk::PipelineResourceRequest pipeline;
        pipeline.pipeline_layout       = core.layout->handle;
        pipeline.topology              = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pipeline.depth                 = defaults.depth;
        pipeline.raster                = defaults.raster;
        pipeline.multisample           = defaults.multisample;
        pipeline.depth.depthTestEnable = pipeline.depth.depthWriteEnable = VK_FALSE;
        pipeline.has_depth_attachment                                    = false;
        pipeline.color_initial_layout = pipeline.color_final_layout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        pipeline.color_load_op                   = VK_ATTACHMENT_LOAD_OP_CLEAR;
        pipeline.color_blend.blendEnable         = stage == Stage::Display || stage == Stage::Post;
        pipeline.color_blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        pipeline.color_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        pipeline.color_blend.colorBlendOp        = VK_BLEND_OP_ADD;
        pipeline.color_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        pipeline.color_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        pipeline.color_blend.alphaBlendOp        = VK_BLEND_OP_ADD;
        pipeline.color_blend.colorWriteMask      = 15;
        pipeline.shader_stages.push(
            shader(shaders::display_vertex.words, vrento::ShaderType::VERTEX));
        if (stage == Stage::Bloom)
            pipeline.shader_stages.push(
                shader(shaders::bloom_fragment.words, vrento::ShaderType::FRAGMENT));
        else if (stage == Stage::Post)
            pipeline.shader_stages.push(
                shader(shaders::postprocess_fragment.words, vrento::ShaderType::FRAGMENT));
        else
            pipeline.shader_stages.push(
                shader(shaders::display_fragment.words, vrento::ShaderType::FRAGMENT));
        auto registered = graphics.pipelines.RegisterRecipe(rstd::move(pipeline), graphics.layouts);
        if (! registered) return Err(error("display recipe"_str, "registration failed"_str));
        core.recipes.push(registered.unwrap());
    }
    return Ok(empty {});
}
struct StageFrame {
    Stage                                       recipe { Stage::Display };
    Parameters                                  values {};
    tex::TextureViewRef                         sampled[sampler_count];
    tex::TextureViewRef                         target;
    vk::AttachmentLoad                          load { vk::AttachmentLoad::Clear };
    Option<vk::BufferAllocation>                parameters;
    Option<registry::PreparedDescriptorBinding> descriptor;
};
struct Frame {
    Arc<Core>                          core;
    Vec<StageFrame>                    stages;
    Vec<tex::TextureViewRef>           scratch;
    Option<vk::BufferUploadBatchLease> uploads;
    tex::SubmissionReceipt             receipt;
    Frame(Arc<Core> owner, tex::SubmissionReceipt work)
        : core(rstd::move(owner)), receipt(rstd::move(work)) {
        core->latest = Some(tex::SubmissionReceipt(receipt));
    }
    ~Frame() {
        if (uploads && ! receipt.submitted() && ! core->session.lost())
            if (! core->buffers.RestoreUploads(uploads.take().unwrap()))
                rstd::panic("cannot restore cancelled display uploads");
    }
};
constexpr vk::GraphSerial kind = 0x5647444953504c59ull;
struct Prepared {
    struct FrameData {
        Arc<Frame> resources;
        unsigned   stage {};
    };
    Arc<FrameData>                        inputs;
    Arc<registry::PipelineResourceResult> pipeline;
    auto Record(vk::GraphRecordContext& context) const -> Result<empty, vk::GraphExecutionError> {
        auto& frame  = *inputs->resources;
        auto& core   = *frame.core;
        auto  native = vk::native_recording(context.submission, core.session);
        if (native.is_err())
            return Err(
                vk::GraphExecutionError { vk::GraphExecutionStage::Record,
                                          {},
                                          {},
                                          vk::GraphExecutionCause::Texture(native.unwrap_err()) });
        auto                                   cmd      = native.unwrap();
        const auto&                            dispatch = core.session.device().handle().Dispatch();
        vvk::CommandBuffer                     command(cmd, dispatch);
        registry::DescriptorBindingRecordState state;
        frame.stages[usize(inputs->stage)].descriptor->Record(
            command, *core.layout->physical->layout, state);
        dispatch.vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline->pipeline->pipeline.handle);
        dispatch.vkCmdDraw(cmd, 3, 1, 0, 0);
        return Ok(empty {});
    }
};
struct PassDesc {
    using PassDescriptor = PassDesc;
    vk::VulkanPassIO           io;
    vk::RenderPassResourceDesc signature;
    Stage                      recipe { Stage::Display };
};
struct Pass {
    using FrameData = Prepared::FrameData;
    Arc<registry::PipelineResourceResult> pipeline;
    auto Instantiate(const vk::GraphPrepareContext& context, Arc<FrameData> frame) const
        -> Result<vrento::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        if (! context.scope)
            return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Schema });
        return Ok(Arc<dyn<vk::VulkanPass>>::make(Prepared { rstd::move(frame), pipeline.clone() }));
    }
};
struct Factory {
    using Pass = display::Pass;
    Arc<Core> core;
    auto      Compile(const PassDesc&              desc,
                      const vk::PassExecutionInfo& execution,
                      const vk::PassCompileContext&)
        -> Result<vrento::Preparation<Arc<dyn<vk::VulkanPass>>>, vk::GraphExecutionError> {
        auto prepared =
            core->preparation->prepare(core->recipes[usize(unsigned(desc.recipe))], desc.signature);
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
        return Ok(Arc<dyn<vk::VulkanPass>>::make(
            Pass { Arc<registry::PipelineResourceResult>::make(value.pipeline.take().unwrap()) }));
    }
};
struct Scopes {
    Arc<Core> core;
    auto      Prepare(const vk::GraphicsDescription& desc, const vk::GraphPrepareContext& context)
        -> Result<vk::Preparation<Arc<dyn<vk::PreparedGraphicsScope>>>, vk::GraphExecutionError> {
        return core->scopes.Prepare(desc, context);
    }
};
struct Shape {
    struct Texture {
        tex::TextureDesc description;
        tex::ViewDesc    view;
        bool             imported {};
        friend bool      operator==(const Texture&, const Texture&) = default;
    };
    struct Draw {
        Stage              recipe;
        unsigned           sampled[sampler_count] {};
        unsigned           target {};
        vk::AttachmentLoad load;
        friend bool        operator==(const Draw&, const Draw&) = default;
    };
    Vec<Texture> textures;
    Vec<Draw>    draws;
    friend bool  operator==(const Shape&, const Shape&) = default;
};
auto parameters(const DisplayOptions& options) -> Parameters {
    Parameters value {};
    value.mapping  = { options.exposure,
                       float(unsigned(options.tone_mapping)),
                       options.grading.brightness,
                       options.grading.saturation };
    value.grading  = { options.grading.contrast, 0, 0, 1 };
    value.vignette = { 0.5f, 0.5f, 0, 0 };
    if (options.vignette) {
        const auto& vignette = *options.vignette;
        value.grading.y      = vignette.intensity;
        value.grading.z      = vignette.inner_radius;
        value.grading.w      = vignette.outer_radius;
        value.vignette       = { vignette.center.x, vignette.center.y, 1, 0 };
    }
    if (bloom_enabled(options))
        value.bloom = {
            options.bloom->intensity, options.bloom->threshold, options.bloom->radius, 0
        };
    value.post     = { options.fxaa ? 1.0f : 0.0f,
                       options.sharpen,
                       options.grain ? options.grain->strength : 0,
                       options.grain ? options.grain->phase : 0 };
    value.screen.z = options.sampling_scale;
    return value;
}
auto scratch(Frame& frame, tex::Extent size) -> Result<tex::TextureViewRef, Error> {
    tex::TextureDesc description;
    description.extent = size;
    description.format = vrento::TextureFormat::RGBA16F;
    description.usage  = tex::Usage::Sampled | tex::Usage::ColorAttachment;
    auto texture       = frame.core->images->CreateTexture(description);
    if (texture.is_err()) return Err(texture_error(texture.unwrap_err()));
    auto view = frame.core->images->CreateView(texture.unwrap(), {});
    if (view.is_err()) return Err(texture_error(view.unwrap_err()));
    frame.scratch.push(view.unwrap());
    return Ok(frame.scratch.last().unwrap().get());
}
auto stage(Frame&              frame,
           Stage               recipe,
           tex::TextureViewRef source,
           tex::TextureViewRef target,
           Parameters          values,
           vk::AttachmentLoad  load = vk::AttachmentLoad::Clear) -> StageFrame& {
    auto extent =
        tex::mip_extent(target.texture().description(), target.description().range.first_mip);
    values.screen.x = 1.0f / float(extent.width);
    values.screen.y = 1.0f / float(extent.height);
    values.screen.w = float(extent.height);
    StageFrame value;
    value.recipe = recipe;
    value.values = values;
    for (auto& sampled : value.sampled) sampled = source;
    value.target = target;
    value.load   = load;
    frame.stages.push(rstd::move(value));
    return frame.stages[frame.stages.len() - usize(1)];
}
auto populate(Frame&                frame,
              const DisplayInput&   input,
              const DisplayTarget&  target,
              const DisplayOptions& options) -> Result<empty, Error> {
    auto                values = parameters(options);
    tex::TextureViewRef levels[5];
    auto                coverage = input.coverage ? *input.coverage : input.color;
    if (bloom_enabled(options)) {
        auto extent  = tex::mip_extent(input.color.texture().description(),
                                       input.color.description().range.first_mip);
        auto extract = scratch(frame, extent);
        if (extract.is_err()) return Err(extract.unwrap_err());
        stage(frame, Stage::Bloom, input.color, *extract, values).sampled[1] = coverage;
        auto previous                                                        = extract.unwrap();
        for (unsigned i = 0; i < 5; ++i) {
            unsigned    divisor = 2u << i;
            tex::Extent size { (extent.width + divisor / 2) / divisor,
                               (extent.height + divisor / 2) / divisor };
            if (size.width == 0) size.width = 1;
            if (size.height == 0) size.height = 1;
            auto vertical = scratch(frame, size);
            if (vertical.is_err()) return Err(vertical.unwrap_err());
            auto horizontal = scratch(frame, size);
            if (horizontal.is_err()) return Err(horizontal.unwrap_err());
            auto blur = values;
            blur.blur = { 0, 1, float(i), 1 };
            stage(frame, Stage::Bloom, previous, *vertical, blur);
            blur.blur = { 1, 0, float(i), 1 };
            stage(frame, Stage::Bloom, *vertical, *horizontal, blur);
            levels[i] = horizontal.unwrap();
            previous  = levels[i];
        }
    }
    tex::TextureViewRef mapped = target.color;
    bool                post   = post_enabled(options);
    if (post) {
        auto extent  = tex::mip_extent(target.color.texture().description(),
                                       target.color.description().range.first_mip);
        auto encoded = scratch(frame, extent);
        if (encoded.is_err()) return Err(encoded.unwrap_err());
        mapped         = encoded.unwrap();
        values.bloom.w = 1;
    }
    auto& display = stage(frame,
                          post ? Stage::Encoded : Stage::Display,
                          input.color,
                          mapped,
                          values,
                          post ? vk::AttachmentLoad::Clear : target.load);
    if (bloom_enabled(options))
        for (unsigned i = 0; i < 5; ++i) display.sampled[i + 2] = levels[i];
    if (post)
        stage(frame, Stage::Post, mapped, target.color, values, target.load).sampled[1] = coverage;
    return Ok(empty {});
}
} // namespace vecgltf::display

namespace rstd
{
template<>
struct Impl<vrento::vulkan::GraphicsScopeBackend, vecgltf::display::Scopes>
    : ImplBase<vecgltf::display::Scopes> {
    auto Prepare(const vrento::vulkan::GraphicsDescription& desc,
                 const vrento::vulkan::GraphPrepareContext& context)
        -> Result<
            vrento::vulkan::Preparation<sync::Arc<dyn<vrento::vulkan::PreparedGraphicsScope>>>,
            vrento::vulkan::GraphExecutionError> {
        return self().Prepare(desc, context);
    }
};
} // namespace rstd

export namespace vecgltf
{
class DisplayRenderer {
    Arc<display::Core>               core_;
    Option<vrento::vulkan::Renderer> compiler_;
    Option<display::Shape>           shape_;
    DisplayStatistics                statistics_;
    explicit DisplayRenderer(Arc<display::Core> core): core_(rstd::move(core)) {}
    auto target_valid(const DisplayTarget& target) const -> Result<empty, Error> {
        using namespace display;
        if (! target.color.valid() ||
            target.color.texture().identity().epoch != core_->session.epoch() ||
            ! target.color.permits(true))
            return Err(error("display target"_str, "invalid target"_str, ErrorKind::InvalidAsset));
        const auto& desc  = target.color.texture().description();
        const auto& range = target.color.description().range;
        if ((desc.format != vrento::TextureFormat::RGBA8 &&
             desc.format != vrento::TextureFormat::RGBA8_SRGB) ||
            desc.samples != 1 || range.mip_count != 1 || range.layer_count != 1 ||
            range.first_mip >= desc.mip_levels || range.first_layer >= desc.layers ||
            target.color.description().dimension != tex::Dimension::D2 ||
            range.aspect != tex::Aspect::Color ||
            ! tex::has(desc.usage, tex::Usage::ColorAttachment) ||
            (target.load != vk::AttachmentLoad::Clear && target.load != vk::AttachmentLoad::Load))
            return Err(error("display target"_str,
                             "unsupported target view or load operation"_str,
                             ErrorKind::InvalidAsset));
        return Ok(empty {});
    }
    auto source_valid(const vrento::texture::TextureViewRef& source,
                      const DisplayTarget&                   target) const -> Result<empty, Error> {
        using namespace display;
        if (! source.valid() || source.texture().identity().epoch != core_->session.epoch() ||
            ! source.permits(false) ||
            source.texture().identity() == target.color.texture().identity())
            return Err(error("display source"_str,
                             "invalid source or source aliases target"_str,
                             ErrorKind::InvalidAsset));
        const auto& desc  = source.texture().description();
        const auto& range = source.description().range;
        if (desc.format != vrento::TextureFormat::RGBA16F || desc.samples != 1 ||
            range.mip_count != 1 || range.layer_count != 1 || range.first_mip >= desc.mip_levels ||
            range.first_layer >= desc.layers ||
            source.description().dimension != tex::Dimension::D2 ||
            range.aspect != tex::Aspect::Color || ! tex::has(desc.usage, tex::Usage::Sampled))
            return Err(error("display source"_str,
                             "source must be a sampled linear RGBA16F view"_str,
                             ErrorKind::InvalidAsset));
        return Ok(empty {});
    }

public:
    DisplayRenderer(const DisplayRenderer&)               = delete;
    DisplayRenderer(DisplayRenderer&&)                    = default;
    auto operator=(DisplayRenderer&&) -> DisplayRenderer& = delete;
    ~DisplayRenderer() { close(); }
    static auto create(vrento::vulkan::DeviceSession           session,
                       vrento::vulkan::PipelinePreparationMode mode =
                           vrento::vulkan::PipelinePreparationMode::Synchronous)
        -> Result<DisplayRenderer, Error> {
        if (! session.valid())
            return Err(display::error("display create"_str, "invalid device"_str));
        auto core        = Arc<display::Core>::make(rstd::move(session));
        auto initialized = display::initialize(*core, mode);
        if (initialized.is_err()) return Err(initialized.unwrap_err());
        return Ok(DisplayRenderer(rstd::move(core)));
    }
    const DisplayStatistics& statistics() const { return statistics_; }
    auto                     graph_statistics() const -> vrento::vulkan::RendererStatistics {
        return compiler_ ? compiler_->statistics() : vrento::vulkan::RendererStatistics {};
    }
    auto upload_statistics() const -> vrento::vulkan::BufferUploadStatistics {
        return core_->buffers.statistics();
    }
    auto prepare_target(const DisplayTarget& target, bool retry = false)
        -> Result<vrento::resource_registry::PipelineReadiness, Error> {
        return prepare_target(target, DisplayOptions {}, retry);
    }
    auto prepare_target(const DisplayTarget& target, DisplayOptions options, bool retry = false)
        -> Result<vrento::resource_registry::PipelineReadiness, Error> {
        using namespace display;
        auto valid = target_valid(target);
        if (valid.is_err()) return Err(valid.unwrap_err());
        valid = options_valid(options);
        if (valid.is_err()) return Err(valid.unwrap_err());
        bool pending {};
        auto prepare = [&](Stage stage, vrento::TextureFormat format) -> Result<empty, Error> {
            vk::RenderPassResourceDesc signature;
            signature.color_format         = vk::ToVkType(format);
            signature.has_depth_attachment = false;
            auto prepared                  = core_->preparation->prepare(
                core_->recipes[usize(unsigned(stage))], signature, retry);
            if (prepared.is_err())
                return Err(cause("display prepare target"_str, prepared.unwrap_err()));
            auto value = prepared.unwrap();
            if (value.error)
                return Err(cause("display prepare target"_str, value.error.take().unwrap()));
            if (value.status == registry::PipelineReadiness::Failed)
                return Err(error("display prepare target"_str, "pipeline preparation failed"_str));
            pending |= value.status == registry::PipelineReadiness::Pending;
            if (value.pipeline) {
                bool retained {};
                for (const auto& cached : core_->warmed)
                    retained |= cached.handle == value.pipeline->handle;
                if (! retained) core_->warmed.push(value.pipeline.take().unwrap());
            }
            return Ok(empty {});
        };
        bool post  = post_enabled(options);
        auto final = prepare(post ? Stage::Post : Stage::Display,
                             target.color.texture().description().format);
        if (final.is_err()) return Err(final.unwrap_err());
        if (bloom_enabled(options)) {
            auto bloom = prepare(Stage::Bloom, vrento::TextureFormat::RGBA16F);
            if (bloom.is_err()) return Err(bloom.unwrap_err());
        }
        if (post) {
            auto encoded = prepare(Stage::Encoded, vrento::TextureFormat::RGBA16F);
            if (encoded.is_err()) return Err(encoded.unwrap_err());
        }
        return Ok(pending ? registry::PipelineReadiness::Pending
                          : registry::PipelineReadiness::Ready);
    }
    // Linear HDR and final output are premultiplied; postprocessing preserves center alpha.
    // RGBA8 retains linear display values; RGBA8_SRGB uses attachment encoding.
    auto record(vrento::texture::Submission&    work,
                vrento::texture::TextureViewRef source,
                DisplayTarget                   target,
                DisplayOptions                  options = {}) -> Result<empty, Error> {
        return record(
            work, DisplayInput { rstd::move(source), None() }, rstd::move(target), options);
    }
    auto record(vrento::texture::Submission& work,
                DisplayInput                 source,
                DisplayTarget                target,
                DisplayOptions               options = {}) -> Result<empty, Error> {
        using namespace display;
        auto valid = target_valid(target);
        if (valid.is_err()) return valid;
        valid = source_valid(source.color, target);
        if (valid.is_err()) return valid;
        if (source.coverage) {
            valid = source_valid(*source.coverage, target);
            if (valid.is_err()) return valid;
            if (tex::mip_extent(source.coverage->texture().description(),
                                source.coverage->description().range.first_mip) !=
                tex::mip_extent(source.color.texture().description(),
                                source.color.description().range.first_mip))
                return Err(error("display coverage"_str,
                                 "coverage and radiance extents differ"_str,
                                 ErrorKind::InvalidAsset));
        }
        valid = options_valid(options);
        if (valid.is_err()) return valid;
        if (! work.valid() || work.receipt().status() != tex::SubmissionStatus::Recording)
            return Err(
                error("display record"_str, "submission is not recording"_str, ErrorKind::Busy));
        if (core_->latest && (core_->latest->status() == tex::SubmissionStatus::Recording ||
                              core_->latest->uncertain()))
            return Err(error("display record"_str,
                             "previous display submission has not been accepted"_str,
                             ErrorKind::Busy));
        auto queue = vk::native_queue(work, core_->session);
        if (queue.is_err()) return Err(texture_error(queue.unwrap_err()));
        auto ready = prepare_target(target, options);
        if (ready.is_err()) return Err(ready.unwrap_err());
        if (ready.unwrap() != registry::PipelineReadiness::Ready)
            return Err(error(
                "display record"_str, "target pipelines are not ready"_str, ErrorKind::Pending));
        struct Guard {
            tex::Submission& work;
            bool             accepted {};
            ~Guard() {
                if (! accepted) work.discard();
            }
        } guard { work };
        auto native = vk::native_recording(work, core_->session);
        if (native.is_err()) return Err(texture_error(native.unwrap_err()));
        auto frame     = Arc<Frame>::make(core_.clone(), work.receipt());
        auto populated = populate(*frame, source, target, options);
        if (populated.is_err()) return Err(populated.unwrap_err());
        Shape                    shape;
        vk::GraphBindings        bindings(1);
        Vec<tex::TextureViewRef> references;
        auto                     bind = [&](tex::TextureViewRef view, bool imported) -> unsigned {
            for (usize i {}; i < references.len(); ++i)
                if (references[i].texture().identity() == view.texture().identity() &&
                    references[i].description() == view.description())
                    return unsigned(i.to_primitive());
            unsigned index = unsigned(references.len().to_primitive());
            bindings
                .bind({ index + 1, 1 },
                      Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(view)))
                .unwrap();
            shape.textures.push({ view.texture().description(), view.description(), imported });
            references.push(rstd::move(view));
            return index;
        };
        for (const auto& stage : frame->stages) {
            Shape::Draw draw;
            draw.recipe = stage.recipe;
            for (unsigned i = 0; i < sampler_count; ++i)
                draw.sampled[i] = bind(stage.sampled[i], true);
            draw.target = bind(stage.target, stage.load == vk::AttachmentLoad::Load);
            draw.load   = stage.load;
            shape.draws.push(rstd::move(draw));
        }
        for (usize i {}; i < frame->stages.len(); ++i) {
            auto& stage      = frame->stages[i];
            stage.parameters = core_->buffers.Allocate(
                { sizeof(Parameters),
                  core_->session.device().limits().minUniformBufferOffsetAlignment,
                  vk::BufferUploadClass::Uniform });
            if (! stage.parameters ||
                core_->buffers
                    .QueueWrite(*stage.parameters,
                                slice<rstd::uint8_t>::from_raw_parts(
                                    reinterpret_cast<const rstd::uint8_t*>(&stage.values),
                                    usize(sizeof(Parameters))))
                    .is_none())
                return Err(error("display parameters"_str, "allocation or upload failed"_str));
            registry::DescriptorImageBinding images[sampler_count];
            for (unsigned j = 0; j < sampler_count; ++j) {
                auto sampled = vk::sampled_image(stage.sampled[j], *core_->sampler);
                if (sampled.is_err()) return Err(texture_error(sampled.unwrap_err()));
                images[j] = { j == 0 ? 0 : j + 1, sampled->image, sampled->layout };
            }
            registry::DescriptorBufferBinding buffer {
                1, stage.parameters->buffer(), stage.parameters->offset(), sizeof(Parameters)
            };
            auto descriptor = core_->descriptors.Prepare(
                core_->session.device(),
                0,
                **core_->session.graphics_resources().descriptors.Resolve(core_->descriptor_layout),
                slice<registry::DescriptorImageBinding>::from_raw_parts(images,
                                                                        usize(sampler_count)),
                slice<registry::DescriptorBufferBinding>::from_raw_parts(&buffer, usize(1)),
                registry::DescriptorBindingReuse::Shared);
            if (descriptor.is_err())
                return Err(cause("display descriptor"_str, descriptor.unwrap_err()));
            stage.descriptor = Some(descriptor.unwrap());
            bindings
                .bind({ references.len().to_primitive() + i.to_primitive() + 1, 1 },
                      Arc<dyn<vk::GraphBinding>>::make(vk::GraphUploadedBufferBinding(
                          core_->session, stage.parameters->clone())))
                .unwrap();
        }
        const auto&               dispatch = core_->session.device().handle().Dispatch();
        vvk::CommandBuffer        command(native.unwrap(), dispatch);
        vk::RecordedBufferUploads recorded;
        if (core_->buffers.HasPendingUploads()) {
            if (! core_->buffers.RecordPendingUploads(command, recorded))
                return Err(error("display upload"_str, "buffer recording failed"_str));
            frame->uploads = core_->buffers.CommitRecordedUploads(rstd::move(recorded));
            if (! frame->uploads)
                return Err(error("display upload"_str, "buffer upload lease failed"_str));
        }
        core_->scopes.collect();
        if (! compiler_) {
            compiler_ = Some(vk::Renderer(core_->session));
            compiler_
                ->bind(Arc<dyn<vk::PassFactory>>::make(
                    vk::TypedPassFactory<PassDesc, Factory> { Factory { core_.clone() } }))
                .unwrap();
        }
        if (! shape_ || *shape_ != shape) {
            vrento::fg::FrameGraphBuilder builder(1);
            for (usize i {}; i < shape.textures.len(); ++i) {
                const auto&                     resource = shape.textures[i];
                vrento::fg::ResourceDescription desc { .id   = { i.to_primitive() + 1, 1 },
                                                       .kind = vrento::fg::ResourceKind::Texture,
                                                       .mip_levels = resource.view.range.mip_count,
                                                       .layers = resource.view.range.layer_count };
                if (resource.imported)
                    builder.import_resource(rstd::move(desc)).unwrap();
                else
                    builder.create_resource(rstd::move(desc)).unwrap();
            }
            Vec<vk::PassInput> descriptions;
            for (usize i {}; i < shape.draws.len(); ++i) {
                const auto&    draw = shape.draws[i];
                vk::ResourceId uniform { shape.textures.len().to_primitive() + i.to_primitive() + 1,
                                         1 };
                builder
                    .import_resource({ .id    = uniform,
                                       .kind  = vrento::fg::ResourceKind::Buffer,
                                       .bytes = sizeof(Parameters) })
                    .unwrap();
                vk::PassId pass_id { i.to_primitive() + 1 };
                builder.add_pass(pass_id).unwrap();
                vk::ResourceId output { draw.target + 1, 1 };
                bool           inherits = draw.load == vk::AttachmentLoad::Load;
                auto write = builder.reserve_write(pass_id, { output, 0 }, inherits, inherits);
                if (write.is_err()) return Err(graph_error(write.unwrap_err()));
                PassDesc desc;
                desc.recipe  = draw.recipe;
                desc.io.kind = kind;
                desc.signature.color_format =
                    vk::ToVkType(shape.textures[usize(draw.target)].description.format);
                desc.signature.has_depth_attachment = false;
                vk::GraphicsDescription graphics;
                graphics.color   = Some(vk::GraphAttachment { output, 1, draw.load });
                desc.io.graphics = Some(graphics);
                auto content     = [&](vk::ResourceId id, unsigned version) {
                    return vrento::pass::ContentRef::bind(builder, { id, version }).unwrap();
                };
                for (unsigned j = 0; j < sampler_count; ++j) {
                    bool duplicate {};
                    for (unsigned k = 0; k < j; ++k)
                        duplicate |= draw.sampled[k] == draw.sampled[j];
                    if (duplicate) continue;
                    unsigned texture = draw.sampled[j];
                    desc.io.inputs.push({ content({ texture + 1, 1 },
                                                  shape.textures[usize(texture)].imported ? 0 : 1),
                                          vk::PassUsage::Sampled,
                                          vk::PassStages::Fragment });
                }
                vk::VulkanResourceUse uniform_use { content(uniform, 0),
                                                    vk::PassUsage::Uniform,
                                                    vk::PassStages::Fragment };
                uniform_use.range.size = sizeof(Parameters);
                desc.io.inputs.push(rstd::move(uniform_use));
                if (inherits) {
                    desc.io.inputs.push({ content(output, 0),
                                          vk::PassUsage::ColorAttachment,
                                          vk::PassStages::Color });
                    desc.io.storage.push({ content(output, 1), content(output, 0) });
                }
                desc.io.outputs.push(
                    { content(output, 1), vk::PassUsage::ColorAttachment, vk::PassStages::Color });
                auto declared = vk::declare_input(builder, pass_id, rstd::move(desc));
                if (declared.is_err()) return Err(graph_error(declared.unwrap_err()));
                descriptions.push(declared.unwrap());
            }
            auto graph = rstd::move(builder).finish();
            if (graph.is_err()) return Err(graph_error(graph.unwrap_err()));
            auto compiled = compiler_->compile(graph.unwrap(), rstd::move(descriptions));
            if (compiled.is_err()) return Err(graph_error(compiled.unwrap_err()));
            shape_ = Some(rstd::move(shape));
        }
        if (compiler_->pending()) {
            auto continued = compiler_->continue_compile(compiler_->status().id);
            if (continued.is_err()) return Err(graph_error(continued.unwrap_err()));
            if (*continued == vk::CompileProgress::Pending)
                return Err(error("display pass"_str, "prewarmed pipeline unavailable"_str));
        }
        auto                      compiled = compiler_->compiled().unwrap();
        Vec<vk::PassFrameBinding> frames;
        for (usize i {}; i < frame->stages.len(); ++i) {
            auto input = compiled.frame<Pass>(
                { i.to_primitive() + 1 },
                1,
                Prepared::FrameData { frame.clone(), unsigned(i.to_primitive()) });
            if (input.is_err()) return Err(graph_error(input.unwrap_err()));
            frames.push(input.unwrap());
        }
        auto prepared =
            compiled.instantiate(bindings,
                                 frames.as_slice(),
                                 Box<dyn<vk::GraphicsScopeBackend>>::make(Scopes { core_.clone() }),
                                 true,
                                 &work);
        if (prepared.is_err()) return Err(graph_error(prepared.unwrap_err()));
        if (! prepared->ready())
            return Err(error("display pass"_str, "unexpected preparation pending"_str));
        auto recorded_frame = prepared->take_ready().record(work);
        if (recorded_frame.is_err()) return Err(graph_error(recorded_frame.unwrap_err()));
        guard.accepted = true;
        ++statistics_.recorded_frames;
        return Ok(empty {});
    }
    void close() {
        compiler_ = None();
        shape_    = None();
        if (core_ && core_->preparation) {
            core_->preparation->close();
            core_->warmed.clear();
        }
    }
};
} // namespace vecgltf
