# Cangjie compiler hard problems

Use this when the task is primarily in `cangjie_compiler/`, especially parser, sema, modules, incremental compilation, codegen, driver/frontend plumbing, or compiler-only test failures.

## Work in the submodule, not via the root wrapper
- For compiler changes, work inside `cangjie_compiler/` directly.
- The root Linux wrapper runs `python3 build.py build -t release --no-tests`, so it skips compiler UT binaries entirely.
- Root `build_scripts/linux/clone_thirdparty.sh` copies the pinned root `third_party/{llvm-project,third_party_flatbuffers}` trees into `cangjie_compiler/third_party/*`. If those directories are missing in the compiler tree, clone/copy them first or use the root wrapper once.

## Exact build commands
- Clean native compiler build: `python3 build.py clean`
- Native compiler build with tests: `python3 build.py build -t release`
- Faster native rebuild without UTs: `python3 build.py build -t release --no-tests`
- Debug build: `python3 build.py build -t debug`
- Install to `cangjie_compiler/output`: `python3 build.py install`
- Quick smoke check after install: `source output/envsetup.sh && cjc -v`

## High-value build gotchas
- `build.py` uses Ninja on Linux/macOS and writes the default native build to `cangjie_compiler/build/build`.
- `build.py test` always runs `ctest --output-on-failure` in `cangjie_compiler/build/build`. It does not target cross-build directories or sanitizer-specific `build-libs-*` directories.
- If `GTest` is missing, CMake only prints a warning and silently disables compiler unittests.
- For native builds, `build.py build` defaults `--product` to `all`. For cross-target builds, it defaults to `libs`, not `cjc`.
- `CMAKE_EXPORT_COMPILE_COMMANDS` is on, so `build/build/compile_commands.json` is the main compile database.

## Focused verification
- Full compiler UT pass: `python3 build.py test`
- Single/focused UT: run `ctest --output-on-failure -R <TestName>` in `cangjie_compiler/build/build`
- Useful test names from CMake: `DriverTest`, `CompilerInstanceTest`, `ParserTest`, `ParseIncrTest`, `TypeCheckerTest`, `GenericTest`, `PackageTest`, `ImportIncrTest`, `CHIROptTests`, `CodeGenTests`
- If you changed only option parsing / driver flow, start with `OptionTest` and `DriverTest`.
- If you changed parser / sema / modules, start with `ParserTest`, `ParseCommentTest`, `ParseIncrTest`, `TypeCheckerTest`, `GenericTest`, `PackageTest`, `ImportIncrTest`.

## Execution flow
- Process entrypoint: `src/main.cpp`
- Normal `cjc` path: `main.cpp` -> `Driver::ParseArgs` -> `Driver::ExecuteCompilation` -> `ExecuteFrontendByDriver` -> backend job execution
- Frontend-only path: invoking `cjc-frontend` switches `main.cpp` to `ExecuteFrontend(...)` in `src/FrontendTool/FrontendTool.cpp`
- Incremental vs full compilation is selected in `NeedCreateIncrementalCompilerInstance(...)`
- Default frontend pipeline implementation lives in `DefaultCompilerInstance`; incremental-specific overrides live in `IncrementalCompilerInstance`

## Where to look first by symptom
- CLI option parsing / env / backend invocation: `src/Driver/`, `include/cangjie/Driver/`
- Parse / frontend pipeline / dump actions: `src/FrontendTool/`, `include/cangjie/FrontendTool/`
- Parser bugs: `src/Parse/`, `unittests/Parse/`
- Sema/type issues: `src/Sema/`, `unittests/Sema/`
- Import/package/incremental dependency issues: `src/Modules/`, `src/IncrementalCompilation/`, `unittests/Modules/`, `unittests/Parse/ParseIncrTest.cpp`
- CHIR/codegen bugs: `src/CHIR/`, `src/CodeGen/`, `unittests/CHIR/`, `unittests/CodeGen/`

## Test architecture
- Many frontend-heavy UTs link against `cangjie-lsp` plus `TestCompilerInstanceObject`, not `cjc`.
- `TestCompilerInstance` sets `cangjieHome` to `build/build` and creates module directories under that build tree, so frontend/module tests depend on artifacts in the native build directory.
- Because of that coupling, when frontend-heavy UTs fail oddly, verify the native build tree is fresh before digging deeper.

## Cross-compilation notes
- Windows `cjc` build is explicit: `python3 build.py build -t release --product cjc --target windows-x86_64 ...` then `python3 build.py install --host windows-x86_64`
- `build.py install` without `--host` installs native build outputs plus any `build-libs-*` directories into `output/`
- `build.py install --host <triple>` installs cross-built compiler outputs into `output-<triple>/`

## Decision rules
- If the change is compiler-only, do not spend time rebuilding runtime/stdx/tools unless the failing path crosses those boundaries.
- Prefer the smallest UT subset that exercises the changed stage, then run broader `python3 build.py test` only after the focused pass is green.
- If the bug only reproduces through `cjc`, inspect `Driver` and `FrontendTool` boundaries before changing parser/sema internals.
