#include "mir/builder/body.hpp"

#include <utility>

namespace xlang::mir::builder {

namespace {

/// What a HIR local is, from how lowering made it: parameters first, then the hidden `$...`
/// locals (drop flags are `$live...`), then the user's own.
[[nodiscard]] Local::Kind kindOf(const hir::Local& l) {
    if (l.is_param) {
        return Local::Kind::Param;
    }
    if (l.name.starts_with("$live")) {
        return Local::Kind::DropFlag;
    }
    return l.name.starts_with('$') ? Local::Kind::Temp : Local::Kind::User;
}

} // namespace

BodyBuilder::BodyBuilder(const hir::Function& f) : fn_(f) {
    body_.name = f.name;
    body_.param_count = f.param_count;
    body_.ret = f.ret;
    body_.span = f.span;
    declareLocals();
    enter(newBlock());
    lowerBody();
}

Body BodyBuilder::finish() && {
    return std::move(body_);
}

void BodyBuilder::declareLocals() {
    for (const hir::Local& l : fn_.locals) {
        body_.locals.push_back({kindOf(l), l.name, l.type, l.span});
    }
}

/// The statements, then the implicit end: a `void` function returns, any other end is unreachable
/// (sema rejects a value function that can fall off its end).
void BodyBuilder::lowerBody() {
    lowerStmts(fn_.body);
    if (!open_) {
        return;
    }
    Terminator end{.kind = Terminator::Kind::Unreachable, .span = fn_.span};
    if (fn_.ret.kind == TypeKind::Void) {
        end.kind = Terminator::Kind::Return;
        end.operand = {.kind = Operand::Kind::Constant,
                       .constant = {.kind = Constant::Kind::None, .type = fn_.ret}};
    }
    terminate(std::move(end));
}

LocalId BodyBuilder::newTemp(const Type& type, const Span& at) {
    body_.locals.push_back({Local::Kind::Temp, {}, type, at});
    return static_cast<LocalId>(body_.locals.size() - 1);
}

BlockId BodyBuilder::newBlock() {
    body_.blocks.emplace_back();
    return static_cast<BlockId>(body_.blocks.size() - 1);
}

void BodyBuilder::enter(BlockId block) {
    current_ = block;
    open_ = true;
}

/// The block statements go to. After a terminator (`return`, `break`, a call that never returns)
/// the code that follows is dead: it gets a fresh block no edge reaches.
BasicBlock& BodyBuilder::cursor() {
    if (!open_) {
        enter(newBlock());
    }
    return body_.blocks[current_];
}

void BodyBuilder::emit(Place dest, Rvalue value, const Span& at) {
    cursor().statements.push_back({std::move(dest), std::move(value), at});
}

void BodyBuilder::terminate(Terminator t) {
    cursor().terminator = std::move(t);
    open_ = false;
}

/// Ends the current block with a jump to `target`, unless it already ended (a `return` in it).
void BodyBuilder::fallTo(BlockId target, const Span& at) {
    if (open_) {
        terminate({.kind = Terminator::Kind::Goto, .targets = {target}, .span = at});
    }
}

std::optional<BlockId> BodyBuilder::unwindTarget() const {
    if (catches_.empty()) {
        return std::nullopt;
    }
    return catches_.back();
}

} // namespace xlang::mir::builder
