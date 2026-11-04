export module vecgltf.metadata;
export import rstd;
export import rstd.json;

export namespace vecgltf
{
using namespace rstd::prelude;
using rstd::sync::Arc;

enum class SourceObjectKind
{
    Document,
    Asset,
    Buffer,
    BufferView,
    Accessor,
    Sparse,
    SparseIndices,
    SparseValues,
    Image,
    Sampler,
    Texture,
    Material,
    TextureInfo,
    Mesh,
    Primitive,
    MorphTarget,
    Node,
    Scene,
    Skin,
    Camera,
    Animation,
    AnimationSampler,
    AnimationChannel,
    PbrMetallicRoughness,
    Perspective,
    Orthographic,
    AnimationTarget
};
struct SourceObjectId {
    unsigned    index { ~0u };
    u64         document {};
    friend bool operator==(const SourceObjectId&, const SourceObjectId&) = default;
};
struct SourceMetadata {
    SourceObjectKind          kind {};
    unsigned                  index {}, parent { ~0u };
    String                    name;
    Option<rstd::json::Value> extras, extensions;
};
struct AssetMetadata {
    String         version, min_version, generator, copyright;
    SourceObjectId source;
};
struct MetadataStore {
    u64                 identity {};
    AssetMetadata       asset;
    Vec<SourceMetadata> records;
};
class MetadataView {
    Arc<MetadataStore> owner_;
    unsigned           index_ {};

public:
    MetadataView(Arc<MetadataStore> owner, unsigned index)
        : owner_(rstd::move(owner)), index_(index) {}
    MetadataView(const MetadataView& other): owner_(other.owner_.clone()), index_(other.index_) {}
    MetadataView(MetadataView&&) = default;
    const SourceMetadata& value() const { return owner_->records[usize(index_)]; }
    const SourceMetadata* operator->() const { return &value(); }
    SourceObjectId        id() const { return { index_, owner_->identity }; }
};
inline auto metadata_view(Arc<MetadataStore> owner, SourceObjectId id) -> Option<MetadataView> {
    if (id.document != owner->identity || usize(id.index) >= owner->records.len()) return None();
    return Some(MetadataView(rstd::move(owner), id.index));
}
class MetadataOwner {
    Arc<MetadataStore> owner_;

public:
    explicit MetadataOwner(Arc<MetadataStore> owner): owner_(rstd::move(owner)) {}
    MetadataOwner(const MetadataOwner& other): owner_(other.owner_.clone()) {}
    MetadataOwner(MetadataOwner&&)                                    = default;
    auto                 operator=(MetadataOwner&&) -> MetadataOwner& = default;
    const MetadataStore& value() const { return *owner_; }
    const MetadataStore* operator->() const { return &value(); }
    auto                 clone() const -> MetadataOwner { return MetadataOwner(owner_.clone()); }
    auto                 view(SourceObjectId id) const -> Option<MetadataView> {
        return metadata_view(owner_.clone(), id);
    }
};
} // namespace vecgltf
