#include "sema/sema/sema.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <random>

namespace xlang {

using sema_detail::alignUp;
using sema_detail::attrHasIdent;
using sema_detail::attrKey;
using sema_detail::findAttr;
using sema_detail::joinDotted;

// Layouts (plan 3.3): primitive sizes, `str`, arrays, and the aggregate layout (scheme v1,
// `@repr(c)`, `@packed`, `@align`, `@layout`, recorded layouts of `declare`d types).

// ---------------------------------------------------------------------------------------------
// Layout

std::optional<std::uint64_t> Sema::primitiveSize(const Type& t, std::uint64_t& align) {
    switch (t.kind) {
    case TypeKind::Bool:
        align = 1;
        return 1;
    case TypeKind::Int: {
        const unsigned bits = t.bits == 0 ? config_.target.pointer_width : t.bits;
        align = bits / 8;
        return bits / 8;
    }
    case TypeKind::Float:
        align = t.bits / 8;
        return t.bits / 8;
    case TypeKind::Pointer:
    case TypeKind::FnPtr:
        align = config_.target.pointer_width / 8;
        return align;
    case TypeKind::Interface:
        align = config_.target.pointer_width / 8;
        return 2 * align;
    default:
        return std::nullopt;
    }
}

std::optional<TypeLayout> Sema::layoutOf(const Type& type, const Span& at) {
    auto resolved = resolve(type);
    if (!resolved) {
        return std::nullopt;
    }
    const Type& t = *resolved;
    TypeLayout out;
    out.scheme = config_.layout_scheme.empty() ? kCurrentLayoutScheme : config_.layout_scheme;
    switch (t.kind) {
    case TypeKind::Void:
        error("E0703", at, "`void` has no size", "sizeless type");
        return std::nullopt;
    case TypeKind::Enum:
        return layoutOf(*t.elem, at);
    case TypeKind::Array: {
        const Expr* len = t.length.get();
        auto value = len != nullptr ? evalConst(*len) : std::nullopt;
        if (!value || value->kind != ConstValue::Kind::Int || value->int_value < 0) {
            if (value) {
                error("E0707",
                      len->span,
                      "array length must be a non-negative integer constant",
                      "invalid length");
            }
            return std::nullopt;
        }
        auto elem = layoutOf(*t.elem, at);
        if (!elem) {
            return std::nullopt;
        }
        out.align = elem->align;
        out.size = elem->size * static_cast<std::uint64_t>(value->int_value);
        out.data_size = out.size;
        return out;
    }
    case TypeKind::Struct:
    case TypeKind::Class: {
        const auto ref = entityByQName(t.path.front());
        if (!ref) {
            error("E0706",
                  at,
                  std::format("unknown type `{}`", t.path.front()),
                  "not a struct or class");
            return std::nullopt;
        }
        return ref->owner->layoutOfName(ref->local, at);
    }
    case TypeKind::Tuple: {
        // A tuple is laid out like `@repr(c)` with the elements in order: no scheme reordering, so
        // `(A, B)` and `(B, A)` differ and `.0` always names element 0. `()` has size 0.
        std::uint64_t cursor = 0;
        std::uint64_t align = 1;
        for (std::size_t i = 0; i < t.args.size(); ++i) {
            auto element = layoutOf(t.args[i], at);
            if (!element) {
                return std::nullopt;
            }
            const std::uint64_t off = alignUp(cursor, element->align);
            out.fields.push_back(FieldLayout{.name = std::to_string(i),
                                             .type = t.args[i],
                                             .offset = off,
                                             .size = element->size,
                                             .align = element->align});
            cursor = off + element->size;
            align = std::max(align, element->align);
        }
        out.align = align;
        out.data_size = cursor;
        out.size = alignUp(cursor, align);
        out.repr_c = true;
        return out;
    }
    case TypeKind::Str: {
        // `str` is `{ ptr, len }`; the fields are visible to `s.ptr` and `s.len`.
        const std::uint64_t word = config_.target.pointer_width / 8;
        out.align = word;
        out.size = 2 * word;
        out.data_size = out.size;
        out.fields.push_back(FieldLayout{.name = "ptr",
                                         .type = Type::makePointer(Type::makeInt(8, false)),
                                         .offset = 0,
                                         .size = word,
                                         .align = word});
        out.fields.push_back(FieldLayout{.name = "len",
                                         .type = Type::makeInt(0, false),
                                         .offset = word,
                                         .size = word,
                                         .align = word});
        return out;
    }
    case TypeKind::Slice: {
        // `[]T` and `[]mut T` are `{ ptr: *T, len: usize }`: the same two words as `str`, with the
        // element type in `ptr` so indexing steps by `sizeof(T)`.
        const std::uint64_t word = config_.target.pointer_width / 8;
        out.align = word;
        out.size = 2 * word;
        out.data_size = out.size;
        const Type elem = t.elem ? *t.elem : Type::makeInt(8, false);
        out.fields.push_back(
            FieldLayout{.name = "ptr", .type = Type::makePointer(elem), .offset = 0, .size = word, .align = word});
        out.fields.push_back(FieldLayout{.name = "len",
                                         .type = Type::makeInt(0, false),
                                         .offset = word,
                                         .size = word,
                                         .align = word});
        return out;
    }
    case TypeKind::Named:
        error("E0703",
              at,
              std::format("size of `{}` is unknown", typeToString(t)),
              "type comes from another module (module loading not available here)");
        return std::nullopt;
    default: {
        std::uint64_t align = 1;
        const auto size = primitiveSize(t, align);
        if (!size) {
            return std::nullopt;
        }
        out.size = *size;
        out.align = align;
        out.data_size = *size;
        return out;
    }
    }
}

std::optional<TypeLayout> Sema::layoutOfName(const std::string& name, const Span& at) {
    if (const auto it = layout_cache_.find(name); it != layout_cache_.end()) {
        return it->second;
    }
    const Symbol* sym = find(name);
    if (sym == nullptr || (sym->kind != Symbol::Kind::Struct && sym->kind != Symbol::Kind::Class)) {
        error("E0706", at, std::format("unknown type `{}`", name), "not a struct or class");
        return std::nullopt;
    }
    if (!layout_in_progress_.insert(name).second) {
        error("E0703",
              at,
              std::format("`{}` contains itself by value", name),
              "recursive type has infinite size; use a pointer");
        return std::nullopt;
    }
    auto result = aggregateLayout(name, *sym->item, at);
    layout_in_progress_.erase(name);
    layout_cache_[name] = result;
    return result;
}

std::optional<TypeLayout>
Sema::aggregateLayout(const std::string& name, const Item& item, const Span& at) {
    const DeclBase& base_decl = declBase(item);
    const std::vector<FieldDecl>* fields = nullptr;
    const ClassDecl* cls = nullptr;
    bool opaque = false;
    bool is_union = false;
    if (const auto* s = std::get_if<StructDecl>(&item.decl)) {
        fields = &s->fields;
        opaque = s->opaque;
        is_union = s->is_union;
    } else {
        cls = &std::get<ClassDecl>(item.decl);
        fields = &cls->fields;
        opaque = cls->opaque;
    }
    if (opaque) {
        error("E0703",
              at,
              std::format("`{}` is opaque and has no known size", name),
              "opaque `declare` types can only be used behind a pointer");
        return std::nullopt;
    }

    TypeLayout out;
    out.scheme = config_.layout_scheme.empty() ? kCurrentLayoutScheme : config_.layout_scheme;
    const Attribute* repr = findAttr(base_decl.attributes, "repr");
    out.repr_c = repr != nullptr && attrHasIdent(*repr, "c");
    out.packed = findAttr(base_decl.attributes, "packed") != nullptr;
    out.is_union = is_union;
    if (const Attribute* layout_attr = findAttr(base_decl.attributes, "layout")) {
        if (const AttributeArg* scheme = attrKey(*layout_attr, "scheme")) {
            if (scheme->text != kCurrentLayoutScheme) {
                error("E0705",
                      scheme->span,
                      std::format("unknown layout scheme `{}`", scheme->text),
                      "known schemes: v1");
            }
            out.scheme = scheme->text;
        }
        if (attrHasIdent(*layout_attr, "shuffle")) {
            out.shuffled = true;
            const AttributeArg* seed = attrKey(*layout_attr, "seed");
            out.seed = seed != nullptr ? static_cast<std::uint64_t>(seed->number)
                                       : std::hash<std::string>{}(name);
        }
    }
    std::uint64_t min_align = 1;
    if (const Attribute* a = findAttr(base_decl.attributes, "align")) {
        if (!a->args.empty() && a->args[0].kind == AttributeArg::Kind::Int &&
            a->args[0].number > 0 && (a->args[0].number & (a->args[0].number - 1)) == 0) {
            min_align = static_cast<std::uint64_t>(a->args[0].number);
        } else {
            error(
                "E0705", a->span, "`@align(N)` needs a power-of-two integer", "invalid alignment");
        }
    }

    // Declared types without `@repr(c)`: recorded layout -> project history -> C assumption.
    if (item.modifiers.declared && !out.repr_c) {
        if (history_ != nullptr) {
            if (const LayoutRecord* rec = history_->find(qualify(name))) {
                TypeLayout recorded = rec->layout;
                recorded.source = "record";
                const std::string current =
                    config_.layout_scheme.empty() ? kCurrentLayoutScheme : config_.layout_scheme;
                if (recorded.scheme != current) {
                    diag_.warning("W0203",
                                  base_decl.name_span,
                                  std::format("layout record of `{}` uses scheme {}, current is {}",
                                              name,
                                              recorded.scheme,
                                              current),
                                  "record scheme differs");
                }
                std::vector<std::string> declared_names;
                for (const FieldDecl& f : *fields) {
                    declared_names.push_back(f.name);
                }
                std::vector<std::string> recorded_names;
                for (const FieldLayout& f : recorded.fields) {
                    recorded_names.push_back(f.name);
                }
                std::sort(declared_names.begin(), declared_names.end());
                std::sort(recorded_names.begin(), recorded_names.end());
                if (!fields->empty() && declared_names != recorded_names) {
                    diag_.warning(
                        "W0202",
                        base_decl.name_span,
                        std::format("declared fields of `{}` differ from its recorded layout",
                                    name),
                        "fields do not match the record");
                }
                // Use the record's offsets, keep types from the declaration where names match.
                for (FieldLayout& rf : recorded.fields) {
                    for (const FieldDecl& f : *fields) {
                        if (f.name == rf.name) {
                            rf.type = f.type;
                        }
                    }
                }
                return recorded;
            }
        }
        Diagnostic d;
        d.code = "W0201";
        d.severity = Severity::Warning;
        d.message = std::format("layout of `{}` is unknown, C layout assumed", name);
        d.labels.push_back({base_decl.name_span, "this type comes from outside"});
        d.detail = std::format("No layout record for `{}` in `xl.layout`.", name);
        d.reason = "The alignment and padding of the other side cannot be verified.";
        d.fix = "add `@repr(c)` if the other side is C/C++, build it with xl so a record exists, "
                "or silence with `@allow(W0201)`";
        diag_.report(std::move(d));
        out.repr_c = true;
        out.source = "assumed-c";
    }

    const std::uint64_t ptr = config_.target.pointer_width / 8;
    std::uint64_t cursor = 0;
    std::uint64_t align = 1;

    if (cls != nullptr) {
        out.has_vptr =
            std::ranges::any_of(cls->methods, [](const FnDecl& m) { return m.is_virtual; });
        if (cls->has_base) {
            Type base_type = Type::makeNamed(cls->base);
            base_type.span = base_decl.name_span;
            const auto base_resolved = resolve(base_type);
            if (!base_resolved) {
                return std::nullopt;
            }
            if (base_resolved->kind != TypeKind::Class) {
                error("E0706",
                      base_decl.name_span,
                      std::format("base `{}` is not a class", joinDotted(cls->base)),
                      "single inheritance needs a class base");
                return std::nullopt;
            }
            auto base = layoutOf(*base_resolved, at);
            if (!base) {
                return std::nullopt;
            }
            for (FieldLayout f : base->fields) {
                f.inherited = true;
                out.fields.push_back(std::move(f));
            }
            out.has_vptr = out.has_vptr || base->has_vptr;
            // Itanium: a polymorphic (non-POD) base lets derived members reuse its tail padding.
            cursor = (out.repr_c && base->has_vptr) ? base->data_size : base->size;
            align = base->align;
        } else if (out.has_vptr) {
            cursor = ptr;
            align = ptr;
        }
        if (out.has_vptr && cursor < ptr) {
            cursor = ptr;
        }
    }

    struct Pending {
        const FieldDecl* decl;
        std::uint64_t size;
        std::uint64_t align;
        std::size_t index;
    };
    std::vector<Pending> pending;
    for (std::size_t i = 0; i < fields->size(); ++i) {
        const FieldDecl& f = (*fields)[i];
        auto resolved = resolve(f.type);
        auto fl = resolved ? layoutOf(*resolved, f.span) : std::nullopt;
        if (!fl) {
            return std::nullopt;
        }
        pending.push_back({&f, fl->size, out.packed ? 1 : fl->align, i});
    }

    std::vector<std::size_t> order(pending.size());
    std::iota(order.begin(), order.end(), 0);
    if (!out.repr_c && !out.packed) {
        // Scheme v1: stable sort by alignment, largest first. Frozen once released.
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return pending[a].align > pending[b].align;
        });
        if (out.shuffled) {
            std::mt19937_64 rng(out.seed);
            std::shuffle(order.begin(), order.end(), rng);
        }
    }

    std::vector<FieldLayout> placed(pending.size());
    for (const std::size_t idx : order) {
        const Pending& p = pending[idx];
        const std::uint64_t off = is_union ? 0 : alignUp(cursor, p.align);
        FieldLayout fl;
        fl.name = p.decl->name;
        fl.type = p.decl->type;
        fl.offset = off;
        fl.size = p.size;
        fl.align = p.align;
        placed[idx] = std::move(fl);
        cursor = is_union ? std::max(cursor, p.size) : off + p.size;
        align = std::max(align, p.align);
    }
    for (FieldLayout& f : placed) {
        out.fields.push_back(std::move(f));
    }

    out.data_size = cursor;
    align = std::max(align, min_align);
    if (cls != nullptr && !cls->has_base && !out.has_vptr && fields->empty() && out.repr_c) {
        cursor = std::max<std::uint64_t>(cursor, 1); // C++: an empty class is one byte
    }
    out.align = align;
    out.size = alignUp(cursor, align);
    if (out.source == "computed" && item.modifiers.declared) {
        out.source = out.repr_c ? "computed" : out.source;
    }
    return out;
}

} // namespace xlang
