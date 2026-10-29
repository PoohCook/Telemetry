// Message consumers. A Sink is anything that wants parsed messages: a logger,
// a statistics collector, a CSV recorder. The Dispatcher owns a list of them.
#pragma once

#include "messages.hpp"
#include "parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tlm {

// Number of ParseError values, for per-error counters. Update if the enum grows.
inline constexpr std::size_t kParseErrorCount =
    std::to_underlying(ParseError::BadPayloadSize) + 1;

// ---------------------------------------------------------------------------
// The interface. Abstract (pure virtual functions), so it can't be created
// directly; you hold derived objects through Sink* or std::unique_ptr<Sink>.
class Sink {
public:
    Sink() = default;

    // Virtual destructor: deleting a derived object through a Sink* must run
    // the derived destructor. Without `virtual`, that's undefined behavior.
    virtual ~Sink();

    // Copying through a base reference would slice off the derived part.
    Sink(const Sink&)            = delete;
    Sink& operator=(const Sink&) = delete;

    virtual void on_message(const Message& m) = 0;              // must override
    virtual void on_error([[maybe_unused]] ParseError e) {}     // may override
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Collects statistics. `final`: nothing can derive from it, which also lets
// the compiler call its functions directly when it knows the exact type.
class StatsSink final : public Sink {
public:
    struct SensorStats {
        std::size_t  count;
        std::int16_t min;
        std::int16_t max;
        double       mean;
        double       median;     // mean of the two middle samples when count is even
    };

    void on_message(const Message& m) override;
    void on_error(ParseError e) override;
    [[nodiscard]] std::string_view name() const noexcept override { return "stats"; }

    [[nodiscard]] std::optional<SensorStats> temperature(std::uint8_t sensor) const;
    [[nodiscard]] std::optional<std::pair<std::uint16_t, std::uint16_t>> voltage_range(std::uint8_t rail) const;
    [[nodiscard]] std::vector<std::uint8_t> sensors() const;
    [[nodiscard]] std::size_t readings_above(std::uint8_t sensor, std::int16_t centi_c) const;

    [[nodiscard]] std::size_t messages() const noexcept { return messages_; }
    [[nodiscard]] std::size_t faults() const noexcept { return faults_; }
    [[nodiscard]] std::optional<Fault> first_fault() const noexcept { return first_fault_; }
    [[nodiscard]] std::size_t errors(ParseError e) const noexcept;

private:
    std::size_t messages_{0};
    std::size_t faults_{0};
    std::optional<Fault> first_fault_;
    std::map<std::uint8_t, std::vector<std::int16_t>> temps_;
    std::map<std::uint8_t, std::pair<std::uint16_t, std::uint16_t>> volts_;
    std::array<std::size_t, kParseErrorCount> errors_{};   // one counter per ParseError
};

// ---------------------------------------------------------------------------
// RAII for a C resource: unique_ptr with a custom deleter calls fclose.
struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// Writes temperature readings as CSV: "seq,sensor,deg_c".
// Rule of zero: FilePtr closes the file, so no destructor is written here.
class CsvSink final : public Sink {
public:
    // Factory instead of a constructor, so failure is a return value
    // (nullptr) rather than an exception.
    [[nodiscard]] static std::unique_ptr<CsvSink> open(const std::filesystem::path& path);

    void on_message(const Message& m) override;
    [[nodiscard]] std::string_view name() const noexcept override { return "csv"; }

private:
    explicit CsvSink(FilePtr file) noexcept : file_{std::move(file)} {}

    FilePtr     file_;
    std::size_t seq_{0};
};

// Formats hundredths of a degree as "12.34" / "-0.05" with integer math only.
// Exposed for testing.
[[nodiscard]] std::string format_centi(std::int16_t centi);

} // namespace tlm
