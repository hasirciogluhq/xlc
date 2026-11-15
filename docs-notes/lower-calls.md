# Lowering port, lane B: calls

Scope: call lowering of the xlang pass (`src/lower/call.xl`, `overload.xl`, `methods.xl`), gated by
`test/unit/xl_lower_test.cpp` (block "Lane B"). The C++ pass stays the production path, so no C++
call code is deleted by this lane.

## Ported and compared

- Function with one declaration: arguments converted to the declared parameter types; E0710 for a
  wrong argument count.
- Overloaded function: the key is the name with each parameter's mangled type after `$`
  (`pick$i32`, `pick$f64`), chosen by the C++ cost rules (literal fit, implicit conversion,
  argument cost); E0404 when none fits, E0403 when several tie.
- Method of a class found in the class or its base classes: the receiver is passed by address
  unless it is already a pointer (E0709 for a receiver that is not a place); the key is
  `Class.method`, or with the mangled parameter types when the class has several methods of that
  name.
- The type of `self` in a method is a pointer to the class, so field access through `self` is
  `field.ptr`.
- Calls through a local or global function value, and through a function-pointer field.
- Call result types (`callType` in typing.xl) follow the same choice, so the type of a call to an
  overloaded function or to a method is known to the enclosing expression.

Gate: 4 new samples (overload by float, overload by integer width, method through a value, method
through a pointer). The gate runs 62 samples in total and all are equal.

## Not ported (still C++ only, or reported)

- Virtual methods: vtable slots are not built by the xl pass; a virtual method call is reported
  with E0709.
- Generic functions and methods: instances are not named or lowered; a generic method call is
  reported with E0709.
- Interface methods, constructors (`T(args)`), sum-variant calls, `operator()` on a class value,
  packs, and the `__builtin_` forms.
- Methods of a base class declared in another module (the lookup is in the module only).
- Class-to-interface argument conversion in overload costs (treated as not convertible).
- Array-of-byte parameters with a string literal argument (their length is not known to the cost).
- Overloaded class methods: a class with two methods of one name makes the xl pass panic while the
  declarations are lowered (`Vec: index 2 out of range`); the cause is not yet found, so no sample
  covers the method overload path.
- A class held by value in a struct, or a class with a base class: the xl layout reports
  E0703 "contains itself by value" for it, while the C++ pass accepts it. No sample covers it.
- Drop calls on call temporaries: a method on a temporary with drop glue is not materialized.
