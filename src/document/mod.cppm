export module vecgltf.document;
export import vecgltf.asset;
export import vecgltf.metadata;

export namespace vecgltf
{
class DocumentBytes {
    Arc<Vec<u8>> owner_;

public:
    explicit DocumentBytes(Arc<Vec<u8>> owner): owner_(rstd::move(owner)) {}
    explicit DocumentBytes(Vec<u8> bytes): owner_(Arc<Vec<u8>>::make(rstd::move(bytes))) {}
    DocumentBytes(const DocumentBytes& other): owner_(other.owner_.clone()) {}
    DocumentBytes(DocumentBytes&&)                              = default;
    auto           operator=(DocumentBytes&&) -> DocumentBytes& = default;
    const Vec<u8>& operator*() const { return *owner_; }
    const Vec<u8>* operator->() const { return &**this; }
    auto           clone() const -> DocumentBytes { return DocumentBytes(owner_.clone()); }
    auto           bytes() const -> slice<u8> { return (**this).as_slice(); }
    auto           len() const -> usize { return (**this).len(); }
};
} // namespace vecgltf

export namespace vecgltf::document
{
enum class Shape
{
    Scalar,
    Vec2,
    Vec3,
    Vec4,
    Mat2,
    Mat3,
    Mat4
};
inline unsigned components(Shape shape) {
    switch (shape) {
    case Shape::Scalar: return 1;
    case Shape::Vec2: return 2;
    case Shape::Vec3: return 3;
    case Shape::Vec4:
    case Shape::Mat2: return 4;
    case Shape::Mat3: return 9;
    case Shape::Mat4: return 16;
    }
    return 0;
}
inline unsigned component_bytes(unsigned type) {
    switch (type) {
    case 5120:
    case 5121: return 1;
    case 5122:
    case 5123: return 2;
    case 5125:
    case 5126: return 4;
    default: return 0;
    }
}
struct Buffer {
    String         name, uri;
    u64            byte_length {};
    bool           has_uri {};
    SourceObjectId source;
};
struct BufferView {
    String         name;
    unsigned       buffer { invalid_index }, target {};
    u64            offset {}, length {}, stride {};
    SourceObjectId source;
};
struct Sparse {
    u64      count {}, indices_offset {}, values_offset {};
    unsigned indices_buffer_view { invalid_index }, values_buffer_view { invalid_index },
        indices_component_type {};
    SourceObjectId source;
};
struct Accessor {
    String         name;
    unsigned       buffer_view { invalid_index }, component_type {};
    u64            offset {}, count {};
    Shape          shape { Shape::Scalar };
    bool           normalized {};
    Option<Sparse> sparse;
    Vec<float>     minimum, maximum;
    SourceObjectId source;
};
struct Image {
    String         name, uri, mime_type;
    unsigned       buffer_view { invalid_index };
    bool           has_uri {};
    SourceObjectId source;
};
struct Sampler {
    String         name;
    unsigned       mag_filter {}, min_filter {}, wrap_s { 10497 }, wrap_t { 10497 };
    SourceObjectId source;
};
struct Texture {
    String         name;
    unsigned       image { invalid_index }, sampler { invalid_index };
    bool           has_compressed_source {};
    SourceObjectId source;
};
struct SpecularGlossiness {
    Vec4           diffuse { 1, 1, 1, 1 };
    Vec3           specular { 1, 1, 1 };
    float          glossiness { 1 };
    TextureBinding diffuse_texture, physical_texture;
};
struct Material {
    vecgltf::Material          core;
    Option<SpecularGlossiness> specular_glossiness;
    float                      emissive_strength { 1 };
    SourceObjectId             source;
};
struct Attribute {
    String   name;
    unsigned accessor { invalid_index };
};
struct MorphTarget {
    Vec<Attribute> attributes;
    SourceObjectId source;
};
struct Primitive {
    Vec<Attribute>   attributes;
    Vec<MorphTarget> targets;
    unsigned         indices { invalid_index }, material { invalid_index }, mode { 4 };
    bool             has_draco {};
    SourceObjectId   source;
};
struct Mesh {
    String         name;
    Vec<Primitive> primitives;
    Vec<float>     weights;
    SourceObjectId source;
};
struct Node {
    String   name;
    unsigned parent { invalid_index }, mesh { invalid_index }, skin { invalid_index },
        camera { invalid_index };
    Vec<unsigned>  children;
    Vec3           translation {}, scale { 1, 1, 1 };
    Vec4           rotation { 0, 0, 0, 1 };
    Mat4           matrix { Mat4::Identity() };
    bool           has_matrix {}, has_trs {};
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
    unsigned       inverse_bind { invalid_index }, skeleton { invalid_index };
    SourceObjectId source;
};
struct Camera {
    String           name;
    CameraProjection projection;
    SourceObjectId   source;
};
struct AnimationSampler {
    unsigned       input { invalid_index }, output { invalid_index };
    Interpolation  interpolation { Interpolation::Linear };
    SourceObjectId source;
};
struct AnimationChannel {
    unsigned       node { invalid_index }, sampler { invalid_index };
    AnimationPath  path { AnimationPath::Translation };
    SourceObjectId source;
};
struct Animation {
    String                name;
    Vec<AnimationSampler> samplers;
    Vec<AnimationChannel> channels;
    SourceObjectId        source;
};
struct ByteRange {
    u64 offset {}, length {};
};
struct Data {
    DocumentBytes     bytes;
    MetadataOwner     metadata;
    String            label;
    Option<ByteRange> binary;
    Vec<Buffer>       buffers;
    Vec<BufferView>   buffer_views;
    Vec<Accessor>     accessors;
    Vec<Image>        images;
    Vec<Sampler>      samplers;
    Vec<Texture>      textures;
    Vec<Material>     materials;
    Vec<Mesh>         meshes;
    Vec<Node>         nodes;
    Vec<Scene>        scenes;
    Vec<Skin>         skins;
    Vec<Camera>       cameras;
    Vec<Animation>    animations;
    Vec<String>       extensions_used, extensions_required;
    unsigned          default_scene { invalid_index };
    SourceObjectId    source;
    Data(DocumentBytes input, Arc<MetadataStore> information)
        : bytes(rstd::move(input)), metadata(rstd::move(information)) {}
};
} // namespace vecgltf::document

export namespace vecgltf
{
struct DocumentOptions {
    usize max_document_bytes { 64 * 1024 * 1024 }, max_objects { 1000000 },
        max_array_elements { 10000000 }, max_metadata_bytes { 16 * 1024 * 1024 }, max_depth { 128 };
};
class Document {
    Arc<document::Data> data_;
    explicit Document(Arc<document::Data> data): data_(rstd::move(data)) {}
    friend auto ParseDocument(DocumentBytes, DocumentOptions, ref<str>) -> Result<Document, Error>;

public:
    Document(const Document& other): data_(other.data_.clone()) {}
    Document(Document&&)                                     = default;
    auto                  operator=(Document&&) -> Document& = default;
    const document::Data& data() const { return *data_; }
    auto metadata_owner() const -> MetadataOwner { return data_->metadata.clone(); }
    auto metadata(SourceObjectId id) const -> Option<MetadataView> {
        return data_->metadata.view(id);
    }
};
auto ParseDocument(DocumentBytes bytes, DocumentOptions options = {}, ref<str> source = {})
    -> Result<Document, Error>;
inline auto ParseDocument(Arc<Vec<u8>> bytes, DocumentOptions options = {}, ref<str> source = {})
    -> Result<Document, Error> {
    return ParseDocument(DocumentBytes(rstd::move(bytes)), options, source);
}
inline auto ParseDocument(Vec<u8> bytes, DocumentOptions options = {}, ref<str> source = {})
    -> Result<Document, Error> {
    return ParseDocument(DocumentBytes(rstd::move(bytes)), options, source);
}
auto ValidateDocument(const Document& document) -> Result<empty, Error>;
} // namespace vecgltf
