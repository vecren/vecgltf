module;
#include "../../third_party/cgltf.h"
#include "../../third_party/stb_image.h"
#include <stdlib.h>
#include <string.h>
module vecgltf.load;
import rstd;

namespace vecgltf
{
using namespace rstd::literals;
namespace
{
String text(const char* value) {
    return String::make(value ? rstd::ffi::CStr::from_ptr(value).to_str().unwrap_or(""_str)
                              : ""_str);
}
Vec<char> terminated(ref<str> value) {
    Vec<char> result;
    for (auto byte : value.as_bytes()) result.push(char(byte.to_primitive()));
    result.push('\0');
    return result;
}
struct Parsed {
    cgltf_data* data {};
    ~Parsed() { cgltf_free(data); }
};
struct Pixels {
    void* data {};
    ~Pixels() { stbi_image_free(data); }
};
struct Allocation {
    void* data {};
    ~Allocation() { free(data); }
};
struct Loader {
    ref<str>          source;
    LoadOptions       options;
    const cgltf_data& data;
    Arc<Asset>        asset { Arc<Asset>::make() };
    usize             vertices {};

    Error error(ErrorKind kind, String location, const char* message) const {
        return { kind, String::make(source), rstd::move(location), text(message) };
    }
    auto floats(const cgltf_accessor* accessor, unsigned components, String location)
        -> Result<Vec<float>, Error> {
        if (! accessor || cgltf_num_components(accessor->type) != components)
            return Err(
                error(ErrorKind::InvalidAsset, rstd::move(location), "unexpected accessor shape"));
        if (accessor->count > options.max_vertices.to_primitive() * 16 ||
            accessor->count > ~rstd::size_t(0) / components)
            return Err(
                error(ErrorKind::Limit, rstd::move(location), "accessor exceeds load limit"));
        Vec<float> result;
        result.resize(usize(accessor->count * components), 0.0f);
        auto unpacked = cgltf_accessor_unpack_floats(accessor, result.begin(), count(result));
        if (unpacked != count(result))
            return Err(
                error(ErrorKind::InvalidAsset, rstd::move(location), "cannot unpack accessor"));
        for (float value : result)
            if (! rstd::f32(value).is_finite())
                return Err(error(
                    ErrorKind::InvalidAsset, rstd::move(location), "non-finite accessor value"));
        return Ok(rstd::move(result));
    }
    auto attribute(const cgltf_attribute* attributes,
                   cgltf_size             size,
                   cgltf_attribute_type   type,
                   unsigned               set = 0) const -> const cgltf_accessor* {
        for (cgltf_size i = 0; i < size; ++i)
            if (attributes[i].type == type && attributes[i].index == int(set))
                return attributes[i].data;
        return nullptr;
    }
    auto binding(const cgltf_texture_view& view, String location) -> Result<TextureBinding, Error> {
        TextureBinding result;
        if (! view.texture) return Ok(result);
        result.texture  = unsigned(view.texture - data.textures);
        result.uv       = unsigned(view.texcoord);
        result.strength = view.scale;
        if (view.has_transform) {
            const auto& t   = view.transform;
            result.offset   = { t.offset[0], t.offset[1] };
            result.scale    = { t.scale[0], t.scale[1] };
            result.rotation = t.rotation;
            if (t.has_texcoord) result.uv = unsigned(t.texcoord);
        }
        if (result.uv > 1)
            return Err(error(ErrorKind::Unsupported,
                             rstd::move(location),
                             "only TEXCOORD_0 and TEXCOORD_1 are supported"));
        return Ok(result);
    }
    auto materials() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Materials);
        for (cgltf_size i = 0; i < data.materials_count; ++i) {
            const auto& m = data.materials[i];
            const auto& p = m.pbr_metallic_roughness;
            Material    result;
            result.name         = text(m.name);
            result.double_sided = m.double_sided;
            result.unlit        = m.unlit;
            result.base_color   = { p.base_color_factor[0],
                                    p.base_color_factor[1],
                                    p.base_color_factor[2],
                                    p.base_color_factor[3] };
            result.metallic     = p.metallic_factor;
            result.roughness    = p.roughness_factor;
            result.emissive = { m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2] };
            result.alpha_cutoff = m.alpha_cutoff;
            result.alpha        = m.alpha_mode == cgltf_alpha_mode_blend  ? AlphaMode::Blend
                                  : m.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask
                                                                          : AlphaMode::Opaque;
            const cgltf_texture_view* inputs[] { &p.base_color_texture,
                                                 &p.metallic_roughness_texture,
                                                 &m.normal_texture,
                                                 &m.occlusion_texture,
                                                 &m.emissive_texture };
            TextureBinding*           outputs[] {
                &result.base, &result.physical, &result.normal, &result.occlusion, &result.emission
            };
            for (unsigned slot = 0; slot < 5; ++slot) {
                auto value =
                    binding(*inputs[slot], rstd::format("materials[{}].textures[{}]", i, slot));
                if (value.is_err()) return Err(value.unwrap_err());
                *outputs[slot] = value.unwrap();
            }
            asset->materials.push(rstd::move(result));
        }
        asset->materials.push(Material {});
        return Ok(empty {});
    }
    auto images() -> Result<empty, Error> {
        LoadScope     scope(options.trace, LoadEvent::Images);
        Vec<unsigned> image_indices;
        image_indices.resize(usize(data.images_count), invalid_index);
        for (cgltf_size i = 0; i < data.images_count; ++i) {
            bool used = false;
            for (cgltf_size t = 0; t < data.textures_count; ++t)
                used = used || data.textures[t].image == &data.images[i];
            if (! used) continue;
            const auto& input = data.images[i];
            int         width {}, height {}, channels {};
            Pixels      pixels;
            LoadScope   decode(options.trace, LoadEvent::ImageDecode);
            if (input.buffer_view) {
                auto bytes = cgltf_buffer_view_data(input.buffer_view);
                if (! bytes || input.buffer_view->size > 0x7fffffff)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("images[{}]", i),
                                     "invalid embedded image"));
                pixels.data = stbi_load_from_memory(
                    bytes, int(input.buffer_view->size), &width, &height, &channels, 4);
            } else if (input.uri && strncmp(input.uri, "data:", 5) == 0) {
                const char* comma = strchr(input.uri, ',');
                if (! comma || comma - input.uri < 7 || strncmp(comma - 7, ";base64", 7) != 0)
                    return Err(error(ErrorKind::Unsupported,
                                     rstd::format("images[{}]", i),
                                     "image data URI must be base64"));
                const char* encoded = comma + 1;
                auto        length  = strlen(encoded);
                if (! length || length % 4)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("images[{}]", i),
                                     "invalid image base64 length"));
                auto size =
                    length / 4 * 3 - (encoded[length - 1] == '=') - (encoded[length - 2] == '=');
                Allocation    bytes;
                cgltf_options opts {};
                if (size > 0x7fffffff ||
                    cgltf_load_buffer_base64(&opts, size, encoded, &bytes.data) !=
                        cgltf_result_success)
                    return Err(error(ErrorKind::Decode,
                                     rstd::format("images[{}]", i),
                                     "cannot decode image data URI"));
                pixels.data = stbi_load_from_memory(static_cast<const unsigned char*>(bytes.data),
                                                    int(size),
                                                    &width,
                                                    &height,
                                                    &channels,
                                                    4);
            } else if (input.uri) {
                auto uri = terminated(text(input.uri).as_str());
                cgltf_decode_uri(uri.begin());
                if (strstr(uri.begin(), "://"))
                    return Err(error(ErrorKind::Unsupported,
                                     rstd::format("images[{}]", i),
                                     "network image URIs are unsupported"));
                auto path       = rstd::path::PathBuf::from(String::make(source));
                auto parent     = path.as_path().parent();
                auto folder     = parent.is_some() ? rstd::path::PathBuf::from(*parent)
                                                   : rstd::path::PathBuf::from("."_str);
                auto relative   = rstd::path::PathBuf::from(text(uri.begin()));
                auto image_path = folder.join(relative.as_path()).as_path().to_string_lossy();
                auto cpath      = terminated(image_path.as_str());
                pixels.data     = stbi_load(cpath.begin(), &width, &height, &channels, 4);
            } else {
                return Err(error(ErrorKind::Unsupported,
                                 rstd::format("images[{}]", i),
                                 "image has no supported source"));
            }
            if (! pixels.data)
                return Err(
                    error(ErrorKind::Decode, rstd::format("images[{}]", i), stbi_failure_reason()));
            decode.finish();
            if (width <= 0 || height <= 0 ||
                rstd::size_t(width) >
                    options.max_image_pixels.to_primitive() / rstd::size_t(height))
                return Err(error(
                    ErrorKind::Limit, rstd::format("images[{}]", i), "image exceeds pixel limit"));
            Image output;
            output.name   = text(input.name);
            output.width  = unsigned(width);
            output.height = unsigned(height);
            LoadScope copy(options.trace, LoadEvent::ImageCopy);
            output.rgba = Vec<u8>::from(slice<u8>::from_raw_parts(
                static_cast<const byte*>(pixels.data), usize(rstd::size_t(width) * height * 4)));
            copy.finish();
            at(image_indices, i) = unsigned(count(asset->images));
            asset->images.push(rstd::move(output));
        }
        for (cgltf_size i = 0; i < data.textures_count; ++i) {
            const auto& input = data.textures[i];
            if (! input.image)
                return Err(error(ErrorKind::Unsupported,
                                 rstd::format("textures[{}]", i),
                                 "texture has no supported core image"));
            Texture output;
            output.image = at(image_indices, unsigned(input.image - data.images));
            if (input.sampler) {
                const auto& s    = *input.sampler;
                auto        wrap = [](cgltf_wrap_mode mode) {
                    return mode == cgltf_wrap_mode_clamp_to_edge     ? Wrap::Clamp
                           : mode == cgltf_wrap_mode_mirrored_repeat ? Wrap::Mirror
                                                                     : Wrap::Repeat;
                };
                output.sampler.s = wrap(s.wrap_s);
                output.sampler.t = wrap(s.wrap_t);
                output.sampler.mag =
                    s.mag_filter == cgltf_filter_type_nearest ? Filter::Nearest : Filter::Linear;
                output.sampler.min =
                    s.min_filter == cgltf_filter_type_nearest ||
                            s.min_filter == cgltf_filter_type_nearest_mipmap_nearest ||
                            s.min_filter == cgltf_filter_type_nearest_mipmap_linear
                        ? Filter::Nearest
                        : Filter::Linear;
                output.sampler.mip = s.min_filter == cgltf_filter_type_nearest ||
                                             s.min_filter == cgltf_filter_type_linear
                                         ? MipFilter::None
                                     : s.min_filter == cgltf_filter_type_nearest_mipmap_nearest ||
                                             s.min_filter == cgltf_filter_type_linear_mipmap_nearest
                                         ? MipFilter::Nearest
                                         : MipFilter::Linear;
            }
            asset->textures.push(rstd::move(output));
        }
        return Ok(empty {});
    }
    auto primitive(const cgltf_primitive& input, unsigned mesh_index, unsigned primitive_index)
        -> Result<Primitive, Error> {
        auto location = rstd::format("meshes[{}].primitives[{}]", mesh_index, primitive_index);
        if (input.type != cgltf_primitive_type_triangles &&
            input.type != cgltf_primitive_type_triangle_strip &&
            input.type != cgltf_primitive_type_triangle_fan)
            return Err(error(ErrorKind::Unsupported,
                             rstd::move(location),
                             "only triangle topologies are supported"));
        const auto* position =
            attribute(input.attributes, input.attributes_count, cgltf_attribute_type_position);
        if (! position ||
            position->count > options.max_vertices.to_primitive() - vertices.to_primitive())
            return Err(error(ErrorKind::Limit,
                             rstd::move(location),
                             "missing positions or vertex limit exceeded"));
        if (input.has_draco_mesh_compression && ! position->buffer_view && ! position->is_sparse)
            return Err(error(ErrorKind::Unsupported,
                             location.clone(),
                             "Draco primitive has no core geometry fallback"));
        vertices += usize(position->count);
        Primitive result;
        result.material = input.material ? unsigned(input.material - data.materials)
                                         : unsigned(data.materials_count);
        result.vertices.resize(usize(position->count), Vertex {});
        bool has_normal {}, has_tangent {}, has_joints {}, has_weights {};
        for (cgltf_size a = 0; a < input.attributes_count; ++a) {
            const auto& attr       = input.attributes[a];
            unsigned    components = unsigned(cgltf_num_components(attr.data->type));
            if (attr.data->count != position->count)
                return Err(
                    error(ErrorKind::InvalidAsset, location.clone(), "attribute counts differ"));
            if (attr.index > 1 || (attr.index != 0 && attr.type != cgltf_attribute_type_texcoord))
                return Err(
                    error(ErrorKind::Unsupported, location.clone(), "unsupported attribute set"));
            auto values = floats(attr.data, components, location.clone());
            if (values.is_err()) return Err(values.unwrap_err());
            auto unpacked = values.unwrap();
            for (cgltf_size v = 0; v < position->count; ++v) {
                auto&        vertex = at(result.vertices, v);
                const float* f      = unpacked.begin() + v * components;
                switch (attr.type) {
                case cgltf_attribute_type_position:
                    if (components != 3)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "position must be VEC3"));
                    vertex.position = { f[0], f[1], f[2] };
                    result.bounds.include(vertex.position);
                    break;
                case cgltf_attribute_type_normal:
                    if (components != 3)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "normal must be VEC3"));
                    vertex.normal = { f[0], f[1], f[2] };
                    has_normal    = true;
                    break;
                case cgltf_attribute_type_tangent:
                    if (components != 4)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "tangent must be VEC4"));
                    vertex.tangent = { f[0], f[1], f[2], f[3] };
                    has_tangent    = true;
                    break;
                case cgltf_attribute_type_texcoord:
                    if (components != 2)
                        return Err(error(
                            ErrorKind::InvalidAsset, location.clone(), "texcoord must be VEC2"));
                    (attr.index == 0 ? vertex.uv0 : vertex.uv1) = { f[0], f[1] };
                    break;
                case cgltf_attribute_type_color:
                    if (components != 3 && components != 4)
                        return Err(error(ErrorKind::InvalidAsset,
                                         location.clone(),
                                         "color must be VEC3 or VEC4"));
                    vertex.color = { f[0], f[1], f[2], components == 4 ? f[3] : 1 };
                    break;
                case cgltf_attribute_type_joints:
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
                case cgltf_attribute_type_weights: {
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
        result.skin_attributes = has_joints;
        Vec<unsigned> indices;
        if (input.indices) {
            if (input.indices->is_sparse)
                return Err(error(
                    ErrorKind::Unsupported, location.clone(), "sparse indices are unsupported"));
            if (input.indices->count > options.max_vertices.to_primitive() * 12)
                return Err(error(ErrorKind::Limit, location.clone(), "index limit exceeded"));
            indices.resize(usize(input.indices->count), 0u);
            if (cgltf_accessor_unpack_indices(
                    input.indices, indices.begin(), sizeof(unsigned), count(indices)) !=
                count(indices))
                return Err(
                    error(ErrorKind::InvalidAsset, location.clone(), "cannot unpack indices"));
        } else
            for (unsigned i = 0; i < position->count; ++i) indices.push(unsigned(i));
        for (auto index : indices)
            if (index >= position->count)
                return Err(
                    error(ErrorKind::InvalidAsset, location.clone(), "index outside vertex range"));
        result.indexed = input.indices || input.type != cgltf_primitive_type_triangles;
        if (input.type == cgltf_primitive_type_triangles) {
            if (count(indices) % 3)
                return Err(error(ErrorKind::InvalidAsset,
                                 location.clone(),
                                 "triangle count is not divisible by three"));
            result.indices = rstd::move(indices);
        } else
            for (rstd::size_t i = 2; i < count(indices); ++i) {
                result.indices.push(unsigned(input.type == cgltf_primitive_type_triangle_fan
                                                 ? at(indices, 0)
                                                 : at(indices, i - (i % 2 ? 1 : 2))));
                result.indices.push(unsigned(input.type == cgltf_primitive_type_triangle_fan
                                                 ? at(indices, i - 1)
                                                 : at(indices, i - (i % 2 ? 2 : 1))));
                result.indices.push(unsigned(at(indices, i)));
            }
        for (cgltf_size t = 0; t < input.targets_count; ++t) {
            MorphTarget          target;
            const auto&          src = input.targets[t];
            Vec<Vec3>*           outputs[] { &target.positions, &target.normals, &target.tangents };
            cgltf_attribute_type types[] { cgltf_attribute_type_position,
                                           cgltf_attribute_type_normal,
                                           cgltf_attribute_type_tangent };
            for (unsigned k = 0; k < 3; ++k) {
                outputs[k]->resize(usize(position->count), Vec3 {});
                auto accessor = attribute(src.attributes, src.attributes_count, types[k]);
                if (! accessor) continue;
                if (accessor->count != position->count)
                    return Err(
                        error(ErrorKind::InvalidAsset, location.clone(), "morph count differs"));
                auto read = floats(accessor, 3, location.clone());
                if (read.is_err()) return Err(read.unwrap_err());
                auto values = read.unwrap();
                for (cgltf_size v = 0; v < position->count; ++v)
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
            for (auto& morph : result.morphs) {
                MorphTarget expanded_morph;
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
                auto uv = [&](const Vertex& v) {
                    return at(asset->materials, result.material).normal.uv == 1 ? v.uv1 : v.uv0;
                };
                auto  ua = uv(va), ub = uv(vb), uc = uv(vc);
                float sx = ub.x - ua.x, sy = ub.y - ua.y, tx = uc.x - ua.x, ty = uc.y - ua.y;
                float determinant = sx * ty - tx * sy;
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
        for (cgltf_size i = 0; i < data.meshes_count; ++i) {
            Mesh mesh;
            mesh.name         = text(data.meshes[i].name);
            const auto& input = data.meshes[i];
            for (cgltf_size p = 0; p < input.primitives_count; ++p) {
                auto result = primitive(input.primitives[p], unsigned(i), unsigned(p));
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
            if (input.weights_count && input.weights_count != morph_count)
                return Err(error(ErrorKind::InvalidAsset,
                                 rstd::format("meshes[{}].weights", i),
                                 "morph weights count differs"));
            for (cgltf_size w = 0; w < morph_count; ++w)
                mesh.weights.push(input.weights_count ? float(input.weights[w]) : 0.0f);
            asset->meshes.push(rstd::move(mesh));
        }
        return Ok(empty {});
    }
    auto scene() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Scene);
        for (cgltf_size i = 0; i < data.nodes_count; ++i) {
            const auto& n = data.nodes[i];
            Node        node;
            node.name   = text(n.name);
            node.parent = n.parent ? unsigned(n.parent - data.nodes) : invalid_index;
            node.mesh   = n.mesh ? unsigned(n.mesh - data.meshes) : invalid_index;
            node.skin   = n.skin ? unsigned(n.skin - data.skins) : invalid_index;
            node.camera = n.camera ? unsigned(n.camera - data.cameras) : invalid_index;
            for (cgltf_size c = 0; c < n.children_count; ++c)
                node.children.push(unsigned(n.children[c] - data.nodes));
            node.translation = { n.translation[0], n.translation[1], n.translation[2] };
            node.scale       = { n.scale[0], n.scale[1], n.scale[2] };
            node.rotation    = { n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3] };
            node.has_matrix  = n.has_matrix;
            for (unsigned k = 0; k < 16; ++k) node.matrix.m[k] = n.matrix[k];
            if (n.mesh) {
                node.weights = at(asset->meshes, node.mesh).weights.clone();
                if (n.weights_count && n.weights_count != count(node.weights))
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("nodes[{}].weights", i),
                                     "morph weights count differs"));
                for (cgltf_size w = 0; w < n.weights_count; ++w) at(node.weights, w) = n.weights[w];
            }
            asset->nodes.push(rstd::move(node));
        }
        for (cgltf_size i = 0; i < data.scenes_count; ++i) {
            Scene result;
            result.name = text(data.scenes[i].name);
            for (cgltf_size j = 0; j < data.scenes[i].nodes_count; ++j)
                result.roots.push(unsigned(data.scenes[i].nodes[j] - data.nodes));
            asset->scenes.push(rstd::move(result));
        }
        if (asset->scenes.is_empty()) {
            Scene result;
            result.name = "Root nodes"_Str;
            for (unsigned i = 0; i < count(asset->nodes); ++i)
                if (at(asset->nodes, i).parent == invalid_index) result.roots.push(unsigned(i));
            asset->scenes.push(rstd::move(result));
        }
        asset->default_scene = data.scene ? unsigned(data.scene - data.scenes) : 0;
        for (cgltf_size i = 0; i < data.skins_count; ++i) {
            const auto& input = data.skins[i];
            Skin        skin;
            skin.name = text(input.name);
            for (cgltf_size j = 0; j < input.joints_count; ++j) {
                skin.joints.push(unsigned(input.joints[j] - data.nodes));
                skin.inverse_bind.push(Mat4::Identity());
            }
            if (input.inverse_bind_matrices) {
                if (input.inverse_bind_matrices->count != input.joints_count)
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("skins[{}]", i),
                                     "inverse bind count differs"));
                auto values = floats(input.inverse_bind_matrices, 16, rstd::format("skins[{}]", i));
                if (values.is_err()) return Err(values.unwrap_err());
                auto unpacked = values.unwrap();
                for (cgltf_size j = 0; j < input.joints_count; ++j)
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
        for (cgltf_size i = 0; i < data.cameras_count; ++i) {
            const auto& input = data.cameras[i];
            Camera      camera;
            camera.name         = text(input.name);
            camera.orthographic = input.type == cgltf_camera_type_orthographic;
            if (camera.orthographic) {
                const auto& c = input.data.orthographic;
                camera.near   = c.znear;
                camera.far    = c.zfar;
                camera.xmag   = c.xmag;
                camera.ymag   = c.ymag;
            } else {
                const auto& c = input.data.perspective;
                camera.near   = c.znear;
                camera.far    = c.has_zfar ? c.zfar : 0;
                camera.yfov   = c.yfov;
                camera.aspect = c.has_aspect_ratio ? c.aspect_ratio : 0;
            }
            asset->cameras.push(rstd::move(camera));
        }
        return Ok(empty {});
    }
    auto animations() -> Result<empty, Error> {
        LoadScope scope(options.trace, LoadEvent::Animations);
        for (cgltf_size i = 0; i < data.animations_count; ++i) {
            const auto& input = data.animations[i];
            Animation   animation;
            animation.name = text(input.name);
            for (cgltf_size s = 0; s < input.samplers_count; ++s) {
                const auto&      sampler = input.samplers[s];
                AnimationSampler output;
                output.interpolation =
                    sampler.interpolation == cgltf_interpolation_type_step ? Interpolation::Step
                    : sampler.interpolation == cgltf_interpolation_type_cubic_spline
                        ? Interpolation::CubicSpline
                        : Interpolation::Linear;
                auto times = floats(
                    sampler.input, 1, rstd::format("animations[{}].samplers[{}].input", i, s));
                auto values = floats(sampler.output,
                                     unsigned(cgltf_num_components(sampler.output->type)),
                                     rstd::format("animations[{}].samplers[{}].output", i, s));
                if (times.is_err()) return Err(times.unwrap_err());
                if (values.is_err()) return Err(values.unwrap_err());
                output.times  = times.unwrap();
                output.values = values.unwrap();
                if (output.times.is_empty())
                    return Err(error(ErrorKind::InvalidAsset,
                                     rstd::format("animations[{}]", i),
                                     "empty animation sampler"));
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
            for (cgltf_size c = 0; c < input.channels_count; ++c) {
                const auto& channel = input.channels[c];
                if (! channel.target_node) continue;
                AnimationChannel output;
                output.node    = unsigned(channel.target_node - data.nodes);
                output.sampler = unsigned(channel.sampler - input.samplers);
                switch (channel.target_path) {
                case cgltf_animation_path_type_translation:
                    output.path = AnimationPath::Translation;
                    break;
                case cgltf_animation_path_type_rotation:
                    output.path = AnimationPath::Rotation;
                    break;
                case cgltf_animation_path_type_scale: output.path = AnimationPath::Scale; break;
                case cgltf_animation_path_type_weights: output.path = AnimationPath::Weights; break;
                default:
                    return Err(error(ErrorKind::Unsupported,
                                     rstd::format("animations[{}].channels[{}]", i, c),
                                     "unknown animation target"));
                }
                const auto& node     = at(asset->nodes, output.node);
                unsigned    expected = output.path == AnimationPath::Rotation ? 4
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
} // namespace

auto LoadAsset(ref<str> path, LoadOptions options) -> Result<Arc<Asset>, Error> {
    LoadScope     scope(options.trace, LoadEvent::Asset);
    auto          filename = terminated(path);
    Parsed        parsed;
    cgltf_options opts {};
    cgltf_result  result;
    {
        LoadScope document(options.trace, LoadEvent::Document);
        result = cgltf_parse_file(&opts, filename.begin(), &parsed.data);
    }
    auto failure = [&](ErrorKind kind, const char* location, const char* message) {
        return Error { kind, String::make(path), text(location), text(message) };
    };
    if (result != cgltf_result_success)
        return Err(failure(ErrorKind::Parse, "document", "cannot parse glTF/GLB file"));
    Loader loader { path, options, *parsed.data };
    auto   supported = [](const char* ext) {
        return strcmp(ext, "KHR_materials_unlit") == 0 || strcmp(ext, "KHR_texture_transform") == 0;
    };
    for (cgltf_size i = 0; i < parsed.data->extensions_required_count; ++i)
        if (! supported(parsed.data->extensions_required[i]))
            return Err(failure(
                ErrorKind::Unsupported, "extensionsRequired", parsed.data->extensions_required[i]));
    for (cgltf_size i = 0; i < parsed.data->extensions_used_count; ++i)
        if (! supported(parsed.data->extensions_used[i]))
            loader.asset->diagnostics.push(
                { text(parsed.data->extensions_used[i]),
                  "Using core fallback for unsupported optional extension"_Str });
    {
        LoadScope buffers(options.trace, LoadEvent::Buffers);
        result = cgltf_load_buffers(&opts, parsed.data, filename.begin());
    }
    if (result != cgltf_result_success)
        return Err(failure(ErrorKind::Io, "buffers", "cannot load external or embedded buffer"));
    {
        LoadScope validation(options.trace, LoadEvent::Validation);
        result = cgltf_validate(parsed.data);
    }
    if (result != cgltf_result_success)
        return Err(failure(ErrorKind::InvalidAsset, "document", "glTF validation failed"));
    auto materials = loader.materials();
    if (materials.is_err()) return Err(materials.unwrap_err());
    auto geometry = loader.geometry();
    if (geometry.is_err()) return Err(geometry.unwrap_err());
    auto scene = loader.scene();
    if (scene.is_err()) return Err(scene.unwrap_err());
    auto animations = loader.animations();
    if (animations.is_err()) return Err(animations.unwrap_err());
    auto images = loader.images();
    if (images.is_err()) return Err(images.unwrap_err());
    static rstd::sync::atomic::Atomic<u64> next { u64(1) };
    loader.asset->generation = next.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed);
    loader.asset->source     = String::make(path);
    return Ok(rstd::move(loader.asset));
}

auto LoadEnvironment(ref<str> path) -> Result<HdrImage, Error> {
    auto   filename = terminated(path);
    int    width {}, height {}, channels {};
    Pixels pixels { stbi_loadf(filename.begin(), &width, &height, &channels, 4) };
    if (! pixels.data)
        return Err(Error { ErrorKind::Decode,
                           String::make(path),
                           "environment"_Str,
                           text(stbi_failure_reason()) });
    if (width <= 0 || height <= 0 || rstd::size_t(width) * height > 67108864)
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
