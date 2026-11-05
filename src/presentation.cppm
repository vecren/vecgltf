export module vecgltf.presentation;
export import vecgltf.display;
export import vecgltf.render;
export import vecgltf.environment;
import rstd.json;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;

export namespace vecgltf
{
enum class PresentationSpace
{
    World,
    View,
    SceneRoot,
    Node
};
struct PresentationReference {
    PresentationSpace space { PresentationSpace::World };
    unsigned          node { gltf::invalid_index };
};
struct PresentationLight {
    gltf::DirectionalLight light;
    PresentationReference  reference;
};
struct PresentationPointLight {
    gltf::PointLight      light;
    PresentationReference reference;
    bool                  reference_attenuation {};
};
struct PresentationSpotLight {
    PresentationPointLight point;
};
struct PresentationCamera {
    CameraLookAt          look_at;
    Vec3                  up { 0, 1, 0 };
    CameraProjection      projection;
    PresentationReference reference;
    // Omitted clip lengths use fitted world near and infinite far, independent of reference scale.
    bool reference_clip {}, explicit_near {}, explicit_far {};
};
struct PresentationEnvironment {
    String                background, diffuse, brdf;
    Vec<String>           specular;
    float                 lod_scale { -1 }, lod_exponent { 1 };
    vrento::TextureFilter brdf_filter { vrento::TextureFilter::LINEAR };
};
struct PresentationSpec {
    String                          source;
    Option<rstd::path::PathBuf>     resource_base_directory;
    gltf::SceneOptions              scene;
    gltf::DisplayOptions            display;
    Option<PresentationEnvironment> environment;
    Vec<PresentationLight>          lights;
    Vec<PresentationPointLight>     point_lights;
    Vec<PresentationSpotLight>      spot_lights;
    gltf::MaterialOverrides         material_overrides;
    PresentationReference           shadow_catcher_reference;
    PresentationReference           environment_reference;
    gltf::Mat4                      environment_to_reference { gltf::Mat4::Identity() };
    bool                            environment_transform {};
    Option<PresentationCamera>      camera;
};
struct PresentationResources {
    ResourceAccess              resolver;
    rstd::path::PathBuf         base_directory;
    Option<rstd::path::PathBuf> root;
    // Cumulative encoded bytes; repeated paths are read and decoded once.
    usize max_bytes { 64 * 1024 * 1024 };
};
struct PresentationOverrides {
    float                  exposure_multiplier { 1 };
    Option<CameraFrame>    camera;
    Option<SceneOptions>   scene;
    Option<DisplayOptions> display;
};
struct ResolvedPresentation {
    Option<CameraFrame> camera;
    SceneOptions        scene;
    DisplayOptions      display;
};
class BoundPresentation {
    Arc<Asset>                asset_;
    u64                       generation_;
    PresentationSpec          spec_;
    Option<EnvironmentImages> environment_;
    friend auto               BindPresentation(PresentationSpec, Arc<Asset>, PresentationResources)
        -> Result<BoundPresentation, Error>;
    friend auto ResolvePresentation(const BoundPresentation&,
                                    const SceneInstance&,
                                    const PresentationOverrides&)
        -> Result<ResolvedPresentation, Error>;
    BoundPresentation(PresentationSpec          spec,
                      Arc<Asset>                asset,
                      Option<EnvironmentImages> environment)
        : asset_(rstd::move(asset)),
          generation_(asset_->generation),
          spec_(rstd::move(spec)),
          environment_(rstd::move(environment)) {}

public:
    BoundPresentation(BoundPresentation&&)                                       = default;
    auto                    operator=(BoundPresentation&&) -> BoundPresentation& = default;
    const PresentationSpec& spec() const { return spec_; }
    auto                    asset() const -> Arc<Asset> { return asset_.clone(); }
    auto                    generation() const -> u64 { return generation_; }
    auto                    prepare_options() const -> PrepareOptions {
        return { .material_overrides = spec_.material_overrides };
    }
    auto take_environment() -> Option<EnvironmentImages> { return environment_.take(); }
};
auto ParsePresentation(slice<u8> bytes, ref<str> source = {}) -> Result<PresentationSpec, Error>;
auto LoadPresentation(ref<str> source) -> Result<PresentationSpec, Error>;
auto BindPresentation(PresentationSpec spec, Arc<Asset> asset, PresentationResources resources = {})
    -> Result<BoundPresentation, Error>;
auto ResolvePresentation(const BoundPresentation&     bound,
                         const SceneInstance&         instance,
                         const PresentationOverrides& overrides = {})
    -> Result<ResolvedPresentation, Error>;
} // namespace vecgltf

namespace vecgltf::presentation_detail
{
using Json = rstd::json::Value;

auto error(ref<str>        source,
           ref<str>        location,
           ref<str>        message,
           gltf::ErrorKind kind = gltf::ErrorKind::InvalidAsset) -> gltf::Error {
    return { kind, String::make(source), String::make(location), String::make(message) };
}
bool contains_nul(ref<str> value) {
    for (auto byte : value.as_bytes())
        if (byte == u8()) return true;
    return false;
}
auto rotation(const gltf::Mat4& matrix, ref<str> source, ref<str> location, bool strip_scale)
    -> Result<gltf::Mat4, gltf::Error> {
    auto invalid = [&]() {
        return error(
            source, location, "reference must have a finite, proper orthogonal rotation"_str);
    };
    for (auto value : matrix.m)
        if (! rstd::f32(value).is_finite()) return Err(invalid());
    if (matrix.m[3] != 0 || matrix.m[7] != 0 || matrix.m[11] != 0 || matrix.m[15] != 1)
        return Err(invalid());
    if (! strip_scale && (matrix.m[12] != 0 || matrix.m[13] != 0 || matrix.m[14] != 0))
        return Err(invalid());
    gltf::Vec3 columns[3];
    for (unsigned column = 0; column < 3; ++column)
        columns[column] = { matrix.m[column * 4],
                            matrix.m[column * 4 + 1],
                            matrix.m[column * 4 + 2] };
    if (! strip_scale) {
        for (unsigned i = 0; i < 3; ++i) {
            if (gltf::absolute(gltf::dot(columns[i], columns[i]) - 1) > 0.0001f)
                return Err(invalid());
            for (unsigned j = 0; j < i; ++j)
                if (gltf::absolute(gltf::dot(columns[i], columns[j])) > 0.0001f)
                    return Err(invalid());
        }
        if (gltf::absolute(matrix.determinant3() - 1) > 0.0001f) return Err(invalid());
    }
    for (auto& column : columns) {
        auto scale =
            gltf::maximum(gltf::absolute(column.x),
                          gltf::maximum(gltf::absolute(column.y), gltf::absolute(column.z)));
        if (scale == 0) return Err(invalid());
        column = { column.x / scale, column.y / scale, column.z / scale };
        column = column / gltf::square_root(gltf::dot(column, column));
    }
    if (gltf::absolute(gltf::dot(columns[0], columns[1])) > 0.0001f ||
        gltf::absolute(gltf::dot(columns[0], columns[2])) > 0.0001f ||
        gltf::absolute(gltf::dot(columns[1], columns[2])) > 0.0001f ||
        gltf::absolute(gltf::dot(columns[0], gltf::cross(columns[1], columns[2])) - 1) > 0.0001f)
        return Err(invalid());
    auto result = gltf::Mat4::Identity();
    for (unsigned column = 0; column < 3; ++column) {
        result.m[column * 4]     = columns[column].x;
        result.m[column * 4 + 1] = columns[column].y;
        result.m[column * 4 + 2] = columns[column].z;
    }
    return Ok(result);
}
auto transpose(const gltf::Mat4& matrix) -> gltf::Mat4 {
    auto result = gltf::Mat4::Identity();
    for (unsigned column = 0; column < 3; ++column)
        for (unsigned row = 0; row < 3; ++row)
            result.m[column * 4 + row] = matrix.m[row * 4 + column];
    return result;
}
using EnvironmentFiles = PresentationEnvironment;
class Reader {
    ref<str>            source_;
    Option<gltf::Error> error_;

public:
    explicit Reader(ref<str> source): source_(source) {}
    void fail(ref<str>        location,
              ref<str>        message,
              gltf::ErrorKind kind = gltf::ErrorKind::InvalidAsset) {
        if (! error_) error_ = Some(error(source_, location, message, kind));
    }
    auto failure() -> Option<gltf::Error> { return rstd::move(error_); }
    bool object(const Json& value, ref<str> location) {
        if (value.is_Object()) return true;
        fail(location, "expected an object"_str);
        return false;
    }
    auto field(const Json& value, ref<str> key) const -> const Json* {
        auto found = value.get(key);
        return found ? &found.unwrap().get() : nullptr;
    }
    auto number(const Json& value, ref<str> location) -> float {
        auto number = value.as_f64();
        if (number && number->is_finite()) {
            auto result = float(number->to_primitive());
            if (rstd::f32(result).is_finite()) return result;
        }
        fail(location, "expected a finite number"_str);
        return 0;
    }
    void number(const Json& value, ref<str> key, float& target, ref<str> path) {
        if (auto input = field(value, key))
            target = number(*input, rstd::format("{}.{}", path, key).as_str());
    }
    void nonnegative(const Json& value, ref<str> key, float& target, ref<str> path) {
        number(value, key, target, path);
        if (target < 0)
            fail(rstd::format("{}.{}", path, key).as_str(), "value must be nonnegative"_str);
    }
    auto text(const Json& value, ref<str> location) -> String {
        auto input = value.as_str();
        if (! input || input->is_empty() || contains_nul(*input)) {
            fail(location, "expected a nonempty string without NUL"_str);
            return {};
        }
        return String::make(*input);
    }
    auto required_text(const Json& value, ref<str> key, ref<str> path) -> String {
        auto location = rstd::format("{}.{}", path, key);
        auto input    = field(value, key);
        if (! input) {
            fail(location.as_str(), "required resource path is missing"_str);
            return {};
        }
        return text(*input, location.as_str());
    }
    void boolean(const Json& value, ref<str> key, bool& target, ref<str> path) {
        if (auto input = field(value, key)) {
            auto result = input->as_bool();
            if (! result)
                fail(rstd::format("{}.{}", path, key).as_str(), "expected a boolean"_str);
            else
                target = *result;
        }
    }
    auto array(const Json& value, unsigned size, ref<str> path) -> const rstd::json::Array* {
        auto result = value.as_array();
        if (! result || result.unwrap().get().len() != usize(size)) {
            fail(path, "array has the wrong number of elements"_str);
            return nullptr;
        }
        return &result.unwrap().get();
    }
    auto vector3(const Json& value, ref<str> path) -> gltf::Vec3 {
        auto values = array(value, 3, path);
        if (! values) return {};
        return { number((*values)[usize()], path),
                 number((*values)[usize(1)], path),
                 number((*values)[usize(2)], path) };
    }
    auto unsigned_integer(const Json& value, ref<str> path) -> unsigned {
        auto input = value.as_f64();
        if (input && input->is_finite() && input->to_primitive() >= 0 &&
            input->to_primitive() <= 4294967295.0 &&
            input->to_primitive() == input->floor().to_primitive())
            return unsigned(input->to_primitive());
        fail(path, "expected an integer from zero to 4294967295"_str);
        return 0;
    }
    void materials(const Json& input, PresentationSpec& settings) {
        auto values = input.as_array();
        if (! values) {
            fail("material_overrides"_str, "expected an array"_str);
            return;
        }
        Vec<gltf::MaterialOverride> overrides;
        for (const auto& value : values.unwrap().get()) {
            auto path = rstd::format("material_overrides[{}]", overrides.len());
            if (! object(value, path.as_str())) return;
            gltf::MaterialOverride entry;
            if (auto material = field(value, "material"_str))
                entry.material =
                    unsigned_integer(*material, rstd::format("{}.material", path).as_str());
            else
                fail(path.as_str(), "material is required"_str);
            if (auto mode = field(value, "alpha_mode"_str)) {
                auto name = text(*mode, rstd::format("{}.alpha_mode", path).as_str());
                if (name == "dither"_str)
                    entry.alpha_mode = gltf::MaterialAlphaMode::Dither;
                else if (name != "asset"_str)
                    fail(rstd::format("{}.alpha_mode", path).as_str(),
                         "alpha mode must be asset or dither"_str);
            }
            for (const auto& previous : overrides)
                if (previous.material == entry.material)
                    fail(path.as_str(), "material override is duplicated"_str);
            overrides.push(rstd::move(entry));
        }
        settings.material_overrides = gltf::MaterialOverrides(rstd::move(overrides));
    }
    void shadow_catcher(const Json& input, PresentationSpec& settings) {
        if (! object(input, "shadow_catcher"_str)) return;
        gltf::ShadowCatcher result;
        gltf::Vec3          position {};
        float               scale = 1;
        if (auto value = field(input, "position"_str))
            position = vector3(*value, "shadow_catcher.position"_str);
        number(input, "scale"_str, scale, "shadow_catcher"_str);
        if (scale <= 0) fail("shadow_catcher.scale"_str, "scale must be positive"_str);
        if (auto value = field(input, "reference"_str))
            settings.shadow_catcher_reference = reference(*value, "shadow_catcher.reference"_str);
        if (settings.shadow_catcher_reference.space == PresentationSpace::View)
            fail("shadow_catcher.reference"_str,
                 "view-space shadow catcher references are unsupported"_str,
                 gltf::ErrorKind::Unsupported);
        nonnegative(input, "opacity"_str, result.opacity, "shadow_catcher"_str);
        if (result.opacity > 1)
            fail("shadow_catcher.opacity"_str, "opacity must be at most one"_str);
        nonnegative(input, "fade"_str, result.fade, "shadow_catcher"_str);
        result.transform = gltf::compose(position, { 0, 0, 0, 1 }, { scale, scale, scale });
        settings.scene.shadow_catcher = Some(result);
    }
    auto reference(const Json& value, ref<str> location) -> PresentationReference {
        if (auto input = value.as_str()) {
            if (*input == "world"_str) return {};
            if (*input == "view"_str) return { PresentationSpace::View };
            if (*input == "scene-root"_str) return { PresentationSpace::SceneRoot };
        } else if (auto input = value.as_f64()) {
            auto number = input->to_primitive();
            if (input->is_finite() && number >= 0 && number < gltf::invalid_index &&
                number == input->floor().to_primitive())
                return { PresentationSpace::Node, unsigned(number) };
        }
        fail(location, "expected world, view, scene-root or a node index"_str);
        return {};
    }
    void camera(const Json& input, PresentationSpec& settings) {
        if (! object(input, "camera"_str)) return;
        PresentationCamera result;
        auto               required_vector = [&](ref<str> key, Vec3& output) {
            auto path = rstd::format("camera.{}", key);
            if (auto value = field(input, key))
                output = vector3(*value, path.as_str());
            else
                fail(path.as_str(), "required camera vector is missing"_str);
        };
        required_vector("position"_str, result.look_at.position);
        required_vector("target"_str, result.look_at.target);
        if (auto up = field(input, "up"_str)) result.up = vector3(*up, "camera.up"_str);
        number(input, "yfov"_str, result.projection.yfov, "camera"_str);
        if (auto value = field(input, "reference"_str))
            result.reference = reference(*value, "camera.reference"_str);
        if (result.reference.space == PresentationSpace::View)
            fail("camera.reference"_str, "camera cannot reference view space"_str);
        if (auto value = field(input, "clip_units"_str)) {
            auto units            = text(*value, "camera.clip_units"_str);
            result.reference_clip = units == "reference"_str;
            if (! result.reference_clip && units != "world"_str)
                fail("camera.clip_units"_str, "clip units must be world or reference"_str);
        }
        result.explicit_near = field(input, "near"_str) != nullptr;
        result.explicit_far  = field(input, "far"_str) != nullptr;
        number(input, "near"_str, result.projection.near, "camera"_str);
        number(input, "far"_str, result.projection.far, "camera"_str);
        auto checked = result.projection;
        checked.near = 0.01f;
        checked.far  = 0;
        if (! camera_frame(result.look_at, checked, result.up) ||
            (result.explicit_near && result.projection.near <= 0) ||
            (result.explicit_far && result.projection.far < 0) ||
            (result.explicit_near && result.explicit_far && result.projection.far != 0 &&
             result.projection.far <= result.projection.near))
            fail("camera"_str, "camera vectors or perspective projection are invalid"_str);
        settings.camera = Some(result);
    }
    auto shadow(const Json& input, ref<str> path) -> gltf::DirectionalShadow {
        gltf::DirectionalShadow result;
        if (! object(input, path)) return result;
        if (auto input_resolution = field(input, "resolution"_str)) {
            auto value = input_resolution->as_f64();
            if (value && value->is_finite() && value->to_primitive() >= 1 &&
                value->to_primitive() <= 4294967295.0 &&
                value->to_primitive() == value->floor().to_primitive())
                result.resolution = unsigned(value->to_primitive());
            else
                fail(rstd::format("{}.resolution", path).as_str(),
                     "expected an integer from one to 4294967295"_str);
        }
        if (auto input_filter = field(input, "filter"_str)) {
            auto name = text(*input_filter, rstd::format("{}.filter", path).as_str());
            if (name == "nearest"_str)
                result.filter = gltf::ShadowFilter::Nearest;
            else if (name == "bilinear"_str)
                result.filter = gltf::ShadowFilter::Bilinear;
            else
                fail(rstd::format("{}.filter", path).as_str(),
                     "shadow filter must be nearest or bilinear"_str);
        }
        nonnegative(input, "bias"_str, result.bias, path);
        nonnegative(input, "slope_bias"_str, result.slope_bias, path);
        nonnegative(input, "max_bias"_str, result.max_bias, path);
        nonnegative(input, "normal_bias"_str, result.normal_bias, path);
        if (result.max_bias < result.bias)
            fail(rstd::format("{}.max_bias", path).as_str(), "max bias must be at least bias"_str);
        return result;
    }
    void lights(const Json& input, PresentationSpec& settings) {
        auto values = input.as_array();
        if (! values || values.unwrap().get().len() > usize(gltf::max_directional_lights)) {
            fail("directional_lights"_str, "expected at most eight directional lights"_str);
            return;
        }
        Vec<gltf::DirectionalLight> initial;
        for (const auto& value : values.unwrap().get()) {
            auto path = rstd::format("directional_lights[{}]", settings.lights.len());
            if (! object(value, path.as_str())) return;
            PresentationLight entry;
            entry.light.intensity = 1;
            if (auto direction = field(value, "direction"_str))
                entry.light.direction =
                    vector3(*direction, rstd::format("{}.direction", path).as_str());
            else
                fail(path.as_str(), "direction is required"_str);
            if (auto color = field(value, "color"_str))
                entry.light.color = vector3(*color, rstd::format("{}.color", path).as_str());
            if (entry.light.color.x < 0 || entry.light.color.y < 0 || entry.light.color.z < 0 ||
                (entry.light.direction.x == 0 && entry.light.direction.y == 0 &&
                 entry.light.direction.z == 0))
                fail(path.as_str(), "light color must be nonnegative and direction nonzero"_str);
            nonnegative(value, "intensity"_str, entry.light.intensity, path.as_str());
            if (auto input = field(value, "reference"_str))
                entry.reference = reference(*input, rstd::format("{}.reference", path).as_str());
            if (auto input = field(value, "shadow"_str))
                entry.light.shadow = Some(shadow(*input, rstd::format("{}.shadow", path).as_str()));
            entry.light.space = entry.reference.space == PresentationSpace::View
                                    ? gltf::LightSpace::View
                                    : gltf::LightSpace::World;
            initial.push(gltf::DirectionalLight(entry.light));
            settings.lights.push(rstd::move(entry));
        }
        settings.scene.direct_light       = None();
        settings.scene.directional_lights = gltf::DirectionalLights(rstd::move(initial));
    }
    void point_lights(const Json& input, PresentationSpec& settings, bool spot = false) {
        auto values = input.as_array();
        if (! values || values.unwrap().get().len() > usize(gltf::max_point_lights)) {
            fail("point_lights"_str, "expected at most eight point lights"_str);
            return;
        }
        Vec<gltf::PointLight> initial;
        Vec<gltf::SpotLight>  initial_spots;
        for (const auto& value : values.unwrap().get()) {
            auto path = spot ? rstd::format("spot_lights[{}]", settings.spot_lights.len())
                             : rstd::format("point_lights[{}]", settings.point_lights.len());
            if (! object(value, path.as_str())) return;
            PresentationPointLight entry;
            entry.light.intensity = 1;
            if (auto position = field(value, "position"_str))
                entry.light.position =
                    vector3(*position, rstd::format("{}.position", path).as_str());
            else
                fail(path.as_str(), "position is required"_str);
            if (auto color = field(value, "color"_str))
                entry.light.color = vector3(*color, rstd::format("{}.color", path).as_str());
            if (entry.light.color.x < 0 || entry.light.color.y < 0 || entry.light.color.z < 0)
                fail(rstd::format("{}.color", path).as_str(),
                     "light color must be nonnegative"_str);
            nonnegative(value, "intensity"_str, entry.light.intensity, path.as_str());
            if (auto attenuation = field(value, "attenuation"_str))
                entry.light.attenuation =
                    vector3(*attenuation, rstd::format("{}.attenuation", path).as_str());
            const auto& attenuation = entry.light.attenuation;
            if (attenuation.x < 0 || attenuation.y < 0 || attenuation.z < 0 ||
                (attenuation.x == 0 && attenuation.y == 0 && attenuation.z == 0))
                fail(rstd::format("{}.attenuation", path).as_str(),
                     "attenuation must be nonnegative and nonzero"_str);
            if (auto reference_value = field(value, "reference"_str))
                entry.reference =
                    reference(*reference_value, rstd::format("{}.reference", path).as_str());
            if (auto units_value = field(value, "attenuation_units"_str)) {
                auto units =
                    text(*units_value, rstd::format("{}.attenuation_units", path).as_str());
                entry.reference_attenuation = units == "reference"_str;
                if (! entry.reference_attenuation && units != "world"_str)
                    fail(path.as_str(), "attenuation units must be world or reference"_str);
            }
            entry.light.space = entry.reference.space == PresentationSpace::View
                                    ? gltf::LightSpace::View
                                    : gltf::LightSpace::World;
            if (spot) {
                gltf::SpotLight light;
                light.point = entry.light;
                if (auto direction = field(value, "direction"_str))
                    light.direction =
                        vector3(*direction, rstd::format("{}.direction", path).as_str());
                else
                    fail(path.as_str(), "direction is required"_str);
                number(value, "inner_cos"_str, light.inner_cos, path.as_str());
                number(value, "outer_cos"_str, light.outer_cos, path.as_str());
                if (gltf::dot(light.direction, light.direction) <= 1e-20f || light.outer_cos < -1 ||
                    light.inner_cos > 1 || light.outer_cos > light.inner_cos)
                    fail(path.as_str(), "invalid spot direction or cone"_str);
                initial_spots.push(rstd::move(light));
                settings.spot_lights.push({ rstd::move(entry) });
            } else {
                initial.push(gltf::PointLight(entry.light));
                settings.point_lights.push(rstd::move(entry));
            }
        }
        if (spot)
            settings.scene.spot_lights = gltf::SpotLights(rstd::move(initial_spots));
        else
            settings.scene.point_lights = gltf::PointLights(rstd::move(initial));
    }
    auto environment(const Json& input, PresentationSpec& settings) -> Option<EnvironmentFiles> {
        if (! object(input, "environment"_str)) return None();
        settings.environment_transform = true;
        if (auto value = field(input, "reference"_str))
            settings.environment_reference = reference(*value, "environment.reference"_str);
        if (auto basis = field(input, "basis"_str)) {
            if (auto values = array(*basis, 16, "environment.basis"_str))
                for (unsigned i = 0; i < 16; ++i)
                    settings.environment_to_reference.m[i] =
                        number((*values)[usize(i)], "environment.basis"_str);
            auto checked = rotation(
                settings.environment_to_reference, source_, "environment.basis"_str, false);
            if (checked.is_err()) {
                if (! error_) error_ = Some(checked.unwrap_err());
            } else
                settings.environment_to_reference = checked.unwrap();
        }
        nonnegative(input, "ibl_intensity"_str, settings.scene.ibl_intensity, "environment"_str);
        nonnegative(input,
                    "background_intensity"_str,
                    settings.scene.background_intensity,
                    "environment"_str);
        boolean(input, "background"_str, settings.scene.background, "environment"_str);
        if (auto color = field(input, "background_color"_str)) {
            auto value = vector3(*color, "environment.background_color"_str);
            if (value.x < 0 || value.y < 0 || value.z < 0)
                fail("environment.background_color"_str,
                     "background color must be nonnegative"_str);
            settings.scene.background_color = Some(value);
        }
        auto files = field(input, "preprocessed"_str);
        if (! files) return None();
        if (! object(*files, "environment.preprocessed"_str)) return None();
        EnvironmentFiles result;
        auto             path = "environment.preprocessed"_str;
        result.background     = required_text(*files, "background"_str, path);
        result.diffuse        = required_text(*files, "diffuse"_str, path);
        result.brdf           = required_text(*files, "brdf"_str, path);
        auto specular         = field(*files, "specular"_str);
        auto images           = specular ? specular->as_array() : Option<ref<rstd::json::Array>> {};
        if (! images || images.unwrap().get().is_empty() || images.unwrap().get().len() > usize(32))
            fail("environment.preprocessed.specular"_str,
                 "expected one to 32 mip resource paths"_str);
        else
            for (const auto& image : images.unwrap().get())
                result.specular.push(text(image, "environment.preprocessed.specular"_str));
        number(*files, "specular_lod_scale"_str, result.lod_scale, path);
        number(*files, "specular_lod_exponent"_str, result.lod_exponent, path);
        if ((result.lod_scale < 0 && result.lod_scale != -1) || result.lod_exponent <= 0)
            fail(path, "invalid specular LOD mapping"_str);
        if (auto filter = field(*files, "brdf_filter"_str)) {
            auto name = text(*filter, "environment.preprocessed.brdf_filter"_str);
            if (name == "nearest"_str)
                result.brdf_filter = vrento::TextureFilter::NEAREST;
            else if (name != "linear"_str)
                fail(path, "BRDF filter must be nearest or linear"_str);
        }
        return Some(rstd::move(result));
    }
    void display(const Json& input, PresentationSpec& settings) {
        if (! object(input, "display"_str)) return;
        auto& output = settings.display;
        nonnegative(input, "exposure"_str, output.exposure, "display"_str);
        nonnegative(input, "brightness"_str, output.grading.brightness, "display"_str);
        nonnegative(input, "saturation"_str, output.grading.saturation, "display"_str);
        number(input, "contrast"_str, output.grading.contrast, "display"_str);
        if (output.grading.contrast < -1)
            fail("display.contrast"_str, "contrast must be at least -1"_str);
        if (auto method = field(input, "tone_mapping"_str)) {
            auto name = text(*method, "display.tone_mapping"_str);
            if (name == "linear"_str)
                output.tone_mapping = gltf::ToneMapping::Linear;
            else if (name == "aces"_str)
                output.tone_mapping = gltf::ToneMapping::ACES;
            else if (name == "reinhard-max"_str)
                output.tone_mapping = gltf::ToneMapping::ReinhardMax;
            else
                fail("display.tone_mapping"_str, "unknown tone mapping method"_str);
        }
        if (auto value = field(input, "vignette"_str)) {
            if (! object(*value, "display.vignette"_str)) return;
            gltf::Vignette vignette;
            nonnegative(*value, "intensity"_str, vignette.intensity, "display.vignette"_str);
            nonnegative(*value, "inner_radius"_str, vignette.inner_radius, "display.vignette"_str);
            nonnegative(*value, "outer_radius"_str, vignette.outer_radius, "display.vignette"_str);
            if (auto center = field(*value, "center"_str))
                if (auto values = array(*center, 2, "display.vignette.center"_str))
                    vignette.center = { number((*values)[usize()], "display.vignette.center"_str),
                                        number((*values)[usize(1)],
                                               "display.vignette.center"_str) };
            if (vignette.intensity > 1 || vignette.outer_radius <= vignette.inner_radius)
                fail("display.vignette"_str, "invalid vignette intensity or radii"_str);
            output.vignette = Some(vignette);
        }
        if (auto value = field(input, "bloom"_str)) {
            if (! object(*value, "display.bloom"_str)) return;
            gltf::Bloom bloom;
            nonnegative(*value, "intensity"_str, bloom.intensity, "display.bloom"_str);
            nonnegative(*value, "threshold"_str, bloom.threshold, "display.bloom"_str);
            nonnegative(*value, "radius"_str, bloom.radius, "display.bloom"_str);
            if (bloom.radius > 1)
                fail("display.bloom.radius"_str, "radius must be at most one"_str);
            output.bloom = Some(bloom);
        }
        boolean(input, "fxaa"_str, output.fxaa, "display"_str);
        nonnegative(input, "sharpen"_str, output.sharpen, "display"_str);
        if (auto value = field(input, "grain"_str)) {
            if (! object(*value, "display.grain"_str)) return;
            gltf::Grain grain;
            nonnegative(*value, "strength"_str, grain.strength, "display.grain"_str);
            number(*value, "phase"_str, grain.phase, "display.grain"_str);
            if (grain.strength > 1)
                fail("display.grain.strength"_str, "strength must be at most one"_str);
            output.grain = Some(grain);
        }
        nonnegative(input, "sampling_scale"_str, output.sampling_scale, "display"_str);
        if (output.sampling_scale == 0)
            fail("display.sampling_scale"_str, "sampling scale must be positive"_str);
        if (auto value = field(input, "color_balance"_str)) {
            if (! object(*value, "display.color_balance"_str)) return;
            gltf::ColorBalance balance;
            auto               parse = [&](ref<str> key, gltf::Vec3& target) {
                if (auto color = field(*value, key)) {
                    auto path = rstd::format("display.color_balance.{}", key);
                    target    = vector3(*color, path.as_str());
                    if (target.x < -1 || target.x > 1 || target.y < -1 || target.y > 1 ||
                        target.z < -1 || target.z > 1)
                        fail(path.as_str(), "color balance components must be in [-1,1]"_str);
                }
            };
            parse("shadows"_str, balance.shadows);
            parse("midtones"_str, balance.midtones);
            parse("highlights"_str, balance.highlights);
            output.color_balance = Some(balance);
        }
    }
};
auto read_document(ref<str> source) -> Result<Vec<u8>, gltf::Error> {
    auto path   = rstd::path::PathBuf::from(source);
    auto opened = rstd::fs::File::open(path.as_path());
    if (opened.is_err())
        return Err(
            error(source, "settings"_str, "cannot open settings file"_str, gltf::ErrorKind::Io));
    auto file = opened.unwrap();
    auto info = file.metadata();
    if (info.is_err() || ! info->is_file())
        return Err(error(
            source, "settings"_str, "settings must be a regular file"_str, gltf::ErrorKind::Io));
    auto size = info->len().to_primitive();
    if (size > 64 * 1024)
        return Err(error(
            source, "settings"_str, "presentation exceeds 64 KiB"_str, gltf::ErrorKind::Limit));
    Vec<u8> bytes;
    bytes.resize(usize(size), u8());
    if (file.read_exact_at(bytes.as_mut_slice().as_mut_ref(), u64()).is_err())
        return Err(
            error(source, "settings"_str, "cannot read settings file"_str, gltf::ErrorKind::Io));
    return Ok(rstd::move(bytes));
}
auto reference_transform(PresentationReference      reference,
                         const gltf::SceneInstance& instance,
                         ref<str>                   source,
                         ref<str>                   location) -> Result<gltf::Mat4, gltf::Error> {
    if (reference.space == PresentationSpace::World) return Ok(gltf::Mat4::Identity());
    auto index = reference.node;
    if (reference.space == PresentationSpace::SceneRoot) {
        auto root = instance.scene_root();
        if (! root)
            return Err(
                error(source, location, "reference needs exactly one visible scene root"_str));
        index = root->index;
    }
    if (reference.space == PresentationSpace::View ||
        index >= gltf::count(instance.asset().nodes) || ! instance.visible(index))
        return Err(error(source, location, "reference node is not valid in this scene"_str));
    const auto& matrix = instance.pose(index).world;
    for (auto value : matrix.m)
        if (! rstd::f32(value).is_finite())
            return Err(
                error(source, location, "reference must have a finite affine transform"_str));
    if (matrix.m[3] != 0 || matrix.m[7] != 0 || matrix.m[11] != 0 || matrix.m[15] != 1)
        return Err(error(source, location, "reference must have a finite affine transform"_str));
    return Ok(matrix);
}
auto reference_rotation(PresentationReference      reference,
                        const gltf::SceneInstance& instance,
                        ref<str>                   source,
                        ref<str>                   location) -> Result<gltf::Mat4, gltf::Error> {
    auto matrix = reference_transform(reference, instance, source, location);
    if (matrix.is_err()) return Err(matrix.unwrap_err());
    return rotation(matrix.unwrap(), source, location, true);
}
auto uniform_scale(const Mat4& matrix, ref<str> source, ref<str> location) -> Result<float, Error> {
    auto checked = rotation(matrix, source, location, true);
    if (checked.is_err()) return Err(checked.unwrap_err());
    double squared[3] {};
    for (unsigned col = 0; col < 3; ++col)
        for (unsigned row = 0; row < 3; ++row)
            squared[col] += double(matrix.m[col * 4 + row]) * matrix.m[col * 4 + row];
    for (unsigned col = 1; col < 3; ++col)
        if (squared[col] < squared[0] * 0.9999 || squared[col] > squared[0] * 1.0001)
            return Err(error(source,
                             location,
                             "reference requires a positive uniform scale"_str,
                             ErrorKind::Unsupported));
    auto scale = float(rstd::f64(squared[0]).sqrt().to_primitive());
    if (! rstd::f32(scale).is_finite() || scale <= 0)
        return Err(error(source, location, "reference scale must be finite and positive"_str));
    return Ok(scale);
}
auto resolve_camera(const PresentationSpec& spec, const SceneInstance& instance)
    -> Result<CameraFrame, Error> {
    OrbitCamera fit;
    fit.fit(instance.bounds());
    if (! spec.camera) {
        auto frame = fit.frame();
        if (! frame.valid())
            return Err(error(spec.source.as_str(), "camera"_str, "fitted camera is invalid"_str));
        return Ok(frame);
    }
    const auto& input  = *spec.camera;
    auto        matrix = reference_transform(
        input.reference, instance, spec.source.as_str(), "camera.reference"_str);
    if (matrix.is_err()) return Err(matrix.unwrap_err());
    auto scale = uniform_scale(matrix.unwrap(), spec.source.as_str(), "camera.reference"_str);
    if (scale.is_err()) return Err(scale.unwrap_err());
    auto rotation_ = rotation(matrix.unwrap(), spec.source.as_str(), "camera.reference"_str, true);
    if (rotation_.is_err()) return Err(rotation_.unwrap_err());
    auto projection = input.projection;
    if (! input.explicit_near)
        projection.near = fit.frame().projection.near;
    else if (input.reference_clip)
        projection.near *= scale.unwrap();
    if (! input.explicit_far)
        projection.far = 0;
    else if (input.reference_clip)
        projection.far *= scale.unwrap();
    auto frame = camera_frame(
        CameraLookAt { matrix->point(input.look_at.position), matrix->point(input.look_at.target) },
        projection,
        rotation_->vector(input.up));
    if (! frame)
        return Err(error(
            spec.source.as_str(), "camera"_str, "resolved camera or clip lengths are invalid"_str));
    return Ok(*frame);
}
} // namespace vecgltf::presentation_detail

export namespace vecgltf
{
auto ParsePresentation(slice<u8> bytes, ref<str> source) -> Result<PresentationSpec, gltf::Error> {
    if (bytes.len() > usize(64 * 1024))
        return Err(presentation_detail::error(
            source, "presentation"_str, "presentation exceeds 64 KiB"_str, ErrorKind::Limit));
    auto parsed = rstd::json::from_slice(bytes, { .max_depth = usize(32) });
    if (parsed.is_err())
        return Err(presentation_detail::error(source,
                                              "settings"_str,
                                              rstd::format("{}", parsed.unwrap_err()).as_str(),
                                              gltf::ErrorKind::Parse));
    auto                        data = parsed.unwrap();
    presentation_detail::Reader reader(source);
    PresentationSpec            output;
    output.source = String::make(source);
    if (! reader.object(data, "settings"_str)) return Err(reader.failure().unwrap());
    auto version = reader.field(data, "version"_str);
    if (! version || reader.number(*version, "version"_str) != 2)
        reader.fail("version"_str, "presentation version must be two"_str);
    if (auto input = reader.field(data, "camera"_str)) reader.camera(*input, output);
    if (auto lights = reader.field(data, "directional_lights"_str)) reader.lights(*lights, output);
    if (auto lights = reader.field(data, "point_lights"_str)) reader.point_lights(*lights, output);
    if (auto lights = reader.field(data, "spot_lights"_str))
        reader.point_lights(*lights, output, true);
    if (auto values = reader.field(data, "material_overrides"_str))
        reader.materials(*values, output);
    if (auto value = reader.field(data, "shadow_catcher"_str))
        reader.shadow_catcher(*value, output);
    if (auto seed = reader.field(data, "alpha_dither_seed"_str))
        output.scene.alpha_dither_seed = reader.unsigned_integer(*seed, "alpha_dither_seed"_str);
    if (auto input = reader.field(data, "environment"_str))
        output.environment = reader.environment(*input, output);
    if (auto input = reader.field(data, "display"_str)) reader.display(*input, output);
    if (auto failed = reader.failure()) return Err(rstd::move(failed).unwrap());
    auto display = ValidateDisplayOptions(output.display);
    if (display.is_err()) return Err(display.unwrap_err());
    return Ok(rstd::move(output));
}
auto LoadPresentation(ref<str> source) -> Result<PresentationSpec, Error> {
    if (source.is_empty() || presentation_detail::contains_nul(source))
        return Err(presentation_detail::error(
            source, "presentation"_str, "invalid presentation path"_str));
    auto bytes = presentation_detail::read_document(source);
    if (bytes.is_err()) return Err(bytes.unwrap_err());
    auto parsed = ParsePresentation(bytes->as_slice(), source);
    if (parsed.is_err()) return Err(parsed.unwrap_err());
    auto output = parsed.unwrap();
    auto path   = rstd::path::PathBuf::from(source);
    auto parent = path.as_path().parent();
    output.resource_base_directory =
        Some(parent ? rstd::path::PathBuf::from(*parent) : rstd::path::PathBuf::from("."_str));
    return Ok(rstd::move(output));
}
} // namespace vecgltf

namespace vecgltf::presentation_detail
{
auto load_environment(const PresentationSpec& configured, PresentationResources resources)
    -> Result<Option<EnvironmentImages>, Error> {
    if (! configured.environment) return Ok(None<EnvironmentImages>());
    const auto& files = configured.environment;
    auto        base  = rstd::move(resources.base_directory);
    if (base.as_path().as_os_str().is_empty() && configured.resource_base_directory)
        base = rstd::path::PathBuf::from(configured.resource_base_directory->as_path());
    if (base.as_path().as_os_str().is_empty())
        return Err(error(configured.source.as_str(),
                         "environment"_str,
                         "environment resources require a base directory"_str));
    if (! resources.resolver.present()) {
        auto root =
            rstd::fs::canonicalize(resources.root ? resources.root->as_path() : base.as_path());
        if (root.is_err())
            return Err(error(configured.source.as_str(),
                             "environment"_str,
                             "cannot resolve resource root"_str,
                             ErrorKind::Io));
        resources.root     = Some(root.unwrap());
        resources.resolver = FileResources();
    }
    struct ImageEntry {
        String                 path;
        unsigned               remaining {};
        Option<gltf::HdrImage> image;
    };
    Vec<ImageEntry> images;
    auto            register_image = [&](const String& path) -> unsigned {
        auto child    = rstd::path::PathBuf::from(path.as_str());
        auto resolved = base.join(child.as_path()).as_path().to_string_lossy();
        for (unsigned i = 0; i < gltf::count(images); ++i)
            if (gltf::at(images, i).path == resolved) {
                ++gltf::at(images, i).remaining;
                return i;
            }
        auto index = gltf::count(images);
        images.push({ rstd::move(resolved), 1, {} });
        return index;
    };
    auto          background = register_image(files->background);
    auto          diffuse    = register_image(files->diffuse);
    auto          brdf       = register_image(files->brdf);
    Vec<unsigned> specular;
    for (const auto& file : files->specular) {
        specular.push(register_image(file));
    }
    usize remaining = resources.max_bytes;
    for (auto& image : images) {
        auto path = rstd::path::PathBuf::from(image.path.as_str());
        auto bytes =
            resources.resolver.Read({ ResourceKind::Image,
                                      configured.source.as_str(),
                                      image.path.as_str(),
                                      path.as_path(),
                                      resources.root ? Some(resources.root->as_path()) : None(),
                                      remaining });
        if (bytes.is_err()) return Err(bytes.unwrap_err());
        if (bytes->bytes().len() > remaining)
            return Err(error(configured.source.as_str(),
                             "environment"_str,
                             "environment resources exceed byte limit"_str,
                             ErrorKind::Limit));
        remaining -= bytes->bytes().len();
        auto loaded = gltf::LoadEnvironment(bytes.unwrap(), image.path.as_str());
        if (loaded.is_err()) return Err(loaded.unwrap_err());
        image.image = Some(loaded.unwrap());
    }
    auto take_image = [&](unsigned index) -> gltf::HdrImage {
        auto& entry = gltf::at(images, index);
        if (--entry.remaining == 0) return entry.image.take().unwrap();
        return { entry.image->width, entry.image->height, entry.image->rgba.clone() };
    };
    gltf::EnvironmentImages environment;
    environment.background = take_image(background);
    environment.diffuse    = take_image(diffuse);
    environment.brdf       = take_image(brdf);
    for (auto index : specular) environment.specular.push(take_image(index));
    environment.specular_lod_scale    = files->lod_scale;
    environment.specular_lod_exponent = files->lod_exponent;
    environment.brdf_filter           = files->brdf_filter;
    return Ok(Some(rstd::move(environment)));
}
auto resolve_scene(const PresentationSpec&    configured,
                   const gltf::SceneInstance& instance,
                   gltf::SceneOptions         current,
                   const CameraFrame&         camera) -> Result<gltf::SceneOptions, gltf::Error> {
    auto result = current;
    if (result.shadow_catcher) {
        auto transform =
            presentation_detail::reference_transform(configured.shadow_catcher_reference,
                                                     instance,
                                                     configured.source.as_str(),
                                                     "shadow_catcher.reference"_str);
        if (transform.is_err()) return Err(transform.unwrap_err());
        result.shadow_catcher->transform = transform.unwrap() * result.shadow_catcher->transform;
    }
    if (current.directional_lights) {
        const auto& inputs = current.directional_lights.values();
        if (! inputs.is_empty() && inputs.len() != configured.lights.len())
            return Err(
                presentation_detail::error(configured.source.as_str(),
                                           "directional_lights"_str,
                                           "light count differs from reference settings"_str));
        Vec<gltf::DirectionalLight> lights;
        for (unsigned i = 0; i < gltf::count(inputs); ++i) {
            auto light     = gltf::at(inputs, i);
            auto reference = gltf::at(configured.lights, i).reference;
            if (reference.space == PresentationSpace::View)
                light.space = gltf::LightSpace::View;
            else {
                auto rotation = presentation_detail::reference_rotation(
                    reference,
                    instance,
                    configured.source.as_str(),
                    rstd::format("directional_lights[{}].reference", i).as_str());
                if (rotation.is_err()) return Err(rotation.unwrap_err());
                light.direction = rotation.unwrap().vector(light.direction);
                light.space     = gltf::LightSpace::World;
            }
            lights.push(rstd::move(light));
        }
        result.directional_lights = gltf::DirectionalLights(rstd::move(lights));
    }
    auto resolve_local_light = [&](gltf::PointLight&             light,
                                   const PresentationPointLight& config) -> Result<Mat4, Error> {
        if (config.reference.space == PresentationSpace::View) {
            light.space = gltf::LightSpace::View;
            return Ok(Mat4::Identity());
        }
        auto transform = presentation_detail::reference_transform(
            config.reference, instance, configured.source.as_str(), "local_light.reference"_str);
        if (transform.is_err()) return Err(transform.unwrap_err());
        auto matrix    = transform.unwrap();
        light.position = matrix.point(light.position);
        if (config.reference_attenuation) {
            auto scale = uniform_scale(
                matrix, configured.source.as_str(), "local_light.attenuation_units"_str);
            if (scale.is_err()) return Err(scale.unwrap_err());
            float s = scale.unwrap();
            light.attenuation.y /= s;
            light.attenuation.z /= s * s;
            if (! rstd::f32(light.attenuation.y).is_finite() ||
                ! rstd::f32(light.attenuation.z).is_finite())
                return Err(error(configured.source.as_str(),
                                 "local_light.attenuation"_str,
                                 "attenuation conversion is not finite"_str));
        }
        light.space = gltf::LightSpace::World;
        return Ok(matrix);
    };
    if (current.point_lights) {
        const auto& inputs = current.point_lights.values();
        if (! inputs.is_empty() && inputs.len() != configured.point_lights.len())
            return Err(
                presentation_detail::error(configured.source.as_str(),
                                           "point_lights"_str,
                                           "light count differs from reference settings"_str));
        Vec<gltf::PointLight> lights;
        for (unsigned i = 0; i < gltf::count(inputs); ++i) {
            auto light    = gltf::at(inputs, i);
            auto resolved = resolve_local_light(light, gltf::at(configured.point_lights, i));
            if (resolved.is_err()) return Err(resolved.unwrap_err());
            lights.push(rstd::move(light));
        }
        result.point_lights = gltf::PointLights(rstd::move(lights));
    }
    if (current.spot_lights) {
        const auto& inputs = current.spot_lights.values();
        if (! inputs.is_empty() && inputs.len() != configured.spot_lights.len())
            return Err(error(configured.source.as_str(),
                             "spot_lights"_str,
                             "light count differs from reference settings"_str));
        Vec<gltf::SpotLight> lights;
        for (unsigned i = 0; i < gltf::count(inputs); ++i) {
            auto light = gltf::at(inputs, i);
            auto resolved =
                resolve_local_light(light.point, gltf::at(configured.spot_lights, i).point);
            if (resolved.is_err()) return Err(resolved.unwrap_err());
            auto matrix = resolved.unwrap();
            if (light.point.space == gltf::LightSpace::World) {
                auto orientation =
                    rotation(matrix, configured.source.as_str(), "spot_lights.reference"_str, true);
                if (orientation.is_err()) return Err(orientation.unwrap_err());
                light.direction = orientation.unwrap().vector(light.direction);
            }
            lights.push(rstd::move(light));
        }
        result.spot_lights = gltf::SpotLights(rstd::move(lights));
    }
    if (configured.environment_transform) {
        Result<Mat4, Error> rotation =
            configured.environment_reference.space == PresentationSpace::View
                ? Result<Mat4, Error>(Ok(compose({}, camera.orientation, { 1, 1, 1 })))
                : presentation_detail::reference_rotation(configured.environment_reference,
                                                          instance,
                                                          configured.source.as_str(),
                                                          "environment.reference"_str);
        if (rotation.is_err()) return Err(rotation.unwrap_err());
        result.world_to_environment =
            presentation_detail::transpose(rotation.unwrap() * configured.environment_to_reference);
    }
    return Ok(rstd::move(result));
}
} // namespace vecgltf::presentation_detail

export namespace vecgltf
{
auto BindPresentation(PresentationSpec spec, Arc<Asset> asset, PresentationResources resources)
    -> Result<BoundPresentation, Error> {
    auto valid = ValidatePrepareOptions(*asset, { .material_overrides = spec.material_overrides });
    if (valid.is_err()) return Err(valid.unwrap_err());
    BoundPresentation bound(rstd::move(spec), rstd::move(asset), {});
    SceneInstance     instance(bound.asset());
    auto              resolved = ResolvePresentation(bound, instance);
    if (resolved.is_err()) return Err(resolved.unwrap_err());
    auto environment = presentation_detail::load_environment(bound.spec(), rstd::move(resources));
    if (environment.is_err()) return Err(environment.unwrap_err());
    bound.environment_ = environment.unwrap();
    return Ok(rstd::move(bound));
}
auto ResolvePresentation(const BoundPresentation&     bound,
                         const SceneInstance&         instance,
                         const PresentationOverrides& overrides)
    -> Result<ResolvedPresentation, Error> {
    const auto& spec = bound.spec();
    if (&instance.asset() != &*bound.asset_ || instance.asset().generation != bound.generation_)
        return Err(
            presentation_detail::error(spec.source.as_str(),
                                       "asset"_str,
                                       "presentation belongs to a different asset generation"_str));
    ResolvedPresentation output;
    if (overrides.camera) {
        if (! overrides.camera->valid())
            return Err(presentation_detail::error(
                spec.source.as_str(), "camera"_str, "camera override is invalid"_str));
        output.camera = overrides.camera;
    } else {
        auto camera = presentation_detail::resolve_camera(spec, instance);
        if (camera.is_err()) return Err(camera.unwrap_err());
        output.camera = Some(camera.unwrap());
    }
    auto scene = presentation_detail::resolve_scene(
        spec, instance, overrides.scene ? *overrides.scene : spec.scene, *output.camera);
    if (scene.is_err()) return Err(scene.unwrap_err());
    output.scene   = scene.unwrap();
    output.display = overrides.display ? *overrides.display : spec.display;
    output.display.exposure *= overrides.exposure_multiplier;
    if (! rstd::f32(overrides.exposure_multiplier).is_finite() ||
        overrides.exposure_multiplier < 0 || ! rstd::f32(output.display.exposure).is_finite())
        return Err(
            presentation_detail::error(spec.source.as_str(),
                                       "display.exposure"_str,
                                       "exposure multiplier must be finite and nonnegative"_str));
    auto display = ValidateDisplayOptions(output.display);
    if (display.is_err()) return Err(display.unwrap_err());
    auto scene_options = ValidateSceneOptions(output.scene, output.camera->view(1).unwrap());
    if (scene_options.is_err()) return Err(scene_options.unwrap_err());
    return Ok(rstd::move(output));
}
} // namespace vecgltf
