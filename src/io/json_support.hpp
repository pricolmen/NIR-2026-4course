#pragma once

#include <nlohmann/json.hpp>
#include "telemetry/common/time.hpp"
#include <array>
#include <cmath>
#include <optional>
#include <limits>
#include <stdexcept>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace telemetry::io::detail {
using Json = nlohmann::json;
inline void require(bool value) { if (!value) { throw std::invalid_argument("Invalid result schema or value"); } }
inline Json parse(const std::string& text) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(text,[&](int,Json::parse_event_t event,Json& value) {
        if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
        if (event == Json::parse_event_t::key) { require(keys.back().insert(value.get<std::string>()).second); }
        if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
        return true;
    });
}
inline std::uint64_t integer(const Json& value) {
    require(value.is_number_integer());
    if (value.is_number_unsigned()) { return value.get<std::uint64_t>(); }
    const auto number = value.get<std::int64_t>(); require(number >= 0); return static_cast<std::uint64_t>(number);
}
inline bool boolean(const Json& value) { require(value.is_boolean()); return value.get<bool>(); }
inline std::string decimal(long double value) {
    require(std::isfinite(value)); std::ostringstream stream; stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<long double>::max_digits10) << value; return stream.str();
}
inline long double real(const Json& value) {
    require(value.is_string()); const auto text = value.get<std::string>();
    std::istringstream stream{text}; stream.imbue(std::locale::classic()); long double result = 0;
    stream >> std::noskipws >> result; require(!stream.fail() && stream.eof() && std::isfinite(result));
    require(decimal(result) == text); return result;
}
inline Json optional_real(std::optional<long double> value) { return value ? Json(decimal(*value)) : Json(nullptr); }
inline std::optional<long double> read_real(const Json& value) { return value.is_null() ? std::nullopt : std::optional<long double>{real(value)}; }
inline Json time(std::optional<SimTime> value) { return value ? Json(value->ticks()) : Json(nullptr); }
inline std::optional<SimTime> read_time(const Json& value) { return value.is_null() ? std::nullopt : std::optional<SimTime>{SimTime{integer(value)}}; }
template<class E,std::size_t N> std::string name(E value,const std::array<const char*,N>& names) {
    const auto index = static_cast<std::size_t>(value); require(index < N); return names[index];
}
template<class E,std::size_t N> E enumeration(const Json& value,const std::array<const char*,N>& names) {
    require(value.is_string()); const auto text = value.get<std::string>();
    for (std::size_t i = 0; i < N; ++i) { if (text == names[i]) { return static_cast<E>(i); } }
    throw std::invalid_argument("Unknown enumeration value");
}
inline constexpr std::array statuses{"alive","suspect","failed"};
inline constexpr std::array timeout_kinds{"suspect","failed"};
inline constexpr std::array heartbeat_decisions{"accepted","non_increasing_sequence","already_failed"};
inline constexpr std::array timeout_decisions{"applied","stale_generation","already_failed"};
inline constexpr std::array result_decisions{"accepted","unknown_task","already_completed","no_current_assignment","attempt_mismatch","node_mismatch"};
inline constexpr std::array skip_reasons{"crashed","stale_token"};
inline constexpr std::array dispositions{"scheduled","trace_lost","blackout","beyond_horizon"};
inline constexpr std::array variants{"fixed_rr","fixed_ll","adaptive_rr","adaptive_ll"};
}  // namespace telemetry::io::detail
