#include "dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace tlm;

namespace {

// A test double: records every call so tests can see what the Dispatcher did.
// Catch2 has no mocking library; for an interface this small, a hand-written
// recorder is simpler than one anyway.
class RecordingSink final : public Sink {
public:
    explicit RecordingSink(std::string label, std::vector<std::string>* log = nullptr)
        : label_{std::move(label)}, log_{log} {}

    void on_message(const Message& m) override
    {
        messages.push_back(m);
        if (log_) log_->push_back(label_ + ":msg");
    }
    void on_error(ParseError e) override
    {
        errors.push_back(e);
        if (log_) log_->push_back(label_ + ":err");
    }
    [[nodiscard]] std::string_view name() const noexcept override { return label_; }

    std::vector<Message>    messages;
    std::vector<ParseError> errors;

private:
    std::string               label_;
    std::vector<std::string>* log_;   // shared call log, to check ordering across sinks
};

// Only overrides the required functions; relies on Sink's default on_error.
class MinimalSink final : public Sink {
public:
    void on_message(const Message&) override { ++count; }
    [[nodiscard]] std::string_view name() const noexcept override { return "minimal"; }
    int count = 0;
};

// Sets a flag in its destructor, to prove deletion through Sink* runs it.
class DestructionProbe final : public Sink {
public:
    explicit DestructionProbe(bool& destroyed) : destroyed_{destroyed} {}
    ~DestructionProbe() override { destroyed_ = true; }
    void on_message(const Message&) override {}
    [[nodiscard]] std::string_view name() const noexcept override { return "probe"; }

private:
    bool& destroyed_;
};

} // namespace

// ---------------------------------------------------------------------------
TEST_CASE("Valid frames reach every sink's on_message", "[dispatcher]")
{
    Dispatcher d;
    auto& a = d.add<RecordingSink>("a");
    auto& b = d.add<RecordingSink>("b");

    const Frame f = encode(Voltage{1, 3300});
    d.dispatch(f.view());

    REQUIRE(a.messages.size() == 1);
    REQUIRE(b.messages.size() == 1);
    CHECK(a.messages[0] == Message{Voltage{1, 3300}});
    CHECK(a.errors.empty());
}

TEST_CASE("Bad frames reach every sink's on_error", "[dispatcher]")
{
    Dispatcher d;
    auto& rec = d.add<RecordingSink>("rec");

    Frame f = encode(Fault{7});
    f.bytes[3] ^= std::byte{0xFF};        // corrupt the payload
    d.dispatch(f.view());

    CHECK(rec.messages.empty());
    REQUIRE(rec.errors.size() == 1);
    CHECK(rec.errors[0] == ParseError::BadCrc);
}

TEST_CASE("Sinks are called in the order they were added", "[dispatcher]")
{
    std::vector<std::string> log;
    Dispatcher d;
    d.add<RecordingSink>("first", &log);
    d.add<RecordingSink>("second", &log);

    d.dispatch(encode(Temperature{1, 1}).view());
    d.dispatch(std::span<const std::byte>{});     // empty: TooShort

    CHECK(log == std::vector<std::string>{"first:msg", "second:msg", "first:err", "second:err"});
}

TEST_CASE("A sink can rely on the default on_error", "[dispatcher]")
{
    Dispatcher d;
    auto& m = d.add<MinimalSink>();
    d.dispatch(std::span<const std::byte>{});     // error: base class no-op handles it
    d.dispatch(encode(Fault{1}).view());
    CHECK(m.count == 1);
}

TEST_CASE("Sinks from a factory can be added as unique_ptr", "[dispatcher]")
{
    Dispatcher d;
    d.add(std::make_unique<StatsSink>());
    d.add(std::unique_ptr<Sink>{});               // null is ignored
    CHECK(d.size() == 1);
}

TEST_CASE("Destroying the Dispatcher runs each derived destructor", "[dispatcher]")
{
    bool destroyed = false;
    {
        Dispatcher d;
        d.add<DestructionProbe>(destroyed);
        CHECK_FALSE(destroyed);
    }   // vector<unique_ptr<Sink>> deletes through Sink*: needs the virtual destructor
    CHECK(destroyed);
}

TEST_CASE("Stats gathered through the Dispatcher", "[dispatcher][stats]")
{
    Dispatcher d;
    auto& stats = d.add<StatsSink>();

    for (const std::int16_t c : std::initializer_list<std::int16_t>{2000, 2100, 2200}) {
        d.dispatch(encode(Temperature{3, c}).view());
    }
    d.dispatch(std::span<const std::byte>{});

    CHECK(stats.messages() == 3);
    CHECK(stats.errors(ParseError::TooShort) == 1);
    REQUIRE(stats.temperature(3).has_value());
    CHECK(stats.temperature(3)->max == 2200);
}

// add<S>() only accepts types derived from Sink.
template <typename S>
concept Addable = requires (Dispatcher d) { d.add<S>(); };

TEST_CASE("add<S>() rejects types that aren't sinks", "[dispatcher][constexpr]")
{
    STATIC_REQUIRE(Addable<StatsSink>);
    STATIC_REQUIRE_FALSE(Addable<std::string>);
    STATIC_REQUIRE_FALSE(Addable<Sink>);           // abstract: not constructible_from
}
