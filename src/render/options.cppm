export module vecgltf.render.options;
export import vecgltf.scene;

export namespace vecgltf
{
enum class DebugView
{
    None,
    BaseColor,
    Normal,
    Metallic,
    Roughness,
    Emissive
};
enum class LightSpace
{
    World,
    View
};
enum class ShadowFilter
{
    Nearest,
    Bilinear
};
struct DirectionalShadow {
    unsigned     resolution { 1024 };
    ShadowFilter filter { ShadowFilter::Bilinear };
    // Receiver depth bias is clamp(slope_bias * tan(theta), bias, max_bias).
    float bias { 0.00005f }, slope_bias { 0.05f }, max_bias { 0.01f };
    // Offset along the world-space surface normal, in scene units.
    float normal_bias {};
};
struct DirectionalLight {
    // Direction toward the light; magnitude does not affect intensity.
    Vec3                      direction { 0.3f, 0.7f, 1 };
    Vec3                      color { 1, 1, 1 };
    float                     intensity { 3 };
    LightSpace                space { LightSpace::World };
    Option<DirectionalShadow> shadow;
};
inline constexpr unsigned max_directional_lights = 8;
class DirectionalLights {
    Option<Arc<Vec<DirectionalLight>>> values_;

public:
    DirectionalLights() = default;
    explicit DirectionalLights(Vec<DirectionalLight> values)
        : values_(Some(Arc<Vec<DirectionalLight>>::make(rstd::move(values)))) {}
    DirectionalLights(const DirectionalLights& other)
        : values_(other.present() ? Some(other.values_->clone()) : None()) {}
    DirectionalLights(DirectionalLights&&)                    = default;
    auto operator=(DirectionalLights&&) -> DirectionalLights& = default;
    auto operator=(const DirectionalLights& other) -> DirectionalLights& {
        if (this != &other) values_ = other.present() ? Some(other.values_->clone()) : None();
        return *this;
    }
    bool                         present() const { return values_.is_some(); }
    explicit                     operator bool() const { return present(); }
    const Vec<DirectionalLight>& values() const { return **values_; }
};
enum class ToneMapping
{
    ACES,
    ReinhardMax,
    Linear
};
struct RenderOptions {
    float                    exposure { 1 };
    DebugView                debug {};
    bool                     background {};
    Option<DirectionalLight> direct_light { Some(DirectionalLight {}) };
    // Scales indirect illumination; the background has an independent intensity.
    float       ibl_intensity { 1 };
    ToneMapping tone_mapping { ToneMapping::ACES };
    // An absent override uses direct_light; an explicit empty list disables direct lighting.
    DirectionalLights directional_lights;
    // Pure rotation from world directions into the environment's Y-up basis.
    Mat4  world_to_environment { Mat4::Identity() };
    float background_intensity { 1 };
};
struct SceneOptions {
    DebugView                debug {};
    bool                     background {};
    Option<DirectionalLight> direct_light { Some(DirectionalLight {}) };
    float                    ibl_intensity { 1 };
    DirectionalLights        directional_lights;
    Mat4                     world_to_environment { Mat4::Identity() };
    float                    background_intensity { 1 };
};
} // namespace vecgltf
