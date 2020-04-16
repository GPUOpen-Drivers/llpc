# LLPC coding standard

## Introduction

This file documents the coding standard used by the LLPC project, that is:
* LLPC itself
* LGC
* The VKGC utilities and definitions that have been separated out of LLPC.

LLPC coding standard is generally based on
[LLVM coding standard](https://llvm.org/docs/CodingStandards.html),
with some specific changes. One notable change is that LLPC follows
[MLIR](https://github.com/tensorflow/mlir/blob/master/g3doc/DeveloperGuide.md)
and LLD in using camelBack for variable and field names; this change is
expected to be made in LLVM per
[this proposal](https://llvm.org/docs/Proposals/VariableNames.html).

## `using namespace`

Do not use `using namespace` in a header file, even inside a namespace.

Do not use `using namespace std` anywhere.

See the
[LLVM coding standard](https://llvm.org/docs/CodingStandards.html#do-not-use-using-namespace-std)
for more details.

## Integer types

Usually you want an integer to be at least 32 bits. In that case use `int` or `unsigned`.
Only use `int32_t` or `uint32_t` if you need an integer of _exactly_ 32 bits, for example
in an externally-defined memory layout.

## Identifier naming

Names should be in CamelCase (initial capital) or camelBack (initial lower-case), depending on
the category of identifier:

* A *type name* (including classes, structs, enums, typedefs, etc) should be a noun phrase
  in CamelCase (e.g. `Boat` or `TextFileReader`).

* A *variable name* should be a noun phrase in camelBack (unlike current LLVM style).

  Much LLVM code uses extreme abbreviation in variable names (e.g. `M` for a module); the LLVM coding
  standard advises against this, and in LLPC we can avoid it by using the same name as the type but
  in camelBack (e.g. `module`) or a somewhat abbreviated form (e.g. `inputMod`).

  Do not use a type prefix on a variable name (e.g. p for pointer) (unlike PAL style).

  A global or static variable name should start with an upper-case letter.

* A *public member variable* (usually a struct member) should be named like a variable.

* A *non-public member variable* should be named like a variable, but
  with an extra `m_` prefix (unlike LLVM style, but like PAL style, and like LLDB and WebKit).

* A *function name* or *method name* should be an imperative verb phrase in camelBack
  (like LLVM style, but unlike PAL style).

  There are some exceptions to the camelBack rule for historical reasons:

  - Following subclasses of llvm::Instruction, a static method that creates an instance of the class is
    called `Create` with an upper-case C.

  - Builder and its subclasses follow llvm::IRBuilder in that any method whose name starts with Create
    has an upper-case C. IRBuilder also uses an initial upper-case letter for a method that sets
    or gets its internal state, but not a method that gets a type or constant.

  Further exceptions outlined in the LLVM coding standard are where a class has a method that
  mimics an STL class action, such as `push_back`. Such a method can keep the STL-like name.

* A *constant* (technically a static variable or class/struct member variable, but `final` so it is used
  like a constant) should be in CamelCase.

* An *enumerator* (e.g. `enum { Foo, Bar }`) acts like a constant, so should start with an
  upper-case letter. In addition, if the enum is not an enum class, and is not hidden inside a
  class that suitably qualifies it, then each enumerator should have
  a prefix corresponding to (not necessarily exactly the same as) the enum declaration name.

Unlike in LLVM, an initialism (e.g. "NGG") should be treated as a word in identifier-naming
rules. Thus, if it appears at the start of a variable or function name, it should be entirely
lower-case (`nggControl`); if it appears at the start of a type or constant name, or anywhere
other than the start in any name, it should have an initial upper-case letter (`NggControl`).

For API compatibility, vkgcDefs.h, llpc.h and vfx.h have not been modified from PAL-style naming,
which means they still
contain struct members with a p prefix for pointer, and CamelCase for methods. Any method
in .cpp files that implements or overrides a method in a class declared there, such as an
ICompiler method, also keeps CamelCase rather than camelBack. Similarly the ElfReader/ElfWriter
methods `ReadFromBuffer`, `GetSectionIndex`, `GetSymbolsBySectionIndex` and `GetSectionData`
remain in PAL-style CamelCase. In addition, ElfReader's `ElfSymbol::pSymName` retains its
PAL-style p prefix as it is directly referenced outside of LLPC/VKGC.

## Local variable initialization

A POD type local variable declaration should always be initialized, even if the initialized
value is always overwritten in later code (for example in both legs of conditional code).
A local variable of aggregate type can be initialized with `= {}` to zero all fields.

A non-POD type with a sensible default constructor could also be initialized to make it
obvious that you are running the default constructor, perhaps by appending `{}` to the
variable name. This is considered unnecessary for well-known types with a sensible default
constructor, such as the standard library and LLVM container types.

## Braces

Brace placement is covered by clang-format below.

C++ allows the body of an if, else, while, do, for to be a single simple statement,
without any braces. We follow the LLVM convention of only using an unbraced body if
it is a single line, including comments. Otherwise the body should be braced. For
an "if..else", if one of the two bodies is braced, the other one should be too. For
an "if..else if..else if..else", if any body is braced, the others should be too.

## Redundant comparisons

Comparison of a bool with `false` or a pointer with `nullptr` is redundant and should
be avoided. Use `!` instead where appropriate.

## Redundant parentheses

Certain cases of theoretically redundant parentheses around a subexpression are needed
because compilers warn about them otherwise, for the good reason that C/C++ operator
precedence makes it difficult to understand and easy to get wrong (in particular
`&` `|` `^` vs comparison operators, and `&&` vs `||`). However redundant parentheses
are not needed in cases that compilers do not warn about, in particular a comparison
inside `&&` or `||`, or a comparison used as the condition of `?:` or in an assignment
or `return`.

## Comments

Despite the look of much LLVM code, the LLVM coding standard does actually recommend
placing a comment at the top of a function/method. :-)

In LLPC, it is mandatory to add a comment at the top of each class declaration, function
definition and out-of-line method definition. The comment should start with a separator
line `// ======` (117 equals signs for a total width of 120).

Parameters should be commented in the main comment at
the top of the function like this:

```
// \param hatstand : [in/out] The Hatstand object to modify
```

A tag of `[in]` is unnecessary, and is assumed for a reference or pointer parameter
where no tag is given.

Parameter comments should not be placed on the parameter line itself and aligned
(as happens in PAL style), to avoid spuriously large diffs when a change or addition
of one parameter means all the others need realigning.

Similarly, a bunch of fields or variables should not have aligned inline comments.
Instead, put each field's comment on the line above the field.

## clang-format

The LLPC coding standard uses a clang-format configuration based on the LLVM one,
with the following changes:

- 120 column limit (rather than 80)
- No putting an out-of-line function/method definition onto a single line
- `#include` blocks are merged together (removing blank lines) before sorting
- `#include` directives are sorted in this order:
  - the `.h` file for the current `.cpp` file;
  - all other ones;
  - `lgc/*` files;
  - `llvm/*` files;
  - standard library.

You should use clang-format to format code modified or added in your change, but do not
reformat any other code. Reviewers should reject a change that has spurious reformatting
outside the lines affected by the substantive change, unless the whole point of the change
is purely reformatting.

You can mark a section of source that you do not want clang-format to touch by surrounding
it with

```
// clang-format off

// clang-format on
```

lines. Use this for something like a table that you have custom formatted for readability.
