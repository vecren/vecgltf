module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:accessor;
import vecgltf.load.accessor;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::accessor
{
struct MemoryResolver {
    ResourceBytes bytes;
    auto          Read(const ResourceRequest& request) const -> Result<ResourceBytes, Error> {
        if (bytes.bytes().len() > request.max_bytes)
            return Err(Error { ErrorKind::Limit,
                               "memory"_Str,
                               "resource"_Str,
                               "resource exceeds test limit"_Str });
        return Ok(ResourceBytes(bytes));
    }
};
} // namespace vecgltf_test::accessor

namespace rstd
{
template<>
struct Impl<vecgltf::ResourceResolver, vecgltf_test::accessor::MemoryResolver>
    : ImplBase<vecgltf_test::accessor::MemoryResolver> {
    auto Read(const vecgltf::ResourceRequest& request) const
        -> Result<vecgltf::ResourceBytes, vecgltf::Error> {
        return this->self().Read(request);
    }
};
} // namespace rstd

namespace vecgltf_test::accessor
{
template<rstd::size_t N>
auto bytes(const unsigned (&values)[N]) -> Vec<u8> {
    Vec<u8> output;
    for (unsigned value : values) output.push(u8(value));
    return output;
}
void word(Vec<u8>& output, unsigned value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i) output.push(u8((value >> (i * 8)) & 255));
}
void real(Vec<u8>& output, float value) {
    word(output, rstd::f32(value).to_bits().to_primitive());
}
auto load(ref<str> body, Vec<u8> data, LoadOptions options = {})
    -> Result<ResolvedDocument, Error> {
    auto text =
        rstd::format("{{\"asset\":{{\"version\":\"2.0\"}},\"buffers\":[{{\"uri\":\"memory.bin\","
                     "\"byteLength\":{}}}],{}}}",
                     data.len(),
                     body);
    auto document = ParseDocument(Vec<u8>::from(text.as_str().as_bytes()));
    if (document.is_err()) return Err(document.unwrap_err());
    options.resource_resolver = ResourceAccess(
        Arc<dyn<ResourceResolver>>::make(MemoryResolver { ResourceBytes(rstd::move(data)) }));
    auto input = LoadInput::memory(document.unwrap(), options, "memory.gltf"_str);
    if (input.is_err()) return Err(input.unwrap_err());
    return ResolveResources(input.unwrap());
}
void near(float value, float expected) {
    rstd_assert(absolute(value - expected) < 0.00001f);
}
void rejects(ref<str> body, Vec<u8> bytes, ErrorKind kind = ErrorKind::InvalidAsset) {
    auto document = load(body, rstd::move(bytes));
    if (document.is_err()) {
        rstd_assert(document.unwrap_err().kind == kind);
        return;
    }
    auto reader = AccessorReader(*document);
    auto result = reader.Validate();
    rstd_assert(result.is_err() && result.unwrap_err().kind == kind);
}

TEST(Accessor, NormalizedComponentsAndUnsignedIndices) {
    struct Sample {
        unsigned type, size, low, high;
        float    minimum;
    };
    Sample samples[] { { 5120, 1, 128, 127, -1 },
                       { 5121, 1, 0, 255, 0 },
                       { 5122, 2, 32768, 32767, -1 },
                       { 5123, 2, 0, 65535, 0 } };
    for (const auto& sample : samples) {
        Vec<u8> data;
        word(data, sample.low, sample.size);
        word(data, sample.high, sample.size);
        auto body =
            rstd::format("\"bufferViews\":[{{\"buffer\":0,\"byteLength\":{}}}],\"accessors\":[{{"
                         "\"bufferView\":0,\"componentType\":{},\"count\":2,\"type\":\"SCALAR\","
                         "\"normalized\":true}}]",
                         data.len(),
                         sample.type);
        auto           document = load(body.as_str(), rstd::move(data)).unwrap();
        AccessorReader reader(document);
        rstd_assert(reader.Validate().is_ok());
        auto values = reader.Floats(0, 1).unwrap();
        near(at(values, 0), sample.minimum);
        near(at(values, 1), 1);
        rstd_assert(reader.Indices(0).is_err());
    }
    unsigned types[] { 5121, 5123, 5125 };
    unsigned sizes[] { 1, 2, 4 }, largest[] { 255, 65535, 0xffffffffu };
    for (unsigned i = 0; i < 3; ++i) {
        Vec<u8> data;
        word(data, 0, sizes[i]);
        word(data, largest[i], sizes[i]);
        auto body =
            rstd::format("\"bufferViews\":[{{\"buffer\":0,\"byteLength\":{}}}],\"accessors\":[{{"
                         "\"bufferView\":0,\"componentType\":{},\"count\":2,\"type\":\"SCALAR\"}}]",
                         data.len(),
                         types[i]);
        auto document = load(body.as_str(), rstd::move(data)).unwrap();
        auto values   = AccessorReader(document).Indices(0).unwrap();
        rstd_assert(count(values) == 2 && at(values, 0) == 0 && at(values, 1) == largest[i]);
    }
}

TEST(Accessor, StrideAndMatrixColumnPadding) {
    Vec<u8> data;
    real(data, 1);
    real(data, 2);
    word(data, 0xdeadbeef);
    real(data, 3);
    real(data, 4);
    auto document =
        load(
            R"("bufferViews":[{"buffer":0,"byteLength":20,"byteStride":12}],"accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"VEC2"}])"_str,
            rstd::move(data))
            .unwrap();
    auto values = AccessorReader(document).Floats(0, 2).unwrap();
    for (unsigned i = 0; i < 4; ++i) near(at(values, i), float(i + 1));

    struct Matrix {
        unsigned rows, size, type;
        ref<str> shape;
    };
    Matrix matrices[] { { 2, 1, 5121, "MAT2"_str },
                        { 3, 1, 5121, "MAT3"_str },
                        { 3, 2, 5123, "MAT3"_str } };
    for (const auto& matrix : matrices) {
        Vec<u8>  padded;
        unsigned value = 1;
        for (unsigned element = 0; element < 2; ++element)
            for (unsigned column = 0; column < matrix.rows; ++column) {
                for (unsigned row = 0; row < matrix.rows; ++row) word(padded, value++, matrix.size);
                if (element == 1 && column + 1 == matrix.rows) continue;
                while (padded.len().to_primitive() % 4) padded.push(u8(255));
            }
        auto body =
            rstd::format("\"bufferViews\":[{{\"buffer\":0,\"byteLength\":{}}}],\"accessors\":[{{"
                         "\"bufferView\":0,\"componentType\":{},\"count\":2,\"type\":\"{}\"}}]",
                         padded.len(),
                         matrix.type,
                         matrix.shape);
        auto           resolved = load(body.as_str(), rstd::move(padded)).unwrap();
        AccessorReader reader(resolved);
        rstd_assert(reader.Validate().is_ok());
        auto output = reader.Floats(0, matrix.rows * matrix.rows).unwrap();
        rstd_assert(count(output) == 2 * matrix.rows * matrix.rows);
        for (unsigned i = 0; i < count(output); ++i) near(at(output, i), float(i + 1));
    }
}

TEST(Accessor, SparseZeroBaseAndOrderedOverrides) {
    Vec<u8> data;
    word(data, 2);
    real(data, 8);
    real(data, 9);
    auto resolved =
        load(
            R"("bufferViews":[{"buffer":0,"byteLength":1},{"buffer":0,"byteOffset":4,"byteLength":8}],"accessors":[{"componentType":5126,"count":4,"type":"VEC2","sparse":{"count":1,"indices":{"bufferView":0,"componentType":5121},"values":{"bufferView":1}}}])"_str,
            rstd::move(data))
            .unwrap();
    AccessorReader reader(resolved);
    rstd_assert(reader.Validate().is_ok());
    auto values = reader.Floats(0, 2).unwrap();
    for (unsigned i = 0; i < 8; ++i) near(at(values, i), i == 4 ? 8 : i == 5 ? 9 : 0);

    unsigned indices[] { 2, 2, 0, 0 };
    auto     repeated = bytes(indices);
    real(repeated, 1);
    real(repeated, 2);
    rejects(
        R"("bufferViews":[{"buffer":0,"byteLength":2},{"buffer":0,"byteOffset":4,"byteLength":8}],"accessors":[{"componentType":5126,"count":4,"type":"SCALAR","sparse":{"count":2,"indices":{"bufferView":0,"componentType":5121},"values":{"bufferView":1}}}])"_str,
        rstd::move(repeated));
    Vec<u8> outside;
    word(outside, 4);
    real(outside, 1);
    rejects(
        R"("bufferViews":[{"buffer":0,"byteLength":1},{"buffer":0,"byteOffset":4,"byteLength":4}],"accessors":[{"componentType":5126,"count":4,"type":"SCALAR","sparse":{"count":1,"indices":{"bufferView":0,"componentType":5121},"values":{"bufferView":1}}}])"_str,
        rstd::move(outside));
}

TEST(Accessor, SparseMatricesAndWideIndices) {
    unsigned types[] { 5121, 5123, 5125 }, sizes[] { 1, 2, 4 };
    for (unsigned index = 0; index < 3; ++index) {
        Vec<u8> data;
        word(data, 1, sizes[index]);
        while (data.len() < usize(4)) data.push(u8());
        unsigned value = 1;
        for (unsigned column = 0; column < 3; ++column) {
            for (unsigned row = 0; row < 3; ++row) data.push(u8(value++));
            if (column < 2) data.push(u8(255));
        }
        auto body = rstd::format(
            "\"bufferViews\":[{{\"buffer\":0,\"byteLength\":{}}},"
            "{{\"buffer\":0,\"byteOffset\":4,\"byteLength\":11}}],\"accessors\":[{{"
            "\"componentType\":5121,\"count\":2,\"type\":\"MAT3\",\"sparse\":{{\"count\":1,"
            "\"indices\":{{\"bufferView\":0,\"componentType\":{}}},"
            "\"values\":{{\"bufferView\":1}}}}}}]",
            sizes[index],
            types[index]);
        auto document = load(body.as_str(), rstd::move(data)).unwrap();
        auto output   = AccessorReader(document).Floats(0, 9).unwrap();
        rstd_assert(count(output) == 18);
        for (unsigned i = 0; i < 18; ++i) near(at(output, i), i < 9 ? 0 : float(i - 8));
    }
}

TEST(Accessor, ExactLargeOffsetsAndCounts) {
    unsigned payload[] { 0, 0, 0, 0, 0, 0, 0, 0 };
    rejects(
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"accessors":[{"bufferView":0,"byteOffset":18446744073709551612,"componentType":5126,"count":1,"type":"SCALAR"}])"_str,
        bytes(payload));
    rejects(
        R"("accessors":[{"componentType":5121,"count":18446744073709551615,"type":"SCALAR"}])"_str,
        bytes(payload),
        ErrorKind::Limit);
}

TEST(Accessor, RejectsBoundsAlignmentTypesAndLimits) {
    unsigned payload[] { 0, 0, 0, 0, 0, 0, 0, 0 };
    ref<str> invalid[] {
        R"("bufferViews":[{"buffer":0,"byteLength":9}],"accessors":[])"_str,
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"SCALAR"}])"_str,
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"accessors":[{"bufferView":0,"byteOffset":1,"componentType":5126,"count":1,"type":"SCALAR"}])"_str,
        R"("bufferViews":[{"buffer":0,"byteLength":8,"byteStride":8}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"}])"_str,
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"accessors":[{"bufferView":0,"componentType":5125,"count":1,"type":"SCALAR","normalized":true}])"_str,
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR","normalized":true}])"_str,
        R"("accessors":[{"byteOffset":1,"componentType":5126,"count":1,"type":"SCALAR"}])"_str,
    };
    for (auto body : invalid) rejects(body, bytes(payload));
    auto limited   = load(R"("accessors":[{"componentType":5126,"count":1,"type":"VEC3"}])"_str,
                          bytes(payload),
                          { .max_accessor_elements = usize(2) })
                         .unwrap();
    auto too_large = AccessorReader(limited).Floats(0, 3);
    rstd_assert(too_large.is_err() && too_large.unwrap_err().kind == ErrorKind::Limit);
    auto zero  = load(R"("accessors":[{"componentType":5126,"count":2,"type":"SCALAR"}])"_str,
                      bytes(payload))
                     .unwrap();
    auto zeros = AccessorReader(zero).Floats(0, 1).unwrap();
    rstd_assert(at(zeros, 0) == 0 && at(zeros, 1) == 0);
    rstd_assert(AccessorReader(zero).Indices(0).is_err());
    rstd_assert(AccessorReader(zero).Floats(0, 2).is_err());
    rstd_assert(AccessorReader(zero).Floats(9, 1).is_err());
    auto strided_indices =
        load(
            R"("bufferViews":[{"buffer":0,"byteLength":8,"byteStride":4}],"accessors":[{"bufferView":0,"componentType":5121,"count":2,"type":"SCALAR"}])"_str,
            bytes(payload))
            .unwrap();
    rstd_assert(AccessorReader(strided_indices).Indices(0).is_err());
    Vec<u8> nonfinite;
    word(nonfinite, 0x7f800000);
    auto infinity =
        load(
            R"("bufferViews":[{"buffer":0,"byteLength":4}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR"}])"_str,
            rstd::move(nonfinite))
            .unwrap();
    auto result = AccessorReader(infinity).Floats(0, 1);
    rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
}
} // namespace vecgltf_test::accessor
