#pragma once
#include <filesystem>
#include <vector>

namespace telemetry::io {
// Private sibling workspace; commits with rename only after successful work.
// Explicit overwrite preserves the previous directory as a sibling backup.
class OutputDirectory {
public:
    OutputDirectory(const std::filesystem::path& destination,bool overwrite,
                    const std::vector<std::filesystem::path>& protected_inputs);
    ~OutputDirectory();
    OutputDirectory(const OutputDirectory&) = delete;
    OutputDirectory& operator=(const OutputDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& workspace() const noexcept { return work_; }
    // Returns the backup path if an existing destination was replaced.
    [[nodiscard]] std::filesystem::path commit();
private:
    std::filesystem::path destination_,work_;
    bool overwrite_,committed_ = false;
};
}  // namespace telemetry::io
