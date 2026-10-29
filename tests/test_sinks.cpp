#include "sinks.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace tlm;
using Catch::Approx;

// ---------------------------------------------------------------------------
TEST_CASE("Sink is a proper polymorphic interface", "[sinks][constexpr]")
{
    STATIC_REQUIRE(std::is_abstract_v<Sink>);                 // can't instantiate Sink itself
    STATIC_REQUIRE(std::has_virtual_destructor_v<Sink>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Sink>); // no slicing copies
    STATIC_REQUIRE(std::is_final_v<StatsSink>);
    STATIC_REQUIRE(std::is_base_of_v<Sink, CsvSink>);
}

// ---------------------------------------------------------------------------
TEST_CASE("StatsSink computes per-sensor temperature statistics", "[sinks][stats]")
{
    StatsSink stats;
    for (const std::int16_t c : std::initializer_list<std::int16_t>{2000, 2500, 3000, 2200}) {   // sensor 1: even count
        stats.on_message(Temperature{1, c});
    }
    for (const std::int16_t c : std::initializer_list<std::int16_t>{-500, 100, 0}) {              // sensor 2: odd count
        stats.on_message(Temperature{2, c});
    }

    SECTION("even number of samples")
    {
        const auto s = stats.temperature(1);
        REQUIRE(s.has_value());
        CHECK(s->count == 4);
        CHECK(s->min == 2000);
        CHECK(s->max == 3000);
        CHECK(s->mean == Approx(2425.0));
        CHECK(s->median == Approx(2350.0));    // (2200 + 2500) / 2
    }
    SECTION("odd number of samples, including negatives")
    {
        const auto s = stats.temperature(2);
        REQUIRE(s.has_value());
        CHECK(s->min == -500);
        CHECK(s->max == 100);
        CHECK(s->median == Approx(0.0));
    }
    SECTION("unknown sensor has no statistics")
    {
        CHECK_FALSE(stats.temperature(9).has_value());
    }
    SECTION("sensor list and threshold counts")
    {
        CHECK(stats.sensors() == std::vector<std::uint8_t>{1, 2});
        CHECK(stats.readings_above(1, 2200) == 2);   // 2500 and 3000
        CHECK(stats.readings_above(9, 0) == 0);
    }
}

TEST_CASE("StatsSink tracks voltage ranges, faults, and errors", "[sinks][stats]")
{
    StatsSink stats;
    stats.on_message(Voltage{0, 3300});
    stats.on_message(Voltage{0, 3250});
    stats.on_message(Voltage{0, 3410});
    stats.on_message(Fault{0x0101});
    stats.on_message(Fault{0x0202});
    stats.on_error(ParseError::BadCrc);
    stats.on_error(ParseError::BadCrc);
    stats.on_error(ParseError::BadSync);

    CHECK(stats.voltage_range(0) == std::pair<std::uint16_t, std::uint16_t>{3250, 3410});
    CHECK_FALSE(stats.voltage_range(1).has_value());

    CHECK(stats.messages() == 5);
    CHECK(stats.faults() == 2);
    REQUIRE(stats.first_fault().has_value());
    CHECK(stats.first_fault()->code == 0x0101);

    CHECK(stats.errors(ParseError::BadCrc) == 2);
    CHECK(stats.errors(ParseError::BadSync) == 1);
    CHECK(stats.errors(ParseError::TooShort) == 0);
}

// ---------------------------------------------------------------------------
TEST_CASE("format_centi uses integer math for fixed-point output", "[sinks][csv]")
{
    CHECK(format_centi(1234) == "12.34");
    CHECK(format_centi(5) == "0.05");
    CHECK(format_centi(-5) == "-0.05");      // the case naive c / 100 gets wrong
    CHECK(format_centi(-1250) == "-12.50");
    CHECK(format_centi(0) == "0.00");
    CHECK(format_centi(-32768) == "-327.68");
}

namespace {

// A unique temporary file, deleted when this object goes out of scope (RAII).
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name)
        : path{std::filesystem::temp_directory_path() / name} {}
    ~TempFile() { std::filesystem::remove(path); }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string contents() const
    {
        std::ifstream in{path};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }
};

} // namespace

TEST_CASE("CsvSink writes temperature rows and closes the file", "[sinks][csv]")
{
    TempFile tmp{"telemetry_kata_test.csv"};

    {
        auto csv = CsvSink::open(tmp.path);
        REQUIRE(csv != nullptr);
        csv->on_message(Temperature{1, 2150});
        csv->on_message(Voltage{0, 3300});       // not a temperature: skipped, but counted
        csv->on_message(Temperature{2, -5});
    }   // unique_ptr destroys the sink -> FilePtr calls fclose -> data flushed

    CHECK(tmp.contents() ==
          "seq,sensor,deg_c\n"
          "1,1,21.50\n"
          "3,2,-0.05\n");
}

TEST_CASE("CsvSink::open reports failure with nullptr", "[sinks][csv]")
{
    CHECK(CsvSink::open("/nonexistent-directory/out.csv") == nullptr);
}
