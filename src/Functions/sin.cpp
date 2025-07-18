#include <Functions/FunctionMathUnary.h>
#include <Functions/FunctionFactory.h>

namespace DB
{
namespace
{

struct SinName { static constexpr auto name = "sin"; };
using FunctionSin = FunctionMathUnary<UnaryFunctionVectorized<SinName, sin>>;

}

REGISTER_FUNCTION(Sin)
{
    factory.registerFunction<FunctionSin>(
        FunctionDocumentation{
            .description = "Returns the sine of the argument.",
            .syntax = "sin(x)",
            .arguments = {{"x", "The number whose sine will be returned.", {"(U)Int*", "Float*", "Decimal*"}}},
            .returned_value = {"Returns the sine of x."},
            .examples = {{.name = "simple", .query = "SELECT sin(1.23)", .result = "0.9424888019316975"}},
            .category = FunctionDocumentation::Category::Mathematical},
        FunctionFactory::Case::Insensitive);
}

}
