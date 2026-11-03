module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:materials;
import vecgltf;
import rstd.json;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::materials
{
TEST(Asset, SpecularGlossiness) {
    auto directory = rstd::fs::TempDir::make("vecgltf-materials"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(directory.path());
    auto name      = rstd::path::PathBuf::from("model.gltf"_str);
    auto file      = root.join(name.as_path());
    auto path      = file.as_path().to_string_lossy();
    auto document  = rstd::json::from_str(R"({
        "asset":{"version":"2.0"},
        "extensionsUsed":["KHR_materials_pbrSpecularGlossiness","KHR_texture_transform"],
        "extensionsRequired":["KHR_materials_pbrSpecularGlossiness"],
        "images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4////fwAJ+wP9KobjigAAAABJRU5ErkJggg=="}],
        "textures":[{"source":0}],
        "materials":[{
            "alphaMode":"BLEND", "doubleSided":true,
            "pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1],"metallicFactor":1},
            "extensions":{"KHR_materials_pbrSpecularGlossiness":{
                "diffuseFactor":[0.2,0.3,0.4,0.5],
                "specularFactor":[0.6,0.7,0.8], "glossinessFactor":0.9,
                "diffuseTexture":{"index":0,"texCoord":1},
                "specularGlossinessTexture":{"index":0,"extensions":{
                    "KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3],"rotation":0.4,"texCoord":1}
                }}
            }}
        }]
    })"_str)
                         .unwrap();
    auto load      = [&]() {
        auto text = rstd::json::to_string(document);
        rstd::fs::write(file.as_path(), text.as_str().as_bytes()).unwrap();
        return LoadAsset(path.as_str());
    };
    auto loaded = load().unwrap();
    rstd_assert(loaded->diagnostics.is_empty());
    const auto& material = at(loaded->materials, 0);
    rstd_assert(material.workflow == MaterialWorkflow::SpecularGlossiness);
    rstd_assert(absolute(material.base_color.x - 0.2f) < 0.00001f);
    rstd_assert(absolute(material.base_color.w - 0.5f) < 0.00001f);
    rstd_assert(absolute(material.specular.y - 0.7f) < 0.00001f);
    rstd_assert(absolute(material.glossiness - 0.9f) < 0.00001f);
    rstd_assert(material.base.texture == 0 && material.base.uv == 1);
    rstd_assert(material.physical.texture == 0 && material.physical.uv == 1);
    rstd_assert(material.physical.offset.x == 0.25f && material.physical.scale.y == 3);
    rstd_assert(absolute(material.physical.rotation - 0.4f) < 0.00001f);
    rstd_assert(material.alpha == AlphaMode::Blend && material.double_sided);
    rstd_assert(at(loaded->materials, 1).workflow == MaterialWorkflow::MetallicRoughness);

    auto& extension      = document["materials"_str][usize(0)]["extensions"_str]
                                   ["KHR_materials_pbrSpecularGlossiness"_str];
    extension            = rstd::json::from_str("{}"_str).unwrap();
    auto        defaults = load().unwrap();
    const auto& value    = at(defaults->materials, 0);
    rstd_assert(value.workflow == MaterialWorkflow::SpecularGlossiness);
    rstd_assert(value.base_color.x == 1 && value.base_color.w == 1);
    rstd_assert((value.specular == Vec3 { 1, 1, 1 }) && value.glossiness == 1);
    rstd_assert(value.base.texture == invalid_index && value.physical.texture == invalid_index);

    ref<str> invalid_values[] { R"({"diffuseFactor":[-0.1,1,1,1]})"_str,
                                R"({"specularFactor":[1,1.1,1]})"_str,
                                R"({"glossinessFactor":-0.1})"_str,
                                R"({"glossinessFactor":1e99})"_str };
    for (auto invalid : invalid_values) {
        extension   = rstd::json::from_str(invalid).unwrap();
        auto result = load();
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
    }
}
} // namespace vecgltf_test::materials
