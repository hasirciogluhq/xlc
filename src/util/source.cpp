#include "xlang/util/source.h"

#include "xlang/util/error.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace xlang {

Span Span::merge(const Span& first, const Span& last) {
    Span out = first;
    out.end_line = last.end_line;
    out.end_column = last.end_column;
    return out;
}

bool Span::contains(const Span& inner) const {
    if (!valid() || file != inner.file) {
        return false;
    }
    const auto before = [](std::uint32_t l1, std::uint32_t c1, std::uint32_t l2, std::uint32_t c2) {
        return l1 < l2 || (l1 == l2 && c1 <= c2);
    };
    return before(line, column, inner.line, inner.column) &&
           before(inner.end_line, inner.end_column, end_line, end_column);
}

SourceFile::SourceFile(std::filesystem::path path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            line_starts_.push_back(i + 1);
        }
    }
}

std::string_view SourceFile::line(std::size_t number) const {
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

FileId SourceManager::addFile(std::filesystem::path path, std::string text) {
    files_.emplace_back(std::move(path), std::move(text));
    return static_cast<FileId>(files_.size() - 1);
}

FileId SourceManager::loadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw XlangError("cannot read `" + path.string() + "`");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return addFile(path, buffer.str());
}

}  // namespace xlang
