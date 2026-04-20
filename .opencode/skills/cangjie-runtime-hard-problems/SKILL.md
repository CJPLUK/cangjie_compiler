# Cangjie runtime hard problems

Use this when the task is primarily in `cangjie_runtime/runtime/`, especially GC/heap, loader, cjthread/concurrency, sanitizer support, platform bring-up, or runtime-only build failures.

## Work in the submodule, not via the root wrapper
- For runtime changes, work inside `cangjie_runtime/runtime/` directly.
- The root wrapper calls `python3 build.py build -t $TARGET -v ${CANGJIE_VERSION}` and then rsyncs runtime outputs into `cangjie_compiler/output`, so wrapper builds hide some standalone runtime details.

## Exact build commands
- Clean runtime build: `python3 build.py clean`
- Native release build: `python3 build.py build --target native -t release -v 0.0.1`
- Native debug build: `python3 build.py build --target native -t debug -v 0.0.1`
- Install to the default output tree: `python3 build.py install`
- Cross-builds require `--target-toolchain`, and some also need `--target-sysroot`

## High-value build gotchas
- `build.py` always configures into `cangjie_runtime/runtime/CMakebuild` and then builds with `make cangjie-runtime` plus `make preinstall`; it does not use Ninja.
- `build.py build` deletes both `output/temp` and `CMakebuild` before every build, even if you did not run `clean`.
- Default install prefix for builds is `runtime/output/common`, with per-target subdirectories like `<platform>_<mode>_<arch>`.
- The root repo wrapper later copies from `runtime/output/common/linux_${TARGET}_${ARCH}` into `cangjie_compiler/output`.
- `build.py clean` also removes `build/cjthread_build`, not just `output` and `CMakebuild`.

## Verification
- Quick standalone verification in this repo is mostly artifact-based: build, install, then confirm the expected runtime libraries land under `runtime/output/common/<platform>_<mode>_<arch>/runtime/` and `.../lib/`.
- No standalone runtime test command is exposed by `runtime/build.py`, and I did not find a repo-local `ctest`/GTest test harness for runtime here.
- For integrated verification from the root repo, the relevant smoke path is rebuilding runtime and then using compiler output via `source cangjie_compiler/output/envsetup.sh && cjc -v`.

## Runtime structure
- Top-level runtime build entry: `runtime/CMakeLists.txt`
- Main implementation tree: `runtime/src/`
- Core subsystems under `runtime/src/`: `Base`, `Common`, `Concurrency`, `Loader`, `Heap`, `Exception`, `ObjectModel`, `UnwindStack`, `Sync`, `CpuProfiler`, `Inspector`
- Platform/arch assembly and shims are selected in `runtime/src/CMakeLists.txt` by `CMAKE_SYSTEM_PROCESSOR` and platform flags
- `cjthread` is built via nested shell/batch scripts from `runtime/build/build_cjthread.sh` or `build_cjthread_windows.bat`, invoked by CMake rather than as normal CMake targets

## Where to look first by symptom
- GC / allocator / barriers / collectors: `runtime/src/Heap/`
- Loader / binary format / semantic version loading: `runtime/src/Loader/`
- Threading / scheduler / aio / cjthread integration: `runtime/src/Concurrency/` and `runtime/src/CJThread/`
- Stack unwinding / exception / crash handling: `runtime/src/UnwindStack/`, `runtime/src/Exception/`, `runtime/src/Signal/`
- Sanitizer-specific issues: `runtime/src/Sanitizer/` plus sanitizer flags in `runtime/CMakeLists.txt`
- Platform bring-up or arch-specific crashes: `runtime/src/CMakeLists.txt` and `runtime/src/arch/*`, `runtime/src/os/*`

## Platform and sanitizer gotchas
- Windows target forbids sanitizer support in `build.py`.
- OHOS and Android cross-builds require explicit toolchain roots; OHOS uses `OHOS_ROOT`, Android uses `ANDROID_ROOT`, both set from `--target-toolchain`.
- iOS builds also expect `--target-sysroot` and update `PATH`/`SDKROOT` in `build.py`.
- Sanitizer support affects install subpaths like `/asan`, `/tsan`, `/hwasan` and changes cjthread build parameters.

## Decision rules
- If the change is runtime-only, prefer standalone runtime builds first; do not rebuild the full root SDK until the runtime artifact shape is correct.
- If the failure only appears after root assembly, compare standalone runtime output under `runtime/output/common/...` with the files copied into `cangjie_compiler/output` by the root wrapper.
- For arch/platform bugs, inspect `runtime/src/CMakeLists.txt` first because it chooses different stub files and platform sources per target.
