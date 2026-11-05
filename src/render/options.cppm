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
struct ShadowOptions {
    unsigned     resolution { 1024 };
    ShadowFilter filter { ShadowFilter::Bilinear };
    // Receiver depth bias is clamp(slope_bias * tan(theta), bias, max_bias).
    float bias { 0.00005f }, slope_bias { 0.05f }, max_bias { 0.01f };
    // Offset along the world-space surface normal, in scene units.
    float normal_bias {};
};
using DirectionalShadow = ShadowOptions;
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
struct PointLight {
    Vec3  position {}, color { 1, 1, 1 };
    float intensity { 3 };
    // Constant, linear and quadratic distance terms.
    Vec3       attenuation { 1, 0, 1 };
    LightSpace space { LightSpace::World };
};
struct SpotLight {
    PointLight point;
    // Direction from the light into the cone; cosine limits are in [-1, 1].
    Vec3                  direction { 0, 0, -1 };
    float                 inner_cos { 1 }, outer_cos { 0 };
    Option<ShadowOptions> shadow;
};
class SpotLights {
    Option<Arc<Vec<SpotLight>>> values_;

public:
    SpotLights() = default;
    explicit SpotLights(Vec<SpotLight> values)
        : values_(Some(Arc<Vec<SpotLight>>::make(rstd::move(values)))) {}
    SpotLights(const SpotLights& other)
        : values_(other.present() ? Some(other.values_->clone()) : None()) {}
    SpotLights(SpotLights&&)                    = default;
    auto operator=(SpotLights&&) -> SpotLights& = default;
    auto operator=(const SpotLights& other) -> SpotLights& {
        if (this != &other) values_ = other.present() ? Some(other.values_->clone()) : None();
        return *this;
    }
    bool                  present() const { return values_.is_some(); }
    explicit              operator bool() const { return present(); }
    const Vec<SpotLight>& values() const { return **values_; }
};
inline constexpr unsigned max_point_lights = 8;
class PointLights {
    Option<Arc<Vec<PointLight>>> values_;

public:
    PointLights() = default;
    explicit PointLights(Vec<PointLight> values)
        : values_(Some(Arc<Vec<PointLight>>::make(rstd::move(values)))) {}
    PointLights(const PointLights& other)
        : values_(other.present() ? Some(other.values_->clone()) : None()) {}
    PointLights(PointLights&&)                    = default;
    auto operator=(PointLights&&) -> PointLights& = default;
    auto operator=(const PointLights& other) -> PointLights& {
        if (this != &other) values_ = other.present() ? Some(other.values_->clone()) : None();
        return *this;
    }
    bool                   present() const { return values_.is_some(); }
    explicit               operator bool() const { return present(); }
    const Vec<PointLight>& values() const { return **values_; }
};
enum class MaterialAlphaMode
{
    Asset,
    // Replaces BLEND with stochastic coverage and depth writes.
    Dither
};
struct MaterialOverride {
    unsigned          material {};
    MaterialAlphaMode alpha_mode { MaterialAlphaMode::Asset };
};
class MaterialOverrides {
    Option<Arc<Vec<MaterialOverride>>> values_;

public:
    MaterialOverrides() = default;
    explicit MaterialOverrides(Vec<MaterialOverride> values)
        : values_(Some(Arc<Vec<MaterialOverride>>::make(rstd::move(values)))) {}
    MaterialOverrides(const MaterialOverrides& other)
        : values_(other.present() ? Some(other.values_->clone()) : None()) {}
    MaterialOverrides(MaterialOverrides&&)                    = default;
    auto operator=(MaterialOverrides&&) -> MaterialOverrides& = default;
    auto operator=(const MaterialOverrides& other) -> MaterialOverrides& {
        if (this != &other) values_ = other.present() ? Some(other.values_->clone()) : None();
        return *this;
    }
    bool                         present() const { return values_.is_some(); }
    explicit                     operator bool() const { return present(); }
    const Vec<MaterialOverride>& values() const { return **values_; }
};
struct ShadowCatcher {
    // Maps the local XY square [-1,1] into world space.
    Mat4 transform { Mat4::Identity() };
    // Shadow alpha is opacity * (1 - visibility) * clamp(1 - radius^2 * fade, 0, 1)^2.
    float opacity { 1 }, fade {};
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
    Mat4        world_to_environment { Mat4::Identity() };
    float       background_intensity { 1 };
    PointLights point_lights;
    // Opaque linear background before geometry; takes precedence over the environment.
    Option<Vec3> background_color;
    // Stable screen-space sample; callers may advance it when accumulating frames.
    unsigned              alpha_dither_seed {};
    Option<ShadowCatcher> shadow_catcher;
    SpotLights            spot_lights;
};
struct SceneOptions {
    DebugView                debug {};
    bool                     background {};
    Option<DirectionalLight> direct_light { Some(DirectionalLight {}) };
    float                    ibl_intensity { 1 };
    DirectionalLights        directional_lights;
    Mat4                     world_to_environment { Mat4::Identity() };
    float                    background_intensity { 1 };
    PointLights              point_lights;
    Option<Vec3>             background_color;
    unsigned                 alpha_dither_seed {};
    Option<ShadowCatcher>    shadow_catcher;
    SpotLights               spot_lights;
};
} // namespace vecgltf
