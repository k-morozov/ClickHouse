#pragma once

#include "config.h"

#if USE_HIVE
#include <IO/PeekableReadBuffer.h>
#include <Processors/Formats/Impl/CSVRowInputFormat.h>


namespace DB
{

/// A stream for input data in Hive Text format.
class HiveTextRowInputFormat final : public CSVRowInputFormat
{
public:
    HiveTextRowInputFormat(SharedHeader header_, ReadBuffer & in_, const Params & params_, const FormatSettings & format_settings_);

    String getName() const override { return "HiveTextRowInputFormat"; }

private:
    HiveTextRowInputFormat(
        SharedHeader header_, std::shared_ptr<PeekableReadBuffer> buf_, const Params & params_, const FormatSettings & format_settings_);
};

class HiveTextFormatReader final : public CSVFormatReader
{
public:
    HiveTextFormatReader(PeekableReadBuffer & buf_, const FormatSettings & format_settings_);

    std::vector<String> readNames() override;
    std::vector<String> readTypes() override;

private:
    std::vector<String> input_field_names;
};

}

#endif
