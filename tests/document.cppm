module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:document;
import vecgltf.document;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::document
{
auto parse(ref<str> text, DocumentOptions options = {}) -> Result<Document, Error> {
    return ParseDocument(Vec<u8>::from(text.as_bytes()), options, "document-test"_str);
}
auto buffer_document(ref<str> value) -> String {
    auto text = String::make(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)"_str);
    text.push_str(value);
    text.push_str("}]}"_str);
    return text;
}
void append32(Vec<u8>& bytes, unsigned value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push(u8((value >> (i * 8)) & 255));
}
auto binary(ref<str> json, bool unknown = false) -> Vec<u8> {
    auto text = Vec<u8>::from(json.as_bytes());
    while (text.len().to_primitive() % 4) text.push(u8(' '));
    Vec<u8> bytes;
    append32(bytes, 0x46546c67);
    append32(bytes, 2);
    append32(bytes, unsigned(12 + 8 + text.len().to_primitive() + 12 + (unknown ? 12 : 0)));
    append32(bytes, unsigned(text.len().to_primitive()));
    append32(bytes, 0x4e4f534a);
    for (auto value : text) bytes.push(u8(value));
    append32(bytes, 4);
    append32(bytes, 0x004e4942);
    bytes.push(u8(42));
    bytes.push(u8());
    bytes.push(u8());
    bytes.push(u8());
    if (unknown) {
        append32(bytes, 4);
        append32(bytes, 0x12345678);
        append32(bytes, 0xabcdef01);
    }
    return bytes;
}
TEST(Document, ExactIntegersAndLastWins) {
    ref<str> values[] { "64"_str, "64.0"_str, "6.4e1"_str };
    for (auto value : values) {
        auto text   = buffer_document(value);
        auto loaded = parse(text.as_str()).unwrap();
        rstd_assert(at(loaded.data().buffers, 0).byte_length == u64(64));
    }
    auto replaced =
        parse(
            R"({"asset":{"version":17,"version":"2.0"},"buffers":[{"byteLength":"bad","byteLength":64}]})"_str)
            .unwrap();
    rstd_assert(at(replaced.data().buffers, 0).byte_length == u64(64));
    auto overflowing_replaced =
        parse(
            R"({"asset":{"version":1e999,"version":"2.0"},"materials":[{"roughnessFactor":1e999,"pbrMetallicRoughness":{"roughnessFactor":1e999,"roughnessFactor":0.5}}]})"_str)
            .unwrap();
    rstd_assert(at(overflowing_replaced.data().materials, 0).core.roughness == 0.5f);
    auto array_replaced =
        parse(R"({"asset":{"version":"2.0"},"buffers":[false],"buffers":[{"byteLength":64}]})"_str)
            .unwrap();
    rstd_assert(array_replaced.data().buffers.len() == usize(1));
    rstd_assert(
        parse(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":64,"byteLength":"bad"}]})"_str)
            .unwrap_err()
            .kind == ErrorKind::InvalidAsset);
    rstd_assert(
        parse(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1e+,"byteLength":64}]})"_str)
            .unwrap_err()
            .kind == ErrorKind::Parse);
    auto maximum =
        parse(
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":18446744073709551615.0}]})"_str)
            .unwrap();
    rstd_assert(at(maximum.data().buffers, 0).byte_length == u64::MAX);
    ref<str> invalid[] {
        "-1"_str, "0.00000000000000000000000001"_str, "18446744073709551616"_str, "64.01"_str
    };
    for (auto value : invalid) {
        auto text   = buffer_document(value);
        auto loaded = parse(text.as_str());
        rstd_assert(loaded.is_err() && loaded.unwrap_err().kind == ErrorKind::InvalidAsset);
    }
}
TEST(Document, MetadataOwnership) {
    auto input = Vec<u8>::from(
        R"({"asset":{"version":"2.0","generator":"test","extras":{"author":"creator","license":"CC","source":"https://example.test","title":"model"}},
            "images":[{"uri":"unused.png","extras":null},{"uri":"used.png","extras":[1,true]}],
            "textures":[{"source":1}],
            "materials":[{"extensions":{"KHR_materials_specular":{"specularFactor":0.75}}}],
            "extensionsUsed":["KHR_materials_specular"]})"_str.as_bytes());
    auto loaded   = ParseDocument(rstd::move(input)).unwrap();
    auto metadata = loaded.metadata_owner();
    rstd_assert(metadata->asset.generator == "test"_str);
    auto first = loaded.metadata(at(loaded.data().images, 0).source).unwrap();
    rstd_assert(first->extras && first->extras->is_null());
    auto second = loaded.metadata(at(loaded.data().images, 1).source).unwrap();
    rstd_assert(second->extras && second->extras->is_array());
    auto material = loaded.metadata(at(loaded.data().materials, 0).source).unwrap();
    rstd_assert(
        material->extensions &&
        (*material->extensions)["KHR_materials_specular"_str]["specularFactor"_str].as_f64());
    auto asset = loaded.metadata(metadata->asset.source).unwrap();
    rstd_assert(asset->extras && (*asset->extras)["title"_str] == "model"_str);
    auto another = parse(R"({"asset":{"version":"2.0"}})"_str).unwrap();
    rstd_assert(another.metadata(at(loaded.data().images, 0).source).is_none());
    loaded = rstd::move(another);
    rstd_assert((*asset->extras)["author"_str] == "creator"_str);
    rstd_assert(first->extras->is_null());
    auto overwritten =
        parse(
            R"({"asset":{"version":"2.0","extras":"old"},"asset":{"version":"2.0","extras":"final"},
             "nodes":[{"name":"old"},{"name":"removed"}],"nodes":[{"name":"final"}],
             "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"extras":"removed"},"extras":"old"},
                           "pbrMetallicRoughness":{"extras":"final"}}],
             "cameras":[{"type":"perspective","perspective":{"yfov":1,"znear":0.1,"extras":"old"},
                         "perspective":{"yfov":1,"znear":0.1,"extras":"final"}}]})"_str)
            .unwrap();
    auto final_metadata = overwritten.metadata_owner();
    rstd_assert(final_metadata->records.len() == usize(7));
    for (unsigned i = 0; i < count(final_metadata->records); ++i) {
        auto view = overwritten.metadata({ i, final_metadata->identity }).unwrap();
        rstd_assert(view->name != "old"_str && view->name != "removed"_str);
        rstd_assert(view->kind != SourceObjectKind::TextureInfo);
        if (view->extras) rstd_assert(*view->extras == "final"_str);
        if (view->parent != invalid_index)
            rstd_assert(view->parent < count(final_metadata->records));
    }
    rstd_assert(overwritten.metadata(at(overwritten.data().nodes, 0).source)->value().name ==
                "final"_str);
}
TEST(Document, BinaryContainer) {
    auto loaded =
        ParseDocument(
            binary(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1}]})"_str, true))
            .unwrap();
    rstd_assert(loaded.data().binary && loaded.data().binary->length == u64(4));
    auto offset = usize(loaded.data().binary->offset.to_primitive());
    rstd_assert(u8((*loaded.data().bytes)[offset]) == u8(42));
    auto bad_padding = binary(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1}]})"_str);
    bad_padding[bad_padding.len() - usize(1)] = u8(1);
    rstd_assert(ParseDocument(rstd::move(bad_padding)).unwrap_err().kind ==
                ErrorKind::InvalidAsset);
    auto truncated = binary(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1}]})"_str);
    truncated.pop();
    rstd_assert(ParseDocument(rstd::move(truncated)).unwrap_err().kind == ErrorKind::Parse);
    auto wrong_header = binary(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1}]})"_str);
    wrong_header[usize(4)] = u8(1);
    rstd_assert(ParseDocument(rstd::move(wrong_header)).unwrap_err().kind == ErrorKind::Parse);
}
TEST(Document, ValidationAndBudgets) {
    auto unknown =
        parse(
            R"({"asset":{"version":"2.0"},"extensionsUsed":["CUSTOM"],"extensionsRequired":["CUSTOM"],"extras":false})"_str)
            .unwrap();
    rstd_assert(unknown.data().extensions_required.len() == usize(1));
    rstd_assert(
        parse(R"({"asset":{"version":"2.0"},"nodes":[{"children":[1]},{"children":[0]}]})"_str)
            .unwrap_err()
            .kind == ErrorKind::InvalidAsset);
    rstd_assert(parse(R"({"asset":{"version":"2.0"},"nodes":[{"children":[1,1]},{}]})"_str)
                    .unwrap_err()
                    .kind == ErrorKind::InvalidAsset);
    rstd_assert(
        parse(
            R"({"asset":{"version":"2.0"},"nodes":[{"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"translation":[0,0,0]}]})"_str)
            .unwrap_err()
            .kind == ErrorKind::InvalidAsset);
    auto limited = parse(R"({"asset":{"version":"2.0"}})"_str, { .max_objects = usize(1) });
    rstd_assert(limited.is_err() && limited.unwrap_err().kind == ErrorKind::Limit);
    auto metadata_limit = parse(R"({"asset":{"version":"2.0","extras":"long"}})"_str,
                                { .max_metadata_bytes = usize(1) });
    rstd_assert(metadata_limit.is_err() && metadata_limit.unwrap_err().kind == ErrorKind::Limit);
    auto replaced_objects = parse(R"({"asset":{"version":"2.0"},"asset":{"version":"2.0"}})"_str,
                                  { .max_objects = usize(2) });
    rstd_assert(replaced_objects.is_err() &&
                replaced_objects.unwrap_err().kind == ErrorKind::Limit);
    auto replaced_metadata = parse(R"({"asset":{"version":"2.0","extras":"a","extras":"b"}})"_str,
                                   { .max_metadata_bytes = usize(3) });
    rstd_assert(replaced_metadata.is_err() &&
                replaced_metadata.unwrap_err().kind == ErrorKind::Limit);
    auto parent =
        parse(
            R"({"asset":{"version":"2.0"},"nodes":[{"children":[1]},{}],"scenes":[{"nodes":[0]}]})"_str)
            .unwrap();
    rstd_assert(at(parent.data().nodes, 1).parent == 0);
}
} // namespace vecgltf_test::document
