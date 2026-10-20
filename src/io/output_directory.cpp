#include "telemetry/io/output_directory.hpp"
#include <stdexcept>
#include <string>

namespace telemetry::io {
namespace {
namespace fs = std::filesystem;
bool contains(const fs::path& parent,const fs::path& child) {
    auto a = parent.begin(),b = child.begin(); for (; a != parent.end(); ++a,++b) { if (b == child.end() || *a != *b) { return false; } } return true;
}
fs::path spare(const fs::path& destination,const char* tag) {
    for (std::uint64_t i = 1; i < 100000; ++i) {
        auto path = destination.parent_path()/("."+destination.filename().string()+tag+std::to_string(i));
        if (!fs::exists(fs::symlink_status(path))) { return path; }
    }
    throw std::runtime_error("Cannot allocate output workspace");
}
void check(const fs::path& path,bool overwrite) {
    if (fs::is_symlink(fs::symlink_status(path))) { throw std::runtime_error("Output destination must not be a symlink"); }
    if (fs::exists(path) && !fs::is_directory(path)) { throw std::runtime_error("Output path is not a directory: "+path.string()); }
    if (fs::exists(path) && !overwrite && !fs::is_empty(path)) {
        throw std::runtime_error("Output directory is not empty; use --overwrite: "+path.string());
    }
}
}
OutputDirectory::OutputDirectory(const fs::path& requested,bool overwrite,const std::vector<fs::path>& inputs) : overwrite_(overwrite) {
    if (requested.empty()) { throw std::invalid_argument("Empty output path"); }
    auto absolute = fs::absolute(requested).lexically_normal();
    while (absolute != absolute.root_path() && absolute.filename().empty()) { absolute = absolute.parent_path(); }
    if (fs::is_symlink(fs::symlink_status(absolute))) { throw std::runtime_error("Output destination must not be a symlink"); }
    destination_ = fs::weakly_canonical(absolute);
    if (destination_ == destination_.root_path() || contains(destination_,fs::weakly_canonical(fs::current_path()))) {
        throw std::runtime_error("Output must not replace the working directory or its ancestors");
    }
    for (const auto& input : inputs) {
        if (contains(destination_,fs::weakly_canonical(fs::absolute(input)))) { throw std::runtime_error("Output would replace an input: "+input.string()); }
    }
    check(destination_,overwrite); fs::create_directories(destination_.parent_path()); work_ = spare(destination_,".work-");
    if (!fs::create_directory(work_)) { throw std::runtime_error("Cannot create output workspace"); }
}
OutputDirectory::~OutputDirectory() {
    if (!committed_ && !work_.empty()) { std::error_code ignored; fs::remove_all(work_,ignored); }
}
fs::path OutputDirectory::commit() {
    if (committed_) { throw std::logic_error("Output already committed"); } check(destination_,overwrite_);
    fs::path backup;
    if (fs::exists(destination_)) { backup = spare(destination_,".previous-"); fs::rename(destination_,backup); }
    try { fs::rename(work_,destination_); committed_ = true; }
    catch (...) { if (!backup.empty()) { fs::rename(backup,destination_); } throw; }
    return backup;
}
}  // namespace telemetry::io
