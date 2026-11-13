#pragma once

#include "xlang/lang/types.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace xlang {

struct FieldLayout {
    std::string name;
    Type type;
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::uint64_t align{1};
    bool inherited{false};  ///< field of a base class
};

/// Size, alignment and field offsets of a struct/class (or the shape of a primitive).
struct TypeLayout {
    std::uint64_t size{0};
    std::uint64_t align{1};
    std::uint64_t data_size{0};  ///< end of the last field, without tail padding (Itanium `dsize`)
    std::vector<FieldLayout> fields;  ///< declaration order (base class fields first)
    bool has_vptr{false};
    bool repr_c{false};
    bool packed{false};
    /// A union (the compiler-made `E$Data` of a payload enum): every field sits at offset 0.
    bool is_union{false};
    bool shuffled{false};
    std::uint64_t seed{0};
    std::string scheme{"v1"};
    std::string source{"computed"};  ///< computed | record | assumed-c
};

/// The newest (and currently only) default layout scheme. Schemes are frozen once released.
inline constexpr const char* kCurrentLayoutScheme = "v1";

/// A persisted layout: what `xl.layout` stores per type.
struct LayoutRecord {
    std::string name;
    TypeLayout layout;
};

/// `xl.layout` project history: text, one record per type.
///
///   type Vec3 size=12 align=4 scheme=v1 repr=xl shuffled=0 seed=0
///     field x offset=0 size=4 align=4
class LayoutHistory {
public:
    /// Missing file = empty history. Malformed lines throw XlangError.
    static LayoutHistory load(const std::string& path);
    void save(const std::string& path) const;
    /// The text form `load` and `save` use; `source` names the input in an error.
    static LayoutHistory read(std::istream& in, const std::string& source);
    void write(std::ostream& out) const;

    void put(LayoutRecord record);
    [[nodiscard]] const LayoutRecord* find(const std::string& name) const;
    [[nodiscard]] const std::map<std::string, LayoutRecord>& records() const { return records_; }

private:
    std::map<std::string, LayoutRecord> records_;
};

[[nodiscard]] std::string formatLayoutRecord(const LayoutRecord& record);

}  // namespace xlang
