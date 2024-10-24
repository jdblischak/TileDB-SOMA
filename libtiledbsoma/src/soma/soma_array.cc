/** @file   soma_array.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2022-2024 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 *   This file defines the SOMAArray class.
 */

#include "soma_array.h"
#include <tiledb/array_experimental.h>
#include "../utils/logger.h"
#include "../utils/util.h"
namespace tiledbsoma {
using namespace tiledb;

//===================================================================
//= public static
//===================================================================

std::unique_ptr<SOMAArray> SOMAArray::create(
    std::shared_ptr<SOMAContext> ctx,
    std::string_view uri,
    ArraySchema schema,
    std::string soma_type,
    std::optional<TimestampRange> timestamp) {
    Array::create(std::string(uri), schema);

    std::shared_ptr<Array> array;
    if (timestamp) {
        array = std::make_shared<Array>(
            *ctx->tiledb_ctx(),
            std::string(uri),
            TILEDB_WRITE,
            TemporalPolicy(
                TimestampStartEnd, timestamp->first, timestamp->second));
    } else {
        array = std::make_shared<Array>(
            *ctx->tiledb_ctx(), std::string(uri), TILEDB_WRITE);
    }

    array->put_metadata(
        SOMA_OBJECT_TYPE_KEY,
        TILEDB_STRING_UTF8,
        static_cast<uint32_t>(soma_type.length()),
        soma_type.c_str());

    array->put_metadata(
        ENCODING_VERSION_KEY,
        TILEDB_STRING_UTF8,
        static_cast<uint32_t>(ENCODING_VERSION_VAL.length()),
        ENCODING_VERSION_VAL.c_str());

    return std::make_unique<SOMAArray>(ctx, array, timestamp);
}

std::unique_ptr<SOMAArray> SOMAArray::open(
    OpenMode mode,
    std::string_view uri,
    std::string_view name,
    std::map<std::string, std::string> platform_config,
    std::vector<std::string> column_names,
    std::string_view batch_size,
    ResultOrder result_order,
    std::optional<TimestampRange> timestamp) {
    LOG_DEBUG(
        fmt::format("[SOMAArray] static method 'cfg' opening array '{}'", uri));
    return std::make_unique<SOMAArray>(
        mode,
        uri,
        std::make_shared<SOMAContext>(platform_config),
        name,
        column_names,
        batch_size,
        result_order,
        timestamp);
}

std::unique_ptr<SOMAArray> SOMAArray::open(
    OpenMode mode,
    std::string_view uri,
    std::shared_ptr<SOMAContext> ctx,
    std::string_view name,
    std::vector<std::string> column_names,
    std::string_view batch_size,
    ResultOrder result_order,
    std::optional<TimestampRange> timestamp) {
    LOG_DEBUG(
        fmt::format("[SOMAArray] static method 'ctx' opening array '{}'", uri));
    return std::make_unique<SOMAArray>(
        mode,
        uri,
        ctx,
        name,
        column_names,
        batch_size,
        result_order,
        timestamp);
}

//===================================================================
//= public non-static
//===================================================================

SOMAArray::SOMAArray(
    OpenMode mode,
    std::string_view uri,
    std::string_view name,
    std::map<std::string, std::string> platform_config,
    std::vector<std::string> column_names,
    std::string_view batch_size,
    ResultOrder result_order,
    std::optional<TimestampRange> timestamp)
    : uri_(util::rstrip_uri(uri))
    , result_order_(result_order)
    , timestamp_(timestamp) {
    ctx_ = std::make_shared<SOMAContext>(platform_config);
    validate(mode, name, timestamp);
    reset(column_names, batch_size, result_order);
    fill_metadata_cache();
}

SOMAArray::SOMAArray(
    OpenMode mode,
    std::string_view uri,
    std::shared_ptr<SOMAContext> ctx,
    std::string_view name,
    std::vector<std::string> column_names,
    std::string_view batch_size,
    ResultOrder result_order,
    std::optional<TimestampRange> timestamp)
    : uri_(util::rstrip_uri(uri))
    , ctx_(ctx)
    , result_order_(result_order)
    , timestamp_(timestamp) {
    validate(mode, name, timestamp);
    reset(column_names, batch_size, result_order);
    fill_metadata_cache();
}

SOMAArray::SOMAArray(
    std::shared_ptr<SOMAContext> ctx,
    std::shared_ptr<Array> arr,
    std::optional<TimestampRange> timestamp)
    : uri_(util::rstrip_uri(arr->uri()))
    , ctx_(ctx)
    , batch_size_("auto")
    , result_order_(ResultOrder::automatic)
    , timestamp_(timestamp)
    , mq_(std::make_unique<ManagedQuery>(arr, ctx_->tiledb_ctx(), name_))
    , arr_(arr) {
    reset({}, batch_size_, result_order_);
    fill_metadata_cache();
}

void SOMAArray::fill_metadata_cache() {
    if (arr_->query_type() == TILEDB_WRITE) {
        meta_cache_arr_ = std::make_shared<Array>(
            *ctx_->tiledb_ctx(),
            uri_,
            TILEDB_READ,
            TemporalPolicy(
                TimestampStartEnd, timestamp()->first, timestamp()->second));
    } else {
        meta_cache_arr_ = arr_;
    }

    metadata_.clear();

    for (uint64_t idx = 0; idx < meta_cache_arr_->metadata_num(); ++idx) {
        std::string key;
        tiledb_datatype_t value_type;
        uint32_t value_num;
        const void* value;
        meta_cache_arr_->get_metadata_from_index(
            idx, &key, &value_type, &value_num, &value);
        MetadataValue mdval(value_type, value_num, value);
        std::pair<std::string, const MetadataValue> mdpair(key, mdval);
        metadata_.insert(mdpair);
    }
}

const std::string SOMAArray::uri() const {
    return uri_;
};

std::shared_ptr<SOMAContext> SOMAArray::ctx() {
    return ctx_;
};

void SOMAArray::open(OpenMode mode, std::optional<TimestampRange> timestamp) {
    timestamp_ = timestamp;

    validate(mode, name_, timestamp);
    reset(column_names(), batch_size_, result_order_);
    fill_metadata_cache();
}

std::unique_ptr<SOMAArray> SOMAArray::reopen(
    OpenMode mode, std::optional<TimestampRange> timestamp) {
    return std::make_unique<SOMAArray>(
        mode,
        uri_,
        ctx_,
        name_,
        column_names(),
        batch_size_,
        result_order_,
        timestamp);
}

void SOMAArray::close() {
    if (arr_->query_type() == TILEDB_WRITE)
        meta_cache_arr_->close();

    // Close the array through the managed query to ensure any pending queries
    // are completed.
    mq_->close();
    metadata_.clear();
}

void SOMAArray::reset(
    std::vector<std::string> column_names,
    std::string_view batch_size,
    ResultOrder result_order) {
    // Reset managed query
    mq_->reset();

    if (!column_names.empty()) {
        mq_->select_columns(column_names);
    }

    switch (result_order) {
        case ResultOrder::automatic:
            if (arr_->schema().array_type() == TILEDB_SPARSE)
                mq_->set_layout(TILEDB_UNORDERED);
            else
                mq_->set_layout(TILEDB_ROW_MAJOR);
            break;
        case ResultOrder::rowmajor:
            mq_->set_layout(TILEDB_ROW_MAJOR);
            break;
        case ResultOrder::colmajor:
            mq_->set_layout(TILEDB_COL_MAJOR);
            break;
        default:
            throw std::invalid_argument(fmt::format(
                "[SOMAArray] invalid ResultOrder({}) passed",
                static_cast<int>(result_order)));
    }

    batch_size_ = batch_size;
    result_order_ = result_order;
    first_read_next_ = true;
    submitted_ = false;
}

std::optional<std::shared_ptr<ArrayBuffers>> SOMAArray::read_next() {
    // If the query is complete, return `std::nullopt`
    if (mq_->is_complete(true)) {
        return std::nullopt;
    }

    // Configure query and allocate result buffers
    mq_->setup_read();

    // Continue to submit the empty query on first read to return empty results
    if (mq_->is_empty_query()) {
        if (first_read_next_) {
            first_read_next_ = false;
            return mq_->results();
        } else {
            return std::nullopt;
        }
    }

    first_read_next_ = false;

    mq_->submit_read();

    // Return the results, possibly incomplete
    return mq_->results();
}

uint64_t SOMAArray::_get_max_capacity(tiledb_datatype_t index_type) {
    switch (index_type) {
        case TILEDB_INT8:
            return std::numeric_limits<int8_t>::max();
        case TILEDB_UINT8:
            return std::numeric_limits<uint8_t>::max();
        case TILEDB_INT16:
            return std::numeric_limits<int16_t>::max();
        case TILEDB_UINT16:
            return std::numeric_limits<uint16_t>::max();
        case TILEDB_INT32:
            return std::numeric_limits<int32_t>::max();
        case TILEDB_UINT32:
            return std::numeric_limits<uint32_t>::max();
        case TILEDB_INT64:
            return std::numeric_limits<int64_t>::max();
        case TILEDB_UINT64:
            return std::numeric_limits<uint64_t>::max();
        default:
            throw TileDBSOMAError(
                "Saw invalid enumeration index type when trying to extend "
                "enumeration");
    }
}

ArraySchemaEvolution SOMAArray::_make_se() {
    ArraySchemaEvolution se(*ctx_->tiledb_ctx());
    return se;
}

void SOMAArray::set_column_data(
    std::string_view name,
    uint64_t num_elems,
    const void* data,
    uint64_t* offsets,
    uint8_t* validity) {
    mq_->setup_write_column(name, num_elems, data, offsets, validity);
};

void SOMAArray::set_column_data(
    std::string_view name,
    uint64_t num_elems,
    const void* data,
    uint32_t* offsets,
    uint8_t* validity) {
    mq_->setup_write_column(name, num_elems, data, offsets, validity);
};

void SOMAArray::set_array_data(
    std::unique_ptr<ArrowSchema> arrow_schema,
    std::unique_ptr<ArrowArray> arrow_array) {
    if (mq_->query_type() != TILEDB_WRITE) {
        throw TileDBSOMAError("[SOMAArray] array must be opened in write mode");
    }

    // Clear any existing columns set in the ArrayBuffers
    reset(column_names(), batch_size_, result_order_);

    // Go through all columns in the ArrowTable and cast the values to what is
    // in the ArraySchema on disk
    ArraySchemaEvolution se = _make_se();
    bool evolve_schema = false;
    for (auto i = 0; i < arrow_schema->n_children; ++i) {
        bool enmr_extended = _cast_column(
            arrow_schema->children[i], arrow_array->children[i], se);
        evolve_schema = evolve_schema || enmr_extended;
    }
    if (evolve_schema) {
        se.array_evolve(uri_);
    }
};

bool SOMAArray::_cast_column(
    ArrowSchema* schema, ArrowArray* array, ArraySchemaEvolution se) {
    auto user_type = ArrowAdapter::to_tiledb_format(schema->format);
    bool has_attr = tiledb_schema()->has_attribute(schema->name);

    // If the attribute is enumerated, but the provided column is not, error out
    if (has_attr && attr_has_enum(schema->name)) {
        if (schema->dictionary == nullptr || array->dictionary == nullptr) {
            throw std::invalid_argument(
                "[SOMAArray] " + std::string(schema->name) +
                " requires dictionary entry");
        }
    }

    // If the attribute is not enumerated, but the provided column is, then we
    // need to use the dictionary values when writing to the array
    if (has_attr && !attr_has_enum(schema->name)) {
        if (schema->dictionary != nullptr && array->dictionary != nullptr) {
            _promote_indexes_to_values(schema, array);

            // Return false because we do not extend the enumeration
            return false;
        }
    }

    // In the general cases that do not apply to the two cases above, we need to
    // cast the passed-in column to be what is the type in the schema on disk.
    // Here we identify the passed-in column type (UserType).
    //
    // If _cast_column_aux extended the enumeration then return true. Otherwise,
    // false
    switch (user_type) {
        case TILEDB_STRING_ASCII:
        case TILEDB_STRING_UTF8:
        case TILEDB_CHAR:
        case TILEDB_GEOM_WKB:
        case TILEDB_GEOM_WKT:
            return _cast_column_aux<std::string>(schema, array, se);
        case TILEDB_BOOL:
            return _cast_column_aux<bool>(schema, array, se);
        case TILEDB_INT8:
            return _cast_column_aux<int8_t>(schema, array, se);
        case TILEDB_UINT8:
            return _cast_column_aux<uint8_t>(schema, array, se);
        case TILEDB_INT16:
            return _cast_column_aux<int16_t>(schema, array, se);
        case TILEDB_UINT16:
            return _cast_column_aux<uint16_t>(schema, array, se);
        case TILEDB_INT32:
            return _cast_column_aux<int32_t>(schema, array, se);
        case TILEDB_UINT32:
            return _cast_column_aux<uint32_t>(schema, array, se);
        case TILEDB_INT64:
        case TILEDB_DATETIME_YEAR:
        case TILEDB_DATETIME_MONTH:
        case TILEDB_DATETIME_WEEK:
        case TILEDB_DATETIME_DAY:
        case TILEDB_DATETIME_HR:
        case TILEDB_DATETIME_MIN:
        case TILEDB_DATETIME_SEC:
        case TILEDB_DATETIME_MS:
        case TILEDB_DATETIME_US:
        case TILEDB_DATETIME_NS:
        case TILEDB_DATETIME_PS:
        case TILEDB_DATETIME_FS:
        case TILEDB_DATETIME_AS:
        case TILEDB_TIME_HR:
        case TILEDB_TIME_MIN:
        case TILEDB_TIME_SEC:
        case TILEDB_TIME_MS:
        case TILEDB_TIME_US:
        case TILEDB_TIME_NS:
        case TILEDB_TIME_PS:
        case TILEDB_TIME_FS:
        case TILEDB_TIME_AS:
            return _cast_column_aux<int64_t>(schema, array, se);
        case TILEDB_UINT64:
            return _cast_column_aux<uint64_t>(schema, array, se);
        case TILEDB_FLOAT32:
            return _cast_column_aux<float>(schema, array, se);
        case TILEDB_FLOAT64:
            return _cast_column_aux<double>(schema, array, se);
        default:
            throw TileDBSOMAError(fmt::format(
                "Saw invalid TileDB user type when attempting to cast table: "
                "{}",
                tiledb::impl::type_to_str(user_type)));
    }
}

void SOMAArray::_promote_indexes_to_values(
    ArrowSchema* schema, ArrowArray* array) {
    // This is a column with a dictionary. However, the associated TileDB
    // attribute on disk is not enumerated. We will need to map the dictionary
    // indexes to the associated dictionary values and write the values to disk.
    // Here, we identify the passed-in column type

    auto value_type = ArrowAdapter::to_tiledb_format(
        schema->dictionary->format);
    switch (value_type) {
        case TILEDB_STRING_ASCII:
        case TILEDB_STRING_UTF8:
        case TILEDB_CHAR:
        case TILEDB_GEOM_WKB:
        case TILEDB_GEOM_WKT:
            return _cast_dictionary_values<std::string>(schema, array);
        case TILEDB_BOOL:
            return _cast_dictionary_values<bool>(schema, array);
        case TILEDB_INT8:
            return _cast_dictionary_values<int8_t>(schema, array);
        case TILEDB_UINT8:
            return _cast_dictionary_values<uint8_t>(schema, array);
        case TILEDB_INT16:
            return _cast_dictionary_values<int16_t>(schema, array);
        case TILEDB_UINT16:
            return _cast_dictionary_values<uint16_t>(schema, array);
        case TILEDB_INT32:
            return _cast_dictionary_values<int32_t>(schema, array);
        case TILEDB_UINT32:
            return _cast_dictionary_values<uint32_t>(schema, array);
        case TILEDB_INT64:
            return _cast_dictionary_values<int64_t>(schema, array);
        case TILEDB_DATETIME_YEAR:
        case TILEDB_DATETIME_MONTH:
        case TILEDB_DATETIME_WEEK:
        case TILEDB_DATETIME_DAY:
        case TILEDB_DATETIME_HR:
        case TILEDB_DATETIME_MIN:
        case TILEDB_DATETIME_SEC:
        case TILEDB_DATETIME_MS:
        case TILEDB_DATETIME_US:
        case TILEDB_DATETIME_NS:
        case TILEDB_DATETIME_PS:
        case TILEDB_DATETIME_FS:
        case TILEDB_DATETIME_AS:
        case TILEDB_TIME_HR:
        case TILEDB_TIME_MIN:
        case TILEDB_TIME_SEC:
        case TILEDB_TIME_MS:
        case TILEDB_TIME_US:
        case TILEDB_TIME_NS:
        case TILEDB_TIME_PS:
        case TILEDB_TIME_FS:
        case TILEDB_TIME_AS:
        case TILEDB_UINT64:
            return _cast_dictionary_values<uint64_t>(schema, array);
        case TILEDB_FLOAT32:
            return _cast_dictionary_values<float>(schema, array);
        case TILEDB_FLOAT64:
            return _cast_dictionary_values<double>(schema, array);
        default:
            throw TileDBSOMAError(fmt::format(
                "Saw invalid TileDB value type when attempting to promote "
                "indexes to values: {}",
                tiledb::impl::type_to_str(value_type)));
    }
}

template <typename T>
void SOMAArray::_cast_dictionary_values(
    ArrowSchema* schema, ArrowArray* array) {
    // This is a column with a dictionary. However, the associated TileDB
    // attribute on disk is not enumerated. Here, we map the dictionary indexes
    // to the associated dictionary values and set the buffers to use the
    // dictionary values to write to disk. Note the specialized templates for
    // string and Boolean types below

    auto value_array = array->dictionary;

    T* valbuf;
    if (value_array->n_buffers == 3) {
        valbuf = (T*)value_array->buffers[2];
    } else {
        valbuf = (T*)value_array->buffers[1];
    }
    std::vector<T> values(valbuf, valbuf + value_array->length);

    std::vector<int64_t> indexes = _get_index_vector(schema, array);

    std::vector<T> index_to_value;
    for (auto i : indexes) {
        index_to_value.push_back(values[i]);
    }

    mq_->setup_write_column(
        schema->name,
        array->length,
        (const void*)index_to_value.data(),
        (uint64_t*)nullptr,
        (uint8_t*)value_array->buffers[0]);
}

template <>
void SOMAArray::_cast_dictionary_values<std::string>(
    ArrowSchema* schema, ArrowArray* array) {
    // String types require special handling due to large vs regular
    // string/binary

    auto value_schema = schema->dictionary;
    auto value_array = array->dictionary;

    uint64_t num_elems = value_array->length;

    std::vector<uint64_t> offsets_v;
    if ((strcmp(value_schema->format, "U") == 0) ||
        (strcmp(value_schema->format, "Z") == 0)) {
        uint64_t* offsets = (uint64_t*)value_array->buffers[1];
        offsets_v.resize(num_elems + 1);
        offsets_v.assign(offsets, offsets + num_elems + 1);
    } else {
        uint32_t* offsets = (uint32_t*)value_array->buffers[1];
        std::vector<uint32_t> offset_holder(offsets, offsets + num_elems + 1);
        for (auto offset : offset_holder) {
            offsets_v.push_back((uint64_t)offset);
        }
    }

    char* data = (char*)value_array->buffers[2];
    std::string data_v(data, data + offsets_v[offsets_v.size() - 1]);

    std::vector<std::string> values;
    for (size_t offset_idx = 0; offset_idx < offsets_v.size() - 1;
         ++offset_idx) {
        auto beg = offsets_v[offset_idx];
        auto sz = offsets_v[offset_idx + 1] - beg;
        values.push_back(data_v.substr(beg, sz));
    }

    std::vector<int64_t> indexes = SOMAArray::_get_index_vector(schema, array);

    uint64_t offset_sum = 0;
    std::vector<uint64_t> value_offsets = {0};
    std::string index_to_value;
    for (auto i : indexes) {
        auto value = values[i];
        offset_sum += value.size();
        value_offsets.push_back(offset_sum);
        index_to_value.insert(index_to_value.end(), value.begin(), value.end());
    }

    mq_->setup_write_column(
        schema->name,
        value_offsets.size() - 1,
        (const void*)index_to_value.data(),
        (uint64_t*)value_offsets.data(),
        (uint8_t*)value_array->buffers[0]);
}

template <>
void SOMAArray::_cast_dictionary_values<bool>(
    ArrowSchema* schema, ArrowArray* array) {
    // Boolean types require special handling due to bit vs uint8_t
    // representation in Arrow vs TileDB respectively

    auto value_schema = schema->dictionary;
    auto value_array = array->dictionary;

    std::vector<int64_t> indexes = _get_index_vector(schema, array);
    std::vector<uint8_t> values = util::cast_bit_to_uint8(
        value_schema, value_array);
    std::vector<uint8_t> index_to_value;

    for (auto i : indexes) {
        index_to_value.push_back(values[i]);
    }

    mq_->setup_write_column(
        schema->name,
        array->length,
        (const void*)index_to_value.data(),
        (uint64_t*)nullptr,
        (uint8_t*)value_array->buffers[0]);
}

template <typename UserType>
bool SOMAArray::_cast_column_aux(
    ArrowSchema* schema, ArrowArray* array, ArraySchemaEvolution se) {
    // We need to cast the passed-in column to be what is the type in the schema
    // on disk. Here we identify the on-disk attribute or dimension type
    // (DiskType).

    tiledb_datatype_t disk_type;
    std::string name(schema->name);
    if (tiledb_schema()->has_attribute(name)) {
        disk_type = tiledb_schema()->attribute(name).type();
    } else {
        disk_type = tiledb_schema()->domain().dimension(name).type();
    }

    // If _set_column extended the enumeration then return true. Otherwise,
    // false
    switch (disk_type) {
        case TILEDB_BOOL:
        case TILEDB_INT8:
            return _set_column<UserType, int8_t>(schema, array, se);
        case TILEDB_UINT8:
            return _set_column<UserType, uint8_t>(schema, array, se);
        case TILEDB_INT16:
            return _set_column<UserType, int16_t>(schema, array, se);
        case TILEDB_UINT16:
            return _set_column<UserType, uint16_t>(schema, array, se);
        case TILEDB_INT32:
            return _set_column<UserType, int32_t>(schema, array, se);
        case TILEDB_UINT32:
            return _set_column<UserType, uint32_t>(schema, array, se);
        case TILEDB_INT64:
        case TILEDB_DATETIME_YEAR:
        case TILEDB_DATETIME_MONTH:
        case TILEDB_DATETIME_WEEK:
        case TILEDB_DATETIME_DAY:
        case TILEDB_DATETIME_HR:
        case TILEDB_DATETIME_MIN:
        case TILEDB_DATETIME_SEC:
        case TILEDB_DATETIME_MS:
        case TILEDB_DATETIME_US:
        case TILEDB_DATETIME_NS:
        case TILEDB_DATETIME_PS:
        case TILEDB_DATETIME_FS:
        case TILEDB_DATETIME_AS:
        case TILEDB_TIME_HR:
        case TILEDB_TIME_MIN:
        case TILEDB_TIME_SEC:
        case TILEDB_TIME_MS:
        case TILEDB_TIME_US:
        case TILEDB_TIME_NS:
        case TILEDB_TIME_PS:
        case TILEDB_TIME_FS:
        case TILEDB_TIME_AS:
            return _set_column<UserType, int64_t>(schema, array, se);
        case TILEDB_UINT64:
            return _set_column<UserType, uint64_t>(schema, array, se);
        case TILEDB_FLOAT32:
            return _set_column<UserType, float>(schema, array, se);
        case TILEDB_FLOAT64:
            return _set_column<UserType, double>(schema, array, se);
        default:
            throw TileDBSOMAError(
                "Saw invalid TileDB disk type when attempting to cast "
                "column: " +
                tiledb::impl::type_to_str(disk_type));
    }
}

template <>
bool SOMAArray::_cast_column_aux<std::string>(
    ArrowSchema* schema, ArrowArray* array, ArraySchemaEvolution se) {
    (void)se;  // se is unused in std::string specialization

    const void* data = nullptr;
    const void* offset = nullptr;
    const void* validity = nullptr;

    if (array->n_buffers == 3) {
        data = array->buffers[2];
        offset = array->buffers[1];
        validity = array->buffers[0];
    } else {
        data = array->buffers[1];
        offset = nullptr;
        validity = array->buffers[0];
    }

    if ((strcmp(schema->format, "U") == 0) ||
        (strcmp(schema->format, "Z") == 0)) {
        mq_->setup_write_column(
            schema->name,
            array->length,
            (const void*)data,
            (uint64_t*)offset,
            (uint8_t*)validity);
    } else {
        mq_->setup_write_column(
            schema->name,
            array->length,
            (const void*)data,
            (uint32_t*)offset,
            (uint8_t*)validity);
    }
    return false;
}

template <>
bool SOMAArray::_cast_column_aux<bool>(
    ArrowSchema* schema, ArrowArray* array, ArraySchemaEvolution se) {
    (void)se;  // se is unused in bool specialization

    auto casted = util::cast_bit_to_uint8(schema, array);
    mq_->setup_write_column(
        schema->name,
        array->length,
        (const void*)casted.data(),
        (uint64_t*)nullptr,
        (uint8_t*)array->buffers[0]);
    return false;
}

bool SOMAArray::_extend_enumeration(
    ArrowSchema* value_schema,
    ArrowArray* value_array,
    ArrowSchema* index_schema,
    ArrowArray* index_array,
    ArraySchemaEvolution se) {
    // For columns with dictionaries, we need to identify whether the

    auto enmr = ArrayExperimental::get_enumeration(
        *ctx_->tiledb_ctx(), *arr_, index_schema->name);
    auto value_type = enmr.type();

    switch (value_type) {
        case TILEDB_STRING_ASCII:
        case TILEDB_STRING_UTF8:
        case TILEDB_CHAR:
        case TILEDB_GEOM_WKB:
        case TILEDB_GEOM_WKT:
            return _extend_and_evolve_schema<std::string>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_INT8:
            return _extend_and_evolve_schema<int8_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_BOOL:
        case TILEDB_UINT8:
            return _extend_and_evolve_schema<uint8_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_INT16:
            return _extend_and_evolve_schema<int16_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_UINT16:
            return _extend_and_evolve_schema<uint16_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_INT32:
            return _extend_and_evolve_schema<int32_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_UINT32:
            return _extend_and_evolve_schema<uint32_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_INT64:
            return _extend_and_evolve_schema<int64_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_UINT64:
            return _extend_and_evolve_schema<uint64_t>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_FLOAT32:
            return _extend_and_evolve_schema<float>(
                value_schema, value_array, index_schema, index_array, se);
        case TILEDB_FLOAT64:
            return _extend_and_evolve_schema<double>(
                value_schema, value_array, index_schema, index_array, se);
        default:
            throw TileDBSOMAError(fmt::format(
                "ArrowAdapter: Unsupported TileDB dict datatype: {} ",
                tiledb::impl::type_to_str(value_type)));
    }
}

template <typename ValueType>
bool SOMAArray::_extend_and_evolve_schema(
    ArrowSchema* value_schema,
    ArrowArray* value_array,
    ArrowSchema* index_schema,
    ArrowArray* index_array,
    ArraySchemaEvolution se) {
    // We need to check if we are writing any new enumeration values. If so,
    // extend and evolve the schema. If not, just set the write buffers to the
    // dictionary's indexes as-is

    // Get all the enumeration values in the passed-in column
    std::vector<ValueType> enums_in_write;
    uint64_t num_elems = value_array->length;
    if (strcmp(value_schema->format, "b") == 0) {
        // Specially handle Boolean types as their representation in Arrow (bit)
        // is different from what is in TileDB (uint8_t)
        auto casted = util::cast_bit_to_uint8(value_schema, value_array);
        enums_in_write.assign(
            (ValueType*)casted.data(), (ValueType*)casted.data() + num_elems);
    } else {
        // General case
        const void* data;
        if (value_array->n_buffers == 3) {
            data = value_array->buffers[2];
        } else {
            data = value_array->buffers[1];
        }
        enums_in_write.assign((ValueType*)data, (ValueType*)data + num_elems);
    }

    // Get all the enumeration values in the on-disk TileDB attribute
    std::string column_name = index_schema->name;
    auto enmr = ArrayExperimental::get_enumeration(
        *ctx_->tiledb_ctx(), *arr_, column_name);
    std::vector<ValueType> enums_existing = enmr.as_vector<ValueType>();

    // Find any new enumeration values
    std::vector<ValueType> extend_values;
    for (auto enum_val : enums_in_write) {
        if (std::find(enums_existing.begin(), enums_existing.end(), enum_val) ==
            enums_existing.end()) {
            extend_values.push_back(enum_val);
        }
    }

    // extend_values = {true, false};
    if (extend_values.size() != 0) {
        // We have new enumeration values; additional processing needed

        // Check if the number of new enumeration will cause an overflow if
        // extended
        auto disk_index_type = tiledb_schema()->attribute(column_name).type();
        uint64_t max_capacity = _get_max_capacity(disk_index_type);
        auto free_capacity = max_capacity - enums_existing.size();
        if (free_capacity < extend_values.size()) {
            throw TileDBSOMAError(
                "Cannot extend enumeration; reached maximum capacity");
        }

        // Take the existing enumeration values on disk and extend with the new
        // enumeration values
        auto extended_enmr = enmr.extend(extend_values);
        se.extend_enumeration(extended_enmr);

        // If the passed-in enumerations are only a subset of the new extended
        // enumerations, then we will need to remap the indexes. ie. the user
        // passes in values [B, C] which maps to indexes [0, 1]. However, the
        // full set of extended enumerations is [A, B, C] which means we need to
        // remap [B, C] to be indexes [1, 2]
        SOMAArray::_remap_indexes(
            column_name,
            extended_enmr,
            enums_in_write,
            index_schema,
            index_array);

        // The enumeration was extended
        return true;
    } else {
        // Example:
        //
        // * Already on storage/schema there are values a,b,c with indices
        //   0,1,2.
        // * User appends values b,c which, within the Arrow data coming in
        //   from the user, have indices 0,1.
        // * We need to remap those to 1,2.
        SOMAArray::_remap_indexes(
            column_name, enmr, enums_in_write, index_schema, index_array);

        // The enumeration was not extended
        return false;
    }
}

template <>
bool SOMAArray::_extend_and_evolve_schema<std::string>(
    ArrowSchema* value_schema,
    ArrowArray* value_array,
    ArrowSchema* index_schema,
    ArrowArray* index_array,
    ArraySchemaEvolution se) {
    uint64_t num_elems = value_array->length;

    std::vector<uint64_t> offsets_v;
    if ((strcmp(value_schema->format, "U") == 0) ||
        (strcmp(value_schema->format, "Z") == 0)) {
        uint64_t* offsets = (uint64_t*)value_array->buffers[1];
        offsets_v.assign(offsets, offsets + num_elems + 1);
    } else {
        uint32_t* offsets = (uint32_t*)value_array->buffers[1];
        for (size_t i = 0; i < num_elems + 1; ++i) {
            offsets_v.push_back((uint64_t)offsets[i]);
        }
    }

    char* data = (char*)value_array->buffers[2];
    std::string data_v(data, data + offsets_v[num_elems]);

    std::vector<std::string> enums_in_write;
    for (size_t i = 0; i < num_elems; ++i) {
        auto beg = offsets_v[i];
        auto sz = offsets_v[i + 1] - beg;
        enums_in_write.push_back(data_v.substr(beg, sz));
    }

    std::string column_name = index_schema->name;
    auto enmr = ArrayExperimental::get_enumeration(
        *ctx_->tiledb_ctx(), *arr_, column_name);
    std::vector<std::string> extend_values;
    auto enums_existing = enmr.as_vector<std::string>();
    for (auto enum_val : enums_in_write) {
        if (std::find(enums_existing.begin(), enums_existing.end(), enum_val) ==
            enums_existing.end()) {
            extend_values.push_back(enum_val);
        }
    }

    if (extend_values.size() != 0) {
        // Check that we extend the enumeration values without
        // overflowing
        auto disk_index_type = tiledb_schema()->attribute(column_name).type();
        uint64_t max_capacity = SOMAArray::_get_max_capacity(disk_index_type);
        auto free_capacity = max_capacity - enums_existing.size();
        if (free_capacity < extend_values.size()) {
            throw TileDBSOMAError(
                "Cannot extend enumeration; reached maximum capacity");
        }

        auto extended_enmr = enmr.extend(extend_values);
        se.extend_enumeration(extended_enmr);

        SOMAArray::_remap_indexes(
            column_name,
            extended_enmr,
            enums_in_write,
            index_schema,
            index_array);

        return true;
    } else {
        // Example:
        //
        // * Already on storage/schema there are values a,b,c with indices
        //   0,1,2.
        // * User appends values b,c which, within the Arrow data coming in
        //   from the user, have indices 0,1.
        // * We need to remap those to 1,2.

        SOMAArray::_remap_indexes(
            column_name, enmr, enums_in_write, index_schema, index_array);
    }
    return false;
}

uint64_t SOMAArray::ndim() const {
    return tiledb_schema()->domain().ndim();
}

std::vector<std::string> SOMAArray::dimension_names() const {
    std::vector<std::string> result;
    auto dimensions = tiledb_schema()->domain().dimensions();
    for (const auto& dim : dimensions) {
        result.push_back(dim.name());
    }
    return result;
}

bool SOMAArray::has_dimension_name(const std::string& name) const {
    auto dimensions = tiledb_schema()->domain().dimensions();
    for (const auto& dim : dimensions) {
        if (dim.name() == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> SOMAArray::attribute_names() const {
    std::vector<std::string> result;
    auto schema = tiledb_schema();
    unsigned n = schema->attribute_num();
    for (unsigned i = 0; i < n; i++) {
        result.push_back(schema->attribute(i).name());
    }
    return result;
}

void SOMAArray::write(bool sort_coords) {
    if (mq_->query_type() != TILEDB_WRITE) {
        throw TileDBSOMAError("[SOMAArray] array must be opened in write mode");
    }
    mq_->submit_write(sort_coords);

    mq_->reset();
}

void SOMAArray::consolidate_and_vacuum(std::vector<std::string> modes) {
    for (auto mode : modes) {
        auto cfg = ctx_->tiledb_ctx()->config();
        cfg["sm.consolidation.mode"] = mode;
        Array::consolidate(Context(cfg), uri_);
        Array::vacuum(Context(cfg), uri_);
    }
}

std::map<std::string, Enumeration> SOMAArray::get_attr_to_enum_mapping() {
    std::map<std::string, Enumeration> result;
    for (uint32_t i = 0; i < arr_->schema().attribute_num(); ++i) {
        auto attr = arr_->schema().attribute(i);
        if (attr_has_enum(attr.name())) {
            auto enmr_label = *get_enum_label_on_attr(attr.name());
            auto enmr = ArrayExperimental::get_enumeration(
                *ctx_->tiledb_ctx(), *arr_, enmr_label);
            result.insert({attr.name(), enmr});
        }
    }
    return result;
}

std::optional<std::string> SOMAArray::get_enum_label_on_attr(
    std::string attr_name) {
    auto attr = arr_->schema().attribute(attr_name);
    return AttributeExperimental::get_enumeration_name(
        *ctx_->tiledb_ctx(), attr);
}

bool SOMAArray::attr_has_enum(std::string attr_name) {
    return get_enum_label_on_attr(attr_name).has_value();
}

void SOMAArray::set_metadata(
    const std::string& key,
    tiledb_datatype_t value_type,
    uint32_t value_num,
    const void* value,
    bool force) {
    if (!force && key.compare(SOMA_OBJECT_TYPE_KEY) == 0)
        throw TileDBSOMAError(SOMA_OBJECT_TYPE_KEY + " cannot be modified.");

    if (!force && key.compare(ENCODING_VERSION_KEY) == 0)
        throw TileDBSOMAError(ENCODING_VERSION_KEY + " cannot be modified.");

    arr_->put_metadata(key, value_type, value_num, value);

    MetadataValue mdval(value_type, value_num, value);
    std::pair<std::string, const MetadataValue> mdpair(key, mdval);
    metadata_.insert(mdpair);
}

void SOMAArray::delete_metadata(const std::string& key, bool force) {
    if (!force && key.compare(SOMA_OBJECT_TYPE_KEY) == 0) {
        throw TileDBSOMAError(SOMA_OBJECT_TYPE_KEY + " cannot be deleted.");
    }

    if (!force && key.compare(ENCODING_VERSION_KEY) == 0) {
        throw TileDBSOMAError(ENCODING_VERSION_KEY + " cannot be deleted.");
    }

    arr_->delete_metadata(key);
    metadata_.erase(key);
}

std::optional<MetadataValue> SOMAArray::get_metadata(const std::string& key) {
    if (metadata_.count(key) == 0) {
        return std::nullopt;
    }

    return metadata_[key];
}

std::map<std::string, MetadataValue> SOMAArray::get_metadata() {
    return metadata_;
}

bool SOMAArray::has_metadata(const std::string& key) {
    return metadata_.count(key) != 0;
}

uint64_t SOMAArray::metadata_num() const {
    return metadata_.size();
}

void SOMAArray::validate(
    OpenMode mode,
    std::string_view name,
    std::optional<TimestampRange> timestamp) {
    // Validate parameters
    auto tdb_mode = mode == OpenMode::read ? TILEDB_READ : TILEDB_WRITE;

    try {
        LOG_DEBUG(fmt::format("[SOMAArray] opening array '{}'", uri_));
        if (timestamp) {
            arr_ = std::make_shared<Array>(
                *ctx_->tiledb_ctx(),
                uri_,
                tdb_mode,
                TemporalPolicy(
                    TimestampStartEnd, timestamp->first, timestamp->second));
        } else {
            arr_ = std::make_shared<Array>(*ctx_->tiledb_ctx(), uri_, tdb_mode);
        }
        LOG_TRACE(fmt::format("[SOMAArray] loading enumerations"));
        ArrayExperimental::load_all_enumerations(
            *ctx_->tiledb_ctx(), *(arr_.get()));
        mq_ = std::make_unique<ManagedQuery>(arr_, ctx_->tiledb_ctx(), name);
    } catch (const std::exception& e) {
        throw TileDBSOMAError(
            fmt::format("Error opening array: '{}'\n  {}", uri_, e.what()));
    }
}

std::optional<TimestampRange> SOMAArray::timestamp() {
    return timestamp_;
}

// Note that ArrowTable is simply our libtiledbsoma pairing of ArrowArray and
// ArrowSchema from nanoarrow.
//
// The domainish enum simply lets us re-use code which is common across
// core domain, core current domain, and core non-empty domain.
ArrowTable SOMAArray::_get_core_domainish(enum Domainish which_kind) {
    int array_ndim = this->ndim();
    auto dimensions = tiledb_schema()->domain().dimensions();

    // Create the schema for the info we return
    std::vector<std::string> names(array_ndim);
    std::vector<tiledb_datatype_t> tiledb_datatypes(array_ndim);

    for (int i = 0; i < (int)array_ndim; i++) {
        const Dimension& core_dim = dimensions[i];
        names[i] = core_dim.name();
        tiledb_datatypes[i] = core_dim.type();
    }

    auto arrow_schema = ArrowAdapter::make_arrow_schema(
        names, tiledb_datatypes);

    // Create the data for the info we return
    auto arrow_array = ArrowAdapter::make_arrow_array_parent(array_ndim);

    for (int i = 0; i < array_ndim; i++) {
        auto core_dim = dimensions[i];
        auto core_type_code = core_dim.type();

        ArrowArray* child = nullptr;

        switch (core_type_code) {
            case TILEDB_INT64:
            case TILEDB_DATETIME_YEAR:
            case TILEDB_DATETIME_MONTH:
            case TILEDB_DATETIME_WEEK:
            case TILEDB_DATETIME_DAY:
            case TILEDB_DATETIME_HR:
            case TILEDB_DATETIME_MIN:
            case TILEDB_DATETIME_SEC:
            case TILEDB_DATETIME_MS:
            case TILEDB_DATETIME_US:
            case TILEDB_DATETIME_NS:
            case TILEDB_DATETIME_PS:
            case TILEDB_DATETIME_FS:
            case TILEDB_DATETIME_AS:
            case TILEDB_TIME_HR:
            case TILEDB_TIME_MIN:
            case TILEDB_TIME_SEC:
            case TILEDB_TIME_MS:
            case TILEDB_TIME_US:
            case TILEDB_TIME_NS:
            case TILEDB_TIME_PS:
            case TILEDB_TIME_FS:
            case TILEDB_TIME_AS:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<int64_t>(core_dim.name(), which_kind));
                break;
            case TILEDB_UINT64:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<uint64_t>(
                        core_dim.name(), which_kind));
                break;
            case TILEDB_INT32:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<int32_t>(core_dim.name(), which_kind));
                break;
            case TILEDB_UINT32:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<uint32_t>(
                        core_dim.name(), which_kind));
                break;
            case TILEDB_INT16:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<int16_t>(core_dim.name(), which_kind));
                break;
            case TILEDB_UINT16:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<uint16_t>(
                        core_dim.name(), which_kind));
                break;
            case TILEDB_INT8:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<int8_t>(core_dim.name(), which_kind));
                break;
            case TILEDB_UINT8:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<uint8_t>(core_dim.name(), which_kind));
                break;

            case TILEDB_FLOAT64:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<double>(core_dim.name(), which_kind));
                break;
            case TILEDB_FLOAT32:
                child = ArrowAdapter::make_arrow_array_child(
                    _core_domainish_slot<float>(core_dim.name(), which_kind));
                break;

            case TILEDB_STRING_ASCII:
            case TILEDB_CHAR:
            case TILEDB_GEOM_WKB:
            case TILEDB_GEOM_WKT:
                child = ArrowAdapter::make_arrow_array_child_string(
                    _core_domainish_slot_string(core_dim.name(), which_kind));
                break;

            default:
                throw TileDBSOMAError(fmt::format(
                    "SOMAArray::_get_core_domainish:dim {} has unhandled type "
                    "{}",
                    core_dim.name(),
                    tiledb::impl::type_to_str(core_type_code)));
        }
        arrow_array->children[i] = child;
    }

    return ArrowTable(std::move(arrow_array), std::move(arrow_schema));
}

uint64_t SOMAArray::nnz() {
    // Verify array is sparse
    if (mq_->schema()->array_type() != TILEDB_SPARSE) {
        throw TileDBSOMAError(
            "[SOMAArray] nnz is only supported for sparse arrays");
    }

    // Load fragment info
    FragmentInfo fragment_info(*ctx_->tiledb_ctx(), uri_);
    fragment_info.load();

    LOG_DEBUG(fmt::format("[SOMAArray] Fragment info for array '{}'", uri_));
    if (LOG_DEBUG_ENABLED()) {
        fragment_info.dump();
    }

    // Find the subset of fragments contained within the read timestamp range
    // [if any]
    std::vector<uint32_t> relevant_fragments;
    for (uint32_t fid = 0; fid < fragment_info.fragment_num(); fid++) {
        auto frag_ts = fragment_info.timestamp_range(fid);
        assert(frag_ts.first <= frag_ts.second);
        if (timestamp_) {
            if (frag_ts.first > timestamp_->second ||
                frag_ts.second < timestamp_->first) {
                // fragment is fully outside the read timestamp range: skip it
                continue;
            } else if (!(frag_ts.first >= timestamp_->first &&
                         frag_ts.second <= timestamp_->second)) {
                // fragment overlaps read timestamp range, but isn't fully
                // contained within: fall back to count_cells to sort that out.
                return _nnz_slow();
            }
        }
        // fall through: fragment is fully contained within the read timestamp
        // range
        relevant_fragments.push_back(fid);

        // If any relevant fragment is a consolidated fragment, fall back to
        // counting cells, because the fragment may contain duplicates.
        // If the application is allowing duplicates (in which case it's the
        // application's job to otherwise ensure uniqueness), then
        // sum-over-fragments is the right thing to do.
        if (!mq_->schema()->allows_dups() && frag_ts.first != frag_ts.second) {
            return _nnz_slow();
        }
    }

    auto fragment_count = relevant_fragments.size();

    if (fragment_count == 0) {
        // No data have been written [in the read timestamp range]
        return 0;
    }

    if (fragment_count == 1) {
        // Only one fragment; return its cell_num
        return fragment_info.cell_num(relevant_fragments[0]);
    }

    // Check for overlapping fragments on the first dimension and
    // compute total_cell_num while going through the loop
    uint64_t total_cell_num = 0;
    std::vector<std::array<uint64_t, 2>> non_empty_domains(fragment_count);

    // The loop after this only works if dim 0 is int64 soma_joinid or
    // soma_dim_0. That's the case for _almost_ all SOMADataFrame objects, but
    // not the "variant-indexed" ones: the SOMA spec only requires
    // that soma_joinid be present as a dim or an attr. It's true for all
    // SOMASparseNDArray objects.
    auto dim = tiledb_schema()->domain().dimension(0);
    auto dim_name = dim.name();
    auto type_code = dim.type();
    if ((dim_name != "soma_joinid" && dim_name != "soma_dim_0") ||
        type_code != TILEDB_INT64) {
        LOG_DEBUG(fmt::format(
            "[SOMAArray::nnz] dim 0 (type={} name={}) isn't int64 "
            "soma_joinid or int64 soma_dim_0: using _nnz_slow",
            tiledb::impl::type_to_str(type_code),
            dim_name));
        return _nnz_slow();
    }

    for (uint32_t i = 0; i < fragment_count; i++) {
        // TODO[perf]: Reading fragment info is not supported on TileDB Cloud
        // yet, but reading one fragment at a time will be slow. Is there
        // another way?
        total_cell_num += fragment_info.cell_num(relevant_fragments[i]);

        fragment_info.get_non_empty_domain(
            relevant_fragments[i], 0, &non_empty_domains[i]);

        LOG_DEBUG(fmt::format(
            "[SOMAArray] fragment {} non-empty domain = [{}, {}]",
            i,
            non_empty_domains[i][0],
            non_empty_domains[i][1]));
    }

    // Sort non-empty domains by the start of their ranges
    std::sort(non_empty_domains.begin(), non_empty_domains.end());

    // After sorting, if the end of a non-empty domain is >= the beginning of
    // the next non-empty domain, there is an overlap
    bool overlap = false;
    for (uint32_t i = 0; i < fragment_count - 1; i++) {
        LOG_DEBUG(fmt::format(
            "[SOMAArray] Checking {} < {}",
            non_empty_domains[i][1],
            non_empty_domains[i + 1][0]));
        if (non_empty_domains[i][1] >= non_empty_domains[i + 1][0]) {
            overlap = true;
            break;
        }
    }

    // If relevant fragments do not overlap, return the total cell_num
    if (!overlap) {
        return total_cell_num;
    }
    // Found relevant fragments with overlap, count cells
    return _nnz_slow();
}

uint64_t SOMAArray::_nnz_slow() {
    LOG_DEBUG(
        "[SOMAArray] nnz() found consolidated or overlapping fragments, "
        "counting cells...");

    auto sr = SOMAArray::open(
        OpenMode::read,
        uri_,
        ctx_,
        "count_cells",
        {mq_->schema()->domain().dimension(0).name()},
        batch_size_,
        result_order_,
        timestamp_);

    uint64_t total_cell_num = 0;
    while (auto batch = sr->read_next()) {
        total_cell_num += (*batch)->num_rows();
    }

    return total_cell_num;
}

std::vector<int64_t> SOMAArray::shape() {
    // There are two reasons for this:
    // * Transitional, non-monolithic, phased, careful development for the
    //   new-shape feature
    // * Even after the new-shape feature is fully released, there will be old
    //   arrays on disk that were created before this feature existed.
    // So this is long-term code.
    return _get_current_domain().is_empty() ? _tiledb_domain() :
                                              _tiledb_current_domain();
}

std::vector<int64_t> SOMAArray::maxshape() {
    return _tiledb_domain();
}

// This is a helper for can_upgrade_shape and can_resize, which have
// much overlap.
StatusAndReason SOMAArray::_can_set_shape_helper(
    const std::vector<int64_t>& newshape,
    bool is_resize,
    std::string function_name_for_messages) {
    // E.g. it's an error to try to upgrade_domain or resize specifying
    // a 3-D shape on a 2-D array.
    auto arg_ndim = newshape.size();
    auto array_ndim = arr_->schema().domain().ndim();
    if (array_ndim != arg_ndim) {
        return std::pair(
            false,
            fmt::format(
                "{}: provided shape has ndim {}, while the array has {}",
                function_name_for_messages,
                arg_ndim,
                array_ndim));
    }

    // Enforce the semantics that tiledbsoma_upgrade_shape must be called
    // only on arrays that don't have a shape set, and resize must be called
    // only on arrays that do.
    bool has_shape = has_current_domain();
    if (is_resize) {
        // They're trying to do resize on an array that doesn't already have a
        // shape.
        if (!has_shape) {
            return std::pair(
                false,
                fmt::format(
                    "{}: array currently has no shape: please "
                    "upgrade the array.",
                    function_name_for_messages));
        }
    } else {
        // They're trying to do upgrade_shape on an array that already has a
        // shape.
        if (has_shape) {
            return std::pair(
                false,
                fmt::format(
                    "{}: array already has a shape: please use resize",
                    function_name_for_messages));
        }
    }

    // * For old-style arrays without shape: core domain (soma maxdomain) may be
    //   small (like 100) or big (like 2 billionish).
    // * For new-style arrays with shape: core current domain (soma domain) will
    //   probably be small and core domain (soma maxdomain) will be huge.
    //
    // In either case, we need to check that the user's requested shape isn't
    // outside the core domain, which is immutable. For old-style arrays,
    //
    // if the requested shape fits in the array's core domain, it's good to go
    // as a new shape.
    auto domain_check = _can_set_shape_domainish_subhelper(
        newshape, false, function_name_for_messages);
    if (!domain_check.first) {
        return domain_check;
    }

    // For new-style arrays, we need to additionally that the the requested
    // shape (core current domain) isn't a downsize of the current one.
    if (has_shape) {
        auto current_domain_check = _can_set_shape_domainish_subhelper(
            newshape, true, function_name_for_messages);
        if (!current_domain_check.first) {
            return current_domain_check;
        }
    }

    return std::pair(true, "");
}

// This is a helper for _can_set_shape_helper: it's used for comparing
// the user's requested shape against the core current domain or core (max)
// domain.
StatusAndReason SOMAArray::_can_set_shape_domainish_subhelper(
    const std::vector<int64_t>& newshape,
    bool check_current_domain,
    std::string function_name_for_messages) {
    Domain domain = arr_->schema().domain();

    for (unsigned i = 0; i < domain.ndim(); i++) {
        const auto& dim = domain.dimension(i);

        const std::string& dim_name = dim.name();

        // These methods are only for SOMA NDArrays, and any other arrays for
        // which the indices are entirely int64.  SOMA DataFrame objects, with
        // multi-type dims, need to go through upgrade_domain -- and this is
        // library-internal code, it's not the user's fault if we got here.
        if (dim.type() != TILEDB_INT64) {
            throw TileDBSOMAError(fmt::format(
                "{}: internal error: expected {} dim to be {}; got {}",
                function_name_for_messages,
                dim_name,
                tiledb::impl::type_to_str(TILEDB_INT64),
                tiledb::impl::type_to_str(dim.type())));
        }

        if (check_current_domain) {
            std::pair<int64_t, int64_t>
                cap = _core_current_domain_slot<int64_t>(dim_name);
            int64_t old_dim_shape = cap.second + 1;

            if (newshape[i] < old_dim_shape) {
                return std::pair(
                    false,
                    fmt::format(
                        "{} for {}: new {} < existing shape {}",
                        function_name_for_messages,
                        dim_name,
                        newshape[i],
                        old_dim_shape));
            }

        } else {
            std::pair<int64_t, int64_t> cap = _core_domain_slot<int64_t>(
                dim_name);
            int64_t old_dim_shape = cap.second + 1;

            if (newshape[i] > old_dim_shape) {
                return std::pair(
                    false,
                    fmt::format(
                        "{} for {}: new {} < maxshape {}",
                        function_name_for_messages,
                        dim_name,
                        newshape[i],
                        old_dim_shape));
            }
        }
    }
    return std::pair(true, "");
}

StatusAndReason SOMAArray::_can_set_soma_joinid_shape_helper(
    int64_t newshape, bool is_resize, std::string function_name_for_messages) {
    // Fail if the array doesn't already have a shape yet (they should upgrade
    // first).
    if (!is_resize) {
        // Upgrading an array to give it a current domain
        if (has_current_domain()) {
            return std::pair(
                false,
                fmt::format(
                    "{}: dataframe already has its domain set.",
                    function_name_for_messages));
        }

    } else {
        // Resizing an array's existing current domain

        if (!has_current_domain()) {
            return std::pair(
                false,
                fmt::format(
                    "{}: dataframe currently has no domain set.",
                    function_name_for_messages));
        }
    }

    // OK if soma_joinid isn't a dim.
    if (!has_dimension_name("soma_joinid")) {
        return std::pair(true, "");
    }

    // Fail if the newshape isn't within the array's core current domain.
    if (is_resize) {
        std::pair cur_dom_lo_hi = _core_current_domain_slot<int64_t>(
            "soma_joinid");
        if (newshape < cur_dom_lo_hi.second) {
            return std::pair(
                false,
                fmt::format(
                    "{}: new soma_joinid shape {} < existing shape {}",
                    function_name_for_messages,
                    newshape,
                    cur_dom_lo_hi.second + 1));
        }
    }

    // Fail if the newshape isn't within the array's core (max) domain.
    std::pair dom_lo_hi = _core_domain_slot<int64_t>("soma_joinid");
    if (newshape > dom_lo_hi.second) {
        return std::pair(
            false,
            fmt::format(
                "{}: new soma_joinid shape {} > maxshape {}",
                function_name_for_messages,
                newshape,
                dom_lo_hi.second + 1));
    }

    // Sucess otherwise.
    return std::pair(true, "");
}

void SOMAArray::_set_shape_helper(
    const std::vector<int64_t>& newshape,
    bool is_resize,
    std::string function_name_for_messages) {
    if (mq_->query_type() != TILEDB_WRITE) {
        throw TileDBSOMAError(fmt::format(
            "{} array must be opened in write mode",
            function_name_for_messages));
    }

    if (!is_resize) {
        // Upgrading an array to install a current domain
        if (!_get_current_domain().is_empty()) {
            throw TileDBSOMAError(fmt::format(
                "{}: array must not already have a shape",
                function_name_for_messages));
        }
    } else {
        // Expanding an array's current domain
        if (_get_current_domain().is_empty()) {
            throw TileDBSOMAError(fmt::format(
                "{} array must already have a shape",
                function_name_for_messages));
        }
    }

    // Variant-indexed dataframes must use a separate path
    _check_dims_are_int64();

    auto tctx = ctx_->tiledb_ctx();
    ArraySchema schema = arr_->schema();
    Domain domain = schema.domain();
    ArraySchemaEvolution schema_evolution(*tctx);
    CurrentDomain new_current_domain(*tctx);

    NDRectangle ndrect(*tctx, domain);

    unsigned n = domain.ndim();
    if ((unsigned)newshape.size() != n) {
        throw TileDBSOMAError(fmt::format(
            "[SOMAArray::resize]: newshape has dimension count {}; array has "
            "{} ",
            newshape.size(),
            n));
    }

    for (unsigned i = 0; i < n; i++) {
        ndrect.set_range<int64_t>(
            domain.dimension(i).name(), 0, newshape[i] - 1);
    }

    new_current_domain.set_ndrectangle(ndrect);
    schema_evolution.expand_current_domain(new_current_domain);
    schema_evolution.array_evolve(uri_);
}

void SOMAArray::_set_soma_joinid_shape_helper(
    int64_t newshape, bool is_resize, std::string function_name_for_messages) {
    if (mq_->query_type() != TILEDB_WRITE) {
        throw TileDBSOMAError(fmt::format(
            "{}: array must be opened in write mode",
            function_name_for_messages));
    }

    if (!is_resize) {
        // Upgrading an array to install a current domain
        if (!_get_current_domain().is_empty()) {
            throw TileDBSOMAError(fmt::format(
                "{}: array must not already have a shape",
                function_name_for_messages));
        }
    } else {
        // Expanding an array's current domain
        if (_get_current_domain().is_empty()) {
            throw TileDBSOMAError(fmt::format(
                "{} array must already have a shape",
                function_name_for_messages));
        }
    }

    ArraySchema schema = arr_->schema();
    Domain domain = schema.domain();
    unsigned ndim = domain.ndim();
    auto tctx = ctx_->tiledb_ctx();
    ArraySchemaEvolution schema_evolution(*tctx);
    CurrentDomain new_current_domain(*tctx);

    if (!is_resize) {
        // For upgrade: copy from the full/wide/max domain except for the
        // soma_joinid restriction.

        NDRectangle ndrect(*tctx, domain);

        for (unsigned i = 0; i < ndim; i++) {
            const Dimension& dim = domain.dimension(i);
            const std::string dim_name = dim.name();
            if (dim_name == "soma_joinid") {
                if (dim.type() != TILEDB_INT64) {
                    throw TileDBSOMAError(fmt::format(
                        "{}: expected soma_joinid to be of type {}; got {}",
                        function_name_for_messages,
                        tiledb::impl::type_to_str(TILEDB_INT64),
                        tiledb::impl::type_to_str(dim.type())));
                }
                ndrect.set_range<int64_t>(dim_name, 0, newshape - 1);
                continue;

                switch (dim.type()) {
                    case TILEDB_STRING_ASCII:
                    case TILEDB_STRING_UTF8:
                    case TILEDB_CHAR:
                    case TILEDB_GEOM_WKB:
                    case TILEDB_GEOM_WKT:
                        // TODO: make these named constants b/c they're shared
                        // with arrow_adapter.
                        ndrect.set_range(dim_name, "", "\xff");
                        break;

                    case TILEDB_INT8:
                        ndrect.set_range<int8_t>(
                            dim_name,
                            dim.domain<int8_t>().first,
                            dim.domain<int8_t>().second);
                        break;
                    case TILEDB_BOOL:
                    case TILEDB_UINT8:
                        ndrect.set_range<uint8_t>(
                            dim_name,
                            dim.domain<uint8_t>().first,
                            dim.domain<uint8_t>().second);
                        break;
                    case TILEDB_INT16:
                        ndrect.set_range<int16_t>(
                            dim_name,
                            dim.domain<int16_t>().first,
                            dim.domain<int16_t>().second);
                        break;
                    case TILEDB_UINT16:
                        ndrect.set_range<uint16_t>(
                            dim_name,
                            dim.domain<uint16_t>().first,
                            dim.domain<uint16_t>().second);
                        break;
                    case TILEDB_INT32:
                        ndrect.set_range<int32_t>(
                            dim_name,
                            dim.domain<int32_t>().first,
                            dim.domain<int32_t>().second);
                        break;
                    case TILEDB_UINT32:
                        ndrect.set_range<uint32_t>(
                            dim_name,
                            dim.domain<uint32_t>().first,
                            dim.domain<uint32_t>().second);
                        break;
                    case TILEDB_INT64:
                    case TILEDB_DATETIME_YEAR:
                    case TILEDB_DATETIME_MONTH:
                    case TILEDB_DATETIME_WEEK:
                    case TILEDB_DATETIME_DAY:
                    case TILEDB_DATETIME_HR:
                    case TILEDB_DATETIME_MIN:
                    case TILEDB_DATETIME_SEC:
                    case TILEDB_DATETIME_MS:
                    case TILEDB_DATETIME_US:
                    case TILEDB_DATETIME_NS:
                    case TILEDB_DATETIME_PS:
                    case TILEDB_DATETIME_FS:
                    case TILEDB_DATETIME_AS:
                    case TILEDB_TIME_HR:
                    case TILEDB_TIME_MIN:
                    case TILEDB_TIME_SEC:
                    case TILEDB_TIME_MS:
                    case TILEDB_TIME_US:
                    case TILEDB_TIME_NS:
                    case TILEDB_TIME_PS:
                    case TILEDB_TIME_FS:
                    case TILEDB_TIME_AS:
                        ndrect.set_range<int64_t>(
                            dim_name,
                            dim.domain<int64_t>().first,
                            dim.domain<int64_t>().second);
                        break;
                    case TILEDB_UINT64:
                        ndrect.set_range<uint64_t>(
                            dim_name,
                            dim.domain<int64_t>().first,
                            dim.domain<int64_t>().second);
                        break;
                    case TILEDB_FLOAT32:
                        ndrect.set_range<float>(
                            dim_name,
                            dim.domain<float>().first,
                            dim.domain<float>().second);
                        break;
                    case TILEDB_FLOAT64:
                        ndrect.set_range<double>(
                            dim_name,
                            dim.domain<double>().first,
                            dim.domain<double>().second);
                        break;
                    default:
                        throw TileDBSOMAError(fmt::format(
                            "{}: internal error: unhandled type {} for {}.",
                            function_name_for_messages,
                            tiledb::impl::type_to_str(dim.type()),
                            dim_name));
                }
            }
        }

        new_current_domain.set_ndrectangle(ndrect);

    } else {
        // For resize: copy from the existing current domain except for the
        // new soma_joinid value.
        CurrentDomain
            old_current_domain = ArraySchemaExperimental::current_domain(
                *tctx, schema);
        NDRectangle ndrect = old_current_domain.ndrectangle();

        for (unsigned i = 0; i < ndim; i++) {
            if (domain.dimension(i).name() == "soma_joinid") {
                ndrect.set_range<int64_t>(
                    domain.dimension(i).name(), 0, newshape - 1);
            }
        }

        new_current_domain.set_ndrectangle(ndrect);
    }

    schema_evolution.expand_current_domain(new_current_domain);
    schema_evolution.array_evolve(uri_);
}

StatusAndReason SOMAArray::can_upgrade_domain(
    const ArrowTable& newdomain, std::string function_name_for_messages) {
    // Enforce the semantics that tiledbsoma_upgrade_domain must be called
    // only on arrays that don't have a shape set, and resize must be called
    // only on arrays that do.
    if (has_current_domain()) {
        return std::pair(
            false,
            fmt::format(
                "{}: dataframe already has a domain",
                function_name_for_messages));
    }

    // * For old-style dataframe without shape: core domain (soma maxdomain) may
    //   be small (like 100) or big (like 2 billionish).
    // * For new-style dataframe with shape: core current domain (soma domain)
    //   will probably be small and core domain (soma maxdomain) will be huge.
    //
    // In either case, we need to check that the user's requested soma domain
    // isn't outside the core domain, which is immutable. For old-style
    // dataframes, if the requested domain fits in the array's core domain, it's
    // good to go as a new soma domain.
    auto domain_check = _can_set_dataframe_domainish_subhelper(
        newdomain, false, function_name_for_messages);
    if (!domain_check.first) {
        return domain_check;
    }

    // For new-style dataframes, we need to additionally that the the requested
    // soma domain (core current domain) isn't a downsize of the current
    // one.
    if (has_current_domain()) {
        auto current_domain_check = _can_set_dataframe_domainish_subhelper(
            newdomain, true, function_name_for_messages);
        if (!current_domain_check.first) {
            return current_domain_check;
        }
    }

    return std::pair(true, "");
}

// This is a helper for can_upgrade_domain: it's used for comparing
// the user's requested soma domain against the core current domain or core
// (max) domain.
StatusAndReason SOMAArray::_can_set_dataframe_domainish_subhelper(
    const ArrowTable& newdomain,
    bool check_current_domain,
    std::string function_name_for_messages) {
    Domain domain = arr_->schema().domain();

    ArrowArray* new_domain_array = newdomain.first.get();
    ArrowSchema* new_domain_schema = newdomain.second.get();

    if (new_domain_schema->n_children != domain.ndim()) {
        return std::pair(
            false,
            fmt::format(
                "{}: requested domain has ndim={} but the dataframe has "
                "ndim={}",
                function_name_for_messages,
                new_domain_schema->n_children,
                domain.ndim()));
    }

    if (new_domain_schema->n_children != new_domain_array->n_children) {
        return std::pair(
            false,
            fmt::format(
                "{}: internal coding error", function_name_for_messages));
    }

    for (unsigned i = 0; i < domain.ndim(); i++) {
        const auto& dim = domain.dimension(i);

        StatusAndReason status_and_reason;

        switch (dim.type()) {
            case TILEDB_STRING_ASCII:
            case TILEDB_STRING_UTF8:
            case TILEDB_CHAR:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_string(
                        check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_BOOL:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<bool>(
                        check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_INT8:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        int8_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_UINT8:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        uint8_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_INT16:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        int16_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_UINT16:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        uint16_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_INT32:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        int32_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_UINT32:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        uint32_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_INT64:
            case TILEDB_DATETIME_YEAR:
            case TILEDB_DATETIME_MONTH:
            case TILEDB_DATETIME_WEEK:
            case TILEDB_DATETIME_DAY:
            case TILEDB_DATETIME_HR:
            case TILEDB_DATETIME_MIN:
            case TILEDB_DATETIME_SEC:
            case TILEDB_DATETIME_MS:
            case TILEDB_DATETIME_US:
            case TILEDB_DATETIME_NS:
            case TILEDB_DATETIME_PS:
            case TILEDB_DATETIME_FS:
            case TILEDB_DATETIME_AS:
            case TILEDB_TIME_HR:
            case TILEDB_TIME_MIN:
            case TILEDB_TIME_SEC:
            case TILEDB_TIME_MS:
            case TILEDB_TIME_US:
            case TILEDB_TIME_NS:
            case TILEDB_TIME_PS:
            case TILEDB_TIME_FS:
            case TILEDB_TIME_AS:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        int64_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_UINT64:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        uint64_t>(check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_FLOAT32:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<float>(
                        check_current_domain, newdomain, dim.name());
                break;
            case TILEDB_FLOAT64:
                status_and_reason =
                    _can_set_dataframe_domainish_slot_checker_non_string<
                        double>(check_current_domain, newdomain, dim.name());
                break;
            default:
                throw TileDBSOMAError(fmt::format(
                    "{}: saw invalid TileDB type when attempting to cast "
                    "domain information: {}",
                    function_name_for_messages,
                    tiledb::impl::type_to_str(dim.type())));
        }

        if (status_and_reason.first == false) {
            return std::pair(
                false,
                fmt::format(
                    "{} for {}: {}",
                    function_name_for_messages,
                    dim.name(),
                    status_and_reason.second));
        }
    }
    return std::pair(true, "");
}

std::vector<int64_t> SOMAArray::_tiledb_current_domain() {
    // Variant-indexed dataframes must use a separate path
    _check_dims_are_int64();

    std::vector<int64_t> result;

    auto current_domain = tiledb::ArraySchemaExperimental::current_domain(
        *ctx_->tiledb_ctx(), arr_->schema());

    if (current_domain.is_empty()) {
        throw TileDBSOMAError(
            "Internal error: current domain requested for an array which "
            "does "
            "not support it");
    }

    auto t = current_domain.type();
    if (t != TILEDB_NDRECTANGLE) {
        throw TileDBSOMAError("current_domain type is not NDRECTANGLE");
    }

    NDRectangle ndrect = current_domain.ndrectangle();

    for (auto dimension_name : dimension_names()) {
        auto range = ndrect.range<int64_t>(dimension_name);
        result.push_back(range[1] + 1);
    }
    return result;
}

std::vector<int64_t> SOMAArray::_tiledb_domain() {
    // Variant-indexed dataframes must use a separate path
    _check_dims_are_int64();

    std::vector<int64_t> result;
    auto dimensions = mq_->schema()->domain().dimensions();

    for (const auto& dim : dimensions) {
        result.push_back(
            dim.domain<int64_t>().second - dim.domain<int64_t>().first + 1);
    }

    return result;
}

std::optional<int64_t> SOMAArray::_maybe_soma_joinid_shape() {
    return _get_current_domain().is_empty() ?
               _maybe_soma_joinid_tiledb_domain() :
               _maybe_soma_joinid_tiledb_current_domain();
}

std::optional<int64_t> SOMAArray::_maybe_soma_joinid_maxshape() {
    return _maybe_soma_joinid_tiledb_domain();
}

std::optional<int64_t> SOMAArray::_maybe_soma_joinid_tiledb_current_domain() {
    const std::string dim_name = "soma_joinid";

    auto dom = arr_->schema().domain();
    if (!dom.has_dimension(dim_name)) {
        return std::nullopt;
    }

    auto current_domain = _get_current_domain();
    if (current_domain.is_empty()) {
        throw TileDBSOMAError("internal coding error");
    }

    auto t = current_domain.type();
    if (t != TILEDB_NDRECTANGLE) {
        throw TileDBSOMAError("current_domain type is not NDRECTANGLE");
    }

    NDRectangle ndrect = current_domain.ndrectangle();

    auto dim = dom.dimension(dim_name);
    if (dim.type() != TILEDB_INT64) {
        throw TileDBSOMAError(fmt::format(
            "expected {} dim to be {}; got {}",
            dim_name,
            tiledb::impl::type_to_str(TILEDB_INT64),
            tiledb::impl::type_to_str(dim.type())));
    }

    auto range = ndrect.range<int64_t>(dim_name);
    auto max = range[1] + 1;
    return std::optional<int64_t>(max);
}

std::optional<int64_t> SOMAArray::_maybe_soma_joinid_tiledb_domain() {
    const std::string dim_name = "soma_joinid";

    auto dom = arr_->schema().domain();
    if (!dom.has_dimension(dim_name)) {
        return std::nullopt;
    }

    auto dim = dom.dimension(dim_name);
    if (dim.type() != TILEDB_INT64) {
        throw TileDBSOMAError(fmt::format(
            "expected {} dim to be {}; got {}",
            dim_name,
            tiledb::impl::type_to_str(TILEDB_INT64),
            tiledb::impl::type_to_str(dim.type())));
    }

    auto max = dim.domain<int64_t>().second + 1;

    return std::optional<int64_t>(max);
}

bool SOMAArray::_dims_are_int64() {
    ArraySchema schema = arr_->schema();
    Domain domain = schema.domain();
    for (auto dimension : domain.dimensions()) {
        if (dimension.type() != TILEDB_INT64) {
            return false;
        }
    }
    return true;
}

void SOMAArray::_check_dims_are_int64() {
    if (!_dims_are_int64()) {
        throw TileDBSOMAError(
            "[SOMAArray] internal coding error: expected all dims to be "
            "int64");
    }
}

}  // namespace tiledbsoma
