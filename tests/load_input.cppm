module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:load_input;
import vecgltf;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::load_input
{
struct Reads {
    String   document;
    Vec<u8>  buffer, image;
    unsigned documents {}, buffers {}, images {};
};
struct Resolver {
    Arc<Reads> reads;
    auto       Read(const ResourceRequest& request) const -> Result<ResourceBytes, Error> {
        if (request.kind == ResourceKind::Document) {
            ++reads->documents;
            return Ok(ResourceBytes(Vec<u8>::from(reads->document.as_str().as_bytes())));
        }
        if (request.kind == ResourceKind::Buffer) {
            ++reads->buffers;
            return Ok(ResourceBytes(reads->buffer.clone()));
        }
        ++reads->images;
        return Ok(ResourceBytes(reads->image.clone()));
    }
};
auto repeated_buffers() -> Arc<Reads> {
    auto reads = Arc<Reads>::make();
    reads->document =
        R"({"asset":{"version":"2.0","extras":{"author":"creator","license":"CC0","source":"https://example.test","title":"sample"}},
                         "buffers":[{"byteLength":4,"uri":"same.bin"},{"byteLength":4,"uri":"same.bin"}]})"_Str;
    reads->buffer.resize(usize(4), u8(7));
    return reads;
}
} // namespace vecgltf_test::load_input

namespace rstd
{
template<>
struct Impl<vecgltf::ResourceResolver, vecgltf_test::load_input::Resolver>
    : ImplBase<vecgltf_test::load_input::Resolver> {
    auto Read(const vecgltf::ResourceRequest& request) const
        -> Result<vecgltf::ResourceBytes, vecgltf::Error> {
        return this->self().Read(request);
    }
};
} // namespace rstd

namespace vecgltf_test::load_input
{
auto options(Arc<Reads> reads) -> LoadOptions {
    LoadOptions result;
    result.resource_resolver =
        ResourceAccess(Arc<dyn<ResourceResolver>>::make(Resolver { rstd::move(reads) }));
    return result;
}
auto mesh_document(ref<str> accessors, ref<str> primitives) -> String {
    auto text = String::make(R"({"asset":{"version":"2.0"},"accessors":)"_str);
    text.push_str(accessors);
    text.push_str(R"(,"meshes":[{"primitives":)"_str);
    text.push_str(primitives);
    text.push_str("}]}"_str);
    return text;
}
TEST(LoadInput, ReusesDocumentAndResources) {
    auto reads = repeated_buffers();
    auto input = LoadDocument("memory/model.gltf"_str, options(reads.clone())).unwrap();
    rstd_assert(input.context.base_directory &&
                input.context.base_directory->as_path().is_absolute());
    rstd_assert(reads->documents == 1 && reads->buffers == 0);
    auto info     = input.document.metadata_owner();
    auto metadata = input.document.metadata(info->asset.source).unwrap();
    rstd_assert(metadata->extras && (*metadata->extras)["author"_str] == "creator"_str);
    auto asset = LoadAsset(rstd::move(input)).unwrap();
    rstd_assert(reads->documents == 1 && reads->buffers == 1 && reads->images == 0);
    auto retained = asset->metadata(info->asset.source).unwrap();
    rstd_assert((*retained->extras)["license"_str] == "CC0"_str);
    rstd_assert(asset->materials.len() == usize(1));
    rstd_assert(at(asset->materials, 0).source.index == invalid_index);
    rstd_assert(asset->scenes.len() == usize(1));
    rstd_assert(at(asset->scenes, 0).source.index == invalid_index);
}
TEST(LoadInput, RejectsBeforeResourceRead) {
    auto unsupported = repeated_buffers();
    unsupported->document =
        R"({"asset":{"version":"2.0"},"extensionsUsed":["CUSTOM"],"extensionsRequired":["CUSTOM"],
                               "buffers":[{"byteLength":4,"uri":"same.bin"}]})"_Str;
    auto first = LoadDocument("memory/model.gltf"_str, options(unsupported.clone())).unwrap();
    rstd_assert(first.document.data().extensions_required.len() == usize(1));
    auto rejected = LoadAsset(rstd::move(first));
    rstd_assert(rejected.is_err() && rejected.unwrap_err().kind == ErrorKind::Unsupported);
    rstd_assert(unsupported->documents == 1 && unsupported->buffers == 0);
    auto limited                     = repeated_buffers();
    auto configuration               = options(limited.clone());
    configuration.max_resource_bytes = usize(3);
    auto second = LoadDocument("memory/model.gltf"_str, configuration).unwrap();
    auto limit  = ResolveResources(rstd::move(second));
    rstd_assert(limit.is_err() && limit.unwrap_err().kind == ErrorKind::Limit);
    rstd_assert(limited->documents == 1 && limited->buffers == 0);
    auto document_limited                     = repeated_buffers();
    auto document_configuration               = options(document_limited.clone());
    document_configuration.max_document_bytes = usize(4);
    auto oversized = LoadDocument("memory/model.gltf"_str, document_configuration);
    rstd_assert(oversized.is_err() && oversized.unwrap_err().kind == ErrorKind::Limit);
    rstd_assert(document_limited->documents == 1 && document_limited->buffers == 0);
}
TEST(LoadInput, GeometryBudgetsAndMorphTypes) {
    auto cumulative =
        mesh_document(R"([{"componentType":5126,"count":3,"type":"VEC3"}])"_str,
                      R"([{"attributes":{"POSITION":0}},{"attributes":{"POSITION":0}}])"_str);
    LoadOptions limited;
    limited.max_indices    = usize(5);
    auto first             = ParseDocument(Vec<u8>::from(cumulative.as_str().as_bytes())).unwrap();
    auto cumulative_result = LoadAsset(LoadInput::memory(rstd::move(first), limited).unwrap());
    rstd_assert(cumulative_result.is_err() &&
                cumulative_result.unwrap_err().kind == ErrorKind::Limit);
    auto strip        = mesh_document(R"([{"componentType":5126,"count":4,"type":"VEC3"}])"_str,
                                      R"([{"attributes":{"POSITION":0},"mode":5}])"_str);
    auto second       = ParseDocument(Vec<u8>::from(strip.as_str().as_bytes())).unwrap();
    auto strip_result = LoadAsset(LoadInput::memory(rstd::move(second), limited).unwrap());
    rstd_assert(strip_result.is_err() && strip_result.unwrap_err().kind == ErrorKind::Limit);
    limited.max_indices = usize(6);
    auto third          = ParseDocument(Vec<u8>::from(strip.as_str().as_bytes())).unwrap();
    auto accepted = LoadAsset(LoadInput::memory(rstd::move(third), limited).unwrap()).unwrap();
    rstd_assert(at(at(accepted->meshes, 0).primitives, 0).indices.len() == usize(6));
    ref<str> invalid_morphs[] {
        R"([{"componentType":5126,"count":3,"type":"VEC3"},{"componentType":5123,"count":3,"type":"VEC3"}])"_str,
        R"([{"componentType":5126,"count":3,"type":"VEC3"},{"componentType":5126,"count":3,"type":"VEC2"}])"_str
    };
    for (auto accessors : invalid_morphs) {
        auto text = mesh_document(
            accessors, R"([{"attributes":{"POSITION":0},"targets":[{"POSITION":1}]}])"_str);
        auto parsed = ParseDocument(Vec<u8>::from(text.as_str().as_bytes())).unwrap();
        auto result = LoadAsset(LoadInput::memory(rstd::move(parsed)).unwrap());
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
    }
}
TEST(LoadInput, MetadataMappingAndLifetime) {
    Option<MetadataView> retained;
    SourceObjectId       first_source;
    {
        auto reads = Arc<Reads>::make();
        reads->document =
            R"({"asset":{"version":"2.0","generator":"test","extras":{"author":"creator","license":"CC0"}},
                              "images":[{"name":"unused","uri":"unused.png","extras":null},
                                        {"name":"used","uri":"pixel.png","extras":[7,true]}],
                              "textures":[{"source":1},{"source":1}],
                              "nodes":[{"name":"skeleton"}],"skins":[{"joints":[0],"skeleton":0}]})"_Str;
        auto path      = rstd::path::PathBuf::from("data/white pixel.png"_str);
        reads->image   = rstd::fs::read(path.as_path()).unwrap();
        auto input     = LoadDocument("memory/model.gltf"_str, options(reads.clone())).unwrap();
        auto doc_owner = input.document.metadata_owner();
        first_source   = at(input.document.data().images, 1).source;
        auto owner     = ResolveResources(rstd::move(input)).unwrap();
        auto asset     = BuildAsset(rstd::move(owner)).unwrap();
        rstd_assert(reads->documents == 1 && reads->images == 1);
        rstd_assert(asset->images.len() == usize(1));
        rstd_assert(at(asset->images, 0).source == first_source);
        rstd_assert(at(asset->textures, 0).image == 0 && at(asset->textures, 1).image == 0);
        rstd_assert(at(asset->skins, 0).skeleton == 0);
        auto image = asset->metadata(at(asset->images, 0).source).unwrap();
        rstd_assert(image->name == "used"_str && image->extras && image->extras->is_array());
        retained   = asset->metadata(doc_owner->asset.source);
        auto other = LoadAsset("data/triangle.gltf"_str).unwrap();
        rstd_assert(other->metadata(first_source).is_none());
    }
    auto view = rstd::move(retained).unwrap();
    rstd_assert(view->extras && (*view->extras)["author"_str] == "creator"_str);
}
} // namespace vecgltf_test::load_input
