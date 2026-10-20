#pragma once
#include "telemetry/io/scenario_loader.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

namespace telemetry::io::input_detail {
using Json = nlohmann::basic_json<std::map,std::vector,std::string,bool,std::int64_t,std::uint64_t,long double>;
using namespace simulation;
namespace fs = std::filesystem;
[[noreturn]] inline void fail(const fs::path& file,const std::string& where,const std::string& detail) {
    throw InputError(file,where,detail);
}
// Public SAX API retains the original numeric token, allowing rejection of
// positive magnitudes that underflow to zero (including c, where zero is valid).
class Reader final : public nlohmann::json_sax<Json> {
    struct Frame { Json value; std::string path; std::optional<std::string> key; };
    const fs::path& file_;
    std::vector<Frame> stack_;
    Json root_;
    std::string next_path() const {
        if (stack_.empty()) { return "$"; }
        const auto& frame = stack_.back();
        return frame.value.is_array() ? frame.path+"["+std::to_string(frame.value.size())+"]" : frame.path+"."+*frame.key;
    }
    bool add(Json value) {
        if (stack_.empty()) { root_ = std::move(value); }
        else {
            auto& frame = stack_.back();
            if (frame.value.is_array()) { frame.value.push_back(std::move(value)); }
            else { frame.value[*frame.key] = std::move(value); frame.key.reset(); }
        }
        return true;
    }
    bool end() {
        auto value = std::move(stack_.back().value); stack_.pop_back();
        return add(std::move(value));
    }
public:
    explicit Reader(const fs::path& file) : file_(file) {}
    Json take() { return std::move(root_); }
    bool null() override { return add(nullptr); }
    bool boolean(bool value) override { return add(value); }
    bool number_integer(number_integer_t value) override { return add(value); }
    bool number_unsigned(number_unsigned_t value) override { return add(value); }
    bool number_float(number_float_t value,const string_t& token) override {
        bool nonzero = false;
        for (const auto c : token) {
            if (c == 'e' || c == 'E') { break; }
            if (c >= '1' && c <= '9') { nonzero = true; }
        }
        if (!std::isfinite(value) || (value == 0 && nonzero)) { fail(file_,next_path(),"numeric overflow/underflow"); }
        return add(value);
    }
    bool string(string_t& value) override { return add(std::move(value)); }
    bool binary(binary_t&) override { fail(file_,next_path(),"binary JSON is not supported"); }
    bool start_object(std::size_t) override { auto path = next_path(); stack_.push_back({Json::object(),std::move(path),{}}); return true; }
    bool start_array(std::size_t) override { auto path = next_path(); stack_.push_back({Json::array(),std::move(path),{}}); return true; }
    bool key(string_t& value) override {
        auto& frame = stack_.back();
        if (frame.value.contains(value)) { fail(file_,frame.path+"."+value,"duplicate JSON key"); }
        frame.key = std::move(value); return true;
    }
    bool end_object() override { return end(); }
    bool end_array() override { return end(); }
    bool parse_error(std::size_t byte,const std::string&,const nlohmann::detail::exception& error) override {
        fail(file_,"JSON byte "+std::to_string(byte),error.what());
    }
};
inline void object(const Json& value,const fs::path& file,const std::string& path,std::initializer_list<std::string_view> fields) {
    if (!value.is_object()) { fail(file,path,"expected object"); }
    for (const auto field : fields) {
        if (!value.contains(std::string{field})) { fail(file,path+"."+std::string{field},"required field missing"); }
    }
    for (const auto& [name,ignored] : value.items()) {
        static_cast<void>(ignored);
        if (std::find(fields.begin(),fields.end(),name) == fields.end()) { fail(file,path+"."+name,"unknown field"); }
    }
}
inline const Json& array(const Json& value,const fs::path& file,const std::string& path) {
    if (!value.is_array()) { fail(file,path,"expected array"); }
    return value;
}
inline std::uint64_t integer(const Json& value,const fs::path& file,const std::string& path,
                      std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max(),bool positive = false) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) { result = value.get<std::uint64_t>(); }
    else if (value.is_number_integer() && value.get<std::int64_t>() >= 0) { result = static_cast<std::uint64_t>(value.get<std::int64_t>()); }
    else { fail(file,path,"expected nonnegative integer token (no fraction, exponent or bool)"); }
    if (result > maximum || (positive && result == 0)) { fail(file,path,"integer outside allowed range"); }
    return result;
}
inline SimTime time(const Json& value,const fs::path& file,const std::string& path) {
    return SimTime{integer(value,file,path,static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))};
}
inline long double real(const Json& value,const fs::path& file,const std::string& path,bool positive) {
    if (!value.is_number()) { fail(file,path,"expected finite number"); }
    const auto result = value.get<long double>();
    if (!std::isfinite(result) || result < 0 || (positive && result == 0)) { fail(file,path,"number outside allowed range"); }
    return result;
}
template<class Action> auto checked(const fs::path& file,const std::string& path,Action action) {
    try { return action(); }
    catch (const std::invalid_argument& e) { fail(file,path,e.what()); }
    catch (const std::overflow_error& e) { fail(file,path,e.what()); }
    catch (const std::underflow_error& e) { fail(file,path,e.what()); }
}
inline Json load(const fs::path& file) {
    std::ifstream stream{file}; if (!stream) { fail(file,"$","cannot open JSON"); }
    Reader reader{file}; if (!Json::sax_parse(stream,&reader) || stream.bad()) { fail(file,"$","JSON read error"); } return reader.take();
}
}  // namespace telemetry::io::input_detail
