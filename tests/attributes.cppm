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
    uv["byteOffset"_str] = number(4);
    document["accessors"_str].as_array_mut().unwrap()->push(rstd::move(uv));
    auto& primitive              = document["meshes"_str][usize(0)]["primitives"_str][usize(0)];
    auto& attributes             = primitive["attributes"_str];
    attributes["TEXCOORD_0"_str] = number(3);
    attributes["TEXCOORD_1"_str] = number(3);
    attributes["TEXCOORD_2"_str] = number(4);
    attributes["TEXCOORD_4"_str] = number(4);
    primitive["material"_str]    = number(0);
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
    document["extensionsUsed"_str] =
        rstd::json::from_str("[\"KHR_texture_transform\"]"_str).unwrap();
    auto& texture =
        document["materials"_str][usize(0)]["pbrMetallicRoughness"_str]["baseColorTexture"_str];
    texture["texCoord"_str] = number(1);
    rstd_assert(load().is_ok());
    texture["texCoord"_str] = number(2);
    auto used               = load();
    rstd_assert(used.is_err() && used.unwrap_err().kind == ErrorKind::Unsupported);
    texture["texCoord"_str] = number(0);
    texture["extensions"_str] =
        rstd::json::from_str("{\"KHR_texture_transform\":{\"texCoord\":4}}"_str).unwrap();
    auto transformed = load();
    rstd_assert(transformed.is_err() && transformed.unwrap_err().kind == ErrorKind::Unsupported);
    texture["extensions"_str]                       = rstd::json::from_str("{}"_str).unwrap();
    document["accessors"_str][usize(4)]["type"_str] = rstd::json::Value::String("VEC3"_Str);
    auto invalid                                    = load();
    rstd_assert(invalid.is_err() && invalid.unwrap_err().kind == ErrorKind::InvalidAsset);
}
} // namespace vecgltf_test::attributes
