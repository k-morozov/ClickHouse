#include <Processors/Formats/Impl/Parquet/Write.h>
#include <Processors/Formats/Impl/Parquet/ThriftUtil.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/encoding.h>
#include <parquet/schema.h>
#include <arrow/util/rle_encoding.h>
#include <lz4.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <xxhash.h>
#include <DataTypes/DataTypeObject.h>
#include <Columns/MaskOperations.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnObject.h>
#include <IO/WriteHelpers.h>
#include <Common/WKB.h>
#include <Common/config_version.h>
#include <Common/formatReadable.h>
#include <Common/HashTable/HashSet.h>
#include <DataTypes/DataTypeEnum.h>
#include <Core/Block.h>
#include <DataTypes/DataTypeCustom.h>

#if USE_SNAPPY
#include <snappy.h>
#endif

namespace DB::ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
}

namespace DB::Parquet
{

namespace parq = parquet::format;

namespace
{

template <typename T, typename SourceType>
struct StatisticsNumeric
{
    T min = std::numeric_limits<T>::has_infinity
        ? std::numeric_limits<T>::infinity() : std::numeric_limits<T>::max();
    T max = std::numeric_limits<T>::has_infinity
        ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::lowest();

    void add(SourceType x)
    {
        min = std::min(min, static_cast<T>(x));
        max = std::max(max, static_cast<T>(x));
    }

    void merge(const StatisticsNumeric & s)
    {
        min = std::min(min, s.min);
        max = std::max(max, s.max);
    }

    void clear() { *this = {}; }

    parq::Statistics get(const WriteOptions &)
    {
        parq::Statistics s;
        if (min > max) // empty
            return s;
        s.__isset.min_value = s.__isset.max_value = true;
        s.min_value.resize(sizeof(T));
        s.max_value.resize(sizeof(T));
        memcpy(s.min_value.data(), &min, sizeof(T));
        memcpy(s.max_value.data(), &max, sizeof(T));
        s.__set_is_min_value_exact(true);
        s.__set_is_max_value_exact(true);

        if constexpr (std::is_signed_v<T>)
        {
            s.__set_min(s.min_value);
            s.__set_max(s.max_value);
        }
        return s;
    }
};

struct StatisticsFixedStringRef
{
    size_t fixed_string_size = UINT64_MAX;
    const uint8_t * min = nullptr;
    const uint8_t * max = nullptr;

    void add(parquet::FixedLenByteArray a)
    {
        chassert(fixed_string_size != UINT64_MAX);
        addMin(a.ptr);
        addMax(a.ptr);
    }

    void merge(const StatisticsFixedStringRef & s)
    {
        chassert(fixed_string_size == UINT64_MAX || fixed_string_size == s.fixed_string_size);
        fixed_string_size = s.fixed_string_size;
        if (s.min == nullptr)
            return;
        addMin(s.min);
        addMax(s.max);
    }

    void clear() { min = max = nullptr; }

    parq::Statistics get(const WriteOptions & options) const
    {
        parq::Statistics s;
        if (min == nullptr || fixed_string_size > options.max_statistics_size)
            return s;
        s.__set_min_value(std::string(reinterpret_cast<const char *>(min), fixed_string_size));
        s.__set_max_value(std::string(reinterpret_cast<const char *>(max), fixed_string_size));
        s.__set_is_min_value_exact(true);
        s.__set_is_max_value_exact(true);
        return s;
    }

    void addMin(const uint8_t * p)
    {
        if (min == nullptr || memcmp(p, min, fixed_string_size) < 0)
            min = p;
    }
    void addMax(const uint8_t * p)
    {
        if (max == nullptr || memcmp(p, max, fixed_string_size) > 0)
            max = p;
    }
};

template<size_t S>
struct StatisticsFixedStringCopy
{
    bool empty = true;
    std::array<uint8_t, S> min {};
    std::array<uint8_t, S> max {};

    void add(parquet::FixedLenByteArray a)
    {
        addMin(a.ptr);
        addMax(a.ptr);
        empty = false;
    }

    void merge(const StatisticsFixedStringCopy<S> & s)
    {
        if (s.empty)
            return;
        addMin(s.min.data());
        addMax(s.max.data());
        empty = false;
    }

    void clear() { empty = true; }

    parq::Statistics get(const WriteOptions &) const
    {
        parq::Statistics s;
        if (empty)
            return s;
        s.__set_min_value(std::string(reinterpret_cast<const char *>(min.data()), S));
        s.__set_max_value(std::string(reinterpret_cast<const char *>(max.data()), S));
        s.__set_is_min_value_exact(true);
        s.__set_is_max_value_exact(true);
        return s;
    }

    void addMin(const uint8_t * p)
    {
        if (empty || memcmp(p, min.data(), S) < 0)
            memcpy(min.data(), p, S);
    }
    void addMax(const uint8_t * p)
    {
        if (empty || memcmp(p, max.data(), S) > 0)
            memcpy(max.data(), p, S);
    }
};

struct StatisticsStringRef
{
    parquet::ByteArray min;
    parquet::ByteArray max;

    void add(parquet::ByteArray x)
    {
        addMin(x);
        addMax(x);
    }

    void merge(const StatisticsStringRef & s)
    {
        if (s.min.ptr == nullptr)
            return;
        addMin(s.min);
        addMax(s.max);
    }

    void clear() { *this = {}; }

    parq::Statistics get(const WriteOptions & options) const
    {
        parq::Statistics s;
        if (min.ptr == nullptr)
            return s;
        if (static_cast<size_t>(min.len) <= options.max_statistics_size)
        {
            s.__set_min_value(std::string(reinterpret_cast<const char *>(min.ptr), static_cast<size_t>(min.len)));
            s.__set_is_min_value_exact(true);
        }
        if (static_cast<size_t>(max.len) <= options.max_statistics_size)
        {
            s.__set_max_value(std::string(reinterpret_cast<const char *>(max.ptr), static_cast<size_t>(max.len)));
            s.__set_is_max_value_exact(true);
        }
        return s;
    }

    void addMin(parquet::ByteArray x)
    {
        if (min.ptr == nullptr || compare(x, min) < 0)
            min = x;
    }

    void addMax(parquet::ByteArray x)
    {
        if (max.ptr == nullptr || compare(x, max) > 0)
            max = x;
    }

    static int compare(parquet::ByteArray a, parquet::ByteArray b)
    {
        int t = memcmp(a.ptr, b.ptr, std::min(a.len, b.len));
        if (t != 0)
            return t;
        return a.len - b.len;
    }
};

/// The column usually needs to be converted to one of Parquet physical types, e.g. UInt16 -> Int32
/// or [element of ColumnString] -> std::string_view.
/// We do this conversion in small batches rather than all at once, just before encoding the batch,
/// in hopes of getting better performance through cache locality.
/// The Converter* structs below are responsible for that.
/// When conversion is not needed, getBatch() will just return pointer into original data.

template <typename Col, typename To, typename MinMaxType = typename std::conditional_t<
        std::is_signed_v<typename Col::Container::value_type>,
        To,
        typename std::make_unsigned_t<To>>>
struct ConverterNumeric
{
    using Statistics = StatisticsNumeric<MinMaxType, To>;

    const Col & column;
    PODArray<To> buf;

    explicit ConverterNumeric(const ColumnPtr & c) : column(assert_cast<const Col &>(*c)) {}

    const To * getBatch(size_t offset, size_t count)
    {
        if constexpr (sizeof(*column.getData().data()) == sizeof(To))
            return reinterpret_cast<const To *>(column.getData().data() + offset);
        else
        {
            buf.resize(count);
            for (size_t i = 0; i < count; ++i)
                buf[i] = static_cast<To>(column.getData()[offset + i]); // NOLINT
            return buf.data();
        }
    }
};

struct ConverterDateTime64WithMultiplier
{
    using Statistics = StatisticsNumeric<Int64, Int64>;

    using Col = ColumnDecimal<DateTime64>;
    const Col & column;
    Int64 multiplier;
    PODArray<Int64> buf;

    ConverterDateTime64WithMultiplier(const ColumnPtr & c, Int64 multiplier_) : column(assert_cast<const Col &>(*c)), multiplier(multiplier_) {}

    const Int64 * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        for (size_t i = 0; i < count; ++i)
            /// Not checking overflow because DateTime64 values should already be in the range where
            /// they fit in Int64 at any allowed scale (i.e. up to nanoseconds).
            buf[i] = column.getData()[offset + i].value * multiplier;
        return buf.data();
    }
};

/// Multiply DateTime by 1000 to get milliseconds (because Parquet doesn't support seconds).
struct ConverterDateTime
{
    using Statistics = StatisticsNumeric<Int64, Int64>;

    using Col = ColumnVector<UInt32>;
    const Col & column;
    PODArray<Int64> buf;

    explicit ConverterDateTime(const ColumnPtr & c) : column(assert_cast<const Col &>(*c)) {}

    const Int64 * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        for (size_t i = 0; i < count; ++i)
            buf[i] = static_cast<Int64>(column.getData()[offset + i]) * 1000;
        return buf.data();
    }
};

struct ConverterString
{
    using Statistics = StatisticsStringRef;

    const ColumnString & column;
    PODArray<parquet::ByteArray> buf;

    explicit ConverterString(const ColumnPtr & c) : column(assert_cast<const ColumnString &>(*c)) {}

    const parquet::ByteArray * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            StringRef s = column.getDataAt(offset + i);
            buf[i] = parquet::ByteArray(static_cast<UInt32>(s.size), reinterpret_cast<const uint8_t *>(s.data));
        }
        return buf.data();
    }
};

template <typename T>
struct ConverterEnumAsString
{
    using Statistics = StatisticsStringRef;

    explicit ConverterEnumAsString(const ColumnPtr & c, const DataTypePtr & enum_type_)
    : column(assert_cast<const ColumnVector<T> &>(*c)), enum_type(assert_cast<const DataTypeEnum<T> *>(enum_type_.get())) {}

    const ColumnVector<T> & column;
    const DataTypeEnum<T> * enum_type;
    PODArray<parquet::ByteArray> buf;

    const parquet::ByteArray * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);

        const auto & data = column.getData();

        for (size_t i = 0; i < count; ++i)
        {
            const T value = data[offset + i];
            const StringRef s = enum_type->getNameForValue(value);
            buf[i] = parquet::ByteArray(static_cast<UInt32>(s.size), reinterpret_cast<const uint8_t *>(s.data));
        }
        return buf.data();
    }
};

struct ConverterFixedString
{
    using Statistics = StatisticsFixedStringRef;

    const ColumnFixedString & column;
    PODArray<parquet::FixedLenByteArray> buf;

    explicit ConverterFixedString(const ColumnPtr & c) : column(assert_cast<const ColumnFixedString &>(*c)) {}

    const parquet::FixedLenByteArray * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        for (size_t i = 0; i < count; ++i)
            buf[i].ptr = reinterpret_cast<const uint8_t *>(column.getChars().data() + (offset + i) * column.getN());
        return buf.data();
    }

    size_t fixedStringSize() { return column.getN(); }
};

struct ConverterFixedStringAsString
{
    using Statistics = StatisticsStringRef;

    const ColumnFixedString & column;
    PODArray<parquet::ByteArray> buf;

    explicit ConverterFixedStringAsString(const ColumnPtr & c) : column(assert_cast<const ColumnFixedString &>(*c)) {}

    const parquet::ByteArray * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        for (size_t i = 0; i < count; ++i)
            buf[i] = parquet::ByteArray(static_cast<UInt32>(column.getN()), reinterpret_cast<const uint8_t *>(column.getChars().data() + (offset + i) * column.getN()));
        return buf.data();
    }
};

template <typename T>
struct ConverterNumberAsFixedString
{
    /// Calculate min/max statistics for little-endian fixed strings, not numbers, because parquet
    /// doesn't know it's numbers.
    using Statistics = StatisticsFixedStringCopy<sizeof(T)>;

    const ColumnVector<T> & column;
    PODArray<parquet::FixedLenByteArray> buf;

    explicit ConverterNumberAsFixedString(const ColumnPtr & c) : column(assert_cast<const ColumnVector<T> &>(*c)) {}

    const parquet::FixedLenByteArray * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        for (size_t i = 0; i < count; ++i)
            buf[i].ptr = reinterpret_cast<const uint8_t *>(column.getData().data() + offset + i);
        return buf.data();
    }

    size_t fixedStringSize() { return sizeof(T); }
};

struct ConverterJSON
{
    using Statistics = StatisticsStringRef;

    const ColumnObject & column;
    DataTypePtr data_type;
    PODArray<parquet::ByteArray> buf;
    std::vector<String> stash;
    const FormatSettings & format_settings;

    explicit ConverterJSON(const ColumnPtr & c, const DataTypePtr & data_type_, const FormatSettings & format_settings_)
        : column(assert_cast<const ColumnObject &>(*c))
        , data_type(data_type_)
        , format_settings(format_settings_)
    {
    }

    const parquet::ByteArray * getBatch(size_t offset, size_t count)
    {
        buf.resize(count);
        stash.clear();
        stash.reserve(count);

        auto serialization = data_type->getDefaultSerialization();

        for (size_t i = 0; i < count; ++i)
        {
            WriteBufferFromOwnString wb;
            serialization->serializeTextJSON(column, offset + i, wb, format_settings);

            stash.emplace_back(std::move(wb.str()));
            const String & s = stash.back();

            buf[i] = parquet::ByteArray(static_cast<UInt32>(s.size()), reinterpret_cast<const uint8_t *>(s.data()));
        }
        return buf.data();
    }
};

/// Like ConverterNumberAsFixedString, but converts to big-endian. (Parquet uses little-endian
/// for INT32 and INT64, but big-endian for decimals represented as FIXED_LEN_BYTE_ARRAY, presumably
/// to make them comparable lexicographically.)
template <typename T>
struct ConverterDecimal
{
    using Statistics = StatisticsFixedStringCopy<sizeof(T)>;

    const ColumnDecimal<T> & column;
    PODArray<uint8_t> data_buf;
    PODArray<parquet::FixedLenByteArray> ptr_buf;

    explicit ConverterDecimal(const ColumnPtr & c) : column(assert_cast<const ColumnDecimal<T> &>(*c)) {}

    const parquet::FixedLenByteArray * getBatch(size_t offset, size_t count)
    {
        data_buf.resize(count * sizeof(T));
        ptr_buf.resize(count);
        memcpy(data_buf.data(), reinterpret_cast<const char *>(column.getData().data() + offset), count * sizeof(T));
        for (size_t i = 0; i < count; ++i)
        {
            std::reverse(data_buf.data() + i * sizeof(T), data_buf.data() + (i + 1) * sizeof(T));
            ptr_buf[i].ptr = data_buf.data() + i * sizeof(T);
        }
        return ptr_buf.data();
    }

    size_t fixedStringSize() { return sizeof(T); }
};

/// Returns either `source` or `scratch`.
PODArray<char> & compress(PODArray<char> & source, PODArray<char> & scratch, CompressionMethod method, int level)
{
    /// We could use wrapWriteBufferWithCompressionMethod() for everything, but I worry about the
    /// overhead of creating a bunch of WriteBuffers on each page (thousands of values).
    switch (method)
    {
        case CompressionMethod::None:
            return source;

        case CompressionMethod::Lz4:
        {
            #pragma clang diagnostic push
            #pragma clang diagnostic ignored "-Wold-style-cast"

            size_t max_dest_size = LZ4_COMPRESSBOUND(source.size());

            #pragma clang diagnostic pop

            if (max_dest_size > std::numeric_limits<int>::max())
                throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress column of size {}", ReadableSize(source.size()));

            scratch.resize(max_dest_size);

            int compressed_size = LZ4_compress_default(
                source.data(),
                scratch.data(),
                static_cast<int>(source.size()),
                static_cast<int>(max_dest_size));

            scratch.resize(static_cast<size_t>(compressed_size));
            return scratch;
        }

#if USE_SNAPPY
        case CompressionMethod::Snappy:
        {
            size_t max_dest_size = snappy::MaxCompressedLength(source.size());

            if (max_dest_size > std::numeric_limits<int>::max())
                throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress column of size {}", formatReadableSizeWithBinarySuffix(source.size()));

            scratch.resize(max_dest_size);

            size_t compressed_size;
            snappy::RawCompress(source.data(), source.size(), scratch.data(), &compressed_size);

            scratch.resize(compressed_size);
            return scratch;
        }
#endif

        default:
        {
            auto dest_buf = std::make_unique<WriteBufferFromVector<PODArray<char>>>(scratch);
            auto compressed_buf = wrapWriteBufferWithCompressionMethod(
                std::move(dest_buf),
                method,
                level,
                /*zstd_window_log*/ 0,
                source.size(),
                /*existing_memory*/ source.data());
            chassert(compressed_buf->position() == source.data());
            chassert(compressed_buf->available() == source.size());
            compressed_buf->position() += source.size();
            compressed_buf->finalize();
            return scratch;
        }
    }
}

void encodeRepDefLevelsRLE(const UInt8 * data, size_t size, UInt8 max_level, PODArray<char> & out)
{
    using arrow::util::RleEncoder;

    chassert(max_level > 0);
    size_t offset = out.size();
    size_t prefix_size = sizeof(Int32);

    int bit_width = bitScanReverse(max_level) + 1;
    int max_rle_size = RleEncoder::MaxBufferSize(bit_width, static_cast<int>(size)) +
                       RleEncoder::MinBufferSize(bit_width);

    out.resize(offset + prefix_size + max_rle_size);

    RleEncoder encoder(reinterpret_cast<uint8_t *>(out.data() + offset + prefix_size), max_rle_size, bit_width);
    for (size_t i = 0; i < size; ++i)
        encoder.Put(data[i]);
    encoder.Flush();
    Int32 len = encoder.len();

    memcpy(out.data() + offset, &len, prefix_size);
    out.resize(offset + prefix_size + len);
}

void addToEncodingsUsed(ColumnChunkWriteState & s, parq::Encoding::type e)
{
    if (!std::count(s.column_chunk.meta_data.encodings.begin(), s.column_chunk.meta_data.encodings.end(), e))
        s.column_chunk.meta_data.encodings.push_back(e);
}

void writePage(const parq::PageHeader & header, const PODArray<char> & compressed, ColumnChunkWriteState & s, bool add_to_offset_index, size_t first_row_index, WriteBuffer & out)
{
    size_t header_size = serializeThriftStruct(header, out);
    out.write(compressed.data(), compressed.size());
    size_t compressed_page_size = header.compressed_page_size + header_size;

    /// Remember first data page and first dictionary page.
    if (header.__isset.data_page_header && s.column_chunk.meta_data.data_page_offset == -1)
        s.column_chunk.meta_data.__set_data_page_offset(s.column_chunk.meta_data.total_compressed_size);
    if (header.__isset.dictionary_page_header && !s.column_chunk.meta_data.__isset.dictionary_page_offset)
        s.column_chunk.meta_data.__set_dictionary_page_offset(s.column_chunk.meta_data.total_compressed_size);

    if (add_to_offset_index)
    {
        parquet::format::PageLocation location;
        /// Offset relative to column chunk. finalizeColumnChunkAndWriteFooter later adjusts it to global offset.
        location.__set_offset(s.column_chunk.meta_data.total_compressed_size);
        location.__set_compressed_page_size(static_cast<int32_t>(compressed_page_size));
        location.__set_first_row_index(first_row_index);
        s.indexes.offset_index.page_locations.emplace_back(location);
    }

    s.column_chunk.meta_data.total_uncompressed_size += header.uncompressed_page_size + header_size;
    s.column_chunk.meta_data.total_compressed_size += compressed_page_size;
}

void makeBloomFilter(const HashSet<UInt64, TrivialHash> & hashes, ColumnChunkIndexes & indexes, const WriteOptions & options)
{
    /// Format documentation: https://parquet.apache.org/docs/file-format/bloomfilter/

    if (hashes.empty())
        return;

    static constexpr UInt32 salt[8] = {
        0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU, 0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

    /// There appear to be undocumented requirements:
    ///  * number of blocks must be a power of two,
    ///  * bloom filter size must be at most 128 MiB.
    /// At least parquet::BlockSplitBloomFilter::Init (which we use to read bloom filters) requires this.
    double requested_num_blocks = hashes.size() * options.bloom_filter_bits_per_value / 256;
    size_t num_blocks = 1;
    while (num_blocks < requested_num_blocks)
    {
        if (num_blocks >= 4 * 1024 * 1024)
            return;
        num_blocks *= 2;
    }
    PODArray<UInt32> & data = indexes.bloom_filter_data;
    data.reserve_exact(num_blocks * 8);
    data.resize_fill(num_blocks * 8);
    for (const auto & cell : hashes)
    {
        size_t h = cell.key;
        size_t block_idx = ((h >> 32) * num_blocks) >> 32;
        chassert(block_idx < num_blocks);
        UInt32 x = UInt32(h); // overflow to take the lower 32 bits
        for (size_t word_idx = 0; word_idx < 8; ++word_idx)
        {
            UInt32 y = x * salt[word_idx]; // overflow to take the lower 32 bits
            size_t bit_idx = y >> 27;
            data[block_idx * 8 + word_idx] |= 1u << bit_idx;
        }
    }

    /// Fill out the paperwork.
    auto & header = indexes.bloom_filter_header;
    header.__set_numBytes(Int32(data.size() * sizeof(data[0])));
    parq::BloomFilterAlgorithm alg;
    alg.__set_BLOCK(parq::SplitBlockAlgorithm());
    header.__set_algorithm(alg);
    parq::BloomFilterHash hash;
    hash.__set_XXHASH(parq::XxHash());
    header.__set_hash(hash);
    parq::BloomFilterCompression comp;
    comp.__set_UNCOMPRESSED(parq::Uncompressed());
    header.__set_compression(comp);
}

template <typename ParquetDType, typename Converter>
void writeColumnImpl(
    ColumnChunkWriteState & s, const WriteOptions & options, WriteBuffer & out, Converter && converter)
{
    size_t num_values = s.max_def > 0 ? s.def.size() : s.primitive_column->size();
    auto encoding = options.encoding;

    typename Converter::Statistics page_statistics;
    typename Converter::Statistics total_statistics;

    bool use_dictionary = options.use_dictionary_encoding && !s.is_bool;

    /// For some readers filter pushdown implementations it's inconvenient if a row straddles pages,
    /// so let's be nice to them and avoid that. This matches arrow's logic.
    /// If we ever use DataPageV2, enable this flag in that case too:
    /// bool pages_change_on_record_boundaries = options.write_page_index || options.data_page_v2;
    bool pages_change_on_record_boundaries = options.write_page_index;
    /// If there are no arrays, record boundaries are everywhere.
    pages_change_on_record_boundaries &= s.max_rep > 0;

    std::optional<parquet::ColumnDescriptor> fixed_string_descr;
    if constexpr (std::is_same_v<ParquetDType, parquet::FLBAType>)
    {
        /// This just communicates one number to MakeTypedEncoder(): the fixed string length.
        fixed_string_descr.emplace(parquet::schema::PrimitiveNode::Make(
            "", parquet::Repetition::REQUIRED, parquet::Type::FIXED_LEN_BYTE_ARRAY,
            parquet::ConvertedType::NONE, static_cast<int>(converter.fixedStringSize())), 0, 0);

        if constexpr (std::is_same_v<typename Converter::Statistics, StatisticsFixedStringRef>)
            page_statistics.fixed_string_size = converter.fixedStringSize();
    }

    /// Could use an arena here (by passing a custom MemoryPool), to reuse memory across pages.
    /// Alternatively, we could avoid using arrow's dictionary encoding code and leverage
    /// ColumnLowCardinality instead. It would work basically the same way as what this function
    /// currently does: add values to the ColumnRowCardinality (instead of `encoder`) in batches,
    /// checking dictionary size after each batch. That might be faster.
    auto encoder = parquet::MakeTypedEncoder<ParquetDType>(
        // ignored if using dictionary
        static_cast<parquet::Encoding::type>(encoding),
        use_dictionary, fixed_string_descr ? &*fixed_string_descr : nullptr);

    struct PageData
    {
        parq::PageHeader header;
        PODArray<char> data;
        size_t first_row_index = 0;
    };
    std::vector<PageData> dict_encoded_pages; // can't write them out until we have full dictionary

    /// Reused across pages to reduce number of allocations and improve locality.
    PODArray<char> encoded;
    PODArray<char> compressed_maybe;

    /// Hash set to deduplicate the values before calculating bloom filter size.
    /// Possible future optimization: if using dictionary encoding, take already-deduplicated values
    /// from the dictionary instead.
    std::optional<HashSet<UInt64, TrivialHash>> hashes_for_bloom_filter;
    if (options.write_bloom_filter)
        hashes_for_bloom_filter.emplace(); // allocates memory for initial size

    /// Start of current page.
    size_t def_offset = 0; // index in def and rep
    size_t data_offset = 0; // index in primitive_column

    auto flush_page = [&](size_t def_count, size_t data_count)
    {
        encoded.clear();

        /// Concatenate encoded rep, def, and data.

        if (s.max_rep > 0)
            encodeRepDefLevelsRLE(s.rep.data() + def_offset, def_count, s.max_rep, encoded);
        if (s.max_def > 0)
            encodeRepDefLevelsRLE(s.def.data() + def_offset, def_count, s.max_def, encoded);

        std::shared_ptr<parquet::Buffer> values = encoder->FlushValues(); // resets it for next page

        encoded.resize(encoded.size() + values->size());
        memcpy(encoded.data() + encoded.size() - values->size(), values->data(), values->size());
        values.reset();

        if (encoded.size() > INT32_MAX)
            throw Exception(ErrorCodes::CANNOT_COMPRESS, "Uncompressed page is too big: {}", encoded.size());

        size_t uncompressed_size = encoded.size();
        auto & compressed = compress(encoded, compressed_maybe, s.compression, s.compression_level);

        if (compressed.size() > INT32_MAX)
            throw Exception(ErrorCodes::CANNOT_COMPRESS, "Compressed page is too big: {}", compressed.size());

        parq::PageHeader header;
        header.__set_type(parq::PageType::DATA_PAGE);
        header.__set_uncompressed_page_size(static_cast<int>(uncompressed_size));
        header.__set_compressed_page_size(static_cast<int>(compressed.size()));
        header.__isset.data_page_header = true;
        auto & d = header.data_page_header;
        d.__set_num_values(static_cast<Int32>(def_count));
        d.__set_encoding(use_dictionary ? parq::Encoding::RLE_DICTIONARY : encoding);
        d.__set_definition_level_encoding(parq::Encoding::RLE);
        d.__set_repetition_level_encoding(parq::Encoding::RLE);
        /// We could also put checksum in `header.crc`, but apparently no one uses it:
        /// https://issues.apache.org/jira/browse/PARQUET-594

        parq::Statistics page_stats = page_statistics.get(options);
        bool has_null_count = s.max_def == 1 && s.max_rep == 0;
        if (has_null_count)
            page_stats.__set_null_count(static_cast<Int64>(def_count - data_count));

        if (options.write_page_statistics)
            d.__set_statistics(page_stats);

        if (options.write_page_index)
        {
            bool all_null_page = data_count == 0;
            s.indexes.column_index.min_values.push_back(page_stats.min_value);
            s.indexes.column_index.max_values.push_back(page_stats.max_value);
            if (has_null_count)
            {
                /// Note: has_null_count is always equal across all pages of the same column.
                s.indexes.column_index.__isset.null_counts = true;
                s.indexes.column_index.null_counts.emplace_back(page_stats.null_count);
            }
            s.indexes.column_index.null_pages.push_back(all_null_page);
        }

        total_statistics.merge(page_statistics);
        page_statistics.clear();

        if (use_dictionary)
        {
            dict_encoded_pages.push_back({.header = std::move(header), .data = {}, .first_row_index = def_offset});
            std::swap(dict_encoded_pages.back().data, compressed);
        }
        else
        {
            writePage(header, compressed, s, options.write_page_index, def_offset, out);
        }
        def_offset += def_count;
        data_offset += data_count;
    };

    auto flush_dict = [&] -> bool
    {
        auto * dict_encoder = dynamic_cast<parquet::DictEncoder<ParquetDType> *>(encoder.get());
        int dict_size = dict_encoder->dict_encoded_size();

        encoded.resize(static_cast<size_t>(dict_size));
        dict_encoder->WriteDict(reinterpret_cast<uint8_t *>(encoded.data()));

        auto & compressed = compress(encoded, compressed_maybe, s.compression, s.compression_level);

        if (compressed.size() > INT32_MAX)
            throw Exception(ErrorCodes::CANNOT_COMPRESS, "Compressed dictionary page is too big: {}", compressed.size());

        parq::PageHeader header;
        header.__set_type(parq::PageType::DICTIONARY_PAGE);
        header.__set_uncompressed_page_size(dict_size);
        header.__set_compressed_page_size(static_cast<int>(compressed.size()));
        header.__isset.dictionary_page_header = true;
        header.dictionary_page_header.__set_num_values(dict_encoder->num_entries());
        header.dictionary_page_header.__set_encoding(parq::Encoding::PLAIN);

        writePage(header, compressed, s, /*add_to_offset_index*/ false, /*first_row_index*/ 0, out);

        for (auto & p : dict_encoded_pages)
            writePage(p.header, p.data, s, options.write_page_index, p.first_row_index, out);

        dict_encoded_pages.clear();
        encoder.reset();

        return true;
    };

    auto is_dict_too_big = [&] {
        auto * dict_encoder = dynamic_cast<parquet::DictEncoder<ParquetDType> *>(encoder.get());
        int dict_size = dict_encoder->dict_encoded_size();
        return static_cast<size_t>(dict_size) >= options.dictionary_size_limit;
    };

    while (def_offset < num_values)
    {
        /// Pick enough data for a page.
        size_t next_def_offset = def_offset;
        size_t next_data_offset = data_offset;
        while (true)
        {
            /// Bite off a batch of defs and corresponding data values.
            size_t def_count = std::min(options.write_batch_size, num_values - next_def_offset);
            size_t data_count = 0;
            if (s.max_def == 0) // no arrays or nullables
                data_count = def_count;
            else
                for (size_t i = 0; i < def_count; ++i)
                    data_count += s.def[next_def_offset + i] == s.max_def;

            if (pages_change_on_record_boundaries)
            {
                /// Each record (table row) starts with a value with rep = 0.
                while (next_def_offset + def_count < num_values && s.rep[next_def_offset + def_count] != 0)
                {
                    data_count += s.def[next_def_offset + def_count] == s.max_def;
                    ++def_count;
                }
            }

            /// Encode the data (but not the levels yet), so that we can estimate its encoded size.
            const typename ParquetDType::c_type * converted = converter.getBatch(next_data_offset, data_count);

            if (options.write_page_statistics || options.write_column_chunk_statistics)
/// Workaround for clang bug: https://github.com/llvm/llvm-project/issues/63630
#ifdef MEMORY_SANITIZER
#pragma clang loop vectorize(disable)
#endif
                for (size_t i = 0; i < data_count; ++i)
                    page_statistics.add(converted[i]);

            if (hashes_for_bloom_filter.has_value())
            {
                for (size_t i = 0; i < data_count; ++i)
                {
                    UInt64 h;
                    constexpr UInt64 seed = 0;
                    if constexpr (std::is_same_v<ParquetDType, parquet::FLBAType>)
                        h = XXH64(converted[i].ptr, converter.fixedStringSize(), seed);
                    else if constexpr (std::is_same_v<ParquetDType, parquet::ByteArrayType>)
                        h = XXH64(converted[i].ptr, converted[i].len, seed);
                    else
                    {
                        static_assert(sizeof(converted[i]) <= 12, "unexpected non-primitive type");
                        h = XXH64(reinterpret_cast<const void*>(&converted[i]), sizeof(converted[i]), seed);
                    }
                    hashes_for_bloom_filter->insert(h);
                }
            }

            encoder->Put(converted, static_cast<int>(data_count));

            next_def_offset += def_count;
            next_data_offset += data_count;

            if (use_dictionary && is_dict_too_big())
            {
                /// Fallback to non-dictionary encoding.
                ///
                /// Discard encoded data and start over.
                /// This is different from what arrow does: arrow writes out the dictionary-encoded
                /// data, then uses non-dictionary encoding for later pages.
                /// Starting over seems better: it produces slightly smaller files (I saw 1-4%) in
                /// exchange for slight decrease in speed (I saw < 5%). This seems like a good
                /// trade because encoding speed is less important than decoding (as evidenced
                /// by arrow not supporting parallel encoding, even though it's easy to support).

                def_offset = 0;
                data_offset = 0;
                dict_encoded_pages.clear();
                use_dictionary = false;

                s.indexes = {};
                /// (no need to clear hashes_for_bloom_filter)

#ifndef NDEBUG
                /// Arrow's DictEncoderImpl destructor asserts that FlushValues() was called, so we
                /// call it even though we don't need its output.
                encoder->FlushValues();
#endif

                encoder = parquet::MakeTypedEncoder<ParquetDType>(
                    static_cast<parquet::Encoding::type>(encoding), /* use_dictionary */ false,
                    fixed_string_descr ? &*fixed_string_descr : nullptr);
                break;
            }

            if (next_def_offset == num_values ||
                static_cast<size_t>(encoder->EstimatedDataEncodedSize()) >= options.data_page_size)
            {
                flush_page(next_def_offset - def_offset, next_data_offset - data_offset);
                break;
            }
        }
    }

    if (use_dictionary)
        flush_dict();

    chassert(data_offset == s.primitive_column->size());

    if (options.write_column_chunk_statistics)
    {
        s.column_chunk.meta_data.__set_statistics(total_statistics.get(options));

        if (s.max_def == 1 && s.max_rep == 0)
            s.column_chunk.meta_data.statistics.__set_null_count(static_cast<Int64>(def_offset - data_offset));
    }

    /// Report which encodings we've used.
    if (s.max_rep > 0 || s.max_def > 0)
        addToEncodingsUsed(s, parq::Encoding::RLE); // levels
    if (use_dictionary)
    {
        addToEncodingsUsed(s, parq::Encoding::PLAIN); // dictionary itself
        addToEncodingsUsed(s, parq::Encoding::RLE_DICTIONARY); // ids
    }
    else
    {
        addToEncodingsUsed(s, encoding);
    }

    if (hashes_for_bloom_filter.has_value())
        makeBloomFilter(*hashes_for_bloom_filter, s.indexes, options);
}

}

void writeColumnChunkBody(
    ColumnChunkWriteState & s, const WriteOptions & options, const FormatSettings & format_settings, WriteBuffer & out)
{
    s.column_chunk.meta_data.__set_num_values(s.max_def > 0 ? s.def.size() : s.primitive_column->size());

    /// We'll be updating these as we go.
    s.column_chunk.meta_data.__set_encodings({});
    s.column_chunk.meta_data.__set_total_compressed_size(0);
    s.column_chunk.meta_data.__set_total_uncompressed_size(0);
    s.column_chunk.meta_data.__set_data_page_offset(-1);

    s.primitive_column = s.primitive_column->convertToFullColumnIfLowCardinality();

    switch (s.primitive_column->getDataType())
    {
        /// Numeric conversion to Int32 or Int64.
        #define N(source_type, parquet_dtype) \
            writeColumnImpl<parquet::parquet_dtype>(s, options, out, \
                ConverterNumeric<ColumnVector<source_type>, parquet::parquet_dtype::c_type>( \
                    s.primitive_column))

        case TypeIndex::UInt8:
            if (s.is_bool)
                writeColumnImpl<parquet::BooleanType>(s, options, out,
                    ConverterNumeric<ColumnVector<UInt8>, bool, bool>(s.primitive_column));
            else
                N(UInt8, Int32Type);
            break;
        case TypeIndex::UInt16 : N(UInt16, Int32Type); break;
        case TypeIndex::UInt64 : N(UInt64, Int64Type); break;
        case TypeIndex::Int8:
        {
            if (options.output_enum_as_byte_array && isEnum8(s.type))
                writeColumnImpl<parquet::ByteArrayType>(
                    s, options, out, ConverterEnumAsString<Int8>(s.primitive_column, s.type));
            else
                N(Int8, Int32Type);
         break;
        }
        case TypeIndex::Int16:
        {
            if (options.output_enum_as_byte_array && isEnum16(s.type))
                writeColumnImpl<parquet::ByteArrayType>(
                    s, options, out, ConverterEnumAsString<Int16>(s.primitive_column, s.type));
            else
                N(Int16, Int32Type);
            break;
        }
        case TypeIndex::Int32  : N(Int32,  Int32Type); break;
        case TypeIndex::Int64  : N(Int64,  Int64Type); break;

        case TypeIndex::UInt32:
            if (s.datetime_multiplier == 1)
                N(UInt32, Int32Type);
            else
            {
                /// It's actually a DateTime that needs to be converted to milliseconds.
                chassert(s.datetime_multiplier == 1000);
                writeColumnImpl<parquet::Int64Type>(s, options, out, ConverterDateTime(s.primitive_column));
            }
            break;

        #undef N

        case TypeIndex::Float32:
            writeColumnImpl<parquet::FloatType>(
                s, options, out, ConverterNumeric<ColumnVector<Float32>, Float32, Float32>(
                    s.primitive_column));
            break;

        case TypeIndex::Float64:
            writeColumnImpl<parquet::DoubleType>(
                s, options, out, ConverterNumeric<ColumnVector<Float64>, Float64, Float64>(
                    s.primitive_column));
            break;

        case TypeIndex::DateTime64:
            if (s.datetime_multiplier == 1)
                writeColumnImpl<parquet::Int64Type>(
                    s, options, out, ConverterNumeric<ColumnDecimal<DateTime64>, Int64, Int64>(
                        s.primitive_column));
            else
                writeColumnImpl<parquet::Int64Type>(
                    s, options, out, ConverterDateTime64WithMultiplier(
                        s.primitive_column, s.datetime_multiplier));
            break;

        case TypeIndex::IPv4:
            writeColumnImpl<parquet::Int32Type>(
                s, options, out, ConverterNumeric<ColumnVector<IPv4>, Int32, UInt32>(
                    s.primitive_column));
            break;

        case TypeIndex::String:
            writeColumnImpl<parquet::ByteArrayType>(
                s, options, out, ConverterString(s.primitive_column));
            break;

        case TypeIndex::FixedString:
            if (options.output_fixed_string_as_fixed_byte_array)
                writeColumnImpl<parquet::FLBAType>(
                s, options, out, ConverterFixedString(s.primitive_column));
            else
                writeColumnImpl<parquet::ByteArrayType>(
                s, options, out, ConverterFixedStringAsString(s.primitive_column));
            break;
        case TypeIndex::Object:
            writeColumnImpl<parquet::ByteArrayType>(s, options, out, ConverterJSON(s.primitive_column, s.type, format_settings));
            break;

        #define F(source_type) \
            writeColumnImpl<parquet::FLBAType>( \
                s, options, out, ConverterNumberAsFixedString<source_type>(s.primitive_column))
        case TypeIndex::UInt128: F(UInt128); break;
        case TypeIndex::UInt256: F(UInt256); break;
        case TypeIndex::Int128:  F(Int128); break;
        case TypeIndex::Int256:  F(Int256); break;
        case TypeIndex::IPv6:    F(IPv6); break;
        #undef F

        #define D(source_type) \
            writeColumnImpl<parquet::FLBAType>( \
                s, options, out, ConverterDecimal<source_type>(s.primitive_column))
        case TypeIndex::Decimal32:  D(Decimal32); break;
        case TypeIndex::Decimal64:  D(Decimal64); break;
        case TypeIndex::Decimal128: D(Decimal128); break;
        case TypeIndex::Decimal256: D(Decimal256); break;
        #undef D

        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected column type: {}", s.primitive_column->getFamilyName());
    }

    /// Free some memory.
    s.primitive_column = {};
    s.def = {};
    s.rep = {};
}

void writeFileHeader(FileWriteState & file, WriteBuffer & out)
{
    chassert(file.offset == 0);
    /// Write the magic bytes. We're a wizard now.
    out.write("PAR1", 4);
    file.offset = 4;
}

void finalizeColumnChunkAndWriteFooter(ColumnChunkWriteState s, FileWriteState & file, WriteBuffer & out)
{
    for (auto & location : s.indexes.offset_index.page_locations)
        location.offset += file.offset;

    if (s.column_chunk.meta_data.data_page_offset != -1)
        s.column_chunk.meta_data.data_page_offset += file.offset;
    if (s.column_chunk.meta_data.__isset.dictionary_page_offset)
        s.column_chunk.meta_data.dictionary_page_offset += file.offset;

    file.offset += s.column_chunk.meta_data.total_compressed_size;
    s.column_chunk.file_offset = file.offset;

    size_t footer_size = serializeThriftStruct(s.column_chunk, out);
    file.offset += footer_size;

    file.unflushed_bloom_filter_bytes += s.indexes.bloom_filter_data.size() * sizeof(s.indexes.bloom_filter_data[0]);
    file.current_row_group.column_indexes.push_back(std::move(s.indexes));
    file.current_row_group.row_group.columns.push_back(std::move(s.column_chunk));
}

static void flushBloomFilters(FileWriteState & file, WriteBuffer & out)
{
    while (file.row_groups_with_flushed_bloom_filter < file.completed_row_groups.size())
    {
        auto & rg = file.completed_row_groups[file.row_groups_with_flushed_bloom_filter];
        chassert(rg.column_indexes.size() == rg.row_group.columns.size());
        for (size_t col_idx = 0; col_idx < rg.column_indexes.size(); ++col_idx)
        {
            auto & indexes = rg.column_indexes[col_idx];
            if (indexes.bloom_filter_data.empty())
                continue;
            auto & column = rg.row_group.columns[col_idx];
            column.meta_data.__set_bloom_filter_offset(file.offset);

            size_t header_size = serializeThriftStruct(indexes.bloom_filter_header, out);
            size_t data_size = indexes.bloom_filter_data.size() * sizeof(indexes.bloom_filter_data[0]);
            out.write(indexes.bloom_filter_data.raw_data(), data_size);

            column.meta_data.__set_bloom_filter_length(Int32(header_size + data_size));
            file.offset += column.meta_data.bloom_filter_length;

            indexes.bloom_filter_data = {}; // free the memory
        }
        file.row_groups_with_flushed_bloom_filter += 1;
    }
    file.unflushed_bloom_filter_bytes = 0;
}

void finalizeRowGroup(FileWriteState & file, size_t num_rows, const WriteOptions & options, WriteBuffer & out)
{
    auto & r = file.current_row_group.row_group;
    if (!file.completed_row_groups.empty())
        chassert(r.columns.size() == file.completed_row_groups[0].row_group.columns.size());
    r.__set_num_rows(num_rows);
    r.__set_total_compressed_size(0);
    for (auto & c : r.columns)
    {
        r.total_byte_size += c.meta_data.total_uncompressed_size;
        r.total_compressed_size += c.meta_data.total_compressed_size;
    }
    chassert(!r.columns.empty());
    {
        auto & m = r.columns[0].meta_data;
        r.__set_file_offset(m.__isset.dictionary_page_offset ? m.dictionary_page_offset : m.data_page_offset);
    }

    file.completed_row_groups.push_back(std::move(file.current_row_group));
    file.current_row_group = {};

    if (options.write_bloom_filter && file.unflushed_bloom_filter_bytes >= options.bloom_filter_flush_threshold_bytes)
        flushBloomFilters(file, out);
}

static void writePageIndex(FileWriteState & file, WriteBuffer & out)
{
    // write column index
    for (auto & rg : file.completed_row_groups)
    {
        chassert(rg.column_indexes.size() == rg.row_group.columns.size());
        for (size_t j = 0; j < rg.column_indexes.size(); ++j)
        {
            auto & column = rg.row_group.columns.at(j);
            column.__set_column_index_offset(file.offset);
            size_t length = serializeThriftStruct(rg.column_indexes.at(j).column_index, out);
            column.__set_column_index_length(static_cast<int32_t>(length));
            file.offset += length;
        }
    }

    // write offset index
    for (auto & rg : file.completed_row_groups)
    {
        for (size_t j = 0; j < rg.column_indexes.size(); ++j)
        {
            auto & column = rg.row_group.columns.at(j);
            column.__set_offset_index_offset(file.offset);
            size_t length = serializeThriftStruct(rg.column_indexes.at(j).offset_index, out);
            column.__set_offset_index_length(static_cast<int32_t>(length));
            file.offset += length;
        }
    }
}

void writeFileFooter(FileWriteState & file,
    SchemaElements schema,
    const WriteOptions & options,
    WriteBuffer & out,
    const Block & header)
{
    chassert(file.offset != 0);
    chassert(file.current_row_group.row_group.columns.empty());

    if (options.write_bloom_filter)
        flushBloomFilters(file, out);

    if (options.write_page_index)
        writePageIndex(file, out);

    parq::FileMetaData meta;
    meta.version = 2;
    meta.schema = std::move(schema);
    meta.row_groups.reserve(file.completed_row_groups.size());
    for (auto & rg : file.completed_row_groups)
    {
        meta.num_rows += rg.row_group.num_rows;
        meta.row_groups.push_back(std::move(rg.row_group));
    }
    meta.__set_created_by(std::string(VERSION_NAME) + " " + VERSION_DESCRIBE);

    if (options.write_page_statistics || options.write_column_chunk_statistics)
    {
        meta.__set_column_orders({});
        for (auto & s : meta.schema)
            if (!s.__isset.num_children)
                meta.column_orders.emplace_back();
        for (auto & c : meta.column_orders)
            c.__set_TYPE_ORDER({});
    }

    /// Documentation about geoparquet metadata: https://geoparquet.org/releases/v1.0.0-beta.1/
    if (options.write_geometadata)
    {
        std::vector<std::pair<std::string, Poco::JSON::Object::Ptr>> geo_columns_metadata;
        for (const auto & [column_name, type] : header.getNamesAndTypesList())
        {
            if (type->getCustomName() &&
                (type->getCustomName()->getName() == WKBPointTransform::name ||
                type->getCustomName()->getName() == WKBLineStringTransform::name ||
                type->getCustomName()->getName() == WKBPolygonTransform::name ||
                type->getCustomName()->getName() == WKBMultiLineStringTransform::name ||
                type->getCustomName()->getName() == WKBMultiPolygonTransform::name))
            {
                Poco::JSON::Object::Ptr geom_meta = new Poco::JSON::Object;
                geom_meta->set("encoding", "WKB");

                Poco::JSON::Array::Ptr geom_types = new Poco::JSON::Array;
                geom_types->add(type->getCustomName()->getName());
                geom_meta->set("geometry_types", geom_types);
                geom_meta->set("crs", "EPSG:4326");

                geo_columns_metadata.push_back({column_name, geom_meta});

                if (type->getCustomName()->getName() == WKBPolygonTransform::name ||
                    type->getCustomName()->getName() == WKBMultiPolygonTransform::name)
                {
                    geom_meta->set("edges", "planar");
                    geom_meta->set("orientation", "counterclockwise");
                }
                geo_columns_metadata.push_back({column_name, geom_meta});
            }
        }

        if (!geo_columns_metadata.empty())
        {
            Poco::JSON::Object::Ptr columns = new Poco::JSON::Object;
            for (const auto & [column_name, column_type] : geo_columns_metadata)
            {
                columns->set(column_name, column_type);
            }

            Poco::JSON::Object::Ptr geo = new Poco::JSON::Object;
            geo->set("version", "1.0.0");
            geo->set("columns", columns);
            geo->set("primary_column", geo_columns_metadata[0].first);

            std::ostringstream // STYLE_CHECK_ALLOW_STD_STRING_STREAM
                oss;
            Poco::JSON::Stringifier::stringify(geo, oss, 4);

            parquet::format::KeyValue key_value;
            key_value.__set_key("geo");
            key_value.__set_value(oss.str());

            meta.key_value_metadata.push_back(std::move(key_value));
            meta.__isset.key_value_metadata = true;
        }
    }

    size_t footer_size = serializeThriftStruct(meta, out);

    if (footer_size > INT32_MAX)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED, "Parquet file metadata too big: {}", footer_size);

    writeIntBinary(static_cast<int>(footer_size), out);
    out.write("PAR1", 4);
    file.offset += footer_size + 8;
}

size_t ColumnChunkWriteState::allocatedBytes() const
{
    size_t r = def.allocated_bytes() + rep.allocated_bytes();
    if (primitive_column)
        r += primitive_column->allocatedBytes();
    return r;
}

}
