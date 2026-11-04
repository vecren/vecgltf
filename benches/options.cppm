export module vecgltf.bench.load:options;
import rstd.argparse;

using namespace rstd::prelude;
using namespace rstd::literals;
namespace cli = rstd::argparse;

export namespace vecgltf_bench
{
struct Input {
    String id, path, digest, revision;
    u64    bytes;
};
struct CpuOptions {
    Vec<Input> inputs;
    String     manifest, json, diagnostic_json, semantic_json;
    String     implementation { "native"_Str }, revision { "worktree"_Str }, selected { "all"_Str };
    u32        epochs { 5 }, iterations { 1 }, warmup { 1 };
    bool       diagnose {}, check_only {};
};
struct StartupOptions {
    String   path;
    unsigned samples { 1 };
};
enum class Mode
{
    Cpu,
    Startup
};
struct Options {
    Mode           mode { Mode::Startup };
    CpuOptions     cpu;
    StartupOptions startup;
};
auto argument(const char* value) -> ref<str> {
    return rstd::ffi::CStr::from_ptr(value).to_str().unwrap_or(""_str);
}
} // namespace vecgltf_bench

namespace vecgltf_bench
{
auto bounded_count(u32 minimum, u32 maximum) {
    return cli::parse_with<u32>(
        [minimum, maximum](ref<rstd::ffi::OsStr> token) -> Result<u32, cli::ValueError> {
            auto parsed = cli::from_str_parser<u32>().parse(token);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            auto value = parsed.unwrap();
            if (value < minimum || value > maximum)
                return Err(cli::ValueError::Message(
                    rstd::format("expected a value in {}..{}", minimum, maximum)));
            return Ok(value);
        });
}
auto nonempty_string() {
    return cli::parse_with<String>(
        [](ref<rstd::ffi::OsStr> token) -> Result<String, cli::ValueError> {
            auto parsed = cli::string_parser().parse(token);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            auto value = parsed.unwrap();
            if (value.is_empty())
                return Err(cli::ValueError::Message("value must not be empty"_Str));
            return Ok(rstd::move(value));
        });
}
bool supplied(const cli::Matches& matches, ref<str> id) {
    auto source = matches.value_source(id);
    return source && source->is_CommandLine();
}
void copy_string(String& target, const cli::Matches& matches, cli::ArgKey<String> key) {
    auto value = matches.get_one(key).unwrap();
    if (value) target = value.unwrap().get().clone();
}
auto count(const cli::Matches& matches, cli::ArgKey<u32> key) -> u32 {
    return matches.get_one(key).unwrap().unwrap().get();
}
} // namespace vecgltf_bench

export namespace vecgltf_bench
{
auto parse(int argc, char** argv) -> Result<Option<Options>, String> {
    auto modes = Vec<String>::make();
    modes.push("cpu"_Str);
    modes.push("startup"_Str);
    auto cases = Vec<String>::make();
    for (auto name : rstd::array<ref<str>, 6> { "all"_str,
                                                "document.parse"_str,
                                                "resources.resolve"_str,
                                                "asset.build"_str,
                                                "asset.load"_str,
                                                "asset.load_drop"_str })
        cases.push(String::make(name));
    auto command = cli::Command::make("load"_str);
    command.about("Measure CPU glTF loading or Vulkan startup"_str);
    auto mode = command.add_arg(
        cli::Arg<String>::value("mode"_str,
                                cli::choice_parser<String>(cli::string_parser(), rstd::move(modes)))
            .long_name("mode"_str)
            .value_name("MODE"_str)
            .default_value("startup"_str)
            .help("Select the benchmark"_str));
    auto models = command.add_arg(cli::Arg<String>::value("models"_str, nonempty_string())
                                      .num_args(cli::NumArgs::any())
                                      .value_name("MODEL"_str)
                                      .help("glTF or GLB input files"_str));
    auto samples =
        command.add_arg(cli::Arg<u32>::value("samples"_str, bounded_count(u32(1), u32(32)))
                            .long_name("samples"_str)
                            .value_name("N"_str)
                            .default_value("1"_str)
                            .help_heading("Startup options"_str)
                            .help("Collect 1..32 startup samples"_str));
    auto manifest = command.add_arg(cli::Arg<String>::value("manifest"_str, nonempty_string())
                                        .long_name("manifest"_str)
                                        .value_name("FILE"_str)
                                        .help_heading("CPU options"_str)
                                        .help("Read benchmark datasets"_str));
    auto json     = command.add_arg(cli::Arg<String>::value("json"_str, nonempty_string())
                                        .long_name("json"_str)
                                        .value_name("FILE"_str)
                                        .help_heading("CPU options"_str)
                                        .help("Write the standard benchmark report"_str));
    auto diagnostic_json =
        command.add_arg(cli::Arg<String>::value("diagnostic-json"_str, nonempty_string())
                            .long_name("diagnostic-json"_str)
                            .value_name("FILE"_str)
                            .help_heading("CPU options"_str)
                            .help("Write the diagnostic report when diagnosing"_str));
    auto semantics = command.add_arg(cli::Arg<String>::value("semantics"_str, nonempty_string())
                                         .long_name("semantics"_str)
                                         .value_name("FILE"_str)
                                         .help_heading("CPU options"_str)
                                         .help("Write Asset semantic summaries"_str));
    auto implementation =
        command.add_arg(cli::Arg<String>::value("implementation"_str, nonempty_string())
                            .long_name("implementation"_str)
                            .value_name("LABEL"_str)
                            .default_value("native"_str)
                            .help_heading("CPU options"_str)
                            .help("Label the measured loader"_str));
    auto revision = command.add_arg(cli::Arg<String>::value("revision"_str, nonempty_string())
                                        .long_name("revision"_str)
                                        .value_name("REVISION"_str)
                                        .default_value("worktree"_str)
                                        .help_heading("CPU options"_str)
                                        .help("Record the loader revision"_str));
    auto selected = command.add_arg(
        cli::Arg<String>::value("case"_str,
                                cli::choice_parser<String>(cli::string_parser(), rstd::move(cases)))
            .long_name("case"_str)
            .value_name("CASE"_str)
            .default_value("all"_str)
            .help_heading("CPU options"_str)
            .help("Select a CPU measurement"_str));
    auto epochs =
        command.add_arg(cli::Arg<u32>::value("epochs"_str, bounded_count(u32(1), u32(1000)))
                            .long_name("epochs"_str)
                            .value_name("N"_str)
                            .default_value("5"_str)
                            .help_heading("CPU options"_str)
                            .help("Sampling epochs"_str));
    auto iterations =
        command.add_arg(cli::Arg<u32>::value("iterations"_str, bounded_count(u32(1), u32(1000000)))
                            .long_name("iterations"_str)
                            .value_name("N"_str)
                            .default_value("1"_str)
                            .help_heading("CPU options"_str)
                            .help("Iterations per epoch"_str));
    auto warmup =
        command.add_arg(cli::Arg<u32>::value("warmup"_str, bounded_count(u32(), u32(1000000)))
                            .long_name("warmup"_str)
                            .value_name("N"_str)
                            .default_value("1"_str)
                            .help_heading("CPU options"_str)
                            .help("Warmup iterations"_str));
    command.add_arg(cli::Arg<bool>::flag("diagnose"_str)
                        .long_name("diagnose"_str)
                        .help_heading("CPU options"_str)
                        .help("Run resource diagnostics instead of timing"_str));
    command.add_arg(cli::Arg<bool>::flag("check-only"_str)
                        .long_name("check-only"_str)
                        .help_heading("CPU options"_str)
                        .help("Only write and check semantic summaries"_str));
    auto built = rstd::move(command).build();
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
    Options result;
    auto    selected_mode = matches.get_one(mode).unwrap().unwrap();
    result.mode           = selected_mode.get() == "cpu"_str ? Mode::Cpu : Mode::Startup;
    auto reject           = [&](ref<str> message) -> Result<Option<Options>, String> {
        return Err(rstd::format("error: {}\n\n{}", message, parser.render_usage()));
    };
    if (result.mode == Mode::Startup) {
        for (auto id : rstd::array<ref<str>, 12> { "manifest"_str,
                                                   "json"_str,
                                                   "diagnostic-json"_str,
                                                   "semantics"_str,
                                                   "implementation"_str,
                                                   "revision"_str,
                                                   "case"_str,
                                                   "epochs"_str,
                                                   "iterations"_str,
                                                   "warmup"_str,
                                                   "diagnose"_str,
                                                   "check-only"_str })
            if (supplied(matches, id))
                return reject(rstd::format("--{} requires --mode cpu", id).as_str());
        auto values = matches.get_many(models).unwrap();
        if (! values || values->len() != usize(1))
            return reject("startup requires exactly one model"_str);
        result.startup.path    = values->next().unwrap().get().clone();
        result.startup.samples = count(matches, samples).to_primitive();
        return Ok(Some(rstd::move(result)));
    }
    if (supplied(matches, "samples"_str)) return reject("--samples requires --mode startup"_str);
    auto values = matches.get_many(models).unwrap();
    if (values)
        while (auto value = values->next()) {
            auto path = value.unwrap().get().clone();
            result.cpu.inputs.push(
                { path.clone(), rstd::move(path), "unspecified"_Str, "unfrozen"_Str, {} });
        }
    copy_string(result.cpu.manifest, matches, manifest);
    copy_string(result.cpu.json, matches, json);
    copy_string(result.cpu.diagnostic_json, matches, diagnostic_json);
    copy_string(result.cpu.semantic_json, matches, semantics);
    copy_string(result.cpu.implementation, matches, implementation);
    copy_string(result.cpu.revision, matches, revision);
    copy_string(result.cpu.selected, matches, selected);
    result.cpu.epochs     = count(matches, epochs);
    result.cpu.iterations = count(matches, iterations);
    result.cpu.warmup     = count(matches, warmup);
    result.cpu.diagnose   = supplied(matches, "diagnose"_str);
    result.cpu.check_only = supplied(matches, "check-only"_str);
    if (result.cpu.inputs.is_empty() && result.cpu.manifest.is_empty())
        return reject("cpu requires a model or --manifest"_str);
    if (result.cpu.check_only && result.cpu.semantic_json.is_empty())
        return reject("--check-only requires --semantics"_str);
    return Ok(Some(rstd::move(result)));
}
} // namespace vecgltf_bench
