// Parses frames and fans each result out to every registered sink.
#pragma once

#include "sinks.hpp"

#include <concepts>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace tlm {

class Dispatcher {
public:
    // Construct a sink in place and keep ownership. Returns a reference so the
    // caller can still query it (e.g. read StatsSink results later). The
    // reference stays valid as long as the Dispatcher lives.
    //
    // Both constraints matter: derived_from<Sink, Sink> is true, so without
    // constructible_from, add<Sink>() would pass the check and only fail
    // later, deep inside make_unique, on the abstract class.
    template <typename S, typename... Args>
        requires std::derived_from<S, Sink> && std::constructible_from<S, Args...>
    S& add(Args&&... args)
    {
        auto sink = std::make_unique<S>(std::forward<Args>(args)...);
        S& ref = *sink;
        sinks_.push_back(std::move(sink));      // unique_ptr<S> converts to unique_ptr<Sink>
        return ref;
    }

    // Take ownership of an already-built sink (e.g. from CsvSink::open).
    void add(std::unique_ptr<Sink> sink);

    // Parse one frame, then call on_message or on_error on every sink, in the
    // order they were added.
    void dispatch(std::span<const std::byte> frame);

    [[nodiscard]] std::size_t size() const noexcept { return sinks_.size(); }

private:
    std::vector<std::unique_ptr<Sink>> sinks_;
};

} // namespace tlm
