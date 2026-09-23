# Toolchain fix: `readAnnotate()` never matched (annotation-driven passes were inert)

**Status**: applied on this machine and verified.
**Target**: `E:\llvm-msvc-ex-2026-7-23` — the OLLVM/xVMP-forked LLVM used for `Release|x64`.
**File**: `llvm/lib/Obfuscation/Utils.cpp`, function `readAnnotate()`.

> **Why this file exists.** The toolchain tree is **not a git checkout** (`llvm-msvc-ex-2026-7-23/.git`
> does not exist), so the change below has no version history of its own. If that build tree is
> recreated, reimaged, or shared with another machine, the fix is lost — and a compiler without it
> fails *silently*, only misbehaving once someone relies on an annotation. This document is the
> portable record. `script/verify_obfuscation.ps1` (step 5) fails the build check if the toolchain is
> rebuilt without it.

---

## Symptom

Every annotation-driven obfuscation pass silently did nothing:

- `__attribute__((annotate("nofla")))` had no effect — once `-mllvm -fla` was on, **every** eligible
  function was flattened (the `-perFLA` probability logic is commented out in `Flattening.cpp`, so
  there was no other way to opt out).
- All annotation-only passes were inert: `alias-access`, `custom-cc`, `ind-br`, `ind-gv`, `x-vm`,
  `x-cfg`, `x-fla-enh`, `x-var-rot` (`IngvObfuscationPass`, `AliasAccess`, `CustomCC`,
  `IndirectBranch`, `xvmPass`, ...).

Because the failure mode is "pass quietly does nothing", nothing in the build ever reported it.

## Root cause

`readAnnotate()` walked `llvm.global.annotations` like this:

```cpp
if (ConstantExpr *expr = dyn_cast<ConstantExpr>(structAn->getOperand(0))) {
  if (expr->getOpcode() == Instruction::BitCast && expr->getOperand(0) == f) {
    ConstantExpr *note = cast<ConstantExpr>(structAn->getOperand(1));
    ...
```

That shape predates opaque pointers. This fork mandates opaque pointers, and its clang emits the
annotated global **directly** as operand 0 (`GlobalValue`), not wrapped in a `ConstantExpr`
`bitcast` — so `dyn_cast<ConstantExpr>` returns null for every entry, the loop body never runs, and
`readAnnotate()` returns an empty string. `toObfuscate()` then can never see `fla` / `nofla` / any
other attribute.

Same tree, same bug class, already-solved elsewhere: `SmallVmpPass.cpp` parses the same global with
`stripPointerCasts()` and works.

## The fix

Replace the `ConstantExpr`-based walk with a `stripPointerCasts()`-based one. This accepts **both**
the new form and the legacy `ConstantExpr` form. Full replacement of `readAnnotate()`:

```cpp
std::string readAnnotate(Function *f) {
  std::string annotation = "";

  // Get annotation variable
  GlobalVariable *glob =
      f->getParent()->getGlobalVariable("llvm.global.annotations");

  if (glob != NULL) {
    // Get the array
    if (ConstantArray *ca = dyn_cast<ConstantArray>(glob->getInitializer())) {
      for (unsigned i = 0, e = ca->getNumOperands(); i != e; ++i) {
        // Get the struct
        if (ConstantStruct *structAn =
                dyn_cast<ConstantStruct>(ca->getOperand(i))) {
          //
          // llvm.global.annotations entry layout:
          //   { ptr <annotated global>, ptr <annotation string>, ... }
          //
          // With opaque pointers (mandatory in this fork) the first operand is a
          // plain GlobalValue, NOT a ConstantExpr bitcast, so the original
          // `dyn_cast<ConstantExpr>(operand(0))` always failed and every
          // annotation-driven pass silently did nothing. Local forks fix this
          // with stripPointerCasts(); do the same, which still accepts the older
          // ConstantExpr form.
          //
          if (structAn->getNumOperands() < 2)
            continue;

          const Value *Annotated = structAn->getOperand(0)->stripPointerCasts();
          if (Annotated != f)
            continue;

          // Operand 1 points at the annotation string global.
          const Value *Note = structAn->getOperand(1)->stripPointerCasts();
          if (const GlobalVariable *AnnoteStr =
                  dyn_cast<GlobalVariable>(Note)) {
            if (const ConstantDataSequential *Data =
                    dyn_cast<ConstantDataSequential>(AnnoteStr->getInitializer())) {
              if (Data->isString()) {
                StringRef Str = Data->getAsString();
                // ConstantDataSequential::getAsString() keeps the trailing NUL;
                // drop it so find("nofla") cannot match across "nofla\0".
                if (!Str.empty() && Str.back() == '\0')
                  Str = Str.drop_back();
                if (!Str.empty())
                  annotation += Str.lower() + " ";
              }
            }
          }
        }
      }
    }
  }
  if (!f->getAnnotationStrings().empty()) {
    std::string extra = std::string(f->getAnnotationStrings().data());
    // Avoid duplicating what the llvm.global.annotations walk already found.
    if (annotation.find(extra) == std::string::npos)
      annotation += extra;
  }
  return annotation;
}
```

Three deliberate details beyond the core `stripPointerCasts()` change:

1. **Trailing NUL is dropped.** `getAsString()` includes the terminating `\0`; the original code kept
   it. All attributes are matched with substring `find()`, so `"nofla\0"` still matches `"nofla"` —
   but keeping the NUL makes the string fragile for any exact comparison added later.
2. **`getAnnotationStrings()` de-duplicated.** Otherwise an annotation found by both mechanisms is
   appended twice, which would corrupt `xVMProtect.cpp`'s positional `ann.erase(...)` calls.
3. **`getNumOperands() < 2` guard** before touching operand 1.

Nothing else in `Utils.cpp` was changed, and no other file was modified. (`Flattening.cpp` was
temporarily instrumented during diagnosis and has been reverted to its original content.)

## Rebuild (required after applying)

```bat
msbuild <toolchain-build>\tools\clang\tools\driver\clang.vcxproj ^
        /p:Configuration=Release /p:Platform=x64 /m
```

This recompiles `Obfuscation.lib` and relinks `clang.exe`, which refreshes `clang-cl.exe`
automatically. Incremental, a few minutes — no full LLVM build needed.

## Verification (performed)

Compile the same function with and without the annotation, both with `-mllvm -fla`:

| Build | Object size |
|---|---|
| `annotate("nofla")` + `-fla` | **696 B** |
| no annotation + `-fla` | **1205 B** |
| no annotation, no `-fla` | **695 B** |

`696 ≈ 695` proves the annotation suppressed flattening entirely; `1205` proves the unannotated
function was flattened (+73%). With temporary pass-level instrumentation (since removed):

```
keep_simple  ann='nofla ' -> NO  (matched nofla)
flatten_me   ann='fla '   -> YES (matched fla)
[cdp-fla] flatten_me blocksBefore=10 blocksAfter=14 flattened=1
```

## ⚠ Consequence to be aware of

Fixing this **activates every annotation-only pass**. As long as the driver source contains no
`annotate(...)` / `#pragma optimize`, the compiled output is unchanged (verified: driver artifacts
before and after the toolchain fix were byte-for-byte identical in size and section layout). But if
annotations are ever added, note that `CustomCC` rewrites calling conventions to `Obfu1`–`Obfu8` —
which is not safe for a kernel driver. Grep before adding annotations:

```bat
findstr /s /c:"annotate(" *.c *.h
```
