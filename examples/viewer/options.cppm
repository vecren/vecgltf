export module vecgltf.viewer:options;
import vecgltf;
import vecgltf.render;
import rstd.argparse;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace gltf = vecgltf;
namespace cli  = rstd::argparse;

export namespace viewer
{
enum class CameraReference
{
    World,
    SceneRoot,
    Node
};
struct Options {
    String   path, capture, environment, camera_path;
    unsigned width { 960 }, height { 720 }, frames {}, scene { gltf::invalid_index },
        animation { gltf::invalid_index }, camera { gltf::invalid_index };
    float                time {}, camera_time {}, zoom {}, roll {};
    gltf::Vec2           rotate {}, pan {};
    Option<gltf::Vec3>   camera_position, camera_target;
    gltf::Vec3           camera_up { 0, 1, 0 };
    float                camera_fov { 45 };
    CameraReference      camera_reference { CameraReference::World };
    unsigned             camera_reference_node {};
    gltf::RenderOptions  render;
    gltf::PrepareOptions prepare;
    bool validation {}, headless {}, fixed_time {}, fixed_camera_time {}, camera_play {},
        external_camera {};
};
} // namespace viewer

namespace viewer
{
enum class RealLimit
{
    Finite,
    Positive,
    Nonnegative,
    Anisotropy,
    Fov
};
struct ReferenceValue {
    CameraReference reference;
    unsigned        node {};
};
auto unsigned_value(unsigned maximum = gltf::invalid_index) {
    return cli::parse_with<u32>(
        [maximum](ref<rstd::ffi::OsStr> text) -> Result<u32, cli::ValueError> {
            auto parsed = cli::from_str_parser<u32>().parse(text);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            auto value = parsed.unwrap();
            if (value > u32(maximum))
                return Err(
                    cli::ValueError::Message(rstd::format("expected a value in 0..{}", maximum)));
            return Ok(value);
        });
}
auto real_value(RealLimit limit = RealLimit::Finite) {
    return cli::parse_with<f32>(
        [limit](ref<rstd::ffi::OsStr> text) -> Result<f32, cli::ValueError> {
            auto parsed = cli::from_str_parser<f32>().parse(text);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            auto value = parsed.unwrap();
            if (! value.is_finite())
                return Err(cli::ValueError::Message("expected a finite number"_Str));
            auto number = value.to_primitive();
            if ((limit == RealLimit::Positive && number <= 0) ||
                (limit == RealLimit::Nonnegative && number < 0) ||
                (limit == RealLimit::Anisotropy && number < 1) ||
                (limit == RealLimit::Fov && (number <= 0 || number >= 180)))
                return Err(cli::ValueError::Message("number is outside the allowed range"_Str));
            return Ok(value);
        });
}
auto text_value() {
    return cli::parse_with<String>(
        [](ref<rstd::ffi::OsStr> text) -> Result<String, cli::ValueError> {
            auto parsed = cli::string_parser().parse(text);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            auto value = parsed.unwrap();
            if (value.is_empty())
                return Err(cli::ValueError::Message("value must not be empty"_Str));
            return Ok(rstd::move(value));
        });
}
auto reference_value() {
    return cli::parse_with<ReferenceValue>(
        [](ref<rstd::ffi::OsStr> text) -> Result<ReferenceValue, cli::ValueError> {
            auto name = text.to_str();
            if (! name) return Err(cli::ValueError::InvalidUtf8());
            if (*name == "world"_str) return Ok(ReferenceValue { CameraReference::World });
            if (*name == "scene-root"_str) return Ok(ReferenceValue { CameraReference::SceneRoot });
            auto index = unsigned_value(gltf::invalid_index - 1).parse(text);
            if (index.is_err()) return Err(index.unwrap_err());
            return Ok(ReferenceValue { CameraReference::Node, index.unwrap().to_primitive() });
        });
}
template<class T, class Parser>
auto add_value(cli::Command& command,
               ref<str>      name,
               Parser        parser,
               ref<str>      help,
               unsigned      count = 1) -> cli::ArgKey<T> {
    return command.add_arg(cli::Arg<T>::value(name, rstd::move(parser))
                               .long_name(name)
                               .num_args(cli::NumArgs::exact(usize(count)))
                               .append()
                               .allow_hyphen_values()
                               .help(help));
}
auto add_flag(cli::Command& command, ref<str> name, ref<str> help) -> cli::ArgKey<bool> {
    return command.add_arg(cli::Arg<bool>::flag(name).long_name(name).help(help));
}
template<class T>
auto last_value(const cli::Matches& matches, cli::ArgKey<T> key) -> Option<ref<T>> {
    auto           values = matches.get_many(key).unwrap();
    Option<ref<T>> last;
    if (values)
        while (auto value = values->next()) last = value;
    return last;
}
bool flag_value(const cli::Matches& matches, cli::ArgKey<bool> key) {
    auto value = matches.get_one(key).unwrap();
    return value && value.unwrap().get();
}
bool explicitly_set(const cli::Matches& matches, ref<str> name) {
    auto source = matches.value_source(name);
    return source && source->is_CommandLine();
}
void copy_text(String& target, const cli::Matches& matches, cli::ArgKey<String> key) {
    auto value = last_value(matches, key);
    if (value) target = value.unwrap().get().clone();
}
void copy_unsigned(unsigned& target, const cli::Matches& matches, cli::ArgKey<u32> key) {
    auto value = last_value(matches, key);
    if (value) target = value.unwrap().get().to_primitive();
}
void copy_real(float& target, const cli::Matches& matches, cli::ArgKey<f32> key) {
    auto value = last_value(matches, key);
    if (value) target = value.unwrap().get().to_primitive();
}
auto vector2_value(const cli::Matches& matches, cli::ArgKey<f32> key) -> Option<gltf::Vec2> {
    auto               values = matches.get_many(key).unwrap();
    Option<gltf::Vec2> result;
    if (values)
        while (auto x = values->next()) {
            auto y = values->next().unwrap();
            result = Some(gltf::Vec2 { x.unwrap().get().to_primitive(), y.get().to_primitive() });
        }
    return result;
}
auto vector3_value(const cli::Matches& matches, cli::ArgKey<f32> key) -> Option<gltf::Vec3> {
    auto               values = matches.get_many(key).unwrap();
    Option<gltf::Vec3> result;
    if (values)
        while (auto x = values->next()) {
            auto y = values->next().unwrap(), z = values->next().unwrap();
            result = Some(gltf::Vec3 {
                x.unwrap().get().to_primitive(), y.get().to_primitive(), z.get().to_primitive() });
        }
    return result;
}
} // namespace viewer

export namespace viewer
{
auto ParseOptions(int argc, char** argv) -> Result<Option<Options>, String> {
    auto command = cli::Command::make("vecgltf-viewer"_str);
    command.about("View a glTF or GLB scene"_str);
    auto model       = command.add_arg(cli::Arg<String>::value("model"_str, text_value())
                                           .value_name("MODEL"_str)
                                           .required()
                                           .help("Scene file"_str));
    auto validation  = add_flag(command, "validation"_str, "Enable Vulkan validation"_str);
    auto background  = add_flag(command, "background"_str, "Show the environment"_str);
    auto headless    = add_flag(command, "headless"_str, "Render without a window"_str);
    auto environment = add_value<String>(command, "environment"_str, text_value(), "HDR image"_str);
    auto capture = add_value<String>(command, "capture"_str, text_value(), "Write a PPM image"_str);
    auto frames  = add_value<u32>(command, "frames"_str, unsigned_value(100000), "Frame count"_str);
    auto scene   = add_value<u32>(command, "scene"_str, unsigned_value(), "Scene index"_str);
    auto camera  = add_value<u32>(
        command, "camera"_str, unsigned_value(gltf::invalid_index - 1), "Camera node"_str);
    auto camera_position = add_value<f32>(
        command, "camera-position"_str, real_value(), "Camera position X Y Z"_str, 3);
    auto camera_target =
        add_value<f32>(command, "camera-target"_str, real_value(), "Camera target X Y Z"_str, 3);
    auto camera_up =
        add_value<f32>(command, "camera-up"_str, real_value(), "Camera up X Y Z"_str, 3);
    auto camera_fov       = add_value<f32>(command,
                                           "camera-fov"_str,
                                           real_value(RealLimit::Fov),
                                           "Vertical field of view in degrees"_str);
    auto camera_reference = add_value<ReferenceValue>(
        command, "camera-reference"_str, reference_value(), "world, scene-root or node"_str);
    auto camera_path =
        add_value<String>(command, "camera-path"_str, text_value(), "Camera track"_str);
    auto camera_time = add_value<f32>(command,
                                      "camera-time"_str,
                                      real_value(RealLimit::Nonnegative),
                                      "Camera track time in seconds"_str);
    auto camera_play = add_flag(command, "camera-play"_str, "Play the camera track"_str);
    auto animation =
        add_value<u32>(command, "animation"_str, unsigned_value(), "Animation index"_str);
    auto debug    = add_value<u32>(command, "debug"_str, unsigned_value(5), "Debug view 0..5"_str);
    auto exposure = add_value<f32>(
        command, "exposure"_str, real_value(RealLimit::Positive), "Exposure scale"_str);
    auto no_direct_light = add_flag(command, "no-direct-light"_str, "Disable direct lighting"_str);
    auto anisotropy      = add_value<f32>(
        command, "anisotropy"_str, real_value(RealLimit::Anisotropy), "Maximum anisotropy"_str);
    auto ibl_intensity = add_value<f32>(command,
                                        "ibl-intensity"_str,
                                        real_value(RealLimit::Nonnegative),
                                        "Environment intensity"_str);
    auto tone_choices  = Vec<String>::make();
    for (auto value : rstd::array<ref<str>, 3> { "aces"_str, "reinhard-max"_str, "linear"_str })
        tone_choices.push(String::make(value));
    auto tone_map = add_value<String>(
        command,
        "tone-map"_str,
        cli::choice_parser<String>(cli::string_parser(), rstd::move(tone_choices)),
        "Tone mapping"_str);
    auto size =
        add_value<u32>(command, "size"_str, unsigned_value(), "Image width and height"_str, 2);
    auto time = add_value<f32>(command, "time"_str, real_value(), "Animation time in seconds"_str);
    auto zoom = add_value<f32>(command, "zoom"_str, real_value(), "Orbit zoom steps"_str);
    auto rotate = add_value<f32>(command, "rotate"_str, real_value(), "Orbit yaw and pitch"_str, 2);
    auto pan    = add_value<f32>(command, "pan"_str, real_value(), "Orbit pan X Y"_str, 2);
    auto roll   = add_value<f32>(command, "roll"_str, real_value(), "Orbit roll in radians"_str);
    auto built  = rstd::move(command).build();
    if (built.is_err()) return Err(rstd::format("{}", built.unwrap_err()));
    auto parser = built.unwrap();
    auto tokens = Vec<rstd::ffi::OsString>::make();
    for (int i = 0; i < argc; ++i)
        tokens.push(rstd::ffi::OsString::from(ref<rstd::ffi::OsStr>::from_encoded_bytes_unchecked(
            rstd::ffi::CStr::from_ptr(argv[i]).to_bytes())));
    auto parsed = parser.parse_from(rstd::move(tokens));
    if (parsed.is_err()) {
        auto error = parser.render_error(parsed.unwrap_err());
        return Err(String::make(error.text()));
    }
    auto outcome = parsed.unwrap();
    if (outcome.is_Display()) {
        const auto& request = outcome.as_Display().request;
        if (request.target() == cli::OutputTarget::Tag::Stderr)
            rstd::io::eprintln("{}", request.text());
        else
            rstd::io::println("{}", request.text());
        return Ok(None());
    }
    auto    matches = rstd::move(outcome).as_Parsed().value;
    Options options;
    options.path = matches.get_one(model).unwrap().unwrap().get().clone();
    copy_text(options.environment, matches, environment);
    copy_text(options.capture, matches, capture);
    copy_text(options.camera_path, matches, camera_path);
    copy_unsigned(options.frames, matches, frames);
    copy_unsigned(options.scene, matches, scene);
    copy_unsigned(options.camera, matches, camera);
    copy_unsigned(options.animation, matches, animation);
    copy_real(options.camera_fov, matches, camera_fov);
    copy_real(options.camera_time, matches, camera_time);
    copy_real(options.render.exposure, matches, exposure);
    copy_real(options.prepare.max_anisotropy, matches, anisotropy);
    copy_real(options.render.ibl_intensity, matches, ibl_intensity);
    copy_real(options.time, matches, time);
    copy_real(options.zoom, matches, zoom);
    copy_real(options.roll, matches, roll);
    options.camera_position = vector3_value(matches, camera_position);
    options.camera_target   = vector3_value(matches, camera_target);
    if (auto value = vector3_value(matches, camera_up)) options.camera_up = value.unwrap();
    if (auto value = vector2_value(matches, rotate)) options.rotate = value.unwrap();
    if (auto value = vector2_value(matches, pan)) options.pan = value.unwrap();
    if (auto value = last_value(matches, debug))
        options.render.debug = gltf::DebugView(value.unwrap().get().to_primitive());
    if (auto value = last_value(matches, camera_reference)) {
        options.camera_reference      = value.unwrap().get().reference;
        options.camera_reference_node = value.unwrap().get().node;
    }
    if (auto value = last_value(matches, tone_map)) {
        const auto& name            = value.unwrap().get();
        options.render.tone_mapping = name == "linear"_str         ? gltf::ToneMapping::Linear
                                      : name == "reinhard-max"_str ? gltf::ToneMapping::ReinhardMax
                                                                   : gltf::ToneMapping::ACES;
    }
    auto sizes = matches.get_many(size).unwrap();
    if (sizes)
        while (auto width = sizes->next()) {
            options.width  = width.unwrap().get().to_primitive();
            options.height = sizes->next().unwrap().get().to_primitive();
        }
    options.validation        = flag_value(matches, validation);
    options.render.background = flag_value(matches, background);
    options.headless          = flag_value(matches, headless);
    options.camera_play       = flag_value(matches, camera_play);
    if (flag_value(matches, no_direct_light)) options.render.direct_light = None();
    options.fixed_time        = explicitly_set(matches, "time"_str);
    options.fixed_camera_time = explicitly_set(matches, "camera-time"_str);
    for (auto name : rstd::array<ref<str>, 5> { "camera-position"_str,
                                                "camera-target"_str,
                                                "camera-up"_str,
                                                "camera-fov"_str,
                                                "camera-reference"_str })
        options.external_camera = options.external_camera || explicitly_set(matches, name);
    auto reject = [&](ref<str> message) -> Result<Option<Options>, String> {
        return Err(rstd::format("error: {}\n\n{}", message, parser.render_usage()));
    };
    if (! options.width || ! options.height || options.width > 8192 || options.height > 8192)
        return reject("image dimensions must be in 1..8192"_str);
    if (options.external_camera &&
        (options.camera_position.is_none() || options.camera_target.is_none() ||
         options.camera != gltf::invalid_index || ! options.camera_path.is_empty() ||
         options.fixed_camera_time || options.camera_play))
        return reject(
            "camera preset needs position and target and cannot use another camera source"_str);
    if (options.camera_play && options.fixed_camera_time)
        return reject("--camera-play and --camera-time cannot be combined"_str);
    if (options.headless && ! options.frames) options.frames = 1;
    return Ok(Some(rstd::move(options)));
}
} // namespace viewer
