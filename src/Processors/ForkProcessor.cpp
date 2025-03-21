#include <Processors/ForkProcessor.h>
#include <Processors/Port.h>

namespace DB
{

ForkProcessor::ForkProcessor(const Block & header, size_t num_outputs)
    : IProcessor(InputPorts{header}, OutputPorts(num_outputs, header))
{
}

ForkProcessor::Status ForkProcessor::prepare()
{
    auto & input = inputs.front();

    /// Check can output.

    bool all_can_push = true;
    size_t num_active_outputs = 0;

    for (const auto & output : outputs)
    {
        if (!output.isFinished())
        {
            ++num_active_outputs;

            /// The order is important.
            if (!output.canPush())
                all_can_push = false;
        }
    }

    if (0 == num_active_outputs)
    {
        input.close();
        return Status::Finished;
    }

    if (!all_can_push)
    {
        input.setNotNeeded();
        return Status::PortFull;
    }

    /// Check can input.

    if (input.isFinished())
    {
        for (auto & output : outputs)
            output.finish();

        return Status::Finished;
    }

    input.setNeeded();

    if (!input.hasData())
        return Status::NeedData;

    /// Move data.

    auto data = input.pull();
    size_t num_processed_outputs = 0;

    for (auto & output : outputs)
    {
        if (!output.isFinished())  /// Skip finished outputs.
        {
            ++num_processed_outputs;
            if (num_processed_outputs == num_active_outputs)
                output.push(std::move(data)); /// NOLINT Can push because no full or unneeded outputs.
            else
                output.push(data.clone()); /// NOLINT
        }
    }

    /// Now, we pulled from input. It must be empty.
    return Status::NeedData;
}

}


