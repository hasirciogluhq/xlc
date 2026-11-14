#pragma once

// The source manager shim: the xlang implementation (src/util/source.xl) owns the registry and
// the diagnostics rendering read it through the bridge; this side keeps a mirror of the files so
// C++ readers (the lexer, the hash in the emit plan) see the text without a round trip.

#include "xlang/util/error.h"
#include "xlang/util/util_c.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    [[nodiscard]] static Span merge(const Span& first, const Span& last) {
        Span out = first;
        out.end_line = last.end_line;
        out.end_column = last.end_column;
        return out;
    }
    /// True when `inner` lies completely inside this span (same file).
    [[nodiscard]] bool contains(const Span& inner) const {
        if (!valid() || file != inner.file) {
            return false;
        }
        const auto before = [](std::uint32_t l1, std::uint32_t c1, std::uint32_t l2,
                               std::uint32_t c2) {
            return l1 < l2 || (l1 == l2 && c1 <= c2);
        };
        return before(line, column, inner.line, inner.column) &&
               before(inner.end_line, inner.end_column, end_line, end_column);
    }
};

class SourceFile {
public:
    SourceFile(std::filesystem::path path, std::string text) : path_(std::move(path)), text_(std::move(text)) {
        line_starts_.push_back(0);
        for (std::size_t i = 0; i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                line_starts_.push_back(i + 1);
            }
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] std::size_t lineCount() const { return line_starts_.size(); }
    /// Text of 1-based line `number` without the line terminator; empty when out of range.
    [[nodiscard]] std::string_view line(std::size_t number) const {
        if (number == 0 || number > line_starts_.size()) {
            return {};
        }
        const std::size_t begin = line_starts_[number - 1];
        std::size_t end = number < line_starts_.size() ? line_starts_[number] : text_.size();
        while (end > begin && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) {
            --end;
        }
        return std::string_view(text_).substr(begin, end - begin);
    }

private:
    std::filesystem::path path_;
    std::string text_;
    std::vector<std::size_t> line_starts_;
};

class SourceManager {
public:
    SourceManager() : handle_(xl::xlcUtilSourcesNew()) {}
    ~SourceManager() {
        if (handle_ != nullptr) {
            xl::xlcUtilSourcesFree(handle_);
        }
    }
    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;

    /// The xlang-side registry the diagnostic engine renders from.
    [[nodiscard]] void* handle() const { return handle_; }

    FileId addFile(std::filesystem::path path, std::string text) {
        const std::string p = path.string();
        const xl::XbStr xb_path{p.data(), static_cast<std::uint32_t>(p.size())};
        const xl::XbStr xb_text{text.data(), static_cast<std::uint32_t>(text.size())};
        const FileId id = xl::xlcUtilAddFile(handle_, &xb_path, &xb_text);
        files_.emplace_back(std::move(path), std::move(text));
        return id;
    }

    /// Reads `path` from disk; throws XlangError when it cannot be read.
    FileId loadFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw XlangError("cannot read `" + path.string() + "`");
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return addFile(path, buffer.str());
    }

    [[nodiscard]] const SourceFile& file(FileId id) const { return files_.at(id); }
    [[nodiscard]] std::size_t fileCount() const { return files_.size(); }

private:
    void* handle_{nullptr};
    std::vector<SourceFile> files_;
};

}  // namespace xlang
