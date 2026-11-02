module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:emissive;
import vecgltf;
import rstd;
import rstd.json;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;
using rstd::json::Array;
using rstd::json::Map;
using rstd::json::Number;
using rstd::json::Value;

namespace vecgltf_test::emissive
{
auto number(double value) -> Value {
    return Value::Number(Number::from_f64(rstd::f64(value)).unwrap());
}

auto document(double strength, bool property, bool extension) -> Value {
    Value result, material;
    result["asset"_str]["version"_str] = Value::String("2.0"_Str);
    Array factor;
    factor.push(number(0.25));
    factor.push(number(0.5));
    factor.push(number(1));
    material["emissiveFactor"_str] = Value::Array(rstd::move(factor));
    if (extension) {
        auto name                        = "KHR_materials_emissive_strength"_str;
        material["extensions"_str][name] = Value::Object(Map::make());
        if (property) material["extensions"_str][name]["emissiveStrength"_str] = number(strength);
        Array used, required;
        used.push(Value::String(String::make(name)));
        required.push(Value::String(String::make(name)));
        result["extensionsUsed"_str]     = Value::Array(rstd::move(used));
        result["extensionsRequired"_str] = Value::Array(rstd::move(required));
    }
    Array materials;
    materials.push(rstd::move(material));
    result["materials"_str] = Value::Array(rstd::move(materials));
    return result;
}

struct Files {
    rstd::path::PathBuf model { rstd::path::PathBuf::from("../extools/test-emissive.gltf"_str) };
    Files() { rstd::fs::create_dir_all(model.as_path().parent().unwrap()).unwrap(); }
    ~Files() { (void)rstd::fs::remove_file(model.as_path()); }
    auto load(double strength, bool property = true, bool extension = true)
        -> Result<Arc<Asset>, Error> {
        auto value = document(strength, property, extension);
        auto text  = rstd::json::to_string(value);
        rstd::fs::write(model.as_path(), text.as_str().as_bytes()).unwrap();
        auto path = model.as_path().to_string_lossy();
        return LoadAsset(path.as_str());
    }
};

void emission(const Asset& asset, Vec3 expected) {
    const auto& value = at(asset.materials, 0).emissive;
    rstd_assert(absolute(value.x - expected.x) < 0.00001f);
    rstd_assert(absolute(value.y - expected.y) < 0.00001f);
    rstd_assert(absolute(value.z - expected.z) < 0.00001f);
}

TEST(Asset, EmissiveStrength) {
    Files files;
    auto  strong = files.load(10).unwrap();
    emission(*strong, { 2.5f, 5, 10 });
    rstd_assert(at(strong->materials, 0).emissive.z > 1);
    auto default_strength = files.load(1, false).unwrap();
    emission(*default_strength, { 0.25f, 0.5f, 1 });
    auto absent = files.load(1, false, false).unwrap();
    emission(*absent, { 0.25f, 0.5f, 1 });
    auto zero = files.load(0).unwrap();
    emission(*zero, {});
    auto negative = files.load(-1);
    rstd_assert(negative.is_err());
    rstd_assert(negative.unwrap_err().kind == ErrorKind::InvalidAsset);
    auto overflow = files.load(1e99);
    rstd_assert(overflow.is_err());
    rstd_assert(overflow.unwrap_err().kind == ErrorKind::InvalidAsset);
}
} // namespace vecgltf_test::emissive
