export module vecgltf.asset;
export import vecgltf.math;
export import rstd;

export namespace vecgltf {
using namespace rstd::prelude;
using rstd::sync::Arc;
inline constexpr unsigned invalid_index = ~0u;
template <class T> auto &at(Vec<T> &items, rstd::size_t index) {
    return items[usize(index)];
}
template <class T> const auto &at(const Vec<T> &items, rstd::size_t index) {
    return items[usize(index)];
}
template <class T> auto count(const Vec<T> &items) {
    return items.len().to_primitive();
}

enum class ErrorKind {
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
    ErrorKind kind;
    String source, location, message;
};
struct Diagnostic {
    String location, message;
};
struct AssetId {
    unsigned index{invalid_index};
    u64 generation{};
};
struct Vertex {
    Vec3 position, normal;
    Vec4 tangent{1, 0, 0, 1};
    Vec2 uv0, uv1;
    Vec4 color{1, 1, 1, 1};
    unsigned joints[4]{};
    Vec4 weights{};
};
struct MorphTarget {
    Vec<Vec3> positions, normals, tangents;
};
struct Primitive {
    Vec<Vertex> vertices;
    Vec<unsigned> indices;
    Vec<MorphTarget> morphs;
    unsigned material{};
    bool indexed{}, skin_attributes{};
    Bounds bounds;
};
struct Mesh {
    String name;
    Vec<Primitive> primitives;
    Vec<float> weights;
};
struct Image {
    String name;
    unsigned width{}, height{};
    Vec<u8> rgba;
};
enum class Wrap { Repeat, Mirror, Clamp };
enum class Filter { Nearest, Linear };
enum class MipFilter { None, Nearest, Linear };
struct Sampler {
    Wrap s{Wrap::Repeat}, t{Wrap::Repeat};
    Filter min{Filter::Linear}, mag{Filter::Linear};
    MipFilter mip{MipFilter::Linear};
};
struct Texture {
    unsigned image{invalid_index};
    Sampler sampler;
};
struct TextureBinding {
    unsigned texture{invalid_index}, uv{};
    Vec2 offset{}, scale{1, 1};
    float rotation{}, strength{1};
};
enum class AlphaMode { Opaque, Mask, Blend };
struct Material {
    String name;
    Vec4 base_color{1, 1, 1, 1};
    Vec3 emissive{};
    float metallic{1}, roughness{1}, alpha_cutoff{0.5f};
    TextureBinding base, physical, normal, occlusion, emission;
    AlphaMode alpha{AlphaMode::Opaque};
    bool double_sided{}, unlit{};
};
struct Node {
    String name;
    unsigned parent{invalid_index}, mesh{invalid_index}, skin{invalid_index},
        camera{invalid_index};
    Vec<unsigned> children;
    Vec3 translation{}, scale{1, 1, 1};
    Vec4 rotation{0, 0, 0, 1};
    Mat4 matrix{Mat4::Identity()};
    bool has_matrix{};
    Vec<float> weights;
};
struct Scene {
    String name;
    Vec<unsigned> roots;
};
struct Skin {
    String name;
    Vec<unsigned> joints;
    Vec<Mat4> inverse_bind;
};
struct Camera {
    String name;
    bool orthographic{};
    float yfov{0.785398f}, aspect{}, near{0.01f}, far{}, xmag{1}, ymag{1};
};
enum class Interpolation { Step, Linear, CubicSpline };
enum class AnimationPath { Translation, Rotation, Scale, Weights };
struct AnimationSampler {
    Interpolation interpolation{Interpolation::Linear};
    Vec<float> times, values;
    unsigned components{};
};
struct AnimationChannel {
    unsigned node{}, sampler{};
    AnimationPath path;
};
struct Animation {
    String name;
    Vec<AnimationSampler> samplers;
    Vec<AnimationChannel> channels;
    float start{}, end{};
};
struct Asset {
    u64 generation{};
    String source;
    unsigned default_scene{};
    Vec<Mesh> meshes;
    Vec<Material> materials;
    Vec<Image> images;
    Vec<Texture> textures;
    Vec<Node> nodes;
    Vec<Scene> scenes;
    Vec<Skin> skins;
    Vec<Camera> cameras;
    Vec<Animation> animations;
    Vec<Diagnostic> diagnostics;
    AssetId node_id(unsigned index) const { return {index, generation}; }
    auto node(AssetId id) const -> Option<ref<Node>> {
        if (id.generation != generation || id.index >= count(nodes))
            return None();
        return Some(ref<Node>::from_raw_parts(&at(nodes, id.index)));
    }
};
} // namespace vecgltf
