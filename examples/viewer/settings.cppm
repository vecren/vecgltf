export module vecgltf.viewer:settings;
import :options;
import vecgltf.display;
import vecgltf.environment;
import rstd.json;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;

export namespace viewer
{
enum class SettingsSpace
{
    World,
    View,
    SceneRoot,
    Node
};
struct SettingsReference {
    SettingsSpace space { SettingsSpace::World };
    unsigned      node { gltf::invalid_index };
};
struct SettingsLight {
    gltf::DirectionalLight light;
    SettingsReference      reference;
};
struct ViewSettings {
    String                          source;
    gltf::SceneOptions              scene;
    gltf::DisplayOptions            display;
    Option<gltf::EnvironmentImages> environment_images;
    Vec<SettingsLight>              lights;
    SettingsReference               environment_reference;
    gltf::Mat4                      environment_to_reference { gltf::Mat4::Identity() };
    bool                            environment_transform {};
};
} // namespace viewer

namespace viewer::settings
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
struct EnvironmentFiles {
    String                background, diffuse, brdf;
    Vec<String>           specular;
    float                 lod_scale { -1 }, lod_exponent { 1 };
    vrento::TextureFilter brdf_filter { vrento::TextureFilter::LINEAR };
};
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
    auto reference(const Json& value, ref<str> location) -> SettingsReference {
        if (auto input = value.as_str()) {
            if (*input == "world"_str) return {};
            if (*input == "view"_str) return { SettingsSpace::View };
            if (*input == "scene-root"_str) return { SettingsSpace::SceneRoot };
        } else if (auto input = value.as_f64()) {
            auto number = input->to_primitive();
            if (input->is_finite() && number >= 0 && number < gltf::invalid_index &&
                number == input->floor().to_primitive())
                return { SettingsSpace::Node, unsigned(number) };
        }
        fail(location, "expected world, view, scene-root or a node index"_str);
        return {};
    }
    void lights(const Json& input, ViewSettings& settings) {
        auto values = input.as_array();
        if (! values || values.unwrap().get().len() > usize(gltf::max_directional_lights)) {
            fail("directional_lights"_str, "expected at most eight directional lights"_str);
            return;
        }
        Vec<gltf::DirectionalLight> initial;
        for (const auto& value : values.unwrap().get()) {
            auto path = rstd::format("directional_lights[{}]", settings.lights.len());
            if (! object(value, path.as_str())) return;
            SettingsLight entry;
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
            entry.light.space = entry.reference.space == SettingsSpace::View
                                    ? gltf::LightSpace::View
                                    : gltf::LightSpace::World;
            initial.push(gltf::DirectionalLight(entry.light));
            settings.lights.push(rstd::move(entry));
        }
        settings.scene.direct_light       = None();
        settings.scene.directional_lights = gltf::DirectionalLights(rstd::move(initial));
    }
    auto environment(const Json& input, ViewSettings& settings) -> Option<EnvironmentFiles> {
        if (! object(input, "environment"_str)) return None();
        settings.environment_transform = true;
        if (auto value = field(input, "reference"_str))
            settings.environment_reference = reference(*value, "environment.reference"_str);
        if (settings.environment_reference.space == SettingsSpace::View)
            fail("environment.reference"_str,
                 "view-space environment references are unsupported"_str,
                 gltf::ErrorKind::Unsupported);
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
    void display(const Json& input, ViewSettings& settings) {
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
    if (size > 1024 * 1024)
        return Err(
            error(source, "settings"_str, "settings exceeds one MiB"_str, gltf::ErrorKind::Limit));
    Vec<u8> bytes;
    bytes.resize(usize(size), u8());
    if (file.read_exact_at(bytes.as_mut_slice().as_mut_ref(), u64()).is_err())
        return Err(
            error(source, "settings"_str, "cannot read settings file"_str, gltf::ErrorKind::Io));
    return Ok(rstd::move(bytes));
}
auto reference_rotation(SettingsReference          reference,
                        const gltf::SceneInstance& instance,
                        ref<str>                   source,
                        ref<str>                   location) -> Result<gltf::Mat4, gltf::Error> {
    if (reference.space == SettingsSpace::World) return Ok(gltf::Mat4::Identity());
    auto index = reference.node;
    if (reference.space == SettingsSpace::SceneRoot) {
        auto root = instance.scene_root();
        if (! root)
            return Err(
                error(source, location, "reference needs exactly one visible scene root"_str));
        index = root->index;
    }
    if (reference.space == SettingsSpace::View || index >= gltf::count(instance.asset().nodes) ||
        ! instance.visible(index))
        return Err(error(source, location, "reference node is not valid in this scene"_str));
    return rotation(instance.pose(index).world, source, location, true);
}
} // namespace viewer::settings

export namespace viewer
{
auto LoadViewSettings(ref<str> source) -> Result<ViewSettings, gltf::Error> {
    if (source.is_empty() || settings::contains_nul(source))
        return Err(settings::error(source, "settings"_str, "invalid settings path"_str));
    auto bytes = settings::read_document(source);
    if (bytes.is_err()) return Err(bytes.unwrap_err());
    auto parsed = rstd::json::from_slice(bytes->as_slice(), { .max_depth = usize(32) });
    if (parsed.is_err())
        return Err(settings::error(source,
                                   "settings"_str,
                                   rstd::format("{}", parsed.unwrap_err()).as_str(),
                                   gltf::ErrorKind::Parse));
    auto             data = parsed.unwrap();
    settings::Reader reader(source);
    ViewSettings     output;
    output.source = String::make(source);
    if (! reader.object(data, "settings"_str)) return Err(reader.failure().unwrap());
    auto version = reader.field(data, "version"_str);
    if (! version || reader.number(*version, "version"_str) != 1)
        reader.fail("version"_str, "settings version must be one"_str);
    if (auto lights = reader.field(data, "directional_lights"_str)) reader.lights(*lights, output);
    Option<settings::EnvironmentFiles> files;
    if (auto input = reader.field(data, "environment"_str))
        files = reader.environment(*input, output);
    if (auto input = reader.field(data, "display"_str)) reader.display(*input, output);
    if (auto failed = reader.failure()) return Err(rstd::move(failed).unwrap());
    if (! files) return Ok(rstd::move(output));
    auto location = rstd::path::PathBuf::from(source);
    auto parent   = location.as_path().parent();
    auto base = parent ? rstd::path::PathBuf::from(*parent) : rstd::path::PathBuf::from("."_str);
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
    for (auto& image : images) {
        auto loaded = gltf::LoadEnvironment(image.path.as_str());
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
    output.environment_images         = Some(rstd::move(environment));
    return Ok(rstd::move(output));
}
auto ResolveSceneOptions(const ViewSettings&        configured,
                         const gltf::SceneInstance& instance,
                         gltf::RenderOptions current) -> Result<gltf::SceneOptions, gltf::Error> {
    gltf::SceneOptions result;
    result.debug                = current.debug;
    result.background           = current.background;
    result.direct_light         = current.direct_light;
    result.ibl_intensity        = current.ibl_intensity;
    result.background_intensity = current.background_intensity;
    result.world_to_environment = current.world_to_environment;
    if (current.directional_lights) {
        const auto& inputs = current.directional_lights.values();
        if (! inputs.is_empty() && inputs.len() != configured.lights.len())
            return Err(settings::error(configured.source.as_str(),
                                       "directional_lights"_str,
                                       "light count differs from reference settings"_str));
        Vec<gltf::DirectionalLight> lights;
        for (unsigned i = 0; i < gltf::count(inputs); ++i) {
            auto light     = gltf::at(inputs, i);
            auto reference = gltf::at(configured.lights, i).reference;
            if (reference.space == SettingsSpace::View)
                light.space = gltf::LightSpace::View;
            else {
                auto rotation = settings::reference_rotation(
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
    if (configured.environment_transform) {
        auto rotation = settings::reference_rotation(configured.environment_reference,
                                                     instance,
                                                     configured.source.as_str(),
                                                     "environment.reference"_str);
        if (rotation.is_err()) return Err(rotation.unwrap_err());
        result.world_to_environment =
            settings::transpose(rotation.unwrap() * configured.environment_to_reference);
    }
    return Ok(rstd::move(result));
}
} // namespace viewer
