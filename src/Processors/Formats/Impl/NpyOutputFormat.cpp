#include <Processors/Formats/Impl/NpyOutputFormat.h>

#include <Core/TypeId.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeArray.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnArray.h>
#include <IO/WriteHelpers.h>
#include <Formats/FormatFactory.h>

#include <Common/assert_cast.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_MANY_COLUMNS;
    extern const int BAD_ARGUMENTS;
    extern const int ILLEGAL_COLUMN;
}

namespace
{

template <typename ColumnType, typename ValueType>
void writeNumpyNumbers(const ColumnPtr & column, WriteBuffer & buf)
{
    const auto * number_column = assert_cast<const ColumnType *>(column.get());
    for (size_t i = 0; i < number_column->size(); ++i)
        writeBinaryLittleEndian(ValueType(number_column->getElement(i)), buf);
}

template <typename ColumnType>
void writeNumpyStrings(const ColumnPtr & column, size_t length, WriteBuffer & buf)
{
    const auto * string_column = assert_cast<const ColumnType *>(column.get());
    for (size_t i = 0; i < string_column->size(); ++i)
        buf.write(string_column->getDataAt(i).data, length);
}

}

String NpyOutputFormat::NumpyDataType::str()
{
    std::ostringstream dtype;
    dtype << endianness << type << std::to_string(size);
    return dtype.str();
}

NpyOutputFormat::NpyOutputFormat(WriteBuffer & out_, const Block & header_) : IOutputFormat(header_, out_)
{
    const auto & header = getPort(PortKind::Main).getHeader();
    auto data_types = header.getDataTypes();
    if (data_types.size() > 1)
        throw Exception(ErrorCodes::TOO_MANY_COLUMNS, "Expected single column for Npy output format, got {}", data_types.size());
    data_type = data_types[0];
}

void NpyOutputFormat::initialize(const ColumnPtr & column)
{
    auto type = data_type;
    ColumnPtr nested_column = column;
    while (type->getTypeId() == TypeIndex::Array)
    {
        const auto * array_column = assert_cast<const ColumnArray *>(nested_column.get());
        numpy_shape.push_back(array_column->getOffsets()[0]);
        type = assert_cast<const DataTypeArray *>(type.get())->getNestedType();
        nested_column = array_column->getDataPtr();
    }

    switch (type->getTypeId())
    {
        case TypeIndex::Int8: numpy_data_type = NumpyDataType('<', 'i', sizeof(Int8)); break;
        case TypeIndex::Int16: numpy_data_type = NumpyDataType('<', 'i', sizeof(Int16)); break;
        case TypeIndex::Int32: numpy_data_type = NumpyDataType('<', 'i', sizeof(Int32)); break;
        case TypeIndex::Int64: numpy_data_type = NumpyDataType('<', 'i', sizeof(Int64)); break;
        case TypeIndex::UInt8: numpy_data_type = NumpyDataType('<', 'u', sizeof(UInt8)); break;
        case TypeIndex::UInt16: numpy_data_type = NumpyDataType('<', 'u', sizeof(UInt16)); break;
        case TypeIndex::UInt32: numpy_data_type = NumpyDataType('<', 'u', sizeof(UInt32)); break;
        case TypeIndex::UInt64: numpy_data_type = NumpyDataType('<', 'u', sizeof(UInt64)); break;
        case TypeIndex::Float32: numpy_data_type = NumpyDataType('<', 'f', sizeof(Float32)); break;
        case TypeIndex::Float64: numpy_data_type = NumpyDataType('<', 'f', sizeof(Float64)); break;
        case TypeIndex::FixedString: numpy_data_type = NumpyDataType('|', 'S', assert_cast<const DataTypeFixedString *>(type.get())->getN()); break;
        case TypeIndex::String: numpy_data_type = NumpyDataType('|', 'S', 0); break;
        default:
            has_exception = true;
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Type {} is not supported for Npy output format", type->getName());
    }
    nested_data_type = type;
}

void NpyOutputFormat::consume(Chunk chunk)
{
    if (!has_exception)
    {
        num_rows += chunk.getNumRows();
        auto column = chunk.getColumns()[0];

        if (!is_initialized)
        {
            initialize(column);
            is_initialized = true;
        }

        /// check shape
        auto type = data_type;
        ColumnPtr nested_column = column;
        int dim = 0;
        while (type->getTypeId() == TypeIndex::Array)
        {
            const auto * array_column = assert_cast<const ColumnArray *>(nested_column.get());
            const auto & array_offset = array_column->getOffsets();
            for (size_t i = 1; i < array_offset.size(); ++i)
            {
                if (array_offset[i] - array_offset[i - 1] != numpy_shape[dim])
                {
                    has_exception = true;
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "ClickHouse doesn't support object types, cannot format ragged nested sequences (which is a list of arrays with different shapes)");
                }
            }
            type = assert_cast<const DataTypeArray *>(type.get())->getNestedType();
            nested_column = array_column->getDataPtr();
            dim++;
        }

        /// for type String, get maximum string length
        if (type->getTypeId() == TypeIndex::String)
        {
            const auto & string_offsets = assert_cast<const ColumnString *>(nested_column.get())->getOffsets();
            for (size_t i = 0; i < string_offsets.size(); ++i)
            {
                size_t string_length = static_cast<size_t>(string_offsets[i] - 1 - string_offsets[i - 1]);
                numpy_data_type.size = numpy_data_type.size > string_length ? numpy_data_type.size : string_length;
            }
        }

        columns.push_back(nested_column);
    }
}

void NpyOutputFormat::finalizeImpl()
{
    if (!has_exception)
    {
        writeHeader();
        writeColumns();
    }
}

void NpyOutputFormat::writeHeader()
{
    std::ostringstream static_header;
    static_header << MAGIC_STRING << MAJOR_VERSION << MINOR_VERSION;
    String static_header_str = static_header.str();

    std::ostringstream shape;
    shape << '(' << std::to_string(num_rows) << ',';
    for (auto dim : numpy_shape)
        shape << std::to_string(dim) << ',';
    shape << ')';

    std::ostringstream dict;
    dict << "{'descr':'" << numpy_data_type.str() << "','fortran_order':False,'shape':" << shape.str() << ",}";
    String dict_str = dict.str();
    String padding_str = "\n";

    /// completes the length of the header, which is divisible by 64.
    size_t dict_length = dict_str.length() + 1;
    size_t header_length = static_header_str.length() + sizeof(UInt32) + dict_length;
    if (header_length % 64)
    {
        header_length = ((header_length / 64) + 1) * 64;
        dict_length = header_length - static_header_str.length() - sizeof(UInt32);
        padding_str = std::string(dict_length - dict_str.length(), '\x20');
        padding_str.back() = '\n';
    }

    out.write(static_header_str.data(), static_header_str.length());
    writeBinaryLittleEndian(assert_cast<UInt32>(dict_length), out);
    out.write(dict_str.data(), dict_str.length());
    out.write(padding_str.data(), padding_str.length());
}

void NpyOutputFormat::writeColumns()
{
    for (auto column : columns)
    {
        switch (nested_data_type->getTypeId())
        {
            case TypeIndex::Int8: writeNumpyNumbers<ColumnInt8, Int8>(column, out); break;
            case TypeIndex::Int16: writeNumpyNumbers<ColumnInt16, Int16>(column, out); break;
            case TypeIndex::Int32: writeNumpyNumbers<ColumnInt32, Int32>(column, out); break;
            case TypeIndex::Int64: writeNumpyNumbers<ColumnInt64, Int64>(column, out); break;
            case TypeIndex::UInt8: writeNumpyNumbers<ColumnUInt8, UInt8>(column, out); break;
            case TypeIndex::UInt16: writeNumpyNumbers<ColumnUInt16, UInt16>(column, out); break;
            case TypeIndex::UInt32: writeNumpyNumbers<ColumnUInt32, UInt32>(column, out); break;
            case TypeIndex::UInt64: writeNumpyNumbers<ColumnUInt64, UInt64>(column, out); break;
            case TypeIndex::Float32: writeNumpyNumbers<ColumnFloat32, Float32>(column, out); break;
            case TypeIndex::Float64: writeNumpyNumbers<ColumnFloat64, Float64>(column, out); break;
            case TypeIndex::FixedString: writeNumpyStrings<ColumnFixedString>(column, numpy_data_type.size, out); break;
            case TypeIndex::String: writeNumpyStrings<ColumnString>(column, numpy_data_type.size, out); break;
            default: break;
        }
    }
}

void registerOutputFormatNpy(FormatFactory & factory)
{
    factory.registerOutputFormat("Npy",[](
        WriteBuffer & buf,
        const Block & sample,
        const FormatSettings &)
    {
        return std::make_shared<NpyOutputFormat>(buf, sample);
    });
    factory.markFormatHasNoAppendSupport("Npy");
}

}
