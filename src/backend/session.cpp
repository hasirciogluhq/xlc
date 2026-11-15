// The session behind the backend's C API (src/backend/session.hpp): the configuration the CLI
// parsed becomes a BuildConfig, the compilation runs in the order the CLI asks for, and the
// lowered module is what the output step reads and prunes.
#include "backend/session.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace xlang::backend {

const char kEmptyBytes[1] = {'\0'};

namespace {

/// Whether the backend colors stderr.
bool g_color = false;

/// The ANSI style `code` (`1;31` bold red, `1;33` bold yellow), or nothing when color is off.
std::string style(std::string_view code) {
    return g_color ? std::format("\x1b[{}m", code) : std::string();
}

} // namespace

XlcStr viewOf(const std::string& s) {
    return XlcStr{s.empty() ? kEmptyBytes : s.data(), static_cast<uint32_t>(s.size())};
}

std::string_view textOf(XlcStr s) {
    return std::string_view(s.bytes == nullptr ? kEmptyBytes : s.bytes, s.len);
}

std::string stringOf(XlcStr s) {
    return std::string(textOf(s));
}

std::vector<std::string> listOf(XlcStrList list) {
    std::vector<std::string> out;
    out.reserve(list.count);
    for (std::uint32_t i = 0; i < list.count; ++i) {
        out.emplace_back(textOf(list.items[i]));
    }
    return out;
}

void setColor(bool on) {
    g_color = on;
}

void printError(std::string_view message) {
    std::cerr << std::format("{}error:{} {}\n", style("1;31"), style("0"), message);
}

void printSummary(const DiagnosticEngine& diagnostics) {
    std::cerr << std::format("{}{} error(s), {} warning(s){}\n",
                             style(diagnostics.hasErrors() ? "1;31" : "1;33"), diagnostics.errorCount(),
                             diagnostics.warningCount(), style("0"));
}

DiagnosticOptions diagnosticOptions(const XlcConfig& config) {
    DiagnosticOptions out;
    out.allow = listOf(config.allow);
    out.deny = listOf(config.deny);
    out.warn_all = config.warn_all != 0;
    out.allow_all = config.allow_all != 0;
    out.color = g_color;
    out.context_errors = static_cast<std::size_t>(config.context_errors);
    out.context_warnings = static_cast<std::size_t>(config.context_warnings);
    out.log_path = stringOf(config.log_path);
    out.format = textOf(config.diag_format) == "json" ? DiagFormat::Json : DiagFormat::Text;
    out.log_level = textOf(config.diag_log_level) == "error"   ? DiagLogLevel::Error
                    : textOf(config.diag_log_level) == "warn" ? DiagLogLevel::Warn
                                                              : DiagLogLevel::All;
    return out;
}

BuildConfig buildConfig(const XlcConfig& raw) {
    BuildConfig config;
    const std::string target = stringOf(raw.target);
    if (!target.empty()) {
        auto parsed = parseTargetTriple(target);
        if (!parsed) {
            throw XlangError(std::format("unsupported target `{}` (x86_64/aarch64 on "
                                         "linux/macos/windows)",
                                         target));
        }
        config.target = std::move(*parsed);
    }
    const std::string opt = stringOf(raw.opt);
    if (!opt.empty()) {
        config.opt_level = std::stoi(opt);
    }
    config.debug = raw.debug != 0;
    config.cpu = stringOf(raw.cpu);
    config.mangle = stringOf(raw.mangle);
    config.safety = stringOf(raw.safety);
    config.layout_scheme = stringOf(raw.layout_scheme);
    config.layout_history = stringOf(raw.layout_history);
    config.crt_link = raw.crt != 0;
    for (const std::string& def : listOf(raw.defines)) {
        const auto [name, value] = std::pair{def.substr(0, def.find('=')),
                                             def.find('=') == std::string::npos
                                                 ? ""
                                                 : def.substr(def.find('=') + 1)};
        if (name.empty()) {
            throw XlangError("empty name in -D");
        }
        config.defines[name] = ConfigValue::parse(value);
    }
    for (const std::string& name : listOf(raw.undefs)) {
        if (config.defines.erase(name) == 0 && config.lookup(name)) {
            throw XlangError(std::format(
                "`{}` is a builtin definition and cannot be undefined", name));
        }
    }
    return config;
}

Session::Session(const XlcConfig& raw) : config_(buildConfig(raw)) {
    diagnostics_.configure(diagnosticOptions(raw));
}

void Session::addModuleRoot(std::string name, fs::path dir) {
    module_roots_.emplace_back(std::move(name), std::move(dir));
}

void Session::addImplicitImport(std::string spec) {
    imports_.push_back(std::move(spec));
}

void Session::loadRoot(const fs::path& root) {
    // The layout history sits next to the root unless `--layout-history` named a file; the
    // compilation borrows it, so it lives in the session.
    const std::string history_path = config_.layout_history.empty()
                                         ? (root.parent_path() / "xl.layout").string()
                                         : config_.layout_history;
    history_ = std::make_unique<LayoutHistory>(LayoutHistory::load(history_path));
    history_path_ = history_path;
    compilation_ = std::make_unique<Compilation>(sources_, config_, diagnostics_, history_.get());
    for (auto& [name, dir] : module_roots_) {
        compilation_->addModuleRoot(name, dir);
    }
    for (const std::string& spec : imports_) {
        compilation_->addImplicitImport(spec);
    }
    compilation_->loadRoot(root);
    root_ = root;
}

void Session::analyze() {
    compilation_->finish();
    if (!diagnostics_.hasErrors()) {
        compilation_->analyzeAll();
    }
}

void Session::ensureLowered() {
    if (lowered_ || diagnostics_.hasErrors()) {
        return;
    }
    hir_module_ = compilation_->lowerAll();
    mir_module_ = mir::build(hir_module_);
    if (!diagnostics_.hasErrors()) {
        analysis::analyze(hir_module_, *mir_module_, diagnostics_);
    }
    lowered_ = true;
}

void Session::keep(const std::vector<std::uint32_t>& functions, const std::vector<std::uint32_t>& globals) {
    // One pass per list: mark the survivors, then move them down in order.
    const auto retain = [](auto& items, const std::vector<std::uint32_t>& live) {
        std::vector<bool> marked(items.size(), false);
        for (const std::uint32_t i : live) {
            marked[i] = true;
        }
        std::remove_reference_t<decltype(items)> out;
        out.reserve(live.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (marked[i]) {
                out.push_back(std::move(items[i]));
            }
        }
        items = std::move(out);
    };
    retain(hir_module_.functions, functions);
    retain(hir_module_.globals, globals);
}

const std::vector<XlcModule>& Session::modules() {
    if (modules_built_) {
        return modules_;
    }
    modules_built_ = true;
    const std::vector<ModuleInfo*>& order = compilation_->order();
    std::map<const ModuleInfo*, std::uint32_t> index;
    for (std::size_t i = 0; i < order.size(); ++i) {
        index[order[i]] = static_cast<std::uint32_t>(i);
    }
    // The dependency lists are complete before any view points into them.
    module_deps_.assign(order.size(), {});
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (const ModuleInfo* d : order[i]->deps) {
            if (const auto it = index.find(d); it != index.end()) {
                module_deps_[i].push_back(it->second);
            }
        }
    }
    modules_.clear();
    for (std::size_t i = 0; i < order.size(); ++i) {
        const ModuleInfo& m = *order[i];
        const bool registered = std::ranges::any_of(m.program.items, [](const Item& item) {
            return item.modifiers.registered;
        });
        const std::string_view text = m.file == kNoFile ? std::string_view() : std::string_view(sources_.file(m.file).text());
        XlcModule row{viewOf(m.key), XlcStr{text.empty() ? kEmptyBytes : text.data(), static_cast<uint32_t>(text.size())},
                      registered ? 1 : 0, XlcIndexList{nullptr, 0}};
        modules_.push_back(row);
    }
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        modules_[i].deps = XlcIndexList{module_deps_[i].data(), static_cast<uint32_t>(module_deps_[i].size())};
    }
    return modules_;
}

std::optional<std::uint64_t> Session::sizeOf(const std::string& name) {
    const auto layout = compilation_->rootSema()->layoutOfName(name, Span{});
    if (!layout) {
        return std::nullopt;
    }
    return layout->size;
}

const std::string& Session::configKey(const std::string& emit) {
    config_key_ = std::format("xlc-{}|{}|{}|{}|O{}|{}|{}|{}|{}|{}|{}", XLANG_VERSION, config_.target.triple,
                              config_.target.os, config_.target.arch, config_.opt_level, config_.debug ? "g" : "-",
                              config_.cpu, config_.mangle, config_.safety, config_.layout_scheme, emit);
    for (const auto& [k, v] : config_.defines) {
        config_key_ += "|D:" + k + "=" + v.toString();
    }
    return config_key_;
}

} // namespace xlang::backend
