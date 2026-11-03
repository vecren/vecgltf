module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:attributes;
import vecgltf;
import rstd.json;
import rstd.test;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vecgltf;

namespace vecgltf_test::attributes
{
auto number(unsigned value) -> rstd::json::Value {
    return rstd::json::Value::Number(rstd::json::Number::from_u64(u64(value)));
}

TEST(Asset, UnusedTexcoords) {
    auto input    = rstd::path::PathBuf::from("data/triangle.gltf"_str);
    auto source   = rstd::fs::read_to_string(input.as_path()).unwrap();
    auto document = rstd::json::from_str(source.as_str()).unwrap();
    auto uv       = rstd::json::from_str(
                        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}"_str)
                        .unwrap();
    document["accessors"_str].as_array_mut().unwrap()->push(uv.clone());
    uv["byteOffset"_str]    = number(12);
    uv["componentType"_str] = number(5121);
    uv["normalized"_str]    = rstd::json::Value::Bool(true);
    document["accessors"_str].as_array_mut().unwrap()->push(rstd::move(uv));
    auto& primitive               = document["meshes"_str][usize(0)]["primitives"_str][usize(0)];
    auto& attributes              = primitive["attributes"_str];
    attributes["TEXCOORD_0"_str]  = number(3);
    attributes["TEXCOORD_1"_str]  = number(3);
    attributes["TEXCOORD_2"_str]  = number(4);
    attributes["TEXCOORD_4"_str]  = number(4);
    attributes["TEXCOORD_29"_str] = number(4);
    primitive["material"_str]     = number(0);
    document["materials"_str] =
        rstd::json::from_str(
            "[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":0}}}]"_str)
            .unwrap();
    auto directory = rstd::fs::TempDir::make("vecgltf-attributes"_str).unwrap();
    auto filename  = rstd::path::PathBuf::from("model.gltf"_str);
    auto root      = rstd::path::PathBuf::from(directory.path());
    auto file      = root.join(filename.as_path());
    auto path      = file.as_path().to_string_lossy();
    auto load      = [&]() {
        auto json = rstd::json::to_string(document);
        rstd::fs::write(file.as_path(), json.as_str().as_bytes()).unwrap();
        return LoadAsset(path.as_str());
    };
    auto        loaded   = load().unwrap();
    const auto& vertices = at(at(loaded->meshes, 0).primitives, 0).vertices;
    rstd_assert(count(vertices) == 3);
    rstd_assert(at(vertices, 1).uv0.x == 1 && at(vertices, 1).uv1.x == 1);
    rstd_assert(at(at(loaded->meshes, 0).primitives, 0).extra_texcoords.is_empty());
    document["extensionsUsed"_str] =
        rstd::json::from_str("[\"KHR_texture_transform\"]"_str).unwrap();
    auto& texture =
        document["materials"_str][usize(0)]["pbrMetallicRoughness"_str]["baseColorTexture"_str];
    texture["texCoord"_str] = number(1);
    rstd_assert(load().is_ok());
    texture["texCoord"_str]    = number(2);
    auto        used           = load().unwrap();
    const auto& used_primitive = at(at(used->meshes, 0).primitives, 0);
    rstd_assert(count(used_primitive.extra_texcoords) == 1);
    auto coord = used_primitive.texcoord(2, 1).unwrap();
    rstd_assert(coord.x == 0 && coord.y == 1);
    rstd_assert(used_primitive.texcoord(0, 1).unwrap().x == 1);
    rstd_assert(used_primitive.texcoord(4, 1).is_none());
    rstd_assert(used_primitive.texcoord(2, 3).is_none());
    texture["texCoord"_str] = number(0);
    texture["extensions"_str] =
        rstd::json::from_str("{\"KHR_texture_transform\":{\"texCoord\":29}}"_str).unwrap();
    auto transformed = load().unwrap();
    rstd_assert(at(transformed->materials, 0).base.uv == 29);
    const auto& transformed_primitive = at(at(transformed->meshes, 0).primitives, 0);
    rstd_assert(transformed_primitive.texcoord(29, 1).unwrap().y == 1);
    rstd_assert(transformed_primitive.texcoord(0, 1).unwrap().x == 1);
    texture["extensions"_str] = rstd::json::from_str("{}"_str).unwrap();

    auto binding = [](unsigned set) {
        rstd::json::Value value;
        value["index"_str]    = number(0);
        value["texCoord"_str] = number(set);
        return value;
    };
    auto& material = document["materials"_str][usize(0)];
    material["pbrMetallicRoughness"_str]["metallicRoughnessTexture"_str] = binding(1);
    material["normalTexture"_str]                                        = binding(2);
    material["occlusionTexture"_str]                                     = binding(4);
    material["emissiveTexture"_str]                                      = binding(29);
    auto        all_slots                                                = load().unwrap();
    const auto& all_primitive = at(at(all_slots->meshes, 0).primitives, 0);
    rstd_assert(count(all_primitive.extra_texcoords) == 3);
    unsigned used_sets[] { 2, 4, 29 };
    for (unsigned set : used_sets) rstd_assert(all_primitive.texcoord(set, 1).unwrap().y == 1);
    const auto& tangent = at(all_primitive.vertices, 0).tangent;
    rstd_assert(absolute(tangent.x) < 0.0001f && absolute(tangent.y + 1) < 0.0001f);

    document["buffers"_str].as_array_mut().unwrap()->push(
        rstd::json::from_str(
            "{\"byteLength\":6,\"uri\":\"data:application/octet-stream;base64,AAACAAEA\"}"_str)
            .unwrap());
    document["bufferViews"_str].as_array_mut().unwrap()->push(
        rstd::json::from_str("{\"buffer\":1,\"byteLength\":6}"_str).unwrap());
    document["accessors"_str].as_array_mut().unwrap()->push(
        rstd::json::from_str(
            "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"_str)
            .unwrap());
    primitive["indices"_str]        = number(5);
    auto        reordered           = load().unwrap();
    const auto& reordered_primitive = at(at(reordered->meshes, 0).primitives, 0);
    rstd_assert(at(reordered_primitive.vertices, 1).position.y == 1);
    rstd_assert(reordered_primitive.texcoord(2, 1).unwrap().x == 0);
    rstd_assert(reordered_primitive.texcoord(2, 1).unwrap().y == 0);
    rstd_assert(reordered_primitive.texcoord(2, 2).unwrap().y == 1);

    material["normalTexture"_str] = binding(7);
    auto missing                  = load();
    rstd_assert(missing.is_err() && missing.unwrap_err().kind == ErrorKind::InvalidAsset);
    material["normalTexture"_str]                   = binding(2);
    document["accessors"_str][usize(4)]["type"_str] = rstd::json::Value::String("VEC3"_Str);
    auto invalid                                    = load();
    rstd_assert(invalid.is_err() && invalid.unwrap_err().kind == ErrorKind::InvalidAsset);
    document["accessors"_str][usize(4)]["type"_str]  = rstd::json::Value::String("VEC2"_Str);
    document["accessors"_str][usize(4)]["count"_str] = number(2);
    auto short_stream                                = load();
    rstd_assert(short_stream.is_err() && short_stream.unwrap_err().kind == ErrorKind::InvalidAsset);
}
} // namespace vecgltf_test::attributes
