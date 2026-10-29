#include "dispatcher.hpp"

#include <utility>

namespace tlm {

void Dispatcher::add(std::unique_ptr<Sink> sink)
{
    if (sink) sinks_.push_back(std::move(sink));
}

void Dispatcher::dispatch(std::span<const std::byte> frame)
{
    const ParseResult result = parse(frame);
    for (const auto& sink : sinks_) {
        if (result) sink->on_message(*result);    // virtual call: runs the derived version
        else        sink->on_error(result.error());
    }
}

} // namespace tlm
