module;
#include "../../third_party/stb_image.h"
module vecgltf.load;
import vecgltf.load.accessor;

namespace vecgltf
{
using namespace rstd::literals;

String load_text(const char* value) {
    return String::make(value ? rstd::ffi::CStr::from_ptr(value).to_str().unwrap_or(""_str)
                              : ""_str);
}
struct DecodedPixels {
    void* data {};
    ~DecodedPixels() { stbi_image_free(data); }
};
enum class AttributeType
{
    Position,
    Normal,
    Tangent,
    Texcoord,
    Color,
    Joints,
    Weights,
    Custom
};
struct AttributeSemantic {
    AttributeType type { AttributeType::Custom };
    unsigned      index {};
};
auto attribute_semantic(ref<str> name) -> AttributeSemantic {
    if (name == "POSITION"_str) return { AttributeType::Position, 0 };
    if (name == "NORMAL"_str) return { AttributeType::Normal, 0 };
    if (name == "TANGENT"_str) return { AttributeType::Tangent, 0 };
    struct Prefix {
        ref<str>      text;
        AttributeType type;
    };
    Prefix prefixes[] { { "TEXCOORD_"_str, AttributeType::Texcoord },
                        { "COLOR_"_str, AttributeType::Color },
                        { "JOINTS_"_str, AttributeType::Joints },
                        { "WEIGHTS_"_str, AttributeType::Weights } };
    for (const auto& prefix : prefixes) {
        auto suffix = name.strip_prefix(prefix.text);
        if (! suffix) continue;
        auto index = rstd::from_str<u32>(*suffix);
        if (index.is_ok()) return { prefix.type, index.unwrap().to_primitive() };
    }
    return {};
}
struct AssetBuilder {
    const ResolvedDocument& resolved;
    const document::Data&   data;
    const LoadOptions&      options;
    ref<str>                source;
    AccessorReader          reader;
    Arc<Asset>              asset { Arc<Asset>::make() };
    usize                   vertices {}, indices_count {}, decoded_pixels {};
    explicit AssetBuilder(const ResolvedDocument& value)
        : resolved(value),
          data(value.document().data()),
          options(value.context().options),
          source(value.context().source.as_str()),
          reader(value) {}
    auto error(ErrorKind kind, String location, const char* message) const -> Error {
        return { kind, String::make(source), rstd::move(location), load_text(message) };
    }
    auto accessor(unsigned index) const -> const document::Accessor* {
        return index < count(data.accessors) ? &at(data.accessors, index) : nullptr;
    }
    auto floats(const document::Accessor* value, unsigned components, String location) const
        -> Result<Vec<float>, Error> {
        if (! value)
            return Err(error(ErrorKind::InvalidAsset, rstd::move(location), "missing accessor"));
        return reader.Floats(
            unsigned(value - data.accessors.begin()), components, location.as_str());
    }
    auto attribute(const Vec<document::Attribute>& attributes,
                   AttributeType                   type,
                   unsigned                        set = 0) const -> const document::Accessor* {
        for (const auto& input : attributes) {
            auto semantic = attribute_semantic(input.name.as_str());
            if (semantic.type == type && semantic.index == set) return accessor(input.accessor);
        }
        return nullptr;
    }
    auto materials() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Materials);
        for (const auto& input : data.materials) {
            const auto& core = input.core;
            Material    result;
            result.name         = core.name.clone();
            result.base_color   = core.base_color;
            result.emissive     = core.emissive * input.emissive_strength;
            result.metallic     = core.metallic;
            result.roughness    = core.roughness;
            result.alpha_cutoff = core.alpha_cutoff;
            result.alpha        = core.alpha;
            result.double_sided = core.double_sided;
            result.unlit        = core.unlit;
            result.base         = core.base;
            result.physical     = core.physical;
            result.normal       = core.normal;
            result.occlusion    = core.occlusion;
            result.emission     = core.emission;
            result.source       = input.source;
            if (input.specular_glossiness) {
                const auto& sg    = *input.specular_glossiness;
                result.workflow   = MaterialWorkflow::SpecularGlossiness;
                result.base_color = sg.diffuse;
                result.specular   = sg.specular;
                result.glossiness = sg.glossiness;
                result.base       = sg.diffuse_texture;
                result.physical   = sg.physical_texture;
            }
            asset->materials.push(rstd::move(result));
        }
        asset->materials.push(Material {});
        return Ok(empty {});
    }
    auto images() -> Result<empty, Error> {
        LoadScope     scope(options.trace, LoadEvent::Images);
        Vec<unsigned> image_indices;
        image_indices.resize(data.images.len(), invalid_index);
        for (unsigned i = 0; i < count(data.images); ++i) {
            auto encoded = resolved.image(i);
            if (! encoded) continue;
            LoadScope decode(options.trace, LoadEvent::ImageDecode);
            auto      bytes = encoded->bytes();
            if (bytes.len().to_primitive() > 0x7fffffff)
                return Err(error(ErrorKind::Limit,
                                 rstd::format("images[{}]", i),
                                 "encoded image exceeds decoder limit"));
            const auto* pointer = reinterpret_cast<const unsigned char*>(bytes.as_byte_ptr());
            int         width {}, height {}, channels {};
            if (! stbi_info_from_memory(
                    pointer, int(bytes.len().to_primitive()), &width, &height, &channels) ||
                width <= 0 || height <= 0)
                return Err(
                    error(ErrorKind::Decode, rstd::format("images[{}]", i), stbi_failure_reason()));
            auto pixels = rstd::uint64_t(width) * rstd::uint64_t(height);
            if (pixels > rstd::uint64_t(~rstd::size_t(0)) / 4 ||
                pixels > options.max_image_pixels.to_primitive() ||
                decoded_pixels > options.max_decoded_pixels ||
                pixels > (options.max_decoded_pixels - decoded_pixels).to_primitive())
                return Err(error(
                    ErrorKind::Limit, rstd::format("images[{}]", i), "image exceeds pixel limit"));
            DecodedPixels decoded { stbi_load_from_memory(
                pointer, int(bytes.len().to_primitive()), &width, &height, &channels, 4) };
            if (! decoded.data)
                return Err(
                    error(ErrorKind::Decode, rstd::format("images[{}]", i), stbi_failure_reason()));
            decode.finish();
            decoded_pixels += usize(pixels);
            Image output;
            output.name   = at(data.images, i).name.clone();
            output.source = at(data.images, i).source;
            output.width  = unsigned(width);
            output.height = unsigned(height);
            LoadScope copy(options.trace, LoadEvent::ImageCopy);
            output.rgba          = Vec<u8>::from(slice<u8>::from_raw_parts(
                static_cast<const byte*>(decoded.data), usize(pixels * 4)));
            at(image_indices, i) = unsigned(count(asset->images));
            asset->images.push(rstd::move(output));
        }
        for (unsigned i = 0; i < count(data.textures); ++i) {
            const auto& input = at(data.textures, i);
            if (input.image == invalid_index)
                return Err(error(ErrorKind::Unsupported,
                                 rstd::format("textures[{}]", i),
                                 "texture has no supported core image"));
            Texture output;
            output.source = input.source;
            output.image  = at(image_indices, input.image);
            if (input.sampler != invalid_index) {
                const auto& s    = at(data.samplers, input.sampler);
                auto        wrap = [](unsigned value) {
                    return value == 33071   ? Wrap::Clamp
                           : value == 33648 ? Wrap::Mirror
                                            : Wrap::Repeat;
                };
                output.sampler.source = s.source;
                output.sampler.s      = wrap(s.wrap_s);
                output.sampler.t      = wrap(s.wrap_t);
                output.sampler.mag    = s.mag_filter == 9728 ? Filter::Nearest : Filter::Linear;
                output.sampler.min =
                    (s.min_filter == 9728 || s.min_filter == 9984 || s.min_filter == 9986)
                        ? Filter::Nearest
                        : Filter::Linear;
                output.sampler.mip =
                    (s.min_filter == 9728 || s.min_filter == 9729)   ? MipFilter::None
                    : (s.min_filter == 9984 || s.min_filter == 9985) ? MipFilter::Nearest
                                                                     : MipFilter::Linear;
            }
            asset->textures.push(rstd::move(output));
        }
        return Ok(empty {});
    }
    auto primitive(const document::Primitive& input, unsigned mesh_index, unsigned primitive_index)
        -> Result<Primitive, Error> {
        auto location = rstd::format("meshes[{}].primitives[{}]", mesh_index, primitive_index);
        if (input.mode != 4 && input.mode != 5 && input.mode != 6)
            return Err(error(ErrorKind::Unsupported,
                             rstd::move(location),
                             "only triangle topologies are supported"));
        const auto* position = attribute(input.attributes, AttributeType::Position);
        if (! position || position->count.to_primitive() >
                              options.max_vertices.to_primitive() - vertices.to_primitive())
            return Err(error(ErrorKind::Limit,
                             rstd::move(location),
                             "missing positions or vertex limit exceeded"));
        if (input.has_draco && position->buffer_view == invalid_index && ! position->sparse)
            return Err(error(ErrorKind::Unsupported,
                             location.clone(),
                             "Draco primitive has no core geometry fallback"));
        vertices += usize(position->count.to_primitive());
        Primitive result;
        result.source = input.source;
        result.material =
            input.material != invalid_index ? input.material : unsigned(count(data.materials));
        result.vertices.resize(usize(position->count.to_primitive()), Vertex {});
        const auto&           material = at(asset->materials, result.material);
        const TextureBinding* bindings[] { &material.base,
                                           &material.physical,
                                           &material.normal,
                                           &material.occlusion,
                                           &material.emission };
        auto                  uses_texcoord = [&](unsigned set) {
            for (const auto* binding : bindings)
                if (binding->texture != invalid_index && binding->uv == set) return true;
            return false;
        };
        for (const auto* binding : bindings)
            if (binding->texture != invalid_index &&
                ! attribute(input.attributes, AttributeType::Texcoord, binding->uv))
                return Err(error(ErrorKind::InvalidAsset,
                                 rstd::format("{}.attributes.TEXCOORD_{}", location, binding->uv),
                                 "material references a missing texcoord set"));
        bool has_normal {}, has_tangent {}, has_joints {}, has_weights {};
        for (const auto& attr : input.attributes) {
            auto semantic = attribute_semantic(attr.name.as_str());
            if (semantic.type == AttributeType::Custom) continue;
            const auto* description = accessor(attr.accessor);
            if (! description)
                return Err(error(ErrorKind::InvalidAsset,
                                 location.clone(),
                                 "attribute accessor is out of range"));
            auto components = document::components(description->shape);
            if (description->count.to_primitive() != position->count.to_primitive())
                return Err(
                    error(ErrorKind::InvalidAsset, location.clone(), "attribute counts differ"));
            bool floating = semantic.type == AttributeType::Position ||
                            semantic.type == AttributeType::Normal ||
                            semantic.type == AttributeType::Tangent;
            if ((floating && description->component_type != 5126) ||
                (semantic.type == AttributeType::Joints && description->component_type != 5121 &&
                 description->component_type != 5123))
                return Err(error(
                    ErrorKind::InvalidAsset, location.clone(), "invalid attribute component type"));
            auto shape        = description->shape;
            bool unit_integer = description->normalized && (description->component_type == 5121 ||
                                                            description->component_type == 5123);
            bool packed_color = semantic.type == AttributeType::Color ||
                                semantic.type == AttributeType::Texcoord ||
                                semantic.type == AttributeType::Weights;
            if ((semantic.type == AttributeType::Position && shape != document::Shape::Vec3) ||
                (semantic.type == AttributeType::Normal && shape != document::Shape::Vec3) ||
                ((semantic.type == AttributeType::Tangent ||
                  semantic.type == AttributeType::Joints ||
                  semantic.type == AttributeType::Weights) &&
                 shape != document::Shape::Vec4) ||
                (semantic.type == AttributeType::Texcoord && shape != document::Shape::Vec2) ||
                (semantic.type == AttributeType::Color && shape != document::Shape::Vec3 &&
                 shape != document::Shape::Vec4) ||
                (packed_color && description->component_type != 5126 && ! unit_integer) ||
                (semantic.type == AttributeType::Joints && description->normalized))
                return Err(error(ErrorKind::InvalidAsset,
                                 location.clone(),
                                 "invalid attribute shape or normalization"));
            if (semantic.type == AttributeType::Texcoord) {
                if (components != 2)
                    return Err(
                        error(ErrorKind::InvalidAsset, location.clone(), "texcoord must be VEC2"));
                if (semantic.index > 1) {
                    if (! uses_texcoord(unsigned(semantic.index))) continue;
                    auto values = floats(description, 2, location.clone());
                    if (values.is_err()) return Err(values.unwrap_err());
                    auto        unpacked = values.unwrap();
                    TexcoordSet stream;
                    stream.index = unsigned(semantic.index);
                    stream.coordinates.reserve(usize(position->count.to_primitive()));
                    for (rstd::size_t v = 0; v < position->count.to_primitive(); ++v)
                        stream.coordinates.push({ at(unpacked, v * 2), at(unpacked, v * 2 + 1) });
                    result.extra_texcoords.push(rstd::move(stream));
                    continue;
                }
            }
            if (semantic.index > 1 ||
                (semantic.index != 0 && semantic.type != AttributeType::Texcoord))
                return Err(
                    error(ErrorKind::Unsupported, location.clone(), "unsupported attribute set"));
            auto values = floats(description, components, location.clone());
            if (values.is_err()) return Err(values.unwrap_err());
            auto unpacked = values.unwrap();
            for (rstd::size_t v = 0; v < position->count.to_primitive(); ++v) {
                auto&        vertex = at(result.vertices, v);
                const float* f      = unpacked.begin() + v * components;
                switch (semantic.type) {
                case AttributeType::Position:
                    if (components != 3)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "position must be VEC3"));
                    vertex.position = { f[0], f[1], f[2] };
                    result.bounds.include(vertex.position);
                    break;
                case AttributeType::Normal:
                    if (components != 3)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "normal must be VEC3"));
                    vertex.normal = { f[0], f[1], f[2] };
                    has_normal    = true;
                    break;
                case AttributeType::Tangent:
                    if (components != 4)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "tangent must be VEC4"));
                    vertex.tangent = { f[0], f[1], f[2], f[3] };
                    has_tangent    = true;
                    break;
                case AttributeType::Texcoord:
                    (semantic.index == 0 ? vertex.uv0 : vertex.uv1) = { f[0], f[1] };
                    break;
                case AttributeType::Color:
                    if (components != 3 && components != 4)
                        return Err(error(ErrorKind::InvalidAsset,
                                         location.clone(),
                                         "color must be VEC3 or VEC4"));
                    vertex.color = { f[0], f[1], f[2], components == 4 ? f[3] : 1 };
                    break;
                case AttributeType::Joints:
                    if (components != 4)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "joints must be VEC4"));
                    for (unsigned j = 0; j < 4; ++j) {
                        if (f[j] < 0 || f[j] > 65535 || f[j] != float(unsigned(f[j])))
                            return Err(error(
                                ErrorKind::InvalidAsset, location.clone(), "invalid joint index"));
                        vertex.joints[j] = unsigned(f[j]);
                    }
                    has_joints = true;
                    break;
                case AttributeType::Weights: {
                    if (components != 4)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "weights must be VEC4"));
                    float sum = f[0] + f[1] + f[2] + f[3];
                    if (sum <= 0 || f[0] < 0 || f[1] < 0 || f[2] < 0 || f[3] < 0)
                        return Err(error(ErrorKind::InvalidAsset,
                                         location.clone(),
                                         "joint weights sum is zero"));
                    vertex.weights = Vec4 { f[0], f[1], f[2], f[3] } * (1 / sum);
                    has_weights    = true;
                    break;
                }
                default: break;
                }
            }
        }
        if (has_joints != has_weights)
            return Err(error(ErrorKind::InvalidAsset,
                             location.clone(),
                             "joints and weights must appear together"));
        result.skin_attributes       = has_joints;
        const auto index_count       = input.indices == invalid_index
                                           ? position->count.to_primitive()
                                           : accessor(input.indices)->count.to_primitive();
        auto       remaining_indices = (options.max_indices - indices_count).to_primitive();
        if (index_count > remaining_indices ||
            (input.mode != 4 && index_count > 2 && index_count - 2 > remaining_indices / 3))
            return Err(
                error(ErrorKind::Limit, location.clone(), "indices exceed cumulative limit"));
        const auto output_count = input.mode == 4   ? index_count
                                  : index_count > 2 ? (index_count - 2) * 3
                                                    : 0;
        indices_count += usize(index_count > output_count ? index_count : output_count);
        Vec<unsigned> indices;
        if (input.indices != invalid_index) {
            auto values = reader.Indices(input.indices, location.as_str());
            if (values.is_err()) return Err(values.unwrap_err());
            indices = values.unwrap();
        } else {
            indices.reserve(usize(index_count));
            for (unsigned i = 0; i < position->count.to_primitive(); ++i) indices.push(unsigned(i));
        }
        for (auto index : indices)
            if (index >= position->count.to_primitive())
                return Err(
                    error(ErrorKind::InvalidAsset, location.clone(), "index outside vertex range"));
        result.indexed = input.indices != invalid_index || input.mode != 4;
        if (input.mode == 4) {
            if (count(indices) % 3)
                return Err(error(ErrorKind::InvalidAsset,
                                 location.clone(),
                                 "triangle count is not divisible by three"));
            result.indices = rstd::move(indices);
        } else {
            result.indices.reserve(usize(output_count));
            for (rstd::size_t i = 2; i < count(indices); ++i) {
                result.indices.push(
                    unsigned(input.mode == 6 ? at(indices, 0) : at(indices, i - (i % 2 ? 1 : 2))));
                result.indices.push(unsigned(input.mode == 6 ? at(indices, i - 1)
                                                             : at(indices, i - (i % 2 ? 2 : 1))));
                result.indices.push(unsigned(at(indices, i)));
            }
        }
        for (rstd::size_t t = 0; t < count(input.targets); ++t) {
            MorphTarget target;
            const auto& src = at(input.targets, t);
            target.source   = src.source;
            Vec<Vec3>*    outputs[] { &target.positions, &target.normals, &target.tangents };
            AttributeType types[] { AttributeType::Position,
                                    AttributeType::Normal,
                                    AttributeType::Tangent };
            for (unsigned k = 0; k < 3; ++k) {
                outputs[k]->resize(usize(position->count.to_primitive()), Vec3 {});
                auto accessor = attribute(src.attributes, types[k]);
                if (! accessor) continue;
                if (accessor->count.to_primitive() != position->count.to_primitive() ||
                    accessor->shape != document::Shape::Vec3 || accessor->component_type != 5126)
                    return Err(error(ErrorKind::InvalidAsset,
                                     location.clone(),
                                     "morph requires matching FLOAT VEC3 accessor"));
                auto read = floats(accessor, 3, location.clone());
                if (read.is_err()) return Err(read.unwrap_err());
                auto values = read.unwrap();
                for (rstd::size_t v = 0; v < position->count.to_primitive(); ++v)
                    at(*outputs[k],
                       v) = { at(values, v * 3), at(values, v * 3 + 1), at(values, v * 3 + 2) };
            }
            result.morphs.push(rstd::move(target));
        }
        result.bounds = {};
        for (auto index : result.indices)
            result.bounds.include(at(result.vertices, index).position);
        if (! has_normal) {
            if (count(result.indices) > count(result.vertices)) {
                auto extra = usize(count(result.indices) - count(result.vertices));
                if (extra > options.max_vertices - vertices)
                    return Err(error(ErrorKind::Limit,
                                     location.clone(),
                                     "flat normal expansion exceeds vertex limit"));
                vertices += extra;
            }
            Vec<Vertex> expanded;
            for (auto index : result.indices) expanded.push(Vertex(at(result.vertices, index)));
            for (auto& stream : result.extra_texcoords) {
                Vec<Vec2> coordinates;
                coordinates.reserve(result.indices.len());
                for (auto index : result.indices)
                    coordinates.push(Vec2(at(stream.coordinates, index)));
                stream.coordinates = rstd::move(coordinates);
            }
            for (auto& morph : result.morphs) {
                MorphTarget expanded_morph;
                expanded_morph.source = morph.source;
                for (auto index : result.indices) {
                    expanded_morph.positions.push(Vec3(at(morph.positions, index)));
                    expanded_morph.normals.push(Vec3(at(morph.normals, index)));
                    expanded_morph.tangents.push(Vec3(at(morph.tangents, index)));
                }
                morph = rstd::move(expanded_morph);
            }
            result.vertices = rstd::move(expanded);
            for (unsigned i = 0; i < count(result.indices); ++i) at(result.indices, i) = i;
            has_tangent = false;
        }
        Vec<Vec3> tangents, bitangents;
        tangents.resize(result.vertices.len(), Vec3 {});
        bitangents.resize(result.vertices.len(), Vec3 {});
        for (rstd::size_t i = 0; i < count(result.indices); i += 3) {
            unsigned a = at(result.indices, i), b = at(result.indices, i + 1),
                     c  = at(result.indices, i + 2);
            auto&    va = at(result.vertices, a);
            auto&    vb = at(result.vertices, b);
            auto&    vc = at(result.vertices, c);
            auto     e1 = vb.position - va.position, e2 = vc.position - va.position;
            if (! has_normal) {
                auto n    = cross(e1, e2);
                va.normal = va.normal + n;
                vb.normal = vb.normal + n;
                vc.normal = vc.normal + n;
            }
            if (! has_tangent) {
                unsigned uv_set = material.normal.texture == invalid_index ? 0 : material.normal.uv;
                auto     ua     = result.texcoord(uv_set, a).unwrap();
                auto     ub     = result.texcoord(uv_set, b).unwrap();
                auto     uc     = result.texcoord(uv_set, c).unwrap();
                float    sx = ub.x - ua.x, sy = ub.y - ua.y, tx = uc.x - ua.x, ty = uc.y - ua.y;
                float    determinant = sx * ty - tx * sy;
                if (absolute(determinant) > 1e-12f) {
                    Vec3     tangent   = (e1 * ty - e2 * sy) / determinant,
                             bitangent = (e2 * sx - e1 * tx) / determinant;
                    unsigned triangle[] { a, b, c };
                    for (unsigned index : triangle) {
                        at(tangents, index)   = at(tangents, index) + tangent;
                        at(bitangents, index) = at(bitangents, index) + bitangent;
                    }
                }
            }
        }
        for (rstd::size_t i = 0; i < count(result.vertices); ++i) {
            auto& v  = at(result.vertices, i);
            v.normal = normalized(v.normal);
            if (! has_tangent) {
                auto tangent = at(tangents, i) - v.normal * dot(v.normal, at(tangents, i));
                if (dot(tangent, tangent) < 1e-12f)
                    tangent =
                        cross(absolute(v.normal.y) < 0.9f ? Vec3 { 0, 1, 0 } : Vec3 { 1, 0, 0 },
                              v.normal);
                tangent   = normalized(tangent);
                v.tangent = { tangent.x,
                              tangent.y,
                              tangent.z,
                              dot(cross(v.normal, tangent), at(bitangents, i)) < 0 ? -1.0f : 1.0f };
            }
        }
        if (! result.indexed) result.indices.clear();
        return Ok(rstd::move(result));
    }
    auto geometry() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Geometry);
        for (rstd::size_t i = 0; i < count(data.meshes); ++i) {
            Mesh mesh;
            mesh.name         = at(data.meshes, i).name.clone();
            mesh.source       = at(data.meshes, i).source;
            const auto& input = at(data.meshes, i);
            for (rstd::size_t p = 0; p < count(input.primitives); ++p) {
                auto result = primitive(at(input.primitives, p), unsigned(i), unsigned(p));
                if (result.is_err()) return Err(result.unwrap_err());
                mesh.primitives.push(result.unwrap());
            }
            auto morph_count =
                mesh.primitives.is_empty() ? 0 : count(at(mesh.primitives, 0).morphs);
            for (const auto& primitive : mesh.primitives)
                if (count(primitive.morphs) != morph_count)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("meshes[{}]", i),
                                     "primitives have different morph target counts"));
            if (count(input.weights) && count(input.weights) != morph_count)
                return Err(error(ErrorKind::InvalidAsset,
                                 rstd::format("meshes[{}].weights", i),
                                 "morph weights count differs"));
            for (rstd::size_t w = 0; w < morph_count; ++w)
                mesh.weights.push(count(input.weights) ? float(at(input.weights, w)) : 0.0f);
            asset->meshes.push(rstd::move(mesh));
        }
        return Ok(empty {});
    }
    auto scene() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Scene);
        for (unsigned i = 0; i < count(data.nodes); ++i) {
            const auto& input = at(data.nodes, i);
            Node        node;
            node.name        = input.name.clone();
            node.parent      = input.parent;
            node.mesh        = input.mesh;
            node.skin        = input.skin;
            node.camera      = input.camera;
            node.children    = input.children.clone();
            node.translation = input.translation;
            node.rotation    = input.rotation;
            node.scale       = input.scale;
            node.matrix      = input.matrix;
            node.has_matrix  = input.has_matrix;
            node.source      = input.source;
            if (! input.weights.is_empty())
                node.weights = input.weights.clone();
            else if (node.mesh != invalid_index)
                node.weights = at(asset->meshes, node.mesh).weights.clone();
            if (node.mesh != invalid_index &&
                node.weights.len() != at(asset->meshes, node.mesh).weights.len())
                return Err(error(ErrorKind::InvalidAsset,
                                 rstd::format("nodes[{}].weights", i),
                                 "morph weight count differs"));
            asset->nodes.push(rstd::move(node));
        }
        for (const auto& input : data.scenes) {
            Scene scene;
            scene.name   = input.name.clone();
            scene.roots  = input.roots.clone();
            scene.source = input.source;
            asset->scenes.push(rstd::move(scene));
        }
        if (asset->scenes.is_empty()) {
            Scene scene;
            for (unsigned i = 0; i < count(asset->nodes); ++i)
                if (at(asset->nodes, i).parent == invalid_index) scene.roots.push(unsigned(i));
            asset->scenes.push(rstd::move(scene));
        }
        asset->default_scene = data.default_scene == invalid_index ? 0 : data.default_scene;
        for (unsigned i = 0; i < count(data.skins); ++i) {
            const auto& input = at(data.skins, i);
            Skin        skin;
            skin.name     = input.name.clone();
            skin.joints   = input.joints.clone();
            skin.skeleton = input.skeleton;
            skin.source   = input.source;
            skin.inverse_bind.resize(input.joints.len(), Mat4::Identity());
            if (input.inverse_bind != invalid_index) {
                const auto* values = accessor(input.inverse_bind);
                if (! values || values->shape != document::Shape::Mat4 ||
                    values->component_type != 5126 ||
                    values->count.to_primitive() != count(input.joints))
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("skins[{}]", i),
                                     "invalid inverse bind accessor"));
                auto decoded = floats(values, 16, rstd::format("skins[{}]", i));
                if (decoded.is_err()) return Err(decoded.unwrap_err());
                auto unpacked = decoded.unwrap();
                for (unsigned j = 0; j < count(skin.joints); ++j)
                    for (unsigned k = 0; k < 16; ++k)
                        at(skin.inverse_bind, j).m[k] = at(unpacked, j * 16 + k);
            }
            asset->skins.push(rstd::move(skin));
        }
        for (unsigned i = 0; i < count(asset->nodes); ++i) {
            const auto& node = at(asset->nodes, i);
            if (node.skin == invalid_index || node.mesh == invalid_index) continue;
            const auto& skin = at(asset->skins, node.skin);
            for (const auto& primitive : at(asset->meshes, node.mesh).primitives) {
                if (! primitive.skin_attributes)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("nodes[{}].skin", i),
                                     "skinned primitive has no joint attributes"));
                for (const auto& vertex : primitive.vertices)
                    for (auto joint : vertex.joints)
                        if (joint >= count(skin.joints))
                            return Err(error(ErrorKind::InvalidAsset,
                                             rstd::format("nodes[{}].skin", i),
                                             "joint index exceeds skin palette"));
            }
        }
        for (const auto& input : data.cameras) {
            Camera camera;
            static_cast<CameraProjection&>(camera) = input.projection;
            camera.name                            = input.name.clone();
            camera.source                          = input.source;
            asset->cameras.push(rstd::move(camera));
        }
        return Ok(empty {});
    }
    auto animations() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Animations);
        for (unsigned i = 0; i < count(data.animations); ++i) {
            const auto& input = at(data.animations, i);
            Animation   animation;
            animation.name   = input.name.clone();
            animation.source = input.source;
            for (unsigned s = 0; s < count(input.samplers); ++s) {
                const auto&      sampler = at(input.samplers, s);
                AnimationSampler output;
                output.interpolation        = sampler.interpolation;
                output.source               = sampler.source;
                const auto* input_accessor  = accessor(sampler.input);
                const auto* output_accessor = accessor(sampler.output);
                if (! input_accessor || ! output_accessor ||
                    input_accessor->component_type != 5126 ||
                    input_accessor->shape != document::Shape::Scalar ||
                    output_accessor->component_type != 5126)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}].samplers[{}]", i, s),
                                     "invalid animation accessor"));
                auto times = floats(
                    input_accessor, 1, rstd::format("animations[{}].samplers[{}].input", i, s));
                auto values = floats(output_accessor,
                                     document::components(output_accessor->shape),
                                     rstd::format("animations[{}].samplers[{}].output", i, s));
                if (times.is_err()) return Err(times.unwrap_err());
                if (values.is_err()) return Err(values.unwrap_err());
                output.times  = times.unwrap();
                output.values = values.unwrap();
                if (output.times.is_empty())
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}]", i),
                                     "empty animation sampler"));
                if (at(output.times, 0) < 0)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}]", i),
                                     "animation time is negative"));
                for (rstd::size_t t = 1; t < count(output.times); ++t)
                    if (at(output.times, t) <= at(output.times, t - 1))
                        return Err(error(ErrorKind::InvalidAsset,
                                         rstd::format("animations[{}]", i),
                                         "animation times must increase"));
                unsigned factor = output.interpolation == Interpolation::CubicSpline ? 3 : 1;
                if (count(output.values) % (count(output.times) * factor))
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}]", i),
                                     "animation output shape differs"));
                output.components = unsigned(count(output.values) / (count(output.times) * factor));
                animation.start =
                    s == 0 ? at(output.times, 0) : minimum(animation.start, at(output.times, 0));
                animation.end = maximum(animation.end, at(output.times, count(output.times) - 1));
                animation.samplers.push(rstd::move(output));
            }
            for (unsigned c = 0; c < count(input.channels); ++c) {
                const auto& channel = at(input.channels, c);
                if (channel.node == invalid_index) continue;
                AnimationChannel output {
                    channel.node, channel.sampler, channel.path, channel.source
                };
                const auto& node   = at(asset->nodes, output.node);
                const auto* values = accessor(at(input.samplers, output.sampler).output);
                auto        expected_shape =
                    output.path == AnimationPath::Weights    ? document::Shape::Scalar
                    : output.path == AnimationPath::Rotation ? document::Shape::Vec4
                                                             : document::Shape::Vec3;
                if (! values || values->shape != expected_shape)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}].channels[{}]", i, c),
                                     "animation output accessor has wrong shape"));
                unsigned expected = output.path == AnimationPath::Rotation ? 4
                                    : output.path == AnimationPath::Weights
                                        ? unsigned(count(node.weights))
                                        : 3;
                if (! expected || at(animation.samplers, output.sampler).components != expected ||
                    (node.has_matrix && output.path != AnimationPath::Weights))
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}].channels[{}]", i, c),
                                     "animation target and output shape differ"));
                animation.channels.push(rstd::move(output));
            }
            asset->animations.push(rstd::move(animation));
        }
        return Ok(empty {});
    }
};

auto BuildAsset(ResolvedDocument input) -> Result<Arc<Asset>, Error> {
    AssetBuilder builder(input);
    {
        LoadScope validation(builder.options.trace, LoadEvent::Validation);
        auto      valid = builder.reader.Validate();
        if (valid.is_err()) return Err(valid.unwrap_err());
    }
    auto materials = builder.materials();
    if (materials.is_err()) return Err(materials.unwrap_err());
    auto geometry = builder.geometry();
    if (geometry.is_err()) return Err(geometry.unwrap_err());
    auto scene = builder.scene();
    if (scene.is_err()) return Err(scene.unwrap_err());
    auto animations = builder.animations();
    if (animations.is_err()) return Err(animations.unwrap_err());
    auto images = builder.images();
    if (images.is_err()) return Err(images.unwrap_err());
    for (const auto& diagnostic : input.diagnostics())
        builder.asset->diagnostics.push(
            { diagnostic.location.clone(), diagnostic.message.clone() });
    builder.asset->source_metadata = Some(input.document().metadata_owner());
    builder.asset->source          = String::make(builder.source);
    static rstd::sync::atomic::Atomic<u64> next { u64(1) };
    builder.asset->generation = next.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed);
    return Ok(rstd::move(builder.asset));
}
auto LoadAsset(LoadInput input) -> Result<Arc<Asset>, Error> {
    LoadScope scope(input.context.options.trace, LoadEvent::Asset);
    auto      resolved = ResolveResources(rstd::move(input));
    if (resolved.is_err()) return Err(resolved.unwrap_err());
    return BuildAsset(resolved.unwrap());
}
auto LoadAsset(ref<str> path, LoadOptions options) -> Result<Arc<Asset>, Error> {
    LoadScope scope(options.trace, LoadEvent::Asset);
    auto      document = LoadDocument(path, options);
    if (document.is_err()) return Err(document.unwrap_err());
    auto resolved = ResolveResources(document.unwrap());
    if (resolved.is_err()) return Err(resolved.unwrap_err());
    return BuildAsset(resolved.unwrap());
}
auto LoadEnvironment(ref<str> path) -> Result<HdrImage, Error> {
    auto filename = rstd::ffi::CString::make(Vec<u8>::from(path.as_bytes()));
    if (filename.is_err())
        return Err(Error {
            ErrorKind::Io, String::make(path), "environment"_Str, "invalid environment path"_Str });
    int           width {}, height {}, channels {};
    DecodedPixels pixels { stbi_loadf(filename->as_ptr(), &width, &height, &channels, 4) };
    if (! pixels.data)
        return Err(Error { ErrorKind::Decode,
                           String::make(path),
                           "environment"_Str,
                           load_text(stbi_failure_reason()) });
    if (width <= 0 || height <= 0 || rstd::size_t(width) > 67108864u / rstd::size_t(height))
        return Err(Error { ErrorKind::Limit,
                           String::make(path),
                           "environment"_Str,
                           "environment exceeds pixel limit"_Str });
    HdrImage image;
    image.width  = unsigned(width);
    image.height = unsigned(height);
    image.rgba   = Vec<float>::from(slice<float>::from_raw_parts(
        static_cast<float*>(pixels.data), usize(rstd::size_t(width) * height * 4)));
    return Ok(rstd::move(image));
}
} // namespace vecgltf
