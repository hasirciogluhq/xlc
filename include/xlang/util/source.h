#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xlang {

using FileId = std::uint32_t;

inline constexpr FileId kNoFile = 0xFFFFFFFFu;

/// A source range: file + start (line, column) + end (line, column, exclusive).
/// Lines and columns are 1-based; columns count bytes.
struct Span {
    FileId file{kNoFile};
    std::uint32_t line{1};
    std::uint32_t column{1};
    std::uint32_t end_line{1};
    std::uint32_t end_column{1};

    [[nodiscard]] bool valid() const { return file != kNoFile; }
    /// Range from the start of `first` to the end of `last`.
    [[nodiscard]] static Span merge(const Span& first, const Span& last);
    /// True when `inner` lies completely inside this span (same file).
    [[nodiscard]] bool contains(const Span& inner) const;
};

class SourceFile {
public:
    SourceFile(std::filesystem::path path, std::string text);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] std::size_t lineCount() const { return line_starts_.size(); }
    /// Text of 1-based line `number` without the line terminator; empty when out of range.
    [[nodiscard]] std::string_view line(std::size_t number) const;

private:
    std::filesystem::path path_;
    std::string text_;
    std::vector<std::size_t> line_starts_;
};

class SourceManager {
public:
    FileId addFile(std::filesystem::path path, std::string text);
    /// Reads `path` from disk; throws XlangError when it cannot be read.
    FileId loadFile(const std::filesystem::path& path);

    [[nodiscard]] const SourceFile& file(FileId id) const { return files_.at(id); }
    [[nodiscard]] std::size_t fileCount() const { return files_.size(); }

private:
    std::vector<SourceFile> files_;
};

}  // namespace xlang
