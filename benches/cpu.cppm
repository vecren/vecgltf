module;
#if defined(__linux__)
#include <sys/resource.h>
#endif

export module vecgltf.bench.load:cpu;
import :options;
import vecgltf;
import rstd.bench;
import rstd.json;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
namespace gltf  = vecgltf;
namespace bench = rstd::bench;

namespace vecgltf_bench::cpu
{
auto error_text(const gltf::Error& error) -> String {
    return rstd::format(
        "{} [{}] {}: {}", error.source, unsigned(error.kind), error.location, error.message);
}

struct Fingerprint {
    rstd::uint64_t value { 14695981039346656037ull };
    void           byte(unsigned char input) { value = (value ^ input) * 1099511628211ull; }
    void           number(rstd::uint64_t input) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(static_cast<unsigned char>(input >> shift));
    }
    void real(float input) { number(rstd::f32(input == 0 ? 0 : input).to_bits().to_primitive()); }
    void text(ref<str> input) {
        number(input.len().to_primitive());
        for (auto item : input.as_bytes()) byte(item.to_primitive());
    }
    void vector(gltf::Vec2 input) {
        real(input.x);
        real(input.y);
    }
    void vector(gltf::Vec3 input) {
        real(input.x);
        real(input.y);
        real(input.z);
    }
    void vector(gltf::Vec4 input) {
        real(input.x);
        real(input.y);
        real(input.z);
        real(input.w);
    }
    void matrix(const gltf::Mat4& input) {
        for (float item : input.m) real(item);
    }
    void binding(const gltf::TextureBinding& input) {
        number(input.texture);
        number(input.uv);
        vector(input.offset);
        vector(input.scale);
        real(input.rotation);
        real(input.strength);
    }
    void floats(const Vec<float>& input) {
        number(input.len().to_primitive());
        for (auto value : input) real(value);
    }
    void indices(const Vec<unsigned>& input) {
        number(input.len().to_primitive());
        for (auto value : input) number(value);
    }
    auto finish() const -> String { return rstd::format("{:016x}", value); }
};

auto json_number(rstd::uint64_t value) -> rstd::json::Value {
    return rstd::json::Value::Number(rstd::json::Number::from_u64(u64(value)));
}
void field(rstd::json::Value& value, ref<str> key, rstd::uint64_t number) {
    value[key] = json_number(number);
}

auto json_text(ref<str> value) -> rstd::json::Value {
    return rstd::json::Value::String(String::make(value));
}
auto json_text(String value) -> rstd::json::Value {
    return rstd::json::Value::String(rstd::move(value));
}

auto source_metadata(Option<gltf::MetadataOwner> metadata, ref<str> owner) -> rstd::json::Value {
    auto output                = rstd::json::Value::Object(rstd::json::Map {});
    output["native_added"_str] = rstd::json::Value::Bool(true);
    output["owner"_str]        = json_text(owner);
    output["available"_str]    = rstd::json::Value::Bool(metadata.is_some());
    if (metadata.is_none()) return output;
    const auto& store = metadata->value();
    field(output, "records"_str, store.records.len().to_primitive());
    output["version"_str]     = json_text(store.asset.version.as_str());
    output["min_version"_str] = json_text(store.asset.min_version.as_str());
    output["generator"_str]   = json_text(store.asset.generator.as_str());
    output["copyright"_str]   = json_text(store.asset.copyright.as_str());
    rstd::uint64_t extras {}, extensions {};
    auto           retained = rstd::json::Value::Array(rstd::json::Array {});
    for (const auto& record : store.records) {
        extras += record.extras.is_some();
        extensions += record.extensions.is_some();
        if (record.extensions.is_none()) continue;
        auto item = rstd::json::Value::Object(rstd::json::Map {});
        field(item, "kind"_str, unsigned(record.kind));
        field(item, "index"_str, record.index);
        field(item, "parent"_str, record.parent);
        item["name"_str]    = json_text(record.name.as_str());
        item["payload"_str] = record.extensions->clone();
        retained.as_array_mut().unwrap()->push(rstd::move(item));
    }
    field(output, "records_with_extras"_str, extras);
    field(output, "records_with_extensions"_str, extensions);
    output["retained_extensions"_str]    = rstd::move(retained);
    auto record                          = metadata->view(store.asset.source);
    output["asset_source_available"_str] = rstd::json::Value::Bool(record.is_some());
    if (record.is_some()) {
        const auto& value                  = record->value();
        output["asset_has_extras"_str]     = rstd::json::Value::Bool(value.extras.is_some());
        output["asset_has_extensions"_str] = rstd::json::Value::Bool(value.extensions.is_some());
        if (value.extras.is_some()) output["asset_extras"_str] = value.extras->clone();
        if (value.extensions.is_some()) output["asset_extensions"_str] = value.extensions->clone();
    }
    return output;
}

auto semantics(const gltf::Asset& asset) -> rstd::json::Value {
    auto output          = rstd::json::Value::Object(rstd::json::Map {});
    output["schema"_str] = json_text("vecgltf.asset.semantic.v1"_str);
    output["hash_algorithm"_str] =
        json_text("fnv1a64/canonical-little-endian-f32/zero-normalized"_str);
    Fingerprint total;
    auto        category = [&](ref<str> name, const Fingerprint& hash, rstd::uint64_t count) {
        auto value          = rstd::json::Value::Object(rstd::json::Map {});
        value["digest"_str] = json_text(hash.finish());
        field(value, "count"_str, count);
        output[name] = rstd::move(value);
        total.text(name);
        total.number(count);
        total.number(hash.value);
    };
    total.number(asset.default_scene);
    Fingerprint    geometry;
    rstd::uint64_t vertices {}, indices {}, image_bytes {};
    for (const auto& mesh : asset.meshes) {
        geometry.text(mesh.name.as_str());
        geometry.floats(mesh.weights);
        geometry.number(mesh.primitives.len().to_primitive());
        for (const auto& primitive : mesh.primitives) {
            geometry.number(primitive.material);
            geometry.number(primitive.indexed);
            geometry.number(primitive.skin_attributes);
            geometry.number(primitive.vertices.len().to_primitive());
            vertices += primitive.vertices.len().to_primitive();
            indices += primitive.indices.len().to_primitive();
            for (const auto& v : primitive.vertices) {
                geometry.vector(v.position);
                geometry.vector(v.normal);
                geometry.vector(v.tangent);
                geometry.vector(v.uv0);
                geometry.vector(v.uv1);
                geometry.vector(v.color);
                for (auto joint : v.joints) geometry.number(joint);
                geometry.vector(v.weights);
            }
            geometry.indices(primitive.indices);
            geometry.number(primitive.bounds.valid);
            geometry.vector(primitive.bounds.min);
            geometry.vector(primitive.bounds.max);
            geometry.number(primitive.morphs.len().to_primitive());
            for (const auto& target : primitive.morphs) {
                const Vec<gltf::Vec3>* streams[] { &target.positions,
                                                   &target.normals,
                                                   &target.tangents };
                for (const auto* values : streams) {
                    geometry.number(values->len().to_primitive());
                    for (const auto& value : *values) geometry.vector(value);
                }
            }
            geometry.number(primitive.extra_texcoords.len().to_primitive());
            for (const auto& set : primitive.extra_texcoords) {
                geometry.number(set.index);
                geometry.number(set.coordinates.len().to_primitive());
                for (const auto& value : set.coordinates) geometry.vector(value);
            }
        }
    }
    category("geometry"_str, geometry, asset.meshes.len().to_primitive());
    Fingerprint materials;
    for (const auto& m : asset.materials) {
        materials.text(m.name.as_str());
        materials.vector(m.base_color);
        materials.vector(m.emissive);
        materials.vector(m.specular);
        materials.real(m.metallic);
        materials.real(m.roughness);
        materials.real(m.glossiness);
        materials.real(m.alpha_cutoff);
        materials.binding(m.base);
        materials.binding(m.physical);
        materials.binding(m.normal);
        materials.binding(m.occlusion);
        materials.binding(m.emission);
        materials.number(unsigned(m.workflow));
        materials.number(unsigned(m.alpha));
        materials.number(m.double_sided);
        materials.number(m.unlit);
    }
    category("materials"_str, materials, asset.materials.len().to_primitive());
    Fingerprint images;
    for (const auto& image : asset.images) {
        images.text(image.name.as_str());
        images.number(image.width);
        images.number(image.height);
        images.number(image.rgba.len().to_primitive());
        image_bytes += image.rgba.len().to_primitive();
        for (auto value : image.rgba) images.byte(value.to_primitive());
    }
    category("images"_str, images, asset.images.len().to_primitive());
    Fingerprint textures;
    for (const auto& t : asset.textures) {
        textures.number(t.image);
        textures.number(unsigned(t.sampler.s));
        textures.number(unsigned(t.sampler.t));
        textures.number(unsigned(t.sampler.min));
        textures.number(unsigned(t.sampler.mag));
        textures.number(unsigned(t.sampler.mip));
    }
    category("textures"_str, textures, asset.textures.len().to_primitive());
    Fingerprint nodes;
    for (const auto& n : asset.nodes) {
        nodes.text(n.name.as_str());
        nodes.number(n.parent);
        nodes.number(n.mesh);
        nodes.number(n.skin);
        nodes.number(n.camera);
        nodes.indices(n.children);
        nodes.vector(n.translation);
        nodes.vector(n.scale);
        nodes.vector(n.rotation);
        nodes.matrix(n.matrix);
        nodes.number(n.has_matrix);
        nodes.floats(n.weights);
    }
    category("nodes"_str, nodes, asset.nodes.len().to_primitive());
    Fingerprint scenes;
    for (const auto& s : asset.scenes) {
        scenes.text(s.name.as_str());
        scenes.indices(s.roots);
    }
    category("scenes"_str, scenes, asset.scenes.len().to_primitive());
    Fingerprint skins;
    for (const auto& s : asset.skins) {
        skins.text(s.name.as_str());
        skins.indices(s.joints);
        skins.number(s.inverse_bind.len().to_primitive());
        for (const auto& matrix : s.inverse_bind) skins.matrix(matrix);
    }
    category("skins"_str, skins, asset.skins.len().to_primitive());
    Fingerprint cameras;
    for (const auto& c : asset.cameras) {
        cameras.text(c.name.as_str());
        cameras.number(c.orthographic);
        cameras.real(c.yfov);
        cameras.real(c.aspect);
        cameras.real(c.near);
        cameras.real(c.far);
        cameras.real(c.xmag);
        cameras.real(c.ymag);
    }
    category("cameras"_str, cameras, asset.cameras.len().to_primitive());
    Fingerprint animations;
    for (const auto& a : asset.animations) {
        animations.text(a.name.as_str());
        animations.real(a.start);
        animations.real(a.end);
        animations.number(a.samplers.len().to_primitive());
        for (const auto& sampler : a.samplers) {
            animations.number(unsigned(sampler.interpolation));
            animations.number(sampler.components);
            animations.floats(sampler.times);
            animations.floats(sampler.values);
        }
        animations.number(a.channels.len().to_primitive());
        for (const auto& channel : a.channels) {
            animations.number(channel.node);
            animations.number(channel.sampler);
            animations.number(unsigned(channel.path));
        }
    }
    category("animations"_str, animations, asset.animations.len().to_primitive());
    output["digest"_str] = json_text(total.finish());
    field(output, "vertices"_str, vertices);
    field(output, "indices"_str, indices);
    field(output, "decoded_image_bytes"_str, image_bytes);
    field(output, "diagnostics"_str, asset.diagnostics.len().to_primitive());
    output["source_metadata"_str] = source_metadata(asset.metadata(), "Asset::metadata"_str);
    return output;
}

auto write_json(ref<str> path, const rstd::json::Value& value) -> Result<empty, String> {
    if (path.is_empty()) return Ok(empty {});
    auto encoded = rstd::json::to_string(value);
    auto result =
        rstd::fs::write(rstd::path::PathBuf::from(path).as_path(), encoded.as_str().as_bytes());
    return result.is_ok() ? Result<empty, String>(Ok(empty {}))
                          : Result<empty, String>(Err(rstd::format("{}", result.unwrap_err())));
}
auto read_manifest(CpuOptions& options) -> Result<empty, String> {
    if (options.manifest.is_empty()) return Ok(empty {});
    auto bytes = rstd::fs::read(rstd::path::PathBuf::from(options.manifest.as_str()).as_path());
    if (bytes.is_err()) return Err(rstd::format("{}", bytes.unwrap_err()));
    auto parsed = rstd::json::from_slice(bytes->as_slice());
    if (parsed.is_err()) return Err("cannot parse benchmark manifest"_Str);
    const auto& value  = *parsed;
    auto        models = value["models"_str].as_array();
    if (models.is_none()) return Err("manifest requires models array"_Str);
    for (const auto& model : **models) {
        auto id = model["id"_str].as_str(), path = model["path"_str].as_str(),
             digest   = model["digest"_str].as_str();
        auto revision = value["revision"_str].as_str();
        auto size     = model["input_bytes"_str].as_u64();
        if (! id || ! path || ! digest || ! revision || ! size)
            return Err("invalid benchmark dataset descriptor"_Str);
        options.inputs.push({ String::make(*id),
                              String::make(*path),
                              String::make(*digest),
                              String::make(*revision),
                              *size });
    }
    return Ok(empty {});
}

struct LoadSession {
    String path;
    bool   drop {};
    String failure;
    auto   check() -> Result<empty, String> {
        auto loaded = gltf::LoadAsset(path.as_str());
        return loaded.is_ok() ? Result<empty, String>(Ok(empty {}))
                              : Result<empty, String>(Err(error_text(loaded.unwrap_err())));
    }
    auto run(bench::Bench& engine, ref<str> name)
        -> Result<bench::BenchmarkResult, bench::BenchError> {
        if (drop) {
            engine.boundary(
                "LoadAsset(path)+result consumption+Asset destruction; no trace/GPU"_str);
            return engine.run_fallible(name, [&]() -> Result<empty, String> {
                auto loaded = gltf::LoadAsset(path.as_str());
                if (loaded.is_err()) return Err(error_text(loaded.unwrap_err()));
                rstd::hint::black_box(*loaded.unwrap());
                return Ok(empty {});
            });
        }
        engine.boundary("LoadAsset(path); output destruction and setup excluded; no trace/GPU"_str);
        auto measured = engine.run_batched(
            name,
            []() {
                return empty {};
            },
            [&](empty) {
                auto loaded = gltf::LoadAsset(path.as_str());
                if (loaded.is_err() && failure.is_empty())
                    failure = error_text(loaded.unwrap_err());
                return loaded;
            },
            { .max_items = usize(1) });
        if (! failure.is_empty()) return Err(bench::BenchError::Operation(failure.clone()));
        return measured;
    }
    auto finish() -> Result<empty, String> { return Ok(empty {}); }
};
struct LoadFactory {
    String path;
    bool   drop {};
    auto   prepare() -> Result<LoadSession, String> {
        return Ok(LoadSession { path.clone(), drop, {} });
    }
};
struct ResourceSession {
    String path;
    auto   check() -> Result<empty, String> { return Ok(empty {}); }
    auto   run(u64 iterations) -> Result<bench::DiagnosticMeasurement, String> {
        bench::DiagnosticMeasurement result;
        result.iterations = iterations;
        result.allocation_unavailable =
            "LoadAsset uses fixed Global containers and codec malloc; no tracked allocator injection"_Str;
        rstd::uint64_t image_bytes {}, vertices {}, indices {};
        for (u64 i; i < iterations; ++i) {
            auto loaded = gltf::LoadAsset(path.as_str());
            if (loaded.is_err()) return Err(error_text(loaded.unwrap_err()));
            auto asset = rstd::move(loaded).unwrap();
            for (const auto& image : asset->images) image_bytes += image.rgba.len().to_primitive();
            for (const auto& mesh : asset->meshes)
                for (const auto& primitive : mesh.primitives) {
                    vertices += primitive.vertices.len().to_primitive();
                    indices += primitive.indices.len().to_primitive();
                }
        }
        result.metrics.push({ "asset.decoded_image_bytes"_Str,
                              bench::DiagnosticUnit::Bytes,
                              bench::MetricDirection::Neutral,
                              "sum returned Asset RGBA payloads"_Str,
                              "Asset::images"_Str,
                              bench::DiagnosticValue::Count(u64(image_bytes)) });
        result.metrics.push({ "asset.vertices"_Str,
                              bench::DiagnosticUnit::Count,
                              bench::MetricDirection::Neutral,
                              "sum returned Asset vertices"_Str,
                              "Asset::meshes"_Str,
                              bench::DiagnosticValue::Count(u64(vertices)) });
        result.metrics.push({ "asset.indices"_Str,
                              bench::DiagnosticUnit::Count,
                              bench::MetricDirection::Neutral,
                              "sum returned Asset indices"_Str,
                              "Asset::meshes"_Str,
                              bench::DiagnosticValue::Count(u64(indices)) });
#if defined(__linux__)
        rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
            result.metrics.push(
                { "process.peak_rss"_Str,
                  bench::DiagnosticUnit::Bytes,
                  bench::MetricDirection::Neutral,
                  "whole process lifetime, not per-load allocation peak"_Str,
                  "getrusage(RUSAGE_SELF).ru_maxrss"_Str,
                  bench::DiagnosticValue::Count(u64(rstd::uint64_t(usage.ru_maxrss) * 1024)) });
#endif
        return Ok(rstd::move(result));
    }
    auto finish() -> Result<empty, String> { return Ok(empty {}); }
};
struct ResourceFactory {
    String path;
    auto   prepare(bench::DiagnosticContext&) -> Result<ResourceSession, String> {
        return Ok(ResourceSession { path.clone() });
    }
};

enum class Stage
{
    Document,
    Resources,
    Asset
};
template<Stage stage>
struct StageSession {
    String               path;
    Option<Arc<Vec<u8>>> bytes;
    String               failure;

    auto setup() {
        if constexpr (stage == Stage::Document)
            return Result<Arc<Vec<u8>>, gltf::Error>(Ok(bytes->clone()));
        else if constexpr (stage == Stage::Resources)
            return gltf::LoadDocument(path.as_str());
        else {
            auto loaded = gltf::LoadDocument(path.as_str());
            if (loaded.is_err())
                return Result<gltf::ResolvedDocument, gltf::Error>(
                    Err(rstd::move(loaded).unwrap_err()));
            return gltf::ResolveResources(rstd::move(loaded).unwrap());
        }
    }
    auto operation(auto input) {
        if constexpr (stage == Stage::Document)
            return gltf::ParseDocument(rstd::move(input), {}, path.as_str());
        else if constexpr (stage == Stage::Resources)
            return gltf::ResolveResources(rstd::move(input));
        else
            return gltf::BuildAsset(rstd::move(input));
    }
    auto check() -> Result<empty, String> {
        auto input = setup();
        if (input.is_err()) return Err(error_text(input.unwrap_err()));
        auto output = operation(rstd::move(input).unwrap());
        if (output.is_err()) return Err(error_text(output.unwrap_err()));
        return Ok(empty {});
    }
    auto run(bench::Bench& engine, ref<str> name)
        -> Result<bench::BenchmarkResult, bench::BenchError> {
        if constexpr (stage == Stage::Document)
            engine.boundary(
                "ParseDocument(owned immutable source); file read and output destruction excluded"_str);
        else if constexpr (stage == Stage::Resources)
            engine.boundary(
                "ResolveResources(LoadInput); document read/parse and output destruction excluded"_str);
        else
            engine.boundary(
                "BuildAsset(ResolvedDocument); source read/parse/resolve and output destruction excluded"_str);
        auto measured = engine.run_batched(
            name,
            [&] {
                return setup();
            },
            [&](auto input) {
                using Output = decltype(operation(rstd::move(input).unwrap()));
                if (input.is_err()) {
                    auto error = rstd::move(input).unwrap_err();
                    if (failure.is_empty()) failure = error_text(error);
                    return Output(Err(rstd::move(error)));
                }
                auto output = operation(rstd::move(input).unwrap());
                if (output.is_err() && failure.is_empty())
                    failure = error_text(output.unwrap_err());
                return output;
            },
            { .max_items = usize(1) });
        if (! failure.is_empty()) return Err(bench::BenchError::Operation(failure.clone()));
        return measured;
    }
    auto finish() -> Result<empty, String> { return Ok(empty {}); }
};
template<Stage stage>
struct StageFactory {
    String path;
    auto   prepare() -> Result<StageSession<stage>, String> {
        StageSession<stage> result { path.clone(), {}, {} };
        if constexpr (stage == Stage::Document) {
            auto input = rstd::fs::read(rstd::path::PathBuf::from(path.as_str()).as_path());
            if (input.is_err()) return Err(rstd::format("{}", input.unwrap_err()));
            result.bytes = Some(Arc<Vec<u8>>::make(rstd::move(input).unwrap()));
        }
        return Ok(rstd::move(result));
    }
};

auto descriptor(const Input& input, const CpuOptions& options, ref<str> kind)
    -> bench::CaseDescriptor {
    bench::CaseDescriptor result;
    result.id             = rstd::format("{}/{}", kind, input.id);
    result.suite          = "vecgltf.cpu"_Str;
    result.revision       = "cpu-load-v1"_Str;
    result.implementation = options.implementation.clone();
    if (input.digest != "unspecified"_str)
        result.dataset = Some(bench::DatasetDescriptor { input.id.clone(),
                                                         input.revision.clone(),
                                                         "sha256-file-tree"_Str,
                                                         input.digest.clone(),
                                                         input.bytes,
                                                         None() });
    result.parameters.push(
        { "cache"_Str,
          bench::ParameterValue::Text(
              "OS cache warm; validation and dataset fingerprint precede timing"_Str) });
    result.parameters.push(
        { "metadata"_Str,
          bench::ParameterValue::Text(options.implementation == "cgltf"_str
                                          ? "runtime-core-only; source metadata absent"_Str
                                          : "Document source metadata retained"_Str) });
    return result;
}
template<Stage stage>
auto add_stage(bench::Suite& suite, const Input& input, const CpuOptions& options, ref<str> kind)
    -> Result<empty, String> {
    if (options.selected != "all"_str && options.selected != kind) return Ok(empty {});
    return suite.add(descriptor(input, options, kind), StageFactory<stage> { input.path.clone() });
}

auto execute(CpuOptions options) -> Result<empty, String> {
    auto manifest = read_manifest(options);
    if (manifest.is_err()) return manifest;
    if (options.inputs.is_empty()) return Err("at least one model or manifest is required"_Str);
    if (options.check_only && options.semantic_json.is_empty())
        return Err("--check-only requires --semantics"_Str);
    bool         semantic_failed {};
    bench::Suite suite;
    auto         semantic_output          = rstd::json::Value::Object(rstd::json::Map {});
    semantic_output["implementation"_str] = json_text(options.implementation.clone());
    semantic_output["revision"_str]       = json_text(options.revision.clone());
    auto semantic_models                  = rstd::json::Value::Array(rstd::json::Array {});
    for (const auto& input : options.inputs) {
        if (! options.semantic_json.is_empty()) {
            auto                        prepared = gltf::LoadDocument(input.path.as_str());
            Option<gltf::MetadataOwner> information;
            if (prepared.is_ok()) information = Some(prepared->document.metadata_owner());
            auto loaded =
                prepared.is_ok()
                    ? gltf::LoadAsset(rstd::move(prepared).unwrap())
                    : Result<Arc<gltf::Asset>, gltf::Error>(Err(rstd::move(prepared).unwrap_err()));
            auto summary          = loaded.is_ok() ? semantics(*loaded.unwrap())
                                                   : rstd::json::Value::Object(rstd::json::Map {});
            summary["loaded"_str] = rstd::json::Value::Bool(loaded.is_ok());
            if (loaded.is_err()) {
                semantic_failed   = true;
                const auto& error = loaded.unwrap_err();
                field(summary, "error_kind"_str, unsigned(error.kind));
                summary["error_location"_str] = json_text(error.location.clone());
                summary["error_message"_str]  = json_text(error.message.clone());
                summary["source_metadata"_str] =
                    source_metadata(rstd::move(information), "Document::metadata_owner"_str);
            }
            summary["id"_str]   = json_text(input.id.clone());
            summary["path"_str] = json_text(input.path.clone());
            semantic_models.as_array_mut().unwrap()->push(rstd::move(summary));
        }
        if (options.check_only) continue;
        auto parsed = add_stage<Stage::Document>(suite, input, options, "document.parse"_str);
        if (parsed.is_err()) return parsed;
        auto resolved = add_stage<Stage::Resources>(suite, input, options, "resources.resolve"_str);
        if (resolved.is_err()) return resolved;
        auto built = add_stage<Stage::Asset>(suite, input, options, "asset.build"_str);
        if (built.is_err()) return built;
        for (bool drop : rstd::array<bool, 2> { false, true }) {
            auto kind = drop ? "asset.load_drop"_str : "asset.load"_str;
            if (options.selected != "all"_str && options.selected != kind) continue;
            auto selected = descriptor(input, options, kind);
            auto id       = selected.id.clone();
            auto added = suite.add(rstd::move(selected), LoadFactory { input.path.clone(), drop });
            if (added.is_err()) return added;
            auto diagnostic =
                suite.add_diagnostic(id.as_str(), ResourceFactory { input.path.clone() });
            if (diagnostic.is_err()) return diagnostic;
        }
    }
    semantic_output["models"_str] = rstd::move(semantic_models);
    auto semantic_written         = write_json(options.semantic_json.as_str(), semantic_output);
    if (semantic_written.is_err()) return semantic_written;
    if (options.check_only)
        return semantic_failed ? Result<empty, String>(Err("CPU Asset semantic check failed"_Str))
                               : Result<empty, String>(Ok(empty {}));
    bench::RunnerOptions settings;
    settings.sampling.epochs                 = usize(options.epochs.to_primitive());
    settings.sampling.exact_epoch_iterations = Some(u64(options.iterations.to_primitive()));
    settings.sampling.warmup_iterations      = u64(options.warmup.to_primitive());
    settings.stop_on_failure                 = false;
    bench::RunEnvironment environment;
    environment.fields.push(
        { "compiler"_Str, Some(String::make(argument(__clang_version__))), "compiler macro"_Str });
    environment.fields.push(
        { "loader_revision"_Str, Some(options.revision.clone()), "--revision"_Str });
#if defined(__OPTIMIZE__)
    environment.fields.push({ "optimized"_Str, Some("true"_Str), "__OPTIMIZE__"_Str });
#else
    environment.fields.push({ "optimized"_Str, Some("false"_Str), "__OPTIMIZE__"_Str });
#endif
    bench::Runner       runner(rstd::move(settings), rstd::move(environment));
    bench::TextReporter reporter;
    auto                report =
        options.diagnose ? runner.diagnose(suite, u64(1), reporter) : runner.run(suite, reporter);
    if (report.is_err()) return Err(rstd::move(report).unwrap_err());
    auto output_path = options.diagnose && ! options.diagnostic_json.is_empty()
                           ? options.diagnostic_json.as_str()
                           : options.json.as_str();
    if (! output_path.is_empty()) {
        auto encoded = bench::report::encode(*report);
        if (encoded.is_err()) return Err(rstd::move(encoded).unwrap_err());
        auto saved = rstd::fs::write(rstd::path::PathBuf::from(output_path).as_path(),
                                     encoded->as_str().as_bytes());
        if (saved.is_err()) return Err(rstd::format("{}", saved.unwrap_err()));
    }
    if (! report->passed()) return Err("CPU loading benchmark failed"_Str);
    return Ok(empty {});
}
} // namespace vecgltf_bench::cpu

export namespace vecgltf_bench
{
auto RunCpu(CpuOptions options) -> Result<empty, String> {
    return cpu::execute(rstd::move(options));
}
} // namespace vecgltf_bench
