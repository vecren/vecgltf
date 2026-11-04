export module vecgltf.load.accessor;
export import vecgltf.load.resources;

using namespace rstd::literals;

export namespace vecgltf
{
class AccessorReader {
    const ResolvedDocument& document_;
    mutable bool            validated_ {};

    struct Layout {
        const document::Accessor* accessor {};
        slice<u8>                 base {}, sparse_indices {}, sparse_values {};
        rstd::uint64_t            stride {}, packed_stride {}, extent {};
        unsigned component_size {}, rows {}, columns {}, column_stride {}, index_size {};
        bool     matrix {}, has_base {};
    };

    auto failure(ErrorKind kind, ref<str> location, ref<str> message) const -> Error {
        return {
            kind, document_.context().source.clone(), String::make(location), String::make(message)
        };
    }
    static auto bits(slice<u8> bytes, rstd::uint64_t offset, unsigned size) -> unsigned {
        unsigned result {};
        for (unsigned byte = 0; byte < size; ++byte)
            result |= unsigned(u8(bytes[usize(offset + byte)]).to_primitive()) << (byte * 8);
        return result;
    }
    static auto scalar(unsigned value, unsigned type, bool normalized) -> float {
        switch (type) {
        case 5120: {
            int signed_value = value < 128 ? int(value) : int(value) - 256;
            return normalized ? maximum(float(signed_value) / 127, -1) : float(signed_value);
        }
        case 5121: return normalized ? float(value) / 255 : float(value);
        case 5122: {
            int signed_value = value < 32768 ? int(value) : int(value) - 65536;
            return normalized ? maximum(float(signed_value) / 32767, -1) : float(signed_value);
        }
        case 5123: return normalized ? float(value) / 65535 : float(value);
        case 5125: return float(value);
        case 5126: return rstd::f32::from_bits(u32(value)).to_primitive();
        }
        return 0;
    }
    auto view(unsigned index, ref<str> location) const -> Result<slice<u8>, Error> {
        const auto& data = document_.document().data();
        if (index >= count(data.buffer_views))
            return Err(
                failure(ErrorKind::InvalidAsset, location, "buffer view is out of range"_str));
        const auto& description = at(data.buffer_views, index);
        if (description.buffer >= count(data.buffers))
            return Err(failure(ErrorKind::InvalidAsset, location, "buffer is out of range"_str));
        auto bytes = document_.buffer(description.buffer);
        if (! bytes)
            return Err(failure(ErrorKind::InvalidAsset, location, "buffer is unresolved"_str));
        auto declared = at(data.buffers, description.buffer).byte_length.to_primitive();
        auto offset = description.offset.to_primitive(), length = description.length.to_primitive();
        auto stride = description.stride.to_primitive();
        if (declared > bytes->len().to_primitive() || ! length || offset > declared ||
            length > declared - offset)
            return Err(failure(
                ErrorKind::InvalidAsset, location, "buffer view exceeds declared buffer"_str));
        if (stride && (stride < 4 || stride > 252 || stride % 4))
            return Err(
                failure(ErrorKind::InvalidAsset, location, "invalid buffer view stride"_str));
        if (description.target && description.target != 34962 && description.target != 34963)
            return Err(
                failure(ErrorKind::InvalidAsset, location, "invalid buffer view target"_str));
        return Ok(slice<u8>::from_raw_parts(bytes.unwrap().as_byte_ptr() + offset, usize(length)));
    }
    auto span(slice<u8>      bytes,
              rstd::uint64_t offset,
              rstd::uint64_t elements,
              rstd::uint64_t stride,
              rstd::uint64_t extent,
              ref<str>       location) const -> Result<empty, Error> {
        auto length = bytes.len().to_primitive();
        if (! elements || ! stride || offset > length || extent > length - offset ||
            elements - 1 > (length - offset - extent) / stride)
            return Err(
                failure(ErrorKind::InvalidAsset, location, "accessor exceeds buffer view"_str));
        return Ok(empty {});
    }
    auto sparse(Layout& layout, ref<str> location, bool ordered) const -> Result<empty, Error> {
        const auto& accessor = *layout.accessor;
        if (! accessor.sparse) return Ok(empty {});
        const auto& value = *accessor.sparse;
        auto        size  = value.count.to_primitive();
        if (! size || size > accessor.count.to_primitive() ||
            (value.indices_component_type != 5121 && value.indices_component_type != 5123 &&
             value.indices_component_type != 5125))
            return Err(failure(
                ErrorKind::InvalidAsset, location, "invalid sparse count or index type"_str));
        auto indices = view(value.indices_buffer_view, location);
        if (indices.is_err()) return Err(indices.unwrap_err());
        auto values = view(value.values_buffer_view, location);
        if (values.is_err()) return Err(values.unwrap_err());
        const auto& views      = document_.document().data().buffer_views;
        const auto& index_view = at(views, value.indices_buffer_view);
        const auto& value_view = at(views, value.values_buffer_view);
        if (index_view.stride != u64() || index_view.target || value_view.stride != u64() ||
            value_view.target)
            return Err(failure(ErrorKind::InvalidAsset,
                               location,
                               "sparse buffer views cannot have stride or target"_str));
        layout.index_size = document::component_bytes(value.indices_component_type);
        auto index_offset = value.indices_offset.to_primitive();
        auto value_offset = value.values_offset.to_primitive();
        if (index_offset % layout.index_size ||
            (index_view.offset.to_primitive() % layout.index_size) ||
            value_offset % layout.component_size ||
            (value_view.offset.to_primitive() % layout.component_size) ||
            (layout.matrix && (value_view.offset.to_primitive() % 4 + value_offset % 4) % 4))
            return Err(failure(ErrorKind::InvalidAsset, location, "misaligned sparse data"_str));
        auto index_bounds =
            span(*indices, index_offset, size, layout.index_size, layout.index_size, location);
        if (index_bounds.is_err()) return index_bounds;
        auto value_bounds =
            span(*values, value_offset, size, layout.packed_stride, layout.extent, location);
        if (value_bounds.is_err()) return value_bounds;
        layout.sparse_indices = *indices;
        layout.sparse_values  = *values;
        if (ordered) {
            unsigned previous {};
            for (rstd::uint64_t i = 0; i < size; ++i) {
                auto index =
                    bits(*indices, index_offset + i * layout.index_size, layout.index_size);
                if (index >= accessor.count.to_primitive() || (i && index <= previous))
                    return Err(failure(ErrorKind::InvalidAsset,
                                       location,
                                       "sparse indices must increase and stay in range"_str));
                previous = index;
            }
        }
        return Ok(empty {});
    }
    auto layout(unsigned index, ref<str> location, bool ordered) const -> Result<Layout, Error> {
        const auto& data = document_.document().data();
        if (index >= count(data.accessors))
            return Err(failure(ErrorKind::InvalidAsset, location, "accessor is out of range"_str));
        const auto& accessor = at(data.accessors, index);
        Layout      result;
        result.accessor       = &accessor;
        result.component_size = document::component_bytes(accessor.component_type);
        auto components       = document::components(accessor.shape);
        if (! result.component_size || ! components ||
            (accessor.normalized && accessor.component_type != 5120 &&
             accessor.component_type != 5121 && accessor.component_type != 5122 &&
             accessor.component_type != 5123))
            return Err(failure(ErrorKind::InvalidAsset,
                               location,
                               "invalid accessor component type or normalization"_str));
        auto elements = accessor.count.to_primitive();
        auto limit    = document_.context().options.max_accessor_elements.to_primitive();
        if (! elements)
            return Err(
                failure(ErrorKind::InvalidAsset, location, "accessor count must be positive"_str));
        if (elements > limit / components)
            return Err(failure(ErrorKind::Limit, location, "accessor element limit exceeded"_str));
        result.matrix        = accessor.shape == document::Shape::Mat2 ||
                               accessor.shape == document::Shape::Mat3 ||
                               accessor.shape == document::Shape::Mat4;
        result.rows          = accessor.shape == document::Shape::Mat2   ? 2
                               : accessor.shape == document::Shape::Mat3 ? 3
                               : accessor.shape == document::Shape::Mat4 ? 4
                                                                         : components;
        result.columns       = result.matrix ? result.rows : 1;
        auto column_bytes    = result.rows * result.component_size;
        result.column_stride = result.matrix ? (column_bytes + 3) & ~3u : column_bytes;
        result.packed_stride = result.columns * result.column_stride;
        result.extent        = (result.columns - 1) * result.column_stride + column_bytes;
        result.stride        = result.packed_stride;
        auto offset          = accessor.offset.to_primitive();
        if (accessor.buffer_view != invalid_index) {
            auto bytes = view(accessor.buffer_view, location);
            if (bytes.is_err()) return Err(bytes.unwrap_err());
            const auto& description = at(data.buffer_views, accessor.buffer_view);
            auto        stride      = description.stride.to_primitive();
            if (stride) result.stride = stride;
            if (offset % result.component_size ||
                description.offset.to_primitive() % result.component_size ||
                (result.matrix && (description.offset.to_primitive() % 4 + offset % 4) % 4) ||
                result.stride < result.extent || result.stride % result.component_size)
                return Err(failure(
                    ErrorKind::InvalidAsset, location, "invalid accessor alignment or stride"_str));
            auto bounds = span(*bytes, offset, elements, result.stride, result.extent, location);
            if (bounds.is_err()) return Err(bounds.unwrap_err());
            result.base     = *bytes;
            result.has_base = true;
        } else if (offset)
            return Err(failure(ErrorKind::InvalidAsset,
                               location,
                               "accessor without buffer view cannot have an offset"_str));
        if ((! accessor.minimum.is_empty() && count(accessor.minimum) != components) ||
            (! accessor.maximum.is_empty() && count(accessor.maximum) != components))
            return Err(failure(
                ErrorKind::InvalidAsset, location, "accessor bounds have the wrong shape"_str));
        for (unsigned component = 0; component < components; ++component) {
            if ((! accessor.minimum.is_empty() &&
                 ! rstd::f32(at(accessor.minimum, component)).is_finite()) ||
                (! accessor.maximum.is_empty() &&
                 ! rstd::f32(at(accessor.maximum, component)).is_finite()) ||
                (! accessor.minimum.is_empty() && ! accessor.maximum.is_empty() &&
                 at(accessor.minimum, component) > at(accessor.maximum, component)))
                return Err(
                    failure(ErrorKind::InvalidAsset, location, "invalid accessor bounds"_str));
        }
        auto checked = sparse(result, location, ordered);
        if (checked.is_err()) return Err(checked.unwrap_err());
        return Ok(result);
    }
    template<class T, class Convert>
    auto unpack(const Layout& layout, Convert convert) const -> Vec<T> {
        const auto& accessor   = *layout.accessor;
        auto        elements   = accessor.count.to_primitive();
        auto        components = document::components(accessor.shape);
        Vec<T>      output;
        output.resize(usize(elements * components), T {});
        auto fill = [&](slice<u8> bytes, rstd::uint64_t offset, rstd::uint64_t destination) {
            for (unsigned component = 0; component < components; ++component) {
                auto source = offset + (component / layout.rows) * layout.column_stride +
                              (component % layout.rows) * layout.component_size;
                at(output, destination * components + component) =
                    convert(bits(bytes, source, layout.component_size));
            }
        };
        if (layout.has_base)
            for (rstd::uint64_t element = 0; element < elements; ++element)
                fill(
                    layout.base, accessor.offset.to_primitive() + element * layout.stride, element);
        if (accessor.sparse) {
            const auto& sparse = *accessor.sparse;
            for (rstd::uint64_t element = 0; element < sparse.count.to_primitive(); ++element) {
                auto destination =
                    bits(layout.sparse_indices,
                         sparse.indices_offset.to_primitive() + element * layout.index_size,
                         layout.index_size);
                fill(layout.sparse_values,
                     sparse.values_offset.to_primitive() + element * layout.packed_stride,
                     destination);
            }
        }
        return output;
    }

public:
    explicit AccessorReader(const ResolvedDocument& document): document_(document) {}
    auto Validate() const -> Result<empty, Error> {
        const auto& data = document_.document().data();
        for (unsigned i = 0; i < count(data.buffer_views); ++i) {
            auto location = rstd::format("bufferViews[{}]", i);
            auto checked  = view(i, location.as_str());
            if (checked.is_err()) return Err(checked.unwrap_err());
        }
        for (unsigned i = 0; i < count(data.accessors); ++i) {
            auto location = rstd::format("accessors[{}]", i);
            auto checked  = layout(i, location.as_str(), true);
            if (checked.is_err()) return Err(checked.unwrap_err());
        }
        validated_ = true;
        return Ok(empty {});
    }
    auto Floats(unsigned index, unsigned expected_components, ref<str> location = {}) const
        -> Result<Vec<float>, Error> {
        LoadScope scope(document_.context().options.trace, LoadEvent::Accessor);
        String    fallback;
        if (location.is_empty()) {
            fallback = rstd::format("accessors[{}]", index);
            location = fallback.as_str();
        }
        auto checked = layout(index, location, ! validated_);
        if (checked.is_err()) return Err(checked.unwrap_err());
        auto        value    = checked.unwrap();
        const auto& accessor = *value.accessor;
        if (document::components(accessor.shape) != expected_components)
            return Err(
                failure(ErrorKind::InvalidAsset, location, "accessor component count differs"_str));
        auto output = unpack<float>(value, [&](unsigned bits) {
            return scalar(bits, accessor.component_type, accessor.normalized);
        });
        for (float component : output)
            if (! rstd::f32(component).is_finite())
                return Err(
                    failure(ErrorKind::InvalidAsset, location, "non-finite accessor value"_str));
        return Ok(rstd::move(output));
    }
    auto Indices(unsigned index, ref<str> location = {}) const -> Result<Vec<unsigned>, Error> {
        LoadScope scope(document_.context().options.trace, LoadEvent::Accessor);
        String    fallback;
        if (location.is_empty()) {
            fallback = rstd::format("accessors[{}]", index);
            location = fallback.as_str();
        }
        auto checked = layout(index, location, ! validated_);
        if (checked.is_err()) return Err(checked.unwrap_err());
        auto        value    = checked.unwrap();
        const auto& accessor = *value.accessor;
        if (accessor.shape != document::Shape::Scalar || accessor.normalized ||
            value.stride != value.component_size ||
            (accessor.component_type != 5121 && accessor.component_type != 5123 &&
             accessor.component_type != 5125))
            return Err(failure(ErrorKind::InvalidAsset,
                               location,
                               "indices require a packed unnormalized unsigned scalar"_str));
        return Ok(unpack<unsigned>(value, [](unsigned bits) {
            return bits;
        }));
    }
};
} // namespace vecgltf
