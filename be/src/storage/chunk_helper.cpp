// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "storage/chunk_helper.h"

#include "column/array_column.h"
#include "column/chunk.h"
#include "column/column_helper.h"
#include "column/column_pool.h"
#include "column/schema.h"
#include "column/type_traits.h"
#include "runtime/descriptors.h"
#include "storage/olap_type_infra.h"
#include "storage/tablet_schema.h"
#include "storage/type_utils.h"
#include "storage/types.h"
#include "util/metrics.h"
#include "util/percentile_value.h"

namespace starrocks {

// NOTE(zc): now CppColumnTraits is only used for this class, so I move it here.
// Someday if it is used by others, please move it into a single file.
// CppColumnTraits
// Infer ColumnType from FieldType
template <FieldType ftype>
struct CppColumnTraits {
    using CppType = typename CppTypeTraits<ftype>::CppType;
    using ColumnType = typename vectorized::ColumnTraits<CppType>::ColumnType;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_BOOL> {
    using ColumnType = vectorized::UInt8Column;
};

// deprecated
template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_DATE> {
    using ColumnType = vectorized::FixedLengthColumn<uint24_t>;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_DATE_V2> {
    using ColumnType = vectorized::DateColumn;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_TIMESTAMP> {
    using ColumnType = vectorized::TimestampColumn;
};

// deprecated
template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_DECIMAL> {
    using ColumnType = vectorized::FixedLengthColumn<decimal12_t>;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_HLL> {
    using ColumnType = vectorized::HyperLogLogColumn;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_PERCENTILE> {
    using ColumnType = vectorized::PercentileColumn;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_OBJECT> {
    using ColumnType = vectorized::BitmapColumn;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_UNSIGNED_INT> {
    using ColumnType = vectorized::UInt32Column;
};

template <>
struct CppColumnTraits<OLAP_FIELD_TYPE_JSON> {
    using ColumnType = vectorized::JsonColumn;
};

vectorized::Field ChunkHelper::convert_field(ColumnId id, const TabletColumn& c) {
    TypeInfoPtr type_info = get_type_info(c);
    starrocks::vectorized::Field f(id, std::string(c.name()), type_info, c.is_nullable());
    f.set_is_key(c.is_key());
    f.set_short_key_length(c.index_length());
    f.set_aggregate_method(c.aggregation());
    f.set_length(c.length());
    return f;
}

vectorized::Schema ChunkHelper::convert_schema(const starrocks::TabletSchema& schema) {
    starrocks::vectorized::Fields fields;
    for (ColumnId cid = 0; cid < schema.num_columns(); ++cid) {
        auto f = convert_field(cid, schema.column(cid));
        fields.emplace_back(std::make_shared<starrocks::vectorized::Field>(std::move(f)));
    }
    return starrocks::vectorized::Schema(std::move(fields), schema.keys_type());
}

starrocks::vectorized::Field ChunkHelper::convert_field_to_format_v2(ColumnId id, const TabletColumn& c) {
    FieldType type = TypeUtils::to_storage_format_v2(c.type());

    TypeInfoPtr type_info = nullptr;
    if (type == OLAP_FIELD_TYPE_ARRAY || type == OLAP_FIELD_TYPE_DECIMAL32 || type == OLAP_FIELD_TYPE_DECIMAL64 ||
        type == OLAP_FIELD_TYPE_DECIMAL128) {
        // ARRAY and DECIMAL should be handled specially
        // Array is nested type, the message is stored in TabletColumn
        // Decimal has precision and scale, the message is stored in TabletColumn
        type_info = get_type_info(c);
    } else {
        type_info = get_type_info(type);
    }
    starrocks::vectorized::Field f(id, std::string(c.name()), type_info, c.is_nullable());
    f.set_is_key(c.is_key());
    f.set_length(c.length());

    if (type == OLAP_FIELD_TYPE_ARRAY) {
        const TabletColumn& sub_column = c.subcolumn(0);
        auto sub_field = convert_field_to_format_v2(id, sub_column);
        f.add_sub_field(sub_field);
    }

    // If origin type needs to be converted format v2, we should change its short key length
    if (TypeUtils::specific_type_of_format_v1(c.type())) {
        // Get TypeInfo with new type
        TypeInfoPtr type_info = get_type_info(type);
        f.set_short_key_length(type_info->size());
    } else {
        f.set_short_key_length(c.index_length());
    }

    f.set_aggregate_method(c.aggregation());
    return f;
}

starrocks::vectorized::Schema ChunkHelper::convert_schema_to_format_v2(const starrocks::TabletSchema& schema) {
    return starrocks::vectorized::Schema(schema.schema());
}

starrocks::vectorized::Schema ChunkHelper::convert_schema_to_format_v2(const starrocks::TabletSchema& schema,
                                                                       const std::vector<ColumnId>& cids) {
    return starrocks::vectorized::Schema(schema.schema(), cids);
}

starrocks::vectorized::Schema ChunkHelper::get_short_key_schema_with_format_v2(const starrocks::TabletSchema& schema) {
    std::vector<ColumnId> short_key_cids;
    for (ColumnId cid = 0; cid < schema.num_short_key_columns(); ++cid) {
        short_key_cids.push_back(cid);
    }
    return starrocks::vectorized::Schema(schema.schema(), short_key_cids);
}

ColumnId ChunkHelper::max_column_id(const starrocks::vectorized::Schema& schema) {
    ColumnId id = 0;
    for (const auto& field : schema.fields()) {
        id = std::max(id, field->id());
    }
    return id;
}

template <typename T>
struct ColumnDeleter {
    ColumnDeleter(uint32_t chunk_size) : chunk_size(chunk_size) {}
    void operator()(vectorized::Column* ptr) const { vectorized::return_column<T>(down_cast<T*>(ptr), chunk_size); }
    uint32_t chunk_size;
};

template <typename T, bool force>
inline std::shared_ptr<T> get_column_ptr(size_t chunk_size) {
    if constexpr (std::negation_v<vectorized::HasColumnPool<T>>) {
        return std::make_shared<T>();
    } else {
        T* ptr = vectorized::get_column<T, force>();
        if (LIKELY(ptr != nullptr)) {
            return std::shared_ptr<T>(ptr, ColumnDeleter<T>(chunk_size));
        } else {
            return std::make_shared<T>();
        }
    }
}

template <typename T, bool force>
inline std::shared_ptr<vectorized::DecimalColumnType<T>> get_decimal_column_ptr(int precision, int scale,
                                                                                size_t chunk_size) {
    auto column = get_column_ptr<T, force>(chunk_size);
    column->set_precision(precision);
    column->set_scale(scale);
    return column;
}

template <bool force>
struct ColumnPtrBuilder {
    template <FieldType ftype>
    vectorized::ColumnPtr operator()(size_t chunk_size, const vectorized::Field& field, int precision, int scale) {
        auto nullable = [&](vectorized::ColumnPtr c) -> vectorized::ColumnPtr {
            return field.is_nullable()
                           ? vectorized::NullableColumn::create(
                                     std::move(c), get_column_ptr<vectorized::NullColumn, force>(chunk_size))
                           : c;
        };

        if constexpr (ftype == OLAP_FIELD_TYPE_ARRAY) {
            auto elements = field.sub_field(0).create_column();
            auto offsets = get_column_ptr<vectorized::UInt32Column, force>(chunk_size);
            auto array = vectorized::ArrayColumn::create(std::move(elements), offsets);
            return nullable(array);
        } else {
            switch (ftype) {
            case OLAP_FIELD_TYPE_DECIMAL32:
                return nullable(
                        get_decimal_column_ptr<vectorized::Decimal32Column, force>(precision, scale, chunk_size));
            case OLAP_FIELD_TYPE_DECIMAL64:
                return nullable(
                        get_decimal_column_ptr<vectorized::Decimal64Column, force>(precision, scale, chunk_size));
            case OLAP_FIELD_TYPE_DECIMAL128:
                return nullable(
                        get_decimal_column_ptr<vectorized::Decimal128Column, force>(precision, scale, chunk_size));
            default: {
                return nullable(get_column_ptr<typename CppColumnTraits<ftype>::ColumnType, force>(chunk_size));
            }
            }
        }
    }
};

template <bool force>
vectorized::ColumnPtr column_from_pool(const vectorized::Field& field, size_t chunk_size) {
    auto precision = field.type()->precision();
    auto scale = field.type()->scale();
    return field_type_dispatch_column(field.type()->type(), ColumnPtrBuilder<force>(), chunk_size, field, precision,
                                      scale);
}

vectorized::Chunk* ChunkHelper::new_chunk_pooled(const vectorized::Schema& schema, size_t chunk_size, bool force) {
    vectorized::Columns columns;
    columns.reserve(schema.num_fields());
    for (size_t i = 0; i < schema.num_fields(); i++) {
        const vectorized::FieldPtr& f = schema.field(i);
        auto column = (force && !config::disable_column_pool) ? column_from_pool<true>(*f, chunk_size)
                                                              : column_from_pool<false>(*f, chunk_size);
        column->reserve(chunk_size);
        columns.emplace_back(std::move(column));
    }
    return new vectorized::Chunk(std::move(columns), std::make_shared<vectorized::Schema>(schema));
}

std::vector<size_t> ChunkHelper::get_char_field_indexes(const vectorized::Schema& schema) {
    std::vector<size_t> char_field_indexes;
    for (size_t i = 0; i < schema.num_fields(); ++i) {
        const auto& field = schema.field(i);
        if (field->type()->type() == OLAP_FIELD_TYPE_CHAR) {
            char_field_indexes.push_back(i);
        }
    }
    return char_field_indexes;
}

void ChunkHelper::padding_char_columns(const std::vector<size_t>& char_column_indexes, const vectorized::Schema& schema,
                                       const starrocks::TabletSchema& tschema, vectorized::Chunk* chunk) {
    size_t num_rows = chunk->num_rows();
    for (auto field_index : char_column_indexes) {
        vectorized::Column* column = chunk->get_column_by_index(field_index).get();
        vectorized::Column* data_column = vectorized::ColumnHelper::get_data_column(column);
        vectorized::BinaryColumn* binary = down_cast<vectorized::BinaryColumn*>(data_column);

        vectorized::Offsets& offset = binary->get_offset();
        vectorized::Bytes& bytes = binary->get_bytes();

        // Padding 0 to CHAR field, the storage bitmap index and zone map need it.
        // TODO(kks): we could improve this if there are many null valus
        auto new_binary = vectorized::BinaryColumn::create();
        vectorized::Offsets& new_offset = new_binary->get_offset();
        vectorized::Bytes& new_bytes = new_binary->get_bytes();

        // |schema| maybe partial columns in vertical compaction, so get char column length by name.
        uint32_t len = tschema.column(tschema.field_index(schema.field(field_index)->name())).length();

        new_offset.resize(num_rows + 1);
        new_bytes.assign(num_rows * len, 0); // padding 0

        uint32_t from = 0;
        for (size_t j = 0; j < num_rows; ++j) {
            uint32_t copy_data_len = std::min(len, offset[j + 1] - offset[j]);
            strings::memcpy_inlined(new_bytes.data() + from, bytes.data() + offset[j], copy_data_len);
            from += len; // no copy data will be 0
        }

        for (size_t j = 1; j <= num_rows; ++j) {
            new_offset[j] = len * j;
        }

        const auto& field = schema.field(field_index);

        if (field->is_nullable()) {
            auto* nullable_column = down_cast<vectorized::NullableColumn*>(column);
            auto new_column = vectorized::NullableColumn::create(new_binary, nullable_column->null_column());
            new_column->swap_column(*column);
        } else {
            new_binary->swap_column(*column);
        }
    }
}

struct ColumnBuilder {
    template <FieldType ftype>
    vectorized::ColumnPtr operator()(bool nullable) {
        [[maybe_unused]] auto NullableIfNeed = [&](vectorized::ColumnPtr col) -> vectorized::ColumnPtr {
            return nullable ? vectorized::NullableColumn::create(std::move(col), vectorized::NullColumn::create())
                            : col;
        };

        if constexpr (ftype == OLAP_FIELD_TYPE_ARRAY) {
            CHECK(false) << "array not supported";
        } else {
            return NullableIfNeed(CppColumnTraits<ftype>::ColumnType::create());
        }
    }
};

vectorized::ColumnPtr ChunkHelper::column_from_field_type(FieldType type, bool nullable) {
    return field_type_dispatch_column(type, ColumnBuilder(), nullable);
}

vectorized::ColumnPtr ChunkHelper::column_from_field(const vectorized::Field& field) {
    auto NullableIfNeed = [&](vectorized::ColumnPtr col) -> vectorized::ColumnPtr {
        return field.is_nullable()
                       ? vectorized::NullableColumn::create(std::move(col), vectorized::NullColumn::create())
                       : col;
    };

    auto type = field.type()->type();
    switch (type) {
    case OLAP_FIELD_TYPE_DECIMAL32:
        return NullableIfNeed(vectorized::Decimal32Column::create(field.type()->precision(), field.type()->scale()));
    case OLAP_FIELD_TYPE_DECIMAL64:
        return NullableIfNeed(vectorized::Decimal64Column::create(field.type()->precision(), field.type()->scale()));
    case OLAP_FIELD_TYPE_DECIMAL128:
        return NullableIfNeed(vectorized::Decimal128Column::create(field.type()->precision(), field.type()->scale()));
    case OLAP_FIELD_TYPE_ARRAY: {
        return NullableIfNeed(vectorized::ArrayColumn::create(column_from_field(field.sub_field(0)),
                                                              vectorized::UInt32Column::create()));
    }
    default:
        return NullableIfNeed(column_from_field_type(type, false));
    }
}

vectorized::ChunkPtr ChunkHelper::new_chunk(const vectorized::Schema& schema, size_t n) {
    size_t fields = schema.num_fields();
    vectorized::Columns columns;
    columns.reserve(fields);
    for (size_t i = 0; i < fields; i++) {
        const vectorized::FieldPtr& f = schema.field(i);
        columns.emplace_back(column_from_field(*f));
        columns.back()->reserve(n);
    }
    return std::make_shared<vectorized::Chunk>(std::move(columns), std::make_shared<vectorized::Schema>(schema));
}

std::shared_ptr<vectorized::Chunk> ChunkHelper::new_chunk(const TupleDescriptor& tuple_desc, size_t n) {
    return new_chunk(tuple_desc.slots(), n);
}

std::shared_ptr<vectorized::Chunk> ChunkHelper::new_chunk(const std::vector<SlotDescriptor*>& slots, size_t n) {
    auto chunk = std::make_shared<vectorized::Chunk>();
    for (const auto slot : slots) {
        auto column = vectorized::ColumnHelper::create_column(slot->type(), slot->is_nullable());
        column->reserve(n);
        chunk->append_column(column, slot->id());
    }
    return chunk;
}

void ChunkHelper::reorder_chunk(const TupleDescriptor& tuple_desc, vectorized::Chunk* chunk) {
    return reorder_chunk(tuple_desc.slots(), chunk);
}

void ChunkHelper::reorder_chunk(const std::vector<SlotDescriptor*>& slots, vectorized::Chunk* chunk) {
    DCHECK(chunk->columns().size() == slots.size());
    auto reordered_chunk = vectorized::Chunk();
    auto& original_chunk = (*chunk);
    for (std::size_t idx = 0; idx < slots.size(); idx++) {
        auto slot_id = slots[idx]->id();
        reordered_chunk.append_column(original_chunk.get_column_by_slot_id(slot_id), slot_id);
    }
    original_chunk.swap_chunk(reordered_chunk);
}

} // namespace starrocks
