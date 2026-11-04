export module vecgltf.asset;
export import vecgltf.math;
export import rstd;
export import vecgltf.metadata;

export namespace vecgltf
{
using namespace rstd::prelude;
using rstd::sync::Arc;
inline constexpr unsigned invalid_index = ~0u;
template<class T>
auto& at(Vec<T>& items, rstd::size_t index) {
    return items[usize(index)];
}
template<class T>
const auto& at(const Vec<T>& items, rstd::size_t index) {
    return items[usize(index)];
}
template<class T>
auto count(const Vec<T>& items) {
    return items.len().to_primitive();
}

enum class ErrorKind
{
    Io,
    Parse,
    InvalidAsset,
    Unsupported,
    Decode,
    Limit,
    Device,
    Busy,
    Pending
};
struct Error {
    ErrorKind                            kind;
    String                               source, location, message;
    Option<Arc<dyn<rstd::error::Error>>> cause;
};
struct Diagnostic {
    String location, message;
};
struct AssetId {
    unsigned index { invalid_index };
    u64      generation {};
};
struct Vertex {
    Vec3     position, normal;
    Vec4     tangent { 1, 0, 0, 1 };
    Vec2     uv0, uv1;
    Vec4     color { 1, 1, 1, 1 };
    unsigned joints[4] {};
    Vec4     weights {};
};
struct MorphTarget {
    Vec<Vec3>      positions, normals, tangents;
    SourceObjectId source;
};
struct TexcoordSet {
    unsigned  index {};
    Vec<Vec2> coordinates;
};
struct Primitive {
    Vec<Vertex>      vertices;
    Vec<unsigned>    indices;
    Vec<MorphTarget> morphs;
    unsigned         material {};
    bool             indexed {}, skin_attributes {};
    Bounds           bounds;
    Vec<TexcoordSet> extra_texcoords;
    SourceObjectId   source;
    auto             texcoord(unsigned set, unsigned vertex) const -> Option<Vec2> {
        if (vertex >= count(vertices)) return None();
        if (set == 0) return Some(Vec2(at(vertices, vertex).uv0));
        if (set == 1) return Some(Vec2(at(vertices, vertex).uv1));
        for (const auto& stream : extra_texcoords)
            if (stream.index == set && stream.coordinates.len() == vertices.len())
                return Some(Vec2(at(stream.coordinates, vertex)));
        return None();
    }
};
struct Mesh {
    String         name;
    Vec<Primitive> primitives;
    Vec<float>     weights;
    SourceObjectId source;
};
struct Image {
    String         name;
    unsigned       width {}, height {};
    Vec<u8>        rgba;
    SourceObjectId source;
};
enum class Wrap
{
    Repeat,
    Mirror,
    Clamp
};
enum class Filter
{
    Nearest,
    Linear
};
enum class MipFilter
{
    None,
    Nearest,
    Linear
};
struct Sampler {
    Wrap           s { Wrap::Repeat }, t { Wrap::Repeat };
    Filter         min { Filter::Linear }, mag { Filter::Linear };
    MipFilter      mip { MipFilter::Linear };
    SourceObjectId source;
};
struct Texture {
    unsigned       image { invalid_index };
    Sampler        sampler;
    SourceObjectId source;
};
struct TextureBinding {
    unsigned texture { invalid_index }, uv {};
    Vec2     offset {}, scale { 1, 1 };
    float    rotation {}, strength { 1 };
};
enum class AlphaMode
{
    Opaque,
    Mask,
    Blend
};
enum class MaterialWorkflow
{
    MetallicRoughness,
    SpecularGlossiness
};
struct Material {
    String           name;
    Vec4             base_color { 1, 1, 1, 1 };
    Vec3             emissive {}, specular { 1, 1, 1 };
    float            metallic { 1 }, roughness { 1 }, glossiness { 1 }, alpha_cutoff { 0.5f };
    TextureBinding   base, physical, normal, occlusion, emission;
    MaterialWorkflow workflow { MaterialWorkflow::MetallicRoughness };
    AlphaMode        alpha { AlphaMode::Opaque };
    bool             double_sided {}, unlit {};
    SourceObjectId   source;
};
struct Node {
    String   name;
    unsigned parent { invalid_index }, mesh { invalid_index }, skin { invalid_index },
        camera { invalid_index };
    Vec<unsigned>  children;
    Vec3           translation {}, scale { 1, 1, 1 };
    Vec4           rotation { 0, 0, 0, 1 };
    Mat4           matrix { Mat4::Identity() };
    bool           has_matrix {};
    Vec<float>     weights;
    SourceObjectId source;
};
struct Scene {
    String         name;
    Vec<unsigned>  roots;
    SourceObjectId source;
};
struct Skin {
    String         name;
    Vec<unsigned>  joints;
    Vec<Mat4>      inverse_bind;
    unsigned       skeleton { invalid_index };
    SourceObjectId source;
};
struct CameraProjection {
    bool  orthographic {};
    float yfov { 0.785398f }, aspect {}, near { 0.01f }, far {}, xmag { 1 }, ymag { 1 };
};
struct Camera : CameraProjection {
    String         name;
    SourceObjectId source;
};
enum class Interpolation
{
    Step,
    Linear,
    CubicSpline
};
enum class AnimationPath
{
    Translation,
    Rotation,
    Scale,
    Weights
};
struct AnimationSampler {
    Interpolation  interpolation { Interpolation::Linear };
    Vec<float>     times, values;
    unsigned       components {};
    SourceObjectId source;
};
struct AnimationChannel {
    unsigned       node {}, sampler {};
    AnimationPath  path;
    SourceObjectId source;
};
struct Animation {
    String                name;
    Vec<AnimationSampler> samplers;
    Vec<AnimationChannel> channels;
    float                 start {}, end {};
    SourceObjectId        source;
};
struct Asset {
    u64                   generation {};
    String                source;
    unsigned              default_scene {};
    Vec<Mesh>             meshes;
    Vec<Material>         materials;
    Vec<Image>            images;
    Vec<Texture>          textures;
    Vec<Node>             nodes;
    Vec<Scene>            scenes;
    Vec<Skin>             skins;
    Vec<Camera>           cameras;
    Vec<Animation>        animations;
    Vec<Diagnostic>       diagnostics;
    Option<MetadataOwner> source_metadata;
    auto                  metadata() const -> Option<MetadataOwner> {
        return source_metadata ? Some(source_metadata->clone()) : None();
    }
    auto metadata(SourceObjectId id) const -> Option<MetadataView> {
        return source_metadata ? source_metadata->view(id) : None();
    }
    AssetId node_id(unsigned index) const { return { index, generation }; }
    auto    node(AssetId id) const -> Option<ref<Node>> {
        if (id.generation != generation || id.index >= count(nodes)) return None();
        return Some(ref<Node>::from_raw_parts(&at(nodes, id.index)));
    }
};
} // namespace vecgltf
