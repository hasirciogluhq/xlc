#include "xlang/sema/layout.h"

#include "xlang/util/error.h"

#include <format>
#include <fstream>
#include <sstream>

namespace xlang {

std::string formatLayoutRecord(const LayoutRecord& record) {
    const TypeLayout& l = record.layout;
    std::string out =
        std::format("type {} size={} align={} scheme={} repr={} vptr={} union={} shuffled={} seed={}\n",
                    record.name,
                    l.size,
                    l.align,
                    l.scheme,
                    l.repr_c ? "c" : "xl",
                    l.has_vptr ? 1 : 0,
                    l.is_union ? 1 : 0,
                    l.shuffled ? 1 : 0,
                    l.seed);
    for (const FieldLayout& f : l.fields) {
        out += std::format(
            "  field {} offset={} size={} align={}\n", f.name, f.offset, f.size, f.align);
    }
    return out;
}

void LayoutHistory::put(LayoutRecord record) {
    const std::string name = record.name;
    records_[name] = std::move(record);
}

const LayoutRecord* LayoutHistory::find(const std::string& name) const {
    const auto it = records_.find(name);
    return it == records_.end() ? nullptr : &it->second;
}

void LayoutHistory::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw XlangError(std::format("cannot write layout history `{}`", path));
    }
    write(out);
}

void LayoutHistory::write(std::ostream& out) const {
    for (const auto& [name, record] : records_) {
        out << formatLayoutRecord(record);
    }
}

namespace {

[[nodiscard]] std::string valueOf(const std::vector<std::string>& words, const std::string& key) {
    const std::string prefix = key + "=";
    for (const std::string& w : words) {
        if (w.starts_with(prefix)) {
            return w.substr(prefix.size());
        }
    }
    return {};
}

} // namespace

LayoutHistory LayoutHistory::load(const std::string& path) {
    std::ifstream in(path);
    return in ? read(in, path) : LayoutHistory{};
}

LayoutHistory LayoutHistory::read(std::istream& in, const std::string& source) {
    LayoutHistory history;
    std::string line;
    std::size_t number = 0;
    LayoutRecord current;
    bool open = false;
    const auto flush = [&] {
        if (open) {
            history.put(current);
        }
        open = false;
    };
    while (std::getline(in, line)) {
        ++number;
        std::istringstream words_in(line);
        std::vector<std::string> words;
        for (std::string w; words_in >> w;) {
            words.push_back(w);
        }
        if (words.empty() || words[0].starts_with("//")) {
            continue;
        }
        try {
            if (words[0] == "type" && words.size() >= 2) {
                flush();
                current = LayoutRecord{};
                current.name = words[1];
                current.layout.size = std::stoull(valueOf(words, "size"));
                current.layout.align = std::stoull(valueOf(words, "align"));
                current.layout.scheme = valueOf(words, "scheme");
                current.layout.repr_c = valueOf(words, "repr") == "c";
                current.layout.has_vptr = valueOf(words, "vptr") == "1";
                current.layout.is_union = valueOf(words, "union") == "1";
                current.layout.shuffled = valueOf(words, "shuffled") == "1";
                current.layout.seed =
                    std::stoull(valueOf(words, "seed").empty() ? "0" : valueOf(words, "seed"));
                current.layout.source = "record";
                open = true;
            } else if (words[0] == "field" && words.size() >= 2 && open) {
                FieldLayout f;
                f.name = words[1];
                f.offset = std::stoull(valueOf(words, "offset"));
                f.size = std::stoull(valueOf(words, "size"));
                f.align = std::stoull(valueOf(words, "align"));
                current.layout.fields.push_back(std::move(f));
            } else {
                throw XlangError("unexpected line");
            }
        } catch (const std::exception&) {
            throw XlangError(std::format("{}:{}: malformed layout record", source, number));
        }
    }
    flush();
    return history;
}

} // namespace xlang
