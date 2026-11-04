export module vecgltf.load.resources;
export import vecgltf.document;
export import vecgltf.trace;
import licrypto;

using namespace rstd::literals;

export namespace vecgltf
{
enum class ResourceKind
{
    Document,
    Buffer,
    Image
};

class ResourceBytes {
    DocumentBytes owner_;
    usize         offset_ {}, length_ {};
    ResourceBytes(DocumentBytes owner, usize offset, usize length)
        : owner_(rstd::move(owner)), offset_(offset), length_(length) {}

public:
    explicit ResourceBytes(Vec<u8> bytes): owner_(rstd::move(bytes)), length_(owner_->len()) {}
    ResourceBytes(const ResourceBytes& other)
        : owner_(other.owner_.clone()), offset_(other.offset_), length_(other.length_) {}
    ResourceBytes(ResourceBytes&&)                           = default;
    auto        operator=(ResourceBytes&&) -> ResourceBytes& = default;
    static auto range(DocumentBytes owner, usize offset, usize length) -> Option<ResourceBytes> {
        if (offset > owner->len() || length > owner->len() - offset) return None();
        return Some(ResourceBytes(rstd::move(owner), offset, length));
    }
    static auto range(Arc<Vec<u8>> owner, usize offset, usize length) -> Option<ResourceBytes> {
        return range(DocumentBytes(rstd::move(owner)), offset, length);
    }
    auto bytes() const -> slice<u8> {
        return slice<u8>::from_raw_parts(owner_->begin() + offset_.to_primitive(), length_);
    }
    auto owner() const -> DocumentBytes { return owner_.clone(); }
    auto subrange(usize offset, usize length) const -> Option<ResourceBytes> {
        if (offset > length_ || length > length_ - offset) return None();
        return Some(ResourceBytes(owner_.clone(), offset_ + offset, length));
    }
};

struct ResourceRequest {
    ResourceKind                  kind;
    ref<str>                      source, uri;
    ref<rstd::path::Path>         path;
    Option<ref<rstd::path::Path>> root;
    usize                         max_bytes;
};
struct ResourceResolver {
    using Trait                  = ResourceResolver;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = ResourceResolver;
        auto Read(const ResourceRequest& request) const -> Result<ResourceBytes, Error> {
            return rstd::trait_call<0>(this, request);
        }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Read>;
};
class ResourceAccess {
    Option<Arc<dyn<ResourceResolver>>> owner_;

public:
    ResourceAccess() = default;
    explicit ResourceAccess(Arc<dyn<ResourceResolver>> owner): owner_(Some(rstd::move(owner))) {}
    ResourceAccess(const ResourceAccess& other)
        : owner_(other.owner_ ? Some(other.owner_->clone()) : None()) {}
    ResourceAccess(ResourceAccess&&)                    = default;
    auto operator=(ResourceAccess&&) -> ResourceAccess& = default;
    bool present() const { return owner_.is_some(); }
    auto Read(const ResourceRequest& request) const -> Result<ResourceBytes, Error> {
        return (*owner_)->Read(request);
    }
};
auto FileResources() -> ResourceAccess;
struct LoadOptions {
    usize                         max_vertices { 10000000 }, max_image_pixels { 67108864 };
    LoadTrace                     trace;
    Option<ref<rstd::path::Path>> resource_root;
    usize                         max_document_bytes { 64 * 1024 * 1024 }, max_objects { 1000000 },
        max_resource_bytes { 1024 * 1024 * 1024 }, max_metadata_bytes { 16 * 1024 * 1024 },
        max_accessor_elements { 160000000 }, max_decoded_pixels { 536870912 };
    ResourceAccess resource_resolver;
    usize          max_indices { 120000000 };
};
struct LoadContext {
    String                      source;
    Option<rstd::path::PathBuf> base_directory, root;
    LoadOptions                 options;
    bool                        file_origin {};
};
struct LoadInput {
    Document    document;
    LoadContext context;
    static auto memory(Document document, LoadOptions options = {}, ref<str> source = {})
        -> Result<LoadInput, Error>;
};
class ResolvedDocument {
    LoadInput                  input_;
    Vec<ResourceBytes>         buffers_;
    Vec<Option<ResourceBytes>> images_;
    Vec<Diagnostic>            diagnostics_;
    ResolvedDocument(LoadInput                  input,
                     Vec<ResourceBytes>         buffers,
                     Vec<Option<ResourceBytes>> images,
                     Vec<Diagnostic>            diagnostics)
        : input_(rstd::move(input)),
          buffers_(rstd::move(buffers)),
          images_(rstd::move(images)),
          diagnostics_(rstd::move(diagnostics)) {}
    friend auto ResolveResources(LoadInput) -> Result<ResolvedDocument, Error>;

public:
    ResolvedDocument(ResolvedDocument&&) = default;
    const Document&    document() const { return input_.document; }
    const LoadContext& context() const { return input_.context; }
    auto               buffer(unsigned index) const -> Option<slice<u8>> {
        return index < count(buffers_) ? Some(at(buffers_, index).bytes()) : None();
    }
    auto image(unsigned index) const -> Option<ResourceBytes> {
        return index < count(images_) && at(images_, index)
                   ? Some(ResourceBytes(*at(images_, index)))
                   : None();
    }
    const Vec<Diagnostic>& diagnostics() const { return diagnostics_; }
};
auto LoadDocument(ref<str> path, LoadOptions options = {}) -> Result<LoadInput, Error>;
auto ResolveResources(LoadInput input) -> Result<ResolvedDocument, Error>;
} // namespace vecgltf

namespace vecgltf
{
auto resource_error(ErrorKind kind, ref<str> source, ref<str> location, ref<str> message) -> Error {
    return { kind, String::make(source), String::make(location), String::make(message) };
}
bool contains_nul(ref<str> value) {
    for (auto byte : value.as_bytes())
        if (byte == u8()) return true;
    return false;
}
struct FileResourceResolver {
    auto Read(const ResourceRequest& request) const -> Result<ResourceBytes, Error> {
        auto path = rstd::path::PathBuf::from(request.path);
        if (request.root) {
            auto canonical = rstd::fs::canonicalize(path.as_path());
            if (canonical.is_err())
                return Err(resource_error(ErrorKind::Io,
                                          request.source,
                                          "resource"_str,
                                          "cannot resolve resource file"_str));
            path = canonical.unwrap();
            if (! path.as_path().starts_with(*request.root))
                return Err(resource_error(ErrorKind::Io,
                                          request.source,
                                          "resource root"_str,
                                          "resource file escapes resource root"_str));
        }
        auto opened = rstd::fs::File::open(path.as_path());
        if (opened.is_err())
            return Err(resource_error(
                ErrorKind::Io, request.source, "resource"_str, "cannot open resource file"_str));
        auto file     = opened.unwrap();
        auto metadata = file.metadata();
        if (metadata.is_err() || ! metadata->is_file())
            return Err(resource_error(ErrorKind::Io,
                                      request.source,
                                      "resource"_str,
                                      "resource is not a regular file"_str));
        auto size = metadata->len().to_primitive();
        if (size > request.max_bytes.to_primitive())
            return Err(resource_error(ErrorKind::Limit,
                                      request.source,
                                      "resource"_str,
                                      "resource exceeds byte limit"_str));
        Vec<u8> bytes;
        bytes.resize(usize(size), u8());
        auto read = file.read_exact_at(bytes.as_mut_slice().as_mut_ref(), u64());
        if (read.is_err())
            return Err(resource_error(
                ErrorKind::Io, request.source, "resource"_str, "cannot read resource file"_str));
        return Ok(ResourceBytes(rstd::move(bytes)));
    }
};
} // namespace vecgltf

namespace rstd
{
template<>
struct Impl<vecgltf::ResourceResolver, vecgltf::FileResourceResolver>
    : ImplBase<vecgltf::FileResourceResolver> {
    auto Read(const vecgltf::ResourceRequest& request) const
        -> Result<vecgltf::ResourceBytes, vecgltf::Error> {
        return this->self().Read(request);
    }
};
} // namespace rstd

namespace vecgltf
{
auto FileResources() -> ResourceAccess {
    return ResourceAccess(Arc<dyn<ResourceResolver>>::make(FileResourceResolver {}));
}
auto make_context(ref<str> source, LoadOptions options, bool file_origin)
    -> Result<LoadContext, Error> {
    if (contains_nul(source))
        return Err(resource_error(
            ErrorKind::InvalidAsset, source, "source"_str, "source contains NUL"_str));
    LoadContext context { String::make(source), {}, {}, options, file_origin };
    if (options.resource_root) {
        auto root = rstd::fs::canonicalize(*options.resource_root);
        if (root.is_err())
            return Err(resource_error(
                ErrorKind::Io, source, "resource root"_str, "cannot resolve resource root"_str));
        context.root = Some(root.unwrap());
        auto info    = rstd::fs::metadata(context.root->as_path());
        if (info.is_err() || ! info->is_dir())
            return Err(resource_error(ErrorKind::Io,
                                      source,
                                      "resource root"_str,
                                      "resource root is not a directory"_str));
    }
    context.options.resource_root = None();
    if (file_origin) {
        auto path = rstd::path::PathBuf::from(source);
        if (! path.as_path().is_absolute()) {
            auto current  = rstd::path::PathBuf::from("."_str);
            auto absolute = rstd::fs::canonicalize(current.as_path());
            if (absolute.is_err())
                return Err(resource_error(
                    ErrorKind::Io, source, "source"_str, "cannot resolve current directory"_str));
            path = absolute->join(path.as_path());
        }
        if (context.root) {
            auto canonical = rstd::fs::canonicalize(path.as_path());
            if (canonical.is_err() || ! canonical->as_path().starts_with(context.root->as_path()))
                return Err(resource_error(ErrorKind::Io,
                                          source,
                                          "resource root"_str,
                                          "document escapes resource root"_str));
            path = canonical.unwrap();
        }
        auto parent = path.as_path().parent();
        context.base_directory =
            Some(parent ? rstd::path::PathBuf::from(*parent) : rstd::path::PathBuf::from("."_str));
        if (! context.options.resource_resolver.present())
            context.options.resource_resolver = FileResources();
    }
    return Ok(rstd::move(context));
}
auto LoadInput::memory(Document document, LoadOptions options, ref<str> source)
    -> Result<LoadInput, Error> {
    if (source.is_empty()) source = document.data().label.as_str();
    auto context = make_context(source, options, false);
    if (context.is_err()) return Err(context.unwrap_err());
    return Ok(LoadInput { rstd::move(document), context.unwrap() });
}
auto LoadDocument(ref<str> path, LoadOptions options) -> Result<LoadInput, Error> {
    LoadScope scope(options.trace, LoadEvent::Document);
    auto      created = make_context(path, options, true);
    if (created.is_err()) return Err(created.unwrap_err());
    auto      context = created.unwrap();
    auto      file    = rstd::path::PathBuf::from(context.source.as_str());
    LoadScope reading(options.trace, LoadEvent::ResourceRead);
    auto      bytes = context.options.resource_resolver.Read(
        { ResourceKind::Document,
          context.source.as_str(),
          context.source.as_str(),
          file.as_path(),
          context.root ? Some(context.root->as_path()) : None(),
          options.max_document_bytes });
    if (bytes.is_err()) return Err(bytes.unwrap_err());
    reading.finish();
    // Document input always spans the returned resource, including custom resolver ranges.
    auto source_bytes = bytes->bytes();
    if (source_bytes.len() > options.max_document_bytes)
        return Err(resource_error(ErrorKind::Limit,
                                  context.source.as_str(),
                                  "document"_str,
                                  "document exceeds byte limit"_str));
    auto owner = bytes->owner();
    if (source_bytes.len() != owner->len()) owner = DocumentBytes(Vec<u8>::from(source_bytes));
    LoadScope parsing(options.trace, LoadEvent::DocumentParse);
    auto      document = ParseDocument(rstd::move(owner),
                                       { options.max_document_bytes,
                                         options.max_objects,
                                         options.max_accessor_elements,
                                         options.max_metadata_bytes },
                                       context.source.as_str());
    if (document.is_err()) return Err(document.unwrap_err());
    return Ok(LoadInput { document.unwrap(), rstd::move(context) });
}
auto decoded_uri(ref<str> uri, ref<str> source) -> Result<String, Error> {
    Vec<u8> bytes;
    auto    input = uri.as_bytes();
    bytes.reserve(input.len());
    auto hex = [](unsigned c) -> int {
        if (c >= '0' && c <= '9') return int(c - '0');
        if (c >= 'A' && c <= 'F') return int(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return int(c - 'a' + 10);
        return -1;
    };
    for (usize i {}; i < input.len(); ++i) {
        auto byte = u8(input[i]).to_primitive();
        if (byte == '%') {
            if (input.len() - i < usize(3))
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          source,
                                          "resource URI"_str,
                                          "invalid percent-encoded URI"_str));
            int a = hex(u8(input[i + usize(1)]).to_primitive());
            int b = hex(u8(input[i + usize(2)]).to_primitive());
            if (a < 0 || b < 0)
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          source,
                                          "resource URI"_str,
                                          "invalid percent-encoded URI"_str));
            byte = static_cast<rstd::uint8_t>(a * 16 + b);
            i += usize(2);
        }
        if (! byte)
            return Err(resource_error(ErrorKind::InvalidAsset,
                                      source,
                                      "resource URI"_str,
                                      "resource URI contains NUL"_str));
        bytes.push(u8(byte));
    }
    auto string = String::from_utf8(rstd::move(bytes));
    if (string.is_err())
        return Err(resource_error(
            ErrorKind::InvalidAsset, source, "resource URI"_str, "resource URI is not UTF-8"_str));
    return Ok(string.unwrap());
}
struct ResourceCacheEntry {
    String        key;
    ResourceBytes value;
};
struct ResourceLoader {
    const LoadContext&      context;
    Vec<ResourceCacheEntry> cache;
    usize                   total {};
    auto                    read(ref<str> uri, ResourceKind kind, usize expected_min_bytes = {})
        -> Result<ResourceBytes, Error> {
        for (const auto& entry : cache)
            if (entry.key == uri) return Ok(ResourceBytes(entry.value));
        LoadScope reading(context.options.trace, LoadEvent::ResourceRead);
        auto      limit = context.options.max_resource_bytes;
        if (total > limit || expected_min_bytes > limit - total)
            return Err(resource_error(ErrorKind::Limit,
                                      context.source.as_str(),
                                      "resources"_str,
                                      "resource byte limit exceeded"_str));
        auto value = [&]() -> Result<ResourceBytes, Error> {
            if (uri.starts_with("data:"_str)) {
                auto comma = uri.find(","_str);
                if (comma.is_none())
                    return Err(resource_error(ErrorKind::InvalidAsset,
                                              context.source.as_str(),
                                              "resource URI"_str,
                                              "invalid data URI"_str));
                auto header = uri.get(usize(), *comma).unwrap();
                if (! header.ends_with(";base64"_str))
                    return Err(resource_error(ErrorKind::Unsupported,
                                              context.source.as_str(),
                                              "resource URI"_str,
                                              "data URI must be base64"_str));
                auto encoded = uri.get(*comma + usize(1), uri.len()).unwrap();
                if (encoded.len() / usize(4) > (limit - total) / usize(3) + usize(1))
                    return Err(resource_error(ErrorKind::Limit,
                                              context.source.as_str(),
                                              "resource URI"_str,
                                              "data URI exceeds byte limit"_str));
                auto decoded = licrypto::base64_decode(encoded);
                if (decoded.is_err())
                    return Err(resource_error(ErrorKind::Decode,
                                              context.source.as_str(),
                                              "resource URI"_str,
                                              "invalid base64 resource"_str));
                return Ok(ResourceBytes(decoded.unwrap()));
            }
            auto decoded = decoded_uri(uri, context.source.as_str());
            if (decoded.is_err()) return Err(decoded.unwrap_err());
            auto path = rstd::path::PathBuf::from(decoded->as_str());
            auto text = decoded->as_str();
            if (text.find("://"_str).is_some() ||
                (context.root &&
                 (path.as_path().is_absolute() || text.find("\\"_str).is_some() ||
                  (text.find(":"_str).is_some() &&
                   (text.find("/"_str).is_none() || *text.find(":"_str) < *text.find("/"_str))))))
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          context.source.as_str(),
                                          "resource URI"_str,
                                          "unsupported resource URI"_str));
            if (! context.options.resource_resolver.present())
                return Err(resource_error(ErrorKind::Unsupported,
                                          context.source.as_str(),
                                          "resource URI"_str,
                                          "external resource needs a resolver"_str));
            if (context.base_directory) path = context.base_directory->join(path.as_path());
            return context.options.resource_resolver.Read(
                { kind,
                  context.source.as_str(),
                  text,
                  path.as_path(),
                  context.root ? Some(context.root->as_path()) : None(),
                  limit - total });
        }();
        if (value.is_err()) return Err(value.unwrap_err());
        auto result = value.unwrap();
        if (result.bytes().len() > limit - total)
            return Err(resource_error(ErrorKind::Limit,
                                      context.source.as_str(),
                                      "resources"_str,
                                      "resource byte limit exceeded"_str));
        total += result.bytes().len();
        cache.push({ String::make(uri), ResourceBytes(result) });
        return Ok(rstd::move(result));
    }
};
auto ResolveResources(LoadInput input) -> Result<ResolvedDocument, Error> {
    const auto& data      = input.document.data();
    const auto& context   = input.context;
    auto        supported = [](ref<str> name) {
        return name == "KHR_materials_unlit"_str || name == "KHR_texture_transform"_str ||
               name == "KHR_materials_pbrSpecularGlossiness"_str ||
               name == "KHR_materials_emissive_strength"_str;
    };
    Vec<Diagnostic> diagnostics;
    for (const auto& required : data.extensions_required)
        if (! supported(required.as_str()))
            return Err(resource_error(ErrorKind::Unsupported,
                                      context.source.as_str(),
                                      "extensionsRequired"_str,
                                      required.as_str()));
    for (const auto& used : data.extensions_used)
        if (! supported(used.as_str()))
            diagnostics.push(
                { used.clone(), "Using core fallback for unsupported optional extension"_Str });
    LoadScope          scope(context.options.trace, LoadEvent::Buffers);
    ResourceLoader     loader { context };
    Vec<ResourceBytes> buffers;
    for (unsigned i = 0; i < count(data.buffers); ++i) {
        const auto& description = at(data.buffers, i);
        auto        read        = [&]() -> Result<ResourceBytes, Error> {
            if (description.has_uri)
                return loader.read(description.uri.as_str(),
                                   ResourceKind::Buffer,
                                   usize(description.byte_length.to_primitive()));
            if (i != 0 || ! data.binary)
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          context.source.as_str(),
                                          "buffers"_str,
                                          "buffer has no source"_str));
            auto range = ResourceBytes::range(data.bytes.clone(),
                                              usize(data.binary->offset.to_primitive()),
                                              usize(data.binary->length.to_primitive()));
            if (! range)
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          context.source.as_str(),
                                          "buffers"_str,
                                          "invalid BIN range"_str));
            return Ok(range.unwrap());
        }();
        if (read.is_err()) return Err(read.unwrap_err());
        auto value    = read.unwrap();
        auto required = description.byte_length.to_primitive();
        if (required > value.bytes().len().to_primitive() ||
            (! description.has_uri && value.bytes().len().to_primitive() - required > 3))
            return Err(resource_error(ErrorKind::InvalidAsset,
                                      context.source.as_str(),
                                      "buffers"_str,
                                      "buffer byte length differs from source"_str));
        if (! description.has_uri) {
            auto size = value.bytes().len();
            if (size > context.options.max_resource_bytes - loader.total)
                return Err(resource_error(ErrorKind::Limit,
                                          context.source.as_str(),
                                          "buffers"_str,
                                          "BIN exceeds resource byte limit"_str));
            loader.total += size;
        }
        buffers.push(value.subrange(usize(), usize(required)).unwrap());
    }
    scope.finish();
    Vec<Option<ResourceBytes>> images;
    images.resize(data.images.len(), None());
    for (unsigned i = 0; i < count(data.images); ++i) {
        bool used {};
        for (const auto& texture : data.textures)
            if (texture.image == i) used = true;
        if (! used) continue;
        const auto& image = at(data.images, i);
        if (image.has_uri) {
            auto bytes = loader.read(image.uri.as_str(), ResourceKind::Image);
            if (bytes.is_err()) return Err(bytes.unwrap_err());
            at(images, i) = Some(bytes.unwrap());
        } else if (image.buffer_view != invalid_index) {
            if (image.buffer_view >= count(data.buffer_views))
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          context.source.as_str(),
                                          "images"_str,
                                          "image buffer view is out of range"_str));
            const auto& view = at(data.buffer_views, image.buffer_view);
            if (view.buffer >= count(buffers))
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          context.source.as_str(),
                                          "bufferViews"_str,
                                          "buffer reference is out of range"_str));
            auto bytes  = at(buffers, view.buffer).bytes();
            auto offset = view.offset.to_primitive(), length = view.length.to_primitive();
            if (offset > bytes.len().to_primitive() || length > bytes.len().to_primitive() - offset)
                return Err(resource_error(ErrorKind::InvalidAsset,
                                          context.source.as_str(),
                                          "images"_str,
                                          "image buffer view exceeds buffer"_str));
            at(images, i) = at(buffers, view.buffer).subrange(usize(offset), usize(length));
        } else
            return Err(resource_error(ErrorKind::Unsupported,
                                      context.source.as_str(),
                                      "images"_str,
                                      "image has no supported source"_str));
    }
    return Ok(ResolvedDocument(
        rstd::move(input), rstd::move(buffers), rstd::move(images), rstd::move(diagnostics)));
}
} // namespace vecgltf
