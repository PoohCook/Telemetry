#include "sinks.hpp"

#include <algorithm>
#include <format>       // std::format_to_n (C++20)
#include <numeric>      // std::accumulate
#include <ranges>
#include <string>
#include <utility>      // std::to_underlying

namespace tlm {

// Defined out of line on purpose: the first non-inline virtual function is
// where the compiler emits the class's vtable, so this keeps it in one .o file.
Sink::~Sink() = default;

// ---------------------------------------------------------------------------
void StatsSink::on_message(const Message& m)
{
    ++messages_;
    std::visit(overloaded{
        [this](const Temperature& t) { temps_[t.sensor_id].push_back(t.centi_c); },
        [this](const Voltage& v) {
            // try_emplace inserts only if the key is new; structured binding
            // unpacks the (iterator, inserted) pair it returns.
            auto [it, inserted] = volts_.try_emplace(v.rail, v.millivolts, v.millivolts);
            if (!inserted) {
                auto& [lo, hi] = it->second;          // references into the map
                lo = std::min(lo, v.millivolts);
                hi = std::max(hi, v.millivolts);
            }
        },
        [this](const Fault& f) {
            ++faults_;
            if (!first_fault_) first_fault_ = f;
        },
    }, m);
}

void StatsSink::on_error(ParseError e)
{
    ++errors_.at(std::to_underlying(e));
}

std::size_t StatsSink::errors(ParseError e) const noexcept
{
    return errors_[std::to_underlying(e)];
}

std::optional<StatsSink::SensorStats> StatsSink::temperature(std::uint8_t sensor) const
{
    const auto it = temps_.find(sensor);
    if (it == temps_.end()) return std::nullopt;

    const std::vector<std::int16_t>& samples = it->second;
    const auto [lo, hi] = std::ranges::minmax(samples);
    const double sum  = std::accumulate(samples.begin(), samples.end(), 0.0);
    const double mean = sum / static_cast<double>(samples.size());

    // Median without fully sorting: nth_element puts the middle element in
    // place in O(n). Work on a copy so the recorded order isn't disturbed.
    std::vector<std::int16_t> work = samples;
    const auto mid = work.begin() + static_cast<std::ptrdiff_t>(work.size() / 2);
    std::ranges::nth_element(work, mid);
    double median = *mid;
    if (work.size() % 2 == 0) {
        // Even count: the other middle value is the largest of the lower half.
        median = (median + *std::ranges::max_element(work.begin(), mid)) / 2.0;
    }

    return SensorStats{samples.size(), lo, hi, mean, median};
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> StatsSink::voltage_range(std::uint8_t rail) const
{
    if (const auto it = volts_.find(rail); it != volts_.end()) {   // if with initializer
        return it->second;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> StatsSink::sensors() const
{
    std::vector<std::uint8_t> ids;
    for (const std::uint8_t id : temps_ | std::views::keys) {   // a view over the map's keys
        ids.push_back(id);
    }
    return ids;                                                  // std::map keeps them sorted
}

std::size_t StatsSink::readings_above(std::uint8_t sensor, std::int16_t centi_c) const
{
    const auto it = temps_.find(sensor);
    if (it == temps_.end()) return 0;
    return static_cast<std::size_t>(
        std::ranges::count_if(it->second, [=](std::int16_t c) { return c > centi_c; }));
}

// ---------------------------------------------------------------------------
std::string format_centi(std::int16_t centi)
{
    const int  v   = centi;                  // widen first: -(-32768) overflows int16
    const int  mag = v < 0 ? -v : v;
    return std::format("{}{}.{:02}", v < 0 ? "-" : "", mag / 100, mag % 100);
}

std::unique_ptr<CsvSink> CsvSink::open(const std::filesystem::path& path)
{
    FilePtr file{std::fopen(path.c_str(), "w")};
    if (!file) return nullptr;
    std::fputs("seq,sensor,deg_c\n", file.get());
    // make_unique can't reach a private constructor, so construct directly.
    return std::unique_ptr<CsvSink>(new CsvSink(std::move(file)));
}

void CsvSink::on_message(const Message& m)
{
    ++seq_;
    const auto* t = std::get_if<Temperature>(&m);
    if (t == nullptr) return;

    // Format into a stack buffer: no heap allocation per line.
    std::array<char, 48> line{};
    const auto res = std::format_to_n(line.data(), line.size(), "{},{},{}\n",
                                      seq_, t->sensor_id, format_centi(t->centi_c));
    std::fwrite(line.data(), 1, static_cast<std::size_t>(res.out - line.data()), file_.get());
}

} // namespace tlm
