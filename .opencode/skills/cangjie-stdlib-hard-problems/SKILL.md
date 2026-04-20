# Cangjie stdlib hard problems

Use this when the task is primarily in `cangjie_runtime/stdlib/`, especially native FFI glue, AST/flatbuffers generation, sanitizer/coverage builds, or stdlib build failures caused by missing compiler/runtime artifacts.

## Work in the submodule, not via the root wrapper
- For stdlib changes, work inside `cangjie_runtime/stdlib/` directly.
- The root wrapper builds stdlib only after sourcing compiler env from `cangjie_compiler/output/envsetup.sh` and passes `--target-lib=$WORKSPACE/cangjie_runtime/runtime/output`, so standalone stdlib builds need equivalent inputs.

## Exact build commands
- Clean stdlib build: `python3 build.py clean`
- Native release build: `python3 build.py build -t release`
- Native debug build: `python3 build.py build -t debug`
- Common root-wrapper-equivalent build in this repo: `python3 build.py build -t release --target-lib=/absolute/path/to/cangjie_runtime/runtime/output`
- Install to `stdlib/output`: `python3 build.py install`

## High-value build gotchas
- `stdlib/build.py` uses Ninja on Linux/macOS and writes the default native build to `cangjie_runtime/stdlib/build/build`.
- Stdlib expects `CANGJIE_HOME` to point at a built compiler/SDK; `stdlib/CMakeLists.txt` includes `$ENV{CANGJIE_HOME}/include`, and several native build steps read headers and libraries from there.
- Runtime libraries are resolved from `--target-lib` first by probing for `common/<platform>_<mode>_<arch>/lib/<target>/*cangjie-aio*`; otherwise stdlib falls back to `$CANGJIE_HOME/lib/<target>`.
- `build.py install` follows the compiler-style multi-build logic and will install every `build-libs-*` directory it finds, not just the most recent one.
- `CMAKE_EXPORT_COMPILE_COMMANDS` is on, so `build/build/compile_commands.json` is the main compile database.

## Critical coupling points
- Build compiler first enough to provide `CANGJIE_HOME/include` and compiler-side libs.
- Build runtime first enough to provide `runtime/output/common/...`, especially `libcangjie-aio.a` and runtime libs consumed by stdlib.
- The `std/ast` native build copies and mutates `$CANGJIE_HOME/lib/<triple>/libcangjie-ast-support.a`; if compiler output is stale or missing, AST-related stdlib work will fail.
- `std/core/native` builds `typetemplate.c` using `$CANGJIE_HOME/include/cangjie/Basic/UGTypeKind.h`, then rewrites symbols with `objcopy`.

## Verification
- No standalone stdlib test command is exposed by `stdlib/build.py`, and I did not find a repo-local stdlib `ctest`/GTest harness here.
- Primary verification is build/install success plus checking artifacts under `stdlib/output/`.
- In the root repo, the wrapper copies `stdlib/output/` into `cangjie_compiler/output/`, so integrated verification is usually rebuilding stdlib and then using the assembled compiler env.

## Stdlib structure
- Top-level build entry: `stdlib/CMakeLists.txt`
- Main package tree: `stdlib/libs/std/`
- Package registration is centralized in `stdlib/libs/std/CMakeLists.txt`
- Native FFI glue for packages lives under `stdlib/libs/std/*/native/`
- Shared/static stdlib assembly logic lives in `stdlib/libs/cmake/`
- Schema/flatbuffers generation for `std.ast` comes from `stdlib/schema/` and `stdlib/libs/std/ast/native/CMakeLists.txt`

## Where to look first by symptom
- Package source/layout issues: `stdlib/libs/std/<package>/`
- Native FFI or linker failures: `stdlib/libs/std/<package>/native/` and `stdlib/libs/CMakeLists.txt`
- AST package / flatbuffers / ast-support issues: `stdlib/libs/std/ast/native/`, `stdlib/schema/`
- Core primitive/type-template issues: `stdlib/libs/std/core/native/`
- Runtime coupling or missing aio/runtime libs: `stdlib/CMakeLists.txt` and `stdlib/libs/CMakeLists.txt`
- Sanitizer / coverage build behavior: `stdlib/CMakeLists.txt` plus `build.py` flags `--stdlib-coverage`, `--asancov`, `--cjlib-sanitizer-support`

## Platform and sanitizer gotchas
- Cross-builds use compiler-style toolchain files under `stdlib/cmake/` and may need `--target-toolchain`, `--target-sysroot`, `--target-lib`, and `--include` together.
- `--stdlib-coverage` and `--asancov` change how native pieces are compiled and linked; `std/core/native` explicitly strips sanitize-coverage flags from some objects.
- The shared `stdFFI` library links against runtime pieces such as `libcangjie-aio.a` and `cangjie-runtime`, so missing runtime output usually surfaces as stdlib linker failures rather than an earlier configure error.

## Decision rules
- If a stdlib failure mentions missing compiler headers or `libcangjie-ast-support.a`, check `CANGJIE_HOME` first.
- If a stdlib failure mentions `cangjie-aio`, `cangjie-runtime`, or runtime lib directories, check `--target-lib` and the runtime build output tree first.
- For package-local changes, rebuild stdlib before escalating to root SDK assembly; most stdlib mistakes can be confirmed from `stdlib/build/build` and `stdlib/output/` alone.
