module vecgltf.document;
import rstd;
import rstd.json;

namespace vecgltf
{
using namespace rstd::literals;
namespace doc = document;

template<class T>
struct DocumentField {
    T             value {};
    Option<Error> error;
};
struct DocumentIssue {
    unsigned      field {};
    Option<Error> error;
};
struct DocumentIssues {
    Vec<DocumentIssue> values;
    u64                seen {};
    void               set(unsigned field, Option<Error> error) {
        if (field < 64) seen = seen | (u64(1) << u64(field));
        for (auto& value : values)
            if (value.field == field) {
                value.error = rstd::move(error);
                return;
            }
        if (error) values.push({ field, rstd::move(error) });
    }
    bool has(unsigned field) const { return (seen & (u64(1) << u64(field))) != u64(); }
    auto first() -> Option<Error> {
        for (auto& value : values)
            if (value.error) return Some(rstd::move(value.error).unwrap());
        return None();
    }
};

class DocumentReader {
    rstd::json::Reader reader_;
    Arc<doc::Data>     data_;
    Arc<MetadataStore> metadata_;
    DocumentOptions    options_;
    String             source_;
    Option<Error>      fatal_;
    usize              objects_ {}, elements_ {}, metadata_bytes_ {};

    auto error(ref<str> location, ref<str> message, ErrorKind kind = ErrorKind::InvalidAsset)
        -> Error {
        return { kind, source_.clone(), String::make(location), String::make(message) };
    }
    void syntax(rstd::json::Error value) {
        if (fatal_) return;
        auto message  = rstd::format("{}", value);
        auto location = rstd::format("document:{}:{}", value.line(), value.column());
        fatal_        = Some(Error { value.is_limit() ? ErrorKind::Limit : ErrorKind::Parse,
                                     source_.clone(),
                                     rstd::move(location),
                                     rstd::move(message),
                                     Some(Arc<dyn<rstd::error::Error>>::make(rstd::move(value))) });
    }
    bool kind(rstd::json::ValueKind expected) {
        if (fatal_) return false;
        auto value = reader_.value_kind();
        if (value.is_err()) {
            syntax(value.unwrap_err());
            return false;
        }
        return value.unwrap() == expected;
    }
    void skip() {
        if (fatal_) return;
        auto result = reader_.skip_value(rstd::json::NumberValidation::SyntaxOnly);
        if (result.is_err()) syntax(result.unwrap_err());
    }
    auto bad(ref<str> location, ref<str> expected) -> Option<Error> {
        skip();
        return Some(error(location, expected));
    }
    bool element(ref<str> location) {
        if (elements_ >= options_.max_array_elements) {
            fatal_ = Some(error(location, "document element limit exceeded"_str, ErrorKind::Limit));
            return false;
        }
        ++elements_;
        return true;
    }
    auto source(SourceObjectKind kind, unsigned index, unsigned parent) -> SourceObjectId {
        if (objects_ >= options_.max_objects || metadata_->records.len() >= usize(~0u)) {
            if (! fatal_)
                fatal_ = Some(
                    error("document"_str, "document object limit exceeded"_str, ErrorKind::Limit));
            return {};
        }
        ++objects_;
        SourceObjectId result { unsigned(metadata_->records.len().to_primitive()),
                                metadata_->identity };
        metadata_->records.push(SourceMetadata { kind, index, parent });
        return result;
    }
    auto location(ref<str> parent, ref<str> field) -> String {
        return parent.is_empty() ? String::make(field) : rstd::format("{}.{}", parent, field);
    }
    template<class Function>
    bool object(ref<str> path, Function function) {
        if (! kind(rstd::json::ValueKind::Object)) {
            skip();
            return false;
        }
        auto begin = reader_.begin_object();
        if (begin.is_err()) {
            syntax(begin.unwrap_err());
            return false;
        }
        bool first = true;
        while (! fatal_) {
            auto key = reader_.next_object_key(first);
            if (key.is_err()) {
                syntax(key.unwrap_err());
                break;
            }
            auto name = key.unwrap();
            if (! name) return true;
            if (! element(path)) break;
            function(name->as_str());
        }
        return true;
    }
    template<class T, class Function>
    auto array(ref<str> path, Function function) -> DocumentField<Vec<T>> {
        DocumentField<Vec<T>> result;
        if (! kind(rstd::json::ValueKind::Array)) {
            result.error = bad(path, "expected array"_str);
            return result;
        }
        auto begin = reader_.begin_array();
        if (begin.is_err()) {
            syntax(begin.unwrap_err());
            return result;
        }
        bool first = true;
        while (! fatal_) {
            auto next = reader_.next_array(first);
            if (next.is_err()) {
                syntax(next.unwrap_err());
                break;
            }
            if (! next.unwrap()) break;
            if (! element(path)) break;
            auto item_path = rstd::format("{}[{}]", path, result.value.len());
            auto item = function(item_path.as_str(), unsigned(result.value.len().to_primitive()));
            if (! result.error && item.error) result.error = rstd::move(item.error);
            result.value.push(rstd::move(item.value));
        }
        return result;
    }
    template<class T>
    void assign(T& target, DocumentField<T> value, DocumentIssues& issues, unsigned field) {
        target = rstd::move(value.value);
        issues.set(field, rstd::move(value.error));
    }
    template<class T>
    void required(DocumentIssues& issues, unsigned field, ref<str> path) {
        if (! issues.has(field))
            issues.set(field, Some(error(path, "required field is missing"_str)));
    }
    void required(DocumentIssues& issues, unsigned field, ref<str> path) {
        required<unsigned>(issues, field, path);
    }
    auto text(ref<str> path) -> DocumentField<String> {
        if (! kind(rstd::json::ValueKind::String)) return { {}, bad(path, "expected string"_str) };
        auto value = reader_.parse_string();
        if (value.is_err()) {
            syntax(value.unwrap_err());
            return {};
        }
        return { value.unwrap(), None() };
    }
    auto natural(ref<str> path) -> DocumentField<u64> {
        if (! kind(rstd::json::ValueKind::Number))
            return { {}, bad(path, "expected non-negative integer"_str) };
        auto number = reader_.parse_unsigned_number();
        if (number.is_err()) {
            syntax(number.unwrap_err());
            return {};
        }
        auto value = number.unwrap().as_u64();
        if (! value)
            return { {},
                     Some(error(path, "expected exact non-negative integer in u64 range"_str)) };
        return { *value, None() };
    }
    auto index(ref<str> path) -> DocumentField<unsigned> {
        auto value = natural(path);
        if (value.error) return { invalid_index, rstd::move(value.error) };
        if (value.value >= u64(invalid_index))
            return { invalid_index, Some(error(path, "index exceeds supported range"_str)) };
        return { unsigned(value.value.to_primitive()), None() };
    }
    auto real(ref<str> path) -> DocumentField<float> {
        if (! kind(rstd::json::ValueKind::Number)) return { {}, bad(path, "expected number"_str) };
        auto value = reader_.parse_f64(rstd::json::FloatOverflow::Infinity);
        if (value.is_err()) {
            syntax(value.unwrap_err());
            return {};
        }
        auto number = float(value.unwrap().to_primitive());
        if (! rstd::f32(number).is_finite())
            return { {}, Some(error(path, "number is not a finite f32"_str)) };
        return { number, None() };
    }
    auto boolean(ref<str> path) -> DocumentField<bool> {
        if (! kind(rstd::json::ValueKind::Boolean))
            return { {}, bad(path, "expected boolean"_str) };
        auto value = reader_.parse_bool();
        if (value.is_err()) {
            syntax(value.unwrap_err());
            return {};
        }
        return { value.unwrap(), None() };
    }
    auto floats(ref<str> path) -> DocumentField<Vec<float>> {
        return array<float>(path, [&](ref<str> item, unsigned) {
            return real(item);
        });
    }
    auto indices(ref<str> path) -> DocumentField<Vec<unsigned>> {
        return array<unsigned>(path, [&](ref<str> item, unsigned) {
            return index(item);
        });
    }
    auto strings(ref<str> path) -> DocumentField<Vec<String>> {
        return array<String>(path, [&](ref<str> item, unsigned) {
            return text(item);
        });
    }
    auto vector2(ref<str> path, Vec2 fallback = {}) -> DocumentField<Vec2> {
        auto value = floats(path);
        if (value.error) return { fallback, rstd::move(value.error) };
        if (value.value.len() != usize(2))
            return { fallback, Some(error(path, "expected two components"_str)) };
        return { { at(value.value, 0), at(value.value, 1) }, None() };
    }
    auto vector3(ref<str> path, Vec3 fallback = {}) -> DocumentField<Vec3> {
        auto value = floats(path);
        if (value.error) return { fallback, rstd::move(value.error) };
        if (value.value.len() != usize(3))
            return { fallback, Some(error(path, "expected three components"_str)) };
        return { { at(value.value, 0), at(value.value, 1), at(value.value, 2) }, None() };
    }
    auto vector4(ref<str> path, Vec4 fallback = {}) -> DocumentField<Vec4> {
        auto value = floats(path);
        if (value.error) return { fallback, rstd::move(value.error) };
        if (value.value.len() != usize(4))
            return { fallback, Some(error(path, "expected four components"_str)) };
        return { { at(value.value, 0), at(value.value, 1), at(value.value, 2), at(value.value, 3) },
                 None() };
    }
    auto matrix(ref<str> path) -> DocumentField<Mat4> {
        auto value = floats(path);
        if (value.error) return { Mat4::Identity(), rstd::move(value.error) };
        if (value.value.len() != usize(16))
            return { Mat4::Identity(), Some(error(path, "expected sixteen components"_str)) };
        Mat4 result;
        for (unsigned i = 0; i < 16; ++i) result.m[i] = at(value.value, i);
        return { result, None() };
    }
    bool metadata(ref<str> key, SourceObjectId id, DocumentIssues& issues, ref<str> path) {
        if (id.index == invalid_index || fatal_) return false;
        auto& record = at(metadata_->records, id.index);
        if (key == "name"_str) {
            assign(record.name, text(path), issues, 29);
            return true;
        }
        if (key != "extras"_str && key != "extensions"_str) return false;
        auto start  = reader_.position();
        auto result = rstd::json::read_value(reader_);
        if (result.is_err()) {
            syntax(result.unwrap_err());
            return true;
        }
        auto bytes = reader_.position() - start;
        if (bytes > options_.max_metadata_bytes - metadata_bytes_) {
            fatal_ = Some(error(path, "metadata byte limit exceeded"_str, ErrorKind::Limit));
            return true;
        }
        metadata_bytes_ += bytes;
        auto value = result.unwrap();
        if (key == "extensions"_str) {
            bool valid        = value.is_object();
            record.extensions = Some(rstd::move(value));
            issues.set(31, valid ? None() : Some(error(path, "extensions must be an object"_str)));
        } else
            record.extras = Some(rstd::move(value));
        return true;
    }
    void name(String& target, SourceObjectId id) {
        if (id.index != invalid_index) target = at(metadata_->records, id.index).name.clone();
    }
    const rstd::json::Value* extension(SourceObjectId id, ref<str> key) {
        if (id.index == invalid_index) return nullptr;
        const auto& record = at(metadata_->records, id.index);
        if (! record.extensions) return nullptr;
        auto value = record.extensions->get(key);
        return value ? &**value : nullptr;
    }
    auto json_real(const rstd::json::Value& value, ref<str> path) -> DocumentField<float> {
        auto number = value.as_f64();
        if (! number) return { {}, Some(error(path, "expected number"_str)) };
        auto result = float(number->to_primitive());
        if (! rstd::f32(result).is_finite())
            return { {}, Some(error(path, "number is not finite f32"_str)) };
        return { result, None() };
    }
    auto json_index(const rstd::json::Value& value, ref<str> path) -> DocumentField<unsigned> {
        auto number = value.as_u64();
        if (number && *number < u64(invalid_index))
            return { unsigned(number->to_primitive()), None() };
        auto floating = value.as_f64();
        if (floating && floating->is_finite() && *floating >= rstd::f64(0) &&
            *floating < rstd::f64(invalid_index) && floating->floor() == *floating)
            return { unsigned(floating->to_primitive()), None() };
        return { invalid_index, Some(error(path, "expected valid index"_str)) };
    }
    template<unsigned N>
    auto json_floats(const rstd::json::Value& value, ref<str> path) -> DocumentField<Vec<float>> {
        DocumentField<Vec<float>> result;
        if (! value.is_array() || value.as_array().unwrap()->len() != usize(N)) {
            result.error = Some(error(path, "unexpected vector length"_str));
            return result;
        }
        for (unsigned i = 0; i < N; ++i) {
            auto item = json_real(value[usize(i)], path);
            if (! result.error && item.error) result.error = rstd::move(item.error);
            result.value.push(float(item.value));
        }
        return result;
    }
    auto texture_value(const rstd::json::Value& value, ref<str> path)
        -> DocumentField<TextureBinding> {
        DocumentField<TextureBinding> result;
        if (! value.is_object()) {
            result.error = Some(error(path, "texture info must be object"_str));
            return result;
        }
        auto image = value.get("index"_str);
        if (! image)
            result.error = Some(error(path, "texture index is missing"_str));
        else {
            auto parsed          = json_index(**image, path);
            result.value.texture = parsed.value;
            result.error         = rstd::move(parsed.error);
        }
        if (auto uv = value.get("texCoord"_str); uv) {
            auto parsed     = json_index(**uv, path);
            result.value.uv = parsed.value;
            if (! result.error) result.error = rstd::move(parsed.error);
        }
        auto transform = value["extensions"_str].get("KHR_texture_transform"_str);
        if (transform) transform_value(result, **transform, path);
        return result;
    }
    void transform_value(DocumentField<TextureBinding>& result,
                         const rstd::json::Value&       transform,
                         ref<str>                       path) {
        if (! transform.is_object()) {
            if (! result.error)
                result.error = Some(error(path, "texture transform must be object"_str));
            return;
        }
        if (auto offset = transform.get("offset"_str); offset) {
            auto value = json_floats<2>(**offset, path);
            if (! value.error)
                result.value.offset = { at(value.value, 0), at(value.value, 1) };
            else if (! result.error)
                result.error = rstd::move(value.error);
        }
        if (auto scale = transform.get("scale"_str); scale) {
            auto value = json_floats<2>(**scale, path);
            if (! value.error)
                result.value.scale = { at(value.value, 0), at(value.value, 1) };
            else if (! result.error)
                result.error = rstd::move(value.error);
        }
        if (auto rotation = transform.get("rotation"_str); rotation) {
            auto value            = json_real(**rotation, path);
            result.value.rotation = value.value;
            if (! result.error) result.error = rstd::move(value.error);
        }
        if (auto uv = transform.get("texCoord"_str); uv) {
            auto value      = json_index(**uv, path);
            result.value.uv = value.value;
            if (! result.error) result.error = rstd::move(value.error);
        }
    }
    auto texture_info(ref<str> path, unsigned index_value, unsigned parent)
        -> DocumentField<TextureBinding> {
        DocumentField<TextureBinding> result;
        auto           id = source(SourceObjectKind::TextureInfo, index_value, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "index"_str)
                assign(result.value.texture, index(child.as_str()), issues, 0);
            else if (key == "texCoord"_str)
                assign(result.value.uv, index(child.as_str()), issues, 1);
            else if (key == "scale"_str || key == "strength"_str)
                assign(result.value.strength, real(child.as_str()), issues, 2);
            else if (! metadata(key, id, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected texture info object"_str)) };
        required(issues, 0, path);
        result.error = issues.first();
        if (auto transform = extension(id, "KHR_texture_transform"_str); transform)
            transform_value(result, *transform, path);
        return result;
    }
    auto buffer(ref<str> path, unsigned i) -> DocumentField<doc::Buffer> {
        DocumentField<doc::Buffer> result;
        auto&                      value = result.value;
        value.source                     = source(SourceObjectKind::Buffer, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "byteLength"_str)
                assign(value.byte_length, natural(child.as_str()), issues, 0);
            else if (key == "uri"_str) {
                value.has_uri = true;
                assign(value.uri, text(child.as_str()), issues, 1);
            } else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected buffer object"_str)) };
        required(issues, 0, path);
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto buffer_view(ref<str> path, unsigned i) -> DocumentField<doc::BufferView> {
        DocumentField<doc::BufferView> result;
        auto&                          value = result.value;
        value.source = source(SourceObjectKind::BufferView, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "buffer"_str)
                assign(value.buffer, index(child.as_str()), issues, 0);
            else if (key == "byteOffset"_str)
                assign(value.offset, natural(child.as_str()), issues, 1);
            else if (key == "byteLength"_str)
                assign(value.length, natural(child.as_str()), issues, 2);
            else if (key == "byteStride"_str)
                assign(value.stride, natural(child.as_str()), issues, 3);
            else if (key == "target"_str)
                assign(value.target, index(child.as_str()), issues, 4);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected buffer view object"_str)) };
        required(issues, 0, path);
        required(issues, 2, path);
        if (issues.has(3) && value.stride == u64())
            issues.set(3, Some(error(path, "explicit byteStride must be positive"_str)));
        if (issues.has(4) && value.target == 0)
            issues.set(4, Some(error(path, "explicit target must name a buffer binding"_str)));
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto sparse_indices(ref<str> path, unsigned parent) -> DocumentField<doc::Sparse> {
        DocumentField<doc::Sparse> result;
        auto                       id = source(SourceObjectKind::SparseIndices, 0, parent);
        DocumentIssues             issues;
        bool                       valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "bufferView"_str)
                assign(result.value.indices_buffer_view, index(child.as_str()), issues, 0);
            else if (key == "byteOffset"_str)
                assign(result.value.indices_offset, natural(child.as_str()), issues, 1);
            else if (key == "componentType"_str)
                assign(result.value.indices_component_type, index(child.as_str()), issues, 2);
            else if (! metadata(key, id, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected sparse indices object"_str)) };
        required(issues, 0, path);
        required(issues, 2, path);
        result.error = issues.first();
        return result;
    }
    auto sparse_values(ref<str> path, unsigned parent) -> DocumentField<doc::Sparse> {
        DocumentField<doc::Sparse> result;
        auto                       id = source(SourceObjectKind::SparseValues, 0, parent);
        DocumentIssues             issues;
        bool                       valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "bufferView"_str)
                assign(result.value.values_buffer_view, index(child.as_str()), issues, 0);
            else if (key == "byteOffset"_str)
                assign(result.value.values_offset, natural(child.as_str()), issues, 1);
            else if (! metadata(key, id, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected sparse values object"_str)) };
        required(issues, 0, path);
        result.error = issues.first();
        return result;
    }
    auto sparse(ref<str> path, unsigned parent) -> DocumentField<doc::Sparse> {
        DocumentField<doc::Sparse> result;
        auto&                      value = result.value;
        value.source                     = source(SourceObjectKind::Sparse, 0, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "count"_str)
                assign(value.count, natural(child.as_str()), issues, 0);
            else if (key == "indices"_str) {
                auto parsed                  = sparse_indices(child.as_str(), value.source.index);
                value.indices_buffer_view    = parsed.value.indices_buffer_view;
                value.indices_offset         = parsed.value.indices_offset;
                value.indices_component_type = parsed.value.indices_component_type;
                issues.set(1, rstd::move(parsed.error));
            } else if (key == "values"_str) {
                auto parsed              = sparse_values(child.as_str(), value.source.index);
                value.values_buffer_view = parsed.value.values_buffer_view;
                value.values_offset      = parsed.value.values_offset;
                issues.set(2, rstd::move(parsed.error));
            } else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected sparse object"_str)) };
        required(issues, 0, path);
        required(issues, 1, path);
        required(issues, 2, path);
        result.error = issues.first();
        return result;
    }
    auto shape(ref<str> path) -> DocumentField<doc::Shape> {
        auto parsed = text(path);
        if (parsed.error) return { {}, rstd::move(parsed.error) };
        ref<str> labels[] { "SCALAR"_str, "VEC2"_str, "VEC3"_str, "VEC4"_str,
                            "MAT2"_str,   "MAT3"_str, "MAT4"_str };
        for (unsigned i = 0; i < 7; ++i)
            if (parsed.value == labels[i]) return { doc::Shape(i), None() };
        return { {}, Some(error(path, "unknown accessor type"_str)) };
    }
    auto accessor(ref<str> path, unsigned i) -> DocumentField<doc::Accessor> {
        DocumentField<doc::Accessor> result;
        auto&                        value = result.value;
        value.source = source(SourceObjectKind::Accessor, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "bufferView"_str)
                assign(value.buffer_view, index(child.as_str()), issues, 0);
            else if (key == "byteOffset"_str)
                assign(value.offset, natural(child.as_str()), issues, 1);
            else if (key == "componentType"_str)
                assign(value.component_type, index(child.as_str()), issues, 2);
            else if (key == "count"_str)
                assign(value.count, natural(child.as_str()), issues, 3);
            else if (key == "type"_str)
                assign(value.shape, shape(child.as_str()), issues, 4);
            else if (key == "normalized"_str)
                assign(value.normalized, boolean(child.as_str()), issues, 5);
            else if (key == "min"_str)
                assign(value.minimum, floats(child.as_str()), issues, 6);
            else if (key == "max"_str)
                assign(value.maximum, floats(child.as_str()), issues, 7);
            else if (key == "sparse"_str) {
                auto parsed  = sparse(child.as_str(), value.source.index);
                value.sparse = Some(rstd::move(parsed.value));
                issues.set(8, rstd::move(parsed.error));
            } else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected accessor object"_str)) };
        required(issues, 2, path);
        required(issues, 3, path);
        required(issues, 4, path);
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto image(ref<str> path, unsigned i) -> DocumentField<doc::Image> {
        DocumentField<doc::Image> result;
        auto&                     value = result.value;
        value.source                    = source(SourceObjectKind::Image, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "uri"_str) {
                value.has_uri = true;
                assign(value.uri, text(child.as_str()), issues, 0);
            } else if (key == "bufferView"_str)
                assign(value.buffer_view, index(child.as_str()), issues, 1);
            else if (key == "mimeType"_str)
                assign(value.mime_type, text(child.as_str()), issues, 2);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected image object"_str)) };
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto sampler(ref<str> path, unsigned i) -> DocumentField<doc::Sampler> {
        DocumentField<doc::Sampler> result;
        auto&                       value = result.value;
        value.source = source(SourceObjectKind::Sampler, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "magFilter"_str)
                assign(value.mag_filter, index(child.as_str()), issues, 0);
            else if (key == "minFilter"_str)
                assign(value.min_filter, index(child.as_str()), issues, 1);
            else if (key == "wrapS"_str)
                assign(value.wrap_s, index(child.as_str()), issues, 2);
            else if (key == "wrapT"_str)
                assign(value.wrap_t, index(child.as_str()), issues, 3);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected sampler object"_str)) };
        if (issues.has(0) && value.mag_filter == 0)
            issues.set(0, Some(error(path, "explicit magFilter must be a valid mode"_str)));
        if (issues.has(1) && value.min_filter == 0)
            issues.set(1, Some(error(path, "explicit minFilter must be a valid mode"_str)));
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto texture(ref<str> path, unsigned i) -> DocumentField<doc::Texture> {
        DocumentField<doc::Texture> result;
        auto&                       value = result.value;
        value.source = source(SourceObjectKind::Texture, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "source"_str)
                assign(value.image, index(child.as_str()), issues, 0);
            else if (key == "sampler"_str)
                assign(value.sampler, index(child.as_str()), issues, 1);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected texture object"_str)) };
        value.has_compressed_source = extension(value.source, "KHR_texture_basisu"_str) ||
                                      extension(value.source, "EXT_texture_webp"_str) ||
                                      extension(value.source, "MSFT_texture_dds"_str);
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto pbr(ref<str> path, unsigned parent) -> DocumentField<vecgltf::Material> {
        DocumentField<vecgltf::Material> result;
        auto           id = source(SourceObjectKind::PbrMetallicRoughness, 0, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "baseColorFactor"_str)
                assign(result.value.base_color, vector4(child.as_str(), { 1, 1, 1, 1 }), issues, 0);
            else if (key == "metallicFactor"_str)
                assign(result.value.metallic, real(child.as_str()), issues, 1);
            else if (key == "roughnessFactor"_str)
                assign(result.value.roughness, real(child.as_str()), issues, 2);
            else if (key == "baseColorTexture"_str)
                assign(result.value.base, texture_info(child.as_str(), 0, id.index), issues, 3);
            else if (key == "metallicRoughnessTexture"_str)
                assign(result.value.physical, texture_info(child.as_str(), 1, id.index), issues, 4);
            else if (! metadata(key, id, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected pbr object"_str)) };
        result.error = issues.first();
        return result;
    }
    void material_extensions(DocumentField<doc::Material>& result, ref<str> path) {
        auto id                 = result.value.source;
        auto unlit              = extension(id, "KHR_materials_unlit"_str);
        result.value.core.unlit = unlit != nullptr;
        if (unlit && ! unlit->is_object() && ! result.error)
            result.error = Some(error(path, "unlit extension must be object"_str));
        if (auto emission = extension(id, "KHR_materials_emissive_strength"_str); emission) {
            if (! emission->is_object()) {
                if (! result.error)
                    result.error = Some(error(path, "invalid emissive strength object"_str));
            } else if (auto factor = emission->get("emissiveStrength"_str); factor) {
                auto parsed                    = json_real(**factor, path);
                result.value.emissive_strength = parsed.value;
                if (! result.error) result.error = rstd::move(parsed.error);
            }
        }
        auto sg = extension(id, "KHR_materials_pbrSpecularGlossiness"_str);
        if (! sg) return;
        doc::SpecularGlossiness value;
        if (! sg->is_object()) {
            if (! result.error)
                result.error = Some(error(path, "invalid specular glossiness object"_str));
            return;
        }
        if (auto factor = sg->get("diffuseFactor"_str); factor) {
            auto parsed = json_floats<4>(**factor, path);
            if (! parsed.error)
                value.diffuse = { at(parsed.value, 0),
                                  at(parsed.value, 1),
                                  at(parsed.value, 2),
                                  at(parsed.value, 3) };
            if (! result.error) result.error = rstd::move(parsed.error);
        }
        if (auto factor = sg->get("specularFactor"_str); factor) {
            auto parsed = json_floats<3>(**factor, path);
            if (! parsed.error)
                value.specular = { at(parsed.value, 0), at(parsed.value, 1), at(parsed.value, 2) };
            if (! result.error) result.error = rstd::move(parsed.error);
        }
        if (auto factor = sg->get("glossinessFactor"_str); factor) {
            auto parsed      = json_real(**factor, path);
            value.glossiness = parsed.value;
            if (! result.error) result.error = rstd::move(parsed.error);
        }
        if (auto binding = sg->get("diffuseTexture"_str); binding) {
            auto parsed           = texture_value(**binding, path);
            value.diffuse_texture = parsed.value;
            if (! result.error) result.error = rstd::move(parsed.error);
        }
        if (auto binding = sg->get("specularGlossinessTexture"_str); binding) {
            auto parsed            = texture_value(**binding, path);
            value.physical_texture = parsed.value;
            if (! result.error) result.error = rstd::move(parsed.error);
        }
        result.value.specular_glossiness = Some(rstd::move(value));
    }
    auto material(ref<str> path, unsigned i) -> DocumentField<doc::Material> {
        DocumentField<doc::Material> result;
        auto&                        value = result.value;
        value.source = source(SourceObjectKind::Material, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "pbrMetallicRoughness"_str) {
                auto parsed           = pbr(child.as_str(), value.source.index);
                value.core.base_color = parsed.value.base_color;
                value.core.metallic   = parsed.value.metallic;
                value.core.roughness  = parsed.value.roughness;
                value.core.base       = parsed.value.base;
                value.core.physical   = parsed.value.physical;
                issues.set(0, rstd::move(parsed.error));
            } else if (key == "emissiveFactor"_str)
                assign(value.core.emissive, vector3(child.as_str()), issues, 1);
            else if (key == "emissiveTexture"_str)
                assign(value.core.emission,
                       texture_info(child.as_str(), 4, value.source.index),
                       issues,
                       2);
            else if (key == "normalTexture"_str)
                assign(value.core.normal,
                       texture_info(child.as_str(), 2, value.source.index),
                       issues,
                       3);
            else if (key == "occlusionTexture"_str)
                assign(value.core.occlusion,
                       texture_info(child.as_str(), 3, value.source.index),
                       issues,
                       4);
            else if (key == "alphaCutoff"_str)
                assign(value.core.alpha_cutoff, real(child.as_str()), issues, 5);
            else if (key == "doubleSided"_str)
                assign(value.core.double_sided, boolean(child.as_str()), issues, 6);
            else if (key == "alphaMode"_str) {
                auto parsed      = text(child.as_str());
                value.core.alpha = parsed.value == "BLEND"_str  ? AlphaMode::Blend
                                   : parsed.value == "MASK"_str ? AlphaMode::Mask
                                                                : AlphaMode::Opaque;
                if (! parsed.error && parsed.value != "OPAQUE"_str && parsed.value != "MASK"_str &&
                    parsed.value != "BLEND"_str)
                    parsed.error = Some(error(child.as_str(), "invalid alpha mode"_str));
                issues.set(7, rstd::move(parsed.error));
            } else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected material object"_str)) };
        name(value.core.name, value.source);
        result.error = issues.first();
        material_extensions(result, path);
        return result;
    }
    auto attributes(ref<str> path) -> DocumentField<Vec<doc::Attribute>> {
        DocumentField<Vec<doc::Attribute>> result;
        DocumentIssues                     issues;
        bool                               valid = object(path, [&](ref<str> key) {
            auto     child    = location(path, key);
            auto     parsed   = index(child.as_str());
            unsigned position = unsigned(result.value.len().to_primitive());
            for (unsigned i = 0; i < count(result.value); ++i)
                if (at(result.value, i).name == key) {
                    position = i;
                    break;
                }
            if (position == count(result.value))
                result.value.push({ String::make(key), parsed.value });
            else
                at(result.value, position).accessor = parsed.value;
            issues.set(position, rstd::move(parsed.error));
        });
        if (! valid) return { {}, Some(error(path, "expected attributes object"_str)) };
        result.error = issues.first();
        return result;
    }
    auto target(ref<str> path, unsigned i, unsigned parent) -> DocumentField<doc::MorphTarget> {
        DocumentField<doc::MorphTarget> result;
        result.value.source     = source(SourceObjectKind::MorphTarget, i, parent);
        auto parsed             = attributes(path);
        result.value.attributes = rstd::move(parsed.value);
        result.error            = rstd::move(parsed.error);
        return result;
    }
    auto primitive(ref<str> path, unsigned i, unsigned parent) -> DocumentField<doc::Primitive> {
        DocumentField<doc::Primitive> result;
        auto&                         value = result.value;
        value.source                        = source(SourceObjectKind::Primitive, i, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "attributes"_str)
                assign(value.attributes, attributes(child.as_str()), issues, 0);
            else if (key == "indices"_str)
                assign(value.indices, index(child.as_str()), issues, 1);
            else if (key == "material"_str)
                assign(value.material, index(child.as_str()), issues, 2);
            else if (key == "mode"_str)
                assign(value.mode, index(child.as_str()), issues, 3);
            else if (key == "targets"_str)
                assign(value.targets,
                       array<doc::MorphTarget>(child.as_str(),
                                               [&](ref<str> item, unsigned n) {
                                                   return target(item, n, value.source.index);
                                               }),
                       issues,
                       4);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected primitive object"_str)) };
        required(issues, 0, path);
        value.has_draco = extension(value.source, "KHR_draco_mesh_compression"_str) != nullptr;
        result.error    = issues.first();
        return result;
    }
    auto mesh(ref<str> path, unsigned i) -> DocumentField<doc::Mesh> {
        DocumentField<doc::Mesh> result;
        auto&                    value = result.value;
        value.source                   = source(SourceObjectKind::Mesh, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "primitives"_str)
                assign(value.primitives,
                       array<doc::Primitive>(child.as_str(),
                                             [&](ref<str> item, unsigned n) {
                                                 return primitive(item, n, value.source.index);
                                             }),
                       issues,
                       0);
            else if (key == "weights"_str)
                assign(value.weights, floats(child.as_str()), issues, 1);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected mesh object"_str)) };
        required(issues, 0, path);
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto node(ref<str> path, unsigned i) -> DocumentField<doc::Node> {
        DocumentField<doc::Node> result;
        auto&                    value = result.value;
        value.source                   = source(SourceObjectKind::Node, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "children"_str)
                assign(value.children, indices(child.as_str()), issues, 0);
            else if (key == "mesh"_str)
                assign(value.mesh, index(child.as_str()), issues, 1);
            else if (key == "skin"_str)
                assign(value.skin, index(child.as_str()), issues, 2);
            else if (key == "camera"_str)
                assign(value.camera, index(child.as_str()), issues, 3);
            else if (key == "translation"_str) {
                value.has_trs = true;
                assign(value.translation, vector3(child.as_str()), issues, 4);
            } else if (key == "rotation"_str) {
                value.has_trs = true;
                assign(value.rotation, vector4(child.as_str(), { 0, 0, 0, 1 }), issues, 5);
            } else if (key == "scale"_str) {
                value.has_trs = true;
                assign(value.scale, vector3(child.as_str(), { 1, 1, 1 }), issues, 6);
            } else if (key == "matrix"_str) {
                value.has_matrix = true;
                assign(value.matrix, matrix(child.as_str()), issues, 7);
            } else if (key == "weights"_str)
                assign(value.weights, floats(child.as_str()), issues, 8);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected node object"_str)) };
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto scene(ref<str> path, unsigned i) -> DocumentField<doc::Scene> {
        DocumentField<doc::Scene> result;
        auto&                     value = result.value;
        value.source                    = source(SourceObjectKind::Scene, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "nodes"_str)
                assign(value.roots, indices(child.as_str()), issues, 0);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected scene object"_str)) };
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto skin(ref<str> path, unsigned i) -> DocumentField<doc::Skin> {
        DocumentField<doc::Skin> result;
        auto&                    value = result.value;
        value.source                   = source(SourceObjectKind::Skin, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "joints"_str)
                assign(value.joints, indices(child.as_str()), issues, 0);
            else if (key == "inverseBindMatrices"_str)
                assign(value.inverse_bind, index(child.as_str()), issues, 1);
            else if (key == "skeleton"_str)
                assign(value.skeleton, index(child.as_str()), issues, 2);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected skin object"_str)) };
        required(issues, 0, path);
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto projection(ref<str> path, bool orthographic, unsigned parent)
        -> DocumentField<CameraProjection> {
        DocumentField<CameraProjection> result;
        result.value.orthographic = orthographic;
        auto id =
            source(orthographic ? SourceObjectKind::Orthographic : SourceObjectKind::Perspective,
                   0,
                   parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "znear"_str)
                assign(result.value.near, real(child.as_str()), issues, 0);
            else if (key == "zfar"_str)
                assign(result.value.far, real(child.as_str()), issues, 1);
            else if (key == "yfov"_str)
                assign(result.value.yfov, real(child.as_str()), issues, 2);
            else if (key == "aspectRatio"_str)
                assign(result.value.aspect, real(child.as_str()), issues, 3);
            else if (key == "xmag"_str)
                assign(result.value.xmag, real(child.as_str()), issues, 4);
            else if (key == "ymag"_str)
                assign(result.value.ymag, real(child.as_str()), issues, 5);
            else if (! metadata(key, id, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected projection object"_str)) };
        required(issues, 0, path);
        if (orthographic) {
            required(issues, 1, path);
            required(issues, 4, path);
            required(issues, 5, path);
        } else {
            required(issues, 2, path);
            if (issues.has(1) && result.value.far <= result.value.near)
                issues.set(1, Some(error(path, "explicit far plane must exceed near plane"_str)));
            if (issues.has(3) && result.value.aspect <= 0)
                issues.set(3, Some(error(path, "explicit aspect ratio must be positive"_str)));
        }
        result.error = issues.first();
        return result;
    }
    auto camera(ref<str> path, unsigned i) -> DocumentField<doc::Camera> {
        DocumentField<doc::Camera> result;
        auto&                      value = result.value;
        value.source                     = source(SourceObjectKind::Camera, i, data_->source.index);
        DocumentIssues   issues;
        String           type;
        CameraProjection perspective, orthographic;
        bool             valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "type"_str)
                assign(type, text(child.as_str()), issues, 0);
            else if (key == "perspective"_str)
                assign(
                    perspective, projection(child.as_str(), false, value.source.index), issues, 1);
            else if (key == "orthographic"_str)
                assign(
                    orthographic, projection(child.as_str(), true, value.source.index), issues, 2);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected camera object"_str)) };
        required(issues, 0, path);
        if (type == "perspective"_str) {
            required(issues, 1, path);
            value.projection = perspective;
        } else if (type == "orthographic"_str) {
            required(issues, 2, path);
            value.projection = orthographic;
        } else
            issues.set(0, Some(error(path, "invalid camera type"_str)));
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto animation_sampler(ref<str> path, unsigned i, unsigned parent)
        -> DocumentField<doc::AnimationSampler> {
        DocumentField<doc::AnimationSampler> result;
        auto&                                value = result.value;
        value.source = source(SourceObjectKind::AnimationSampler, i, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "input"_str)
                assign(value.input, index(child.as_str()), issues, 0);
            else if (key == "output"_str)
                assign(value.output, index(child.as_str()), issues, 1);
            else if (key == "interpolation"_str) {
                auto parsed         = text(child.as_str());
                value.interpolation = parsed.value == "STEP"_str ? Interpolation::Step
                                      : parsed.value == "CUBICSPLINE"_str
                                          ? Interpolation::CubicSpline
                                          : Interpolation::Linear;
                if (! parsed.error && parsed.value != "LINEAR"_str && parsed.value != "STEP"_str &&
                    parsed.value != "CUBICSPLINE"_str)
                    parsed.error = Some(error(child.as_str(), "invalid interpolation"_str));
                issues.set(2, rstd::move(parsed.error));
            } else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected animation sampler object"_str)) };
        required(issues, 0, path);
        required(issues, 1, path);
        result.error = issues.first();
        return result;
    }
    auto animation_target(ref<str> path, unsigned parent) -> DocumentField<doc::AnimationChannel> {
        DocumentField<doc::AnimationChannel> result;
        auto           id = source(SourceObjectKind::AnimationTarget, 0, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "node"_str)
                assign(result.value.node, index(child.as_str()), issues, 0);
            else if (key == "path"_str) {
                auto parsed       = text(child.as_str());
                result.value.path = parsed.value == "rotation"_str  ? AnimationPath::Rotation
                                    : parsed.value == "scale"_str   ? AnimationPath::Scale
                                    : parsed.value == "weights"_str ? AnimationPath::Weights
                                                                    : AnimationPath::Translation;
                if (! parsed.error && parsed.value != "translation"_str &&
                    parsed.value != "rotation"_str && parsed.value != "scale"_str &&
                    parsed.value != "weights"_str)
                    parsed.error = Some(error(child.as_str(),
                                              "unknown animation target path"_str,
                                              ErrorKind::Unsupported));
                issues.set(1, rstd::move(parsed.error));
            } else if (! metadata(key, id, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected animation target object"_str)) };
        required(issues, 1, path);
        result.error = issues.first();
        return result;
    }
    auto animation_channel(ref<str> path, unsigned i, unsigned parent)
        -> DocumentField<doc::AnimationChannel> {
        DocumentField<doc::AnimationChannel> result;
        auto&                                value = result.value;
        value.source = source(SourceObjectKind::AnimationChannel, i, parent);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "sampler"_str)
                assign(value.sampler, index(child.as_str()), issues, 0);
            else if (key == "target"_str) {
                auto parsed = animation_target(child.as_str(), value.source.index);
                value.node  = parsed.value.node;
                value.path  = parsed.value.path;
                issues.set(1, rstd::move(parsed.error));
            } else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected animation channel object"_str)) };
        required(issues, 0, path);
        required(issues, 1, path);
        result.error = issues.first();
        return result;
    }
    auto animation(ref<str> path, unsigned i) -> DocumentField<doc::Animation> {
        DocumentField<doc::Animation> result;
        auto&                         value = result.value;
        value.source = source(SourceObjectKind::Animation, i, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "samplers"_str)
                assign(value.samplers,
                       array<doc::AnimationSampler>(child.as_str(),
                                                    [&](ref<str> item, unsigned n) {
                                                        return animation_sampler(
                                                            item, n, value.source.index);
                                                    }),
                       issues,
                       0);
            else if (key == "channels"_str)
                assign(value.channels,
                       array<doc::AnimationChannel>(child.as_str(),
                                                    [&](ref<str> item, unsigned n) {
                                                        return animation_channel(
                                                            item, n, value.source.index);
                                                    }),
                       issues,
                       1);
            else if (! metadata(key, value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected animation object"_str)) };
        required(issues, 0, path);
        required(issues, 1, path);
        name(value.name, value.source);
        result.error = issues.first();
        return result;
    }
    auto asset(ref<str> path) -> DocumentField<AssetMetadata> {
        DocumentField<AssetMetadata> result;
        result.value.source = source(SourceObjectKind::Asset, 0, data_->source.index);
        DocumentIssues issues;
        bool           valid = object(path, [&](ref<str> key) {
            auto child = location(path, key);
            if (key == "version"_str)
                assign(result.value.version, text(child.as_str()), issues, 0);
            else if (key == "minVersion"_str)
                assign(result.value.min_version, text(child.as_str()), issues, 1);
            else if (key == "generator"_str)
                assign(result.value.generator, text(child.as_str()), issues, 2);
            else if (key == "copyright"_str)
                assign(result.value.copyright, text(child.as_str()), issues, 3);
            else if (! metadata(key, result.value.source, issues, child.as_str()))
                skip();
        });
        if (! valid) return { {}, Some(error(path, "expected asset object"_str)) };
        required(issues, 0, path);
        result.error = issues.first();
        return result;
    }

public:
    DocumentReader(ref<str>           json,
                   Arc<doc::Data>     data,
                   Arc<MetadataStore> metadata,
                   DocumentOptions    options,
                   ref<str>           source)
        : reader_(json, { .max_depth = options.max_depth }),
          data_(rstd::move(data)),
          metadata_(rstd::move(metadata)),
          options_(options),
          source_(String::make(source)) {}
    auto read() -> Result<empty, Error> {
        data_->source = source(SourceObjectKind::Document, 0, invalid_index);
        DocumentIssues issues;
        bool           valid = object(""_str, [&](ref<str> key) {
            auto path = String::make(key);
            if (key == "asset"_str)
                assign(metadata_->asset, asset(path.as_str()), issues, 0);
            else if (key == "buffers"_str)
                assign(data_->buffers,
                       array<doc::Buffer>(path.as_str(),
                                          [&](ref<str> item, unsigned i) {
                                              return buffer(item, i);
                                          }),
                       issues,
                       1);
            else if (key == "bufferViews"_str)
                assign(data_->buffer_views,
                       array<doc::BufferView>(path.as_str(),
                                              [&](ref<str> item, unsigned i) {
                                                  return buffer_view(item, i);
                                              }),
                       issues,
                       2);
            else if (key == "accessors"_str)
                assign(data_->accessors,
                       array<doc::Accessor>(path.as_str(),
                                            [&](ref<str> item, unsigned i) {
                                                return accessor(item, i);
                                            }),
                       issues,
                       3);
            else if (key == "images"_str)
                assign(data_->images,
                       array<doc::Image>(path.as_str(),
                                         [&](ref<str> item, unsigned i) {
                                             return image(item, i);
                                         }),
                       issues,
                       4);
            else if (key == "samplers"_str)
                assign(data_->samplers,
                       array<doc::Sampler>(path.as_str(),
                                           [&](ref<str> item, unsigned i) {
                                               return sampler(item, i);
                                           }),
                       issues,
                       5);
            else if (key == "textures"_str)
                assign(data_->textures,
                       array<doc::Texture>(path.as_str(),
                                           [&](ref<str> item, unsigned i) {
                                               return texture(item, i);
                                           }),
                       issues,
                       6);
            else if (key == "materials"_str)
                assign(data_->materials,
                       array<doc::Material>(path.as_str(),
                                            [&](ref<str> item, unsigned i) {
                                                return material(item, i);
                                            }),
                       issues,
                       7);
            else if (key == "meshes"_str)
                assign(data_->meshes,
                       array<doc::Mesh>(path.as_str(),
                                        [&](ref<str> item, unsigned i) {
                                            return mesh(item, i);
                                        }),
                       issues,
                       8);
            else if (key == "nodes"_str)
                assign(data_->nodes,
                       array<doc::Node>(path.as_str(),
                                        [&](ref<str> item, unsigned i) {
                                            return node(item, i);
                                        }),
                       issues,
                       9);
            else if (key == "scenes"_str)
                assign(data_->scenes,
                       array<doc::Scene>(path.as_str(),
                                         [&](ref<str> item, unsigned i) {
                                             return scene(item, i);
                                         }),
                       issues,
                       10);
            else if (key == "skins"_str)
                assign(data_->skins,
                       array<doc::Skin>(path.as_str(),
                                        [&](ref<str> item, unsigned i) {
                                            return skin(item, i);
                                        }),
                       issues,
                       11);
            else if (key == "cameras"_str)
                assign(data_->cameras,
                       array<doc::Camera>(path.as_str(),
                                          [&](ref<str> item, unsigned i) {
                                              return camera(item, i);
                                          }),
                       issues,
                       12);
            else if (key == "animations"_str)
                assign(data_->animations,
                       array<doc::Animation>(path.as_str(),
                                             [&](ref<str> item, unsigned i) {
                                                 return animation(item, i);
                                             }),
                       issues,
                       13);
            else if (key == "scene"_str)
                assign(data_->default_scene, index(path.as_str()), issues, 14);
            else if (key == "extensionsUsed"_str)
                assign(data_->extensions_used, strings(path.as_str()), issues, 15);
            else if (key == "extensionsRequired"_str)
                assign(data_->extensions_required, strings(path.as_str()), issues, 16);
            else if (! metadata(key, data_->source, issues, path.as_str()))
                skip();
        });
        if (fatal_) return Err(rstd::move(fatal_).unwrap());
        if (! valid) return Err(error("document"_str, "root must be an object"_str));
        auto finished = reader_.finish();
        if (finished.is_err()) {
            syntax(finished.unwrap_err());
            return Err(rstd::move(fatal_).unwrap());
        }
        required(issues, 0, "asset"_str);
        auto issue = issues.first();
        if (issue) return Err(rstd::move(issue).unwrap());
        return Ok(empty {});
    }
};

template<class Function>
void document_sources(doc::Data& data, MetadataStore& metadata, Function function) {
    function(data.source);
    function(metadata.asset.source);
    auto objects = [&](auto& values) {
        for (auto& value : values) function(value.source);
    };
    objects(data.buffers);
    objects(data.buffer_views);
    objects(data.accessors);
    for (auto& value : data.accessors)
        if (value.sparse) function(value.sparse->source);
    objects(data.images);
    objects(data.samplers);
    objects(data.textures);
    objects(data.materials);
    objects(data.meshes);
    for (auto& mesh : data.meshes)
        for (auto& primitive : mesh.primitives) {
            function(primitive.source);
            objects(primitive.targets);
        }
    objects(data.nodes);
    objects(data.scenes);
    objects(data.skins);
    objects(data.cameras);
    objects(data.animations);
    for (auto& animation : data.animations) {
        objects(animation.samplers);
        objects(animation.channels);
    }
}
void compact_metadata(doc::Data& data, MetadataStore& metadata) {
    auto&   records = metadata.records;
    Vec<u8> keep;
    keep.resize(records.len(), u8());
    document_sources(data, metadata, [&](SourceObjectId& id) {
        if (id.index < count(records)) keep[usize(id.index)] = u8(1);
    });
    Vec<u64> selected;
    selected.resize(records.len(), u64());
    auto children = [&](bool textures) {
        for (unsigned i = count(records); i > 0;) {
            --i;
            const auto& record = at(records, i);
            if (record.parent >= count(records) || u8(keep[usize(record.parent)]) == u8()) continue;
            unsigned slot = invalid_index;
            switch (record.kind) {
            case SourceObjectKind::TextureInfo:
                if (textures && record.index < 5) slot = record.index;
                break;
            case SourceObjectKind::SparseIndices:
                if (! textures) slot = 5;
                break;
            case SourceObjectKind::SparseValues:
                if (! textures) slot = 6;
                break;
            case SourceObjectKind::PbrMetallicRoughness:
                if (! textures) slot = 7;
                break;
            case SourceObjectKind::Perspective:
                if (! textures) slot = 8;
                break;
            case SourceObjectKind::Orthographic:
                if (! textures) slot = 9;
                break;
            case SourceObjectKind::AnimationTarget:
                if (! textures) slot = 10;
                break;
            default: break;
            }
            if (slot == invalid_index) continue;
            auto  bit  = u64(1) << u64(slot);
            auto& seen = selected[usize(record.parent)];
            if ((seen & bit) != u64()) continue;
            seen           = seen | bit;
            keep[usize(i)] = u8(1);
        }
    };
    children(false);
    children(true);
    Vec<unsigned> remap;
    remap.resize(records.len(), unsigned(invalid_index));
    Vec<SourceMetadata> compact;
    for (unsigned i = 0; i < count(records); ++i)
        if (u8(keep[usize(i)]) != u8()) {
            at(remap, i) = count(compact);
            compact.push(rstd::move(at(records, i)));
        }
    for (auto& record : compact)
        if (record.parent != invalid_index) record.parent = at(remap, record.parent);
    document_sources(data, metadata, [&](SourceObjectId& id) {
        if (id.index != invalid_index) id.index = at(remap, id.index);
    });
    records = rstd::move(compact);
}

auto ParseDocument(DocumentBytes bytes, DocumentOptions options, ref<str> source)
    -> Result<Document, Error> {
    auto failure = [&](ErrorKind kind, ref<str> location, ref<str> message) {
        return Error { kind, String::make(source), String::make(location), String::make(message) };
    };
    if (bytes->len() > options.max_document_bytes)
        return Err(failure(ErrorKind::Limit, "document"_str, "document byte limit exceeded"_str));
    auto read_u32 = [&](rstd::size_t offset) {
        unsigned result {};
        for (unsigned i = 0; i < 4; ++i)
            result |= unsigned(u8((*bytes)[usize(offset + i)]).to_primitive()) << (i * 8);
        return result;
    };
    static rstd::sync::atomic::Atomic<u64> next { u64(1) };
    auto                                   metadata = Arc<MetadataStore>::make();
    metadata->identity = next.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed);
    auto data          = Arc<doc::Data>::make(bytes.clone(), metadata.clone());
    data->label        = String::make(source);
    usize json_offset {}, json_length = bytes->len();
    if (bytes->len() >= usize(4) && read_u32(0) == 0x46546c67u) {
        if (bytes->len() < usize(12) || read_u32(4) != 2 || usize(read_u32(8)) != bytes->len())
            return Err(failure(ErrorKind::Parse, "GLB.header"_str, "invalid GLB header"_str));
        rstd::size_t offset = 12;
        bool         found_json {}, found_bin {};
        unsigned     chunk_index {};
        while (offset < bytes->len().to_primitive()) {
            if (bytes->len().to_primitive() - offset < 8)
                return Err(failure(ErrorKind::Parse, "GLB.chunk"_str, "truncated GLB chunk"_str));
            auto length = read_u32(offset), type = read_u32(offset + 4);
            offset += 8;
            if ((length & 3) || rstd::size_t(length) > bytes->len().to_primitive() - offset)
                return Err(
                    failure(ErrorKind::Parse, "GLB.chunk"_str, "invalid GLB chunk length"_str));
            if (! found_json && type != 0x4e4f534au)
                return Err(
                    failure(ErrorKind::Parse, "GLB.chunk"_str, "first chunk must be JSON"_str));
            if (type == 0x4e4f534au) {
                if (found_json)
                    return Err(
                        failure(ErrorKind::Parse, "GLB.JSON"_str, "duplicate JSON chunk"_str));
                found_json  = true;
                json_offset = usize(offset);
                json_length = usize(length);
            } else if (type == 0x004e4942u) {
                if (found_bin || chunk_index != 1)
                    return Err(failure(ErrorKind::Parse,
                                       "GLB.BIN"_str,
                                       "BIN must be the second and unique BIN chunk"_str));
                found_bin    = true;
                data->binary = Some(doc::ByteRange { u64(offset), u64(length) });
            }
            offset += length;
            ++chunk_index;
        }
        if (! found_json)
            return Err(failure(ErrorKind::Parse, "GLB.JSON"_str, "missing JSON chunk"_str));
    }
    if (json_length >= usize(3) && u8((*bytes)[json_offset]) == u8(0xef) &&
        u8((*bytes)[json_offset + usize(1)]) == u8(0xbb) &&
        u8((*bytes)[json_offset + usize(2)]) == u8(0xbf)) {
        json_offset += usize(3);
        json_length -= usize(3);
    }
    auto input = slice<u8>::from_raw_parts(bytes->data() + json_offset.to_primitive(), json_length);
    auto text  = rstd::str_::from_utf8(input);
    if (text.is_err())
        return Err(failure(ErrorKind::Parse, "document"_str, "JSON is not valid UTF-8"_str));
    DocumentReader reader(text.unwrap(), data.clone(), metadata.clone(), options, source);
    auto           parsed = reader.read();
    if (parsed.is_err()) return Err(parsed.unwrap_err());
    compact_metadata(*data, *metadata);
    for (unsigned parent = 0; parent < count(data->nodes); ++parent)
        for (auto child : at(data->nodes, parent).children)
            if (child < count(data->nodes)) at(data->nodes, child).parent = parent;
    Document result(rstd::move(data));
    auto     validated = ValidateDocument(result);
    if (validated.is_err()) return Err(validated.unwrap_err());
    return Ok(rstd::move(result));
}
auto ValidateDocument(const Document& document) -> Result<empty, Error> {
    const auto& data = document.data();
    auto fail = [&](ref<str> location, ref<str> message, ErrorKind kind = ErrorKind::InvalidAsset) {
        return Error { kind, data.label.clone(), String::make(location), String::make(message) };
    };
    auto in_range = [](unsigned index, rstd::size_t size) {
        return index == invalid_index || index < size;
    };
    const auto& info = data.metadata->asset;
    if (info.version != "2.0"_str)
        return Err(
            fail("asset.version"_str, "only glTF 2.0 is supported"_str, ErrorKind::Unsupported));
    if (! info.min_version.is_empty() && info.min_version != "2.0"_str)
        return Err(fail("asset.minVersion"_str,
                        "minimum glTF version is unsupported"_str,
                        ErrorKind::Unsupported));
    for (const auto& required : data.extensions_required) {
        bool found {};
        for (const auto& used : data.extensions_used) found = found || used == required;
        if (! found)
            return Err(fail("extensionsRequired"_str,
                            "required extension is absent from extensionsUsed"_str));
    }
    for (unsigned i = 0; i < count(data.buffers); ++i) {
        const auto& buffer = at(data.buffers, i);
        if (buffer.byte_length == u64())
            return Err(fail(rstd::format("buffers[{}].byteLength", i).as_str(),
                            "buffer length must be positive"_str));
        if (buffer.has_uri && buffer.uri.is_empty())
            return Err(fail(rstd::format("buffers[{}].uri", i).as_str(),
                            "buffer URI must not be empty"_str));
    }
    if (data.binary && ! data.buffers.is_empty() && ! at(data.buffers, 0).has_uri) {
        auto declared = at(data.buffers, 0).byte_length;
        if (declared > data.binary->length || data.binary->length - declared > u64(3))
            return Err(
                fail("buffers[0].byteLength"_str, "BIN length differs from buffer length"_str));
        for (u64 i = declared; i < data.binary->length; ++i)
            if (u8((*data.bytes)[usize((data.binary->offset + i).to_primitive())]) != u8())
                return Err(fail("GLB.BIN.padding"_str, "BIN padding must be zero"_str));
    }
    for (unsigned i = 0; i < count(data.buffer_views); ++i) {
        const auto& view = at(data.buffer_views, i);
        auto        path = rstd::format("bufferViews[{}]", i);
        if (view.buffer >= count(data.buffers) || view.length == u64())
            return Err(fail(path.as_str(), "invalid buffer view reference or length"_str));
        auto size = at(data.buffers, view.buffer).byte_length;
        if (view.offset > size || view.length > size - view.offset)
            return Err(fail(path.as_str(), "buffer view exceeds its buffer"_str));
        if (view.stride != u64() &&
            (view.stride < u64(4) || view.stride > u64(252) || view.stride % u64(4) != u64()))
            return Err(
                fail(path.as_str(), "buffer view stride must be a multiple of four in 4..252"_str));
        if (view.target && view.target != 34962 && view.target != 34963)
            return Err(fail(path.as_str(), "unknown buffer view target"_str));
    }
    for (unsigned i = 0; i < count(data.accessors); ++i) {
        const auto& accessor = at(data.accessors, i);
        auto        path     = rstd::format("accessors[{}]", i);
        if (! in_range(accessor.buffer_view, count(data.buffer_views)) ||
            ! doc::component_bytes(accessor.component_type) || accessor.count == u64())
            return Err(
                fail(path.as_str(), "invalid accessor reference, component type or count"_str));
        if (accessor.normalized &&
            (accessor.component_type == 5125 || accessor.component_type == 5126))
            return Err(fail(path.as_str(), "normalized is invalid for this component type"_str));
        if ((! accessor.minimum.is_empty() &&
             count(accessor.minimum) != doc::components(accessor.shape)) ||
            (! accessor.maximum.is_empty() &&
             count(accessor.maximum) != doc::components(accessor.shape)))
            return Err(fail(path.as_str(), "accessor min/max shape differs"_str));
        if (accessor.sparse) {
            const auto& sparse = *accessor.sparse;
            if (sparse.count == u64() || sparse.count > accessor.count ||
                sparse.indices_buffer_view >= count(data.buffer_views) ||
                sparse.values_buffer_view >= count(data.buffer_views) ||
                (sparse.indices_component_type != 5121 && sparse.indices_component_type != 5123 &&
                 sparse.indices_component_type != 5125))
                return Err(fail(path.as_str(), "invalid sparse accessor"_str));
        }
    }
    for (unsigned i = 0; i < count(data.images); ++i) {
        const auto& image = at(data.images, i);
        if (image.has_uri == (image.buffer_view != invalid_index) ||
            ! in_range(image.buffer_view, count(data.buffer_views)) ||
            (image.has_uri && image.uri.is_empty()) ||
            (image.buffer_view != invalid_index && image.mime_type.is_empty()))
            return Err(fail(rstd::format("images[{}]", i).as_str(),
                            "image needs exactly one valid URI or buffer view source"_str));
    }
    for (unsigned i = 0; i < count(data.samplers); ++i) {
        const auto& sampler = at(data.samplers, i);
        bool        mag =
            sampler.mag_filter == 0 || sampler.mag_filter == 9728 || sampler.mag_filter == 9729;
        bool min  = sampler.min_filter == 0 || sampler.min_filter == 9728 ||
                    sampler.min_filter == 9729 ||
                    (sampler.min_filter >= 9984 && sampler.min_filter <= 9987);
        auto wrap = [](unsigned value) {
            return value == 33071 || value == 33648 || value == 10497;
        };
        if (! mag || ! min || ! wrap(sampler.wrap_s) || ! wrap(sampler.wrap_t))
            return Err(fail(rstd::format("samplers[{}]", i).as_str(), "invalid sampler mode"_str));
    }
    for (unsigned i = 0; i < count(data.textures); ++i) {
        const auto& texture = at(data.textures, i);
        if (! in_range(texture.image, count(data.images)) ||
            ! in_range(texture.sampler, count(data.samplers)))
            return Err(
                fail(rstd::format("textures[{}]", i).as_str(), "invalid texture reference"_str));
    }
    auto unit = [](float value) {
        return value >= 0 && value <= 1;
    };
    auto binding_valid = [&](const TextureBinding& binding) {
        return in_range(binding.texture, count(data.textures));
    };
    for (unsigned i = 0; i < count(data.materials); ++i) {
        const auto& material = at(data.materials, i);
        const auto& core     = material.core;
        auto        path     = rstd::format("materials[{}]", i);
        if (! unit(core.base_color.x) || ! unit(core.base_color.y) || ! unit(core.base_color.z) ||
            ! unit(core.base_color.w) || ! unit(core.metallic) || ! unit(core.roughness) ||
            ! unit(core.emissive.x) || ! unit(core.emissive.y) || ! unit(core.emissive.z) ||
            ! unit(core.occlusion.strength) || core.alpha_cutoff < 0 ||
            material.emissive_strength < 0)
            return Err(fail(path.as_str(), "invalid material factors"_str));
        const TextureBinding* bindings[] {
            &core.base, &core.physical, &core.normal, &core.occlusion, &core.emission
        };
        for (const auto* binding : bindings)
            if (! binding_valid(*binding))
                return Err(fail(path.as_str(), "invalid texture binding"_str));
        if (material.specular_glossiness) {
            const auto& sg = *material.specular_glossiness;
            if (! unit(sg.diffuse.x) || ! unit(sg.diffuse.y) || ! unit(sg.diffuse.z) ||
                ! unit(sg.diffuse.w) || ! unit(sg.specular.x) || ! unit(sg.specular.y) ||
                ! unit(sg.specular.z) || ! unit(sg.glossiness) ||
                ! binding_valid(sg.diffuse_texture) || ! binding_valid(sg.physical_texture))
                return Err(
                    fail(path.as_str(), "invalid specular glossiness factors or binding"_str));
        }
    }
    for (unsigned i = 0; i < count(data.meshes); ++i) {
        const auto& mesh = at(data.meshes, i);
        auto        path = rstd::format("meshes[{}]", i);
        if (mesh.primitives.is_empty())
            return Err(fail(path.as_str(), "mesh has no primitives"_str));
        for (const auto& primitive : mesh.primitives) {
            if (! in_range(primitive.indices, count(data.accessors)) ||
                ! in_range(primitive.material, count(data.materials)) || primitive.mode > 6)
                return Err(fail(path.as_str(), "invalid primitive reference or topology"_str));
            for (const auto& attribute : primitive.attributes)
                if (attribute.accessor >= count(data.accessors))
                    return Err(fail(path.as_str(), "invalid attribute accessor"_str));
            for (const auto& target : primitive.targets)
                for (const auto& attribute : target.attributes)
                    if (attribute.accessor >= count(data.accessors))
                        return Err(fail(path.as_str(), "invalid morph accessor"_str));
        }
    }
    Vec<unsigned> parents;
    parents.resize(data.nodes.len(), invalid_index);
    for (unsigned i = 0; i < count(data.nodes); ++i) {
        const auto& node = at(data.nodes, i);
        auto        path = rstd::format("nodes[{}]", i);
        if (! in_range(node.mesh, count(data.meshes)) || ! in_range(node.skin, count(data.skins)) ||
            ! in_range(node.camera, count(data.cameras)) || (node.has_matrix && node.has_trs))
            return Err(fail(path.as_str(), "invalid node reference or conflicting matrix/TRS"_str));
        for (auto child : node.children) {
            if (child >= count(data.nodes) || at(parents, child) != invalid_index)
                return Err(fail(path.as_str(),
                                "node child is invalid, duplicated or has multiple parents"_str));
            at(parents, child) = i;
        }
    }
    Vec<unsigned> colors;
    colors.resize(data.nodes.len(), 0);
    for (unsigned start = 0; start < count(data.nodes); ++start) {
        unsigned current = start;
        while (current != invalid_index && at(colors, current) == 0) {
            at(colors, current) = 1;
            current             = at(parents, current);
        }
        if (current != invalid_index && at(colors, current) == 1)
            return Err(fail("nodes"_str, "node hierarchy contains a cycle"_str));
        current = start;
        while (current != invalid_index && at(colors, current) == 1) {
            at(colors, current) = 2;
            current             = at(parents, current);
        }
    }
    for (unsigned i = 0; i < count(data.scenes); ++i) {
        Vec<bool> seen;
        seen.resize(data.nodes.len(), false);
        for (auto root : at(data.scenes, i).roots) {
            if (root >= count(data.nodes) || at(parents, root) != invalid_index || at(seen, root))
                return Err(fail(rstd::format("scenes[{}].nodes", i).as_str(),
                                "scene root is invalid or duplicated"_str));
            at(seen, root) = true;
        }
    }
    if (! in_range(data.default_scene, count(data.scenes)))
        return Err(fail("scene"_str, "default scene is invalid"_str));
    for (unsigned i = 0; i < count(data.skins); ++i) {
        const auto& skin = at(data.skins, i);
        if (skin.joints.is_empty() || ! in_range(skin.skeleton, count(data.nodes)) ||
            ! in_range(skin.inverse_bind, count(data.accessors)))
            return Err(fail(rstd::format("skins[{}]", i).as_str(), "invalid skin reference"_str));
        Vec<bool> seen;
        seen.resize(data.nodes.len(), false);
        for (auto joint : skin.joints) {
            if (joint >= count(data.nodes) || at(seen, joint))
                return Err(fail(rstd::format("skins[{}].joints", i).as_str(),
                                "skin joint is invalid or duplicated"_str));
            at(seen, joint) = true;
        }
    }
    for (unsigned i = 0; i < count(data.cameras); ++i) {
        const auto& p = at(data.cameras, i).projection;
        if (p.orthographic) {
            if (p.xmag <= 0 || p.ymag <= 0 || p.near < 0 || p.far <= p.near)
                return Err(fail(rstd::format("cameras[{}]", i).as_str(),
                                "invalid orthographic projection"_str));
        } else if (p.yfov <= 0 || p.yfov >= 3.14159265359f || p.aspect < 0 || p.near <= 0 ||
                   (p.far != 0 && p.far <= p.near))
            return Err(fail(rstd::format("cameras[{}]", i).as_str(),
                            "invalid perspective projection"_str));
    }
    for (unsigned i = 0; i < count(data.animations); ++i) {
        const auto& animation = at(data.animations, i);
        for (const auto& sampler : animation.samplers)
            if (sampler.input >= count(data.accessors) || sampler.output >= count(data.accessors))
                return Err(fail(rstd::format("animations[{}].samplers", i).as_str(),
                                "invalid animation accessor"_str));
        for (const auto& channel : animation.channels)
            if (channel.sampler >= count(animation.samplers) ||
                ! in_range(channel.node, count(data.nodes)))
                return Err(fail(rstd::format("animations[{}].channels", i).as_str(),
                                "invalid animation channel reference"_str));
    }
    return Ok(empty {});
}
} // namespace vecgltf
