# Cangjie SDK assembly repo

## What this repo is
- Root repo assembles an SDK from pinned submodules, not a single buildable codebase. Most real code changes belong in `cangjie_compiler`, `cangjie_runtime`, `cangjie_stdx`, or `cangjie_tools`.
- Root `third_party/` holds pinned clones for LLVM and FlatBuffers. `build_scripts/linux/clone_thirdparty.sh` copies them into `cangjie_compiler/third_party/*` during compiler builds.

## Build entrypoints
- Real root entrypoint on Linux: `bash build_scripts/linux/all.sh`.
- Component wrappers: `bash build_scripts/linux/{compiler,runtime,stdlib,stdx,cjpm,cjfmt,hyperlang,lsp}.sh`.
- macOS has parallel wrappers under `build_scripts/macos/`.
- Use long timeouts for builds; the repo’s own guidance is about 90 minutes.

## Build wiring and gotchas
- Build order matters: wrappers expect compiler install artifacts first. `build_scripts/linux/envsetup.sh` sources `cangjie_compiler/output/envsetup.sh` and then exports `CANGJIE_STDX_PATH`.
- Linux wrappers require `ARCH` to already be set (`x86_64` or `aarch64`). `compose.yaml` sets it; the scripts themselves do not.
- `--jobs N` is supported by `build_scripts/linux/*.sh` via `init_env.sh`.
- `--debug` only changes the runtime wrapper (`runtime.sh` uses `-t $TARGET`). The compiler, stdlib, stdx, cjpm, cjfmt, hyperlang, and lsp wrappers are still hardcoded to `release`.
- For iterative work, prefer `bash build_scripts/linux/all.sh --skip-clean --skip-bundle ...` or the narrow component wrapper with `--skip-clean`.
- `bundleSDK.sh` assembles the final tarball under `software/cangjie-sdk-${SDK_NAME}-${CANGJIE_VERSION}.tar.gz` by copying `cangjie_compiler/output` and overlaying runtime, stdlib, stdx, and tool binaries.

## Component-specific verification
- Compiler wrapper builds with `python3 build.py build -t release --no-tests`, so wrapper builds do not compile compiler unittests. If you need compiler tests, work in `cangjie_compiler` directly instead of relying on the root wrapper.
- Compiler quick smoke check from the wrapper is `source cangjie_compiler/output/envsetup.sh && cjc -v`.
- LSP has an explicit test command: run `python3 build.py test` in `cangjie_tools/cangjie-language-server/build` after a build with `CANGJIE_HOME` set to a built SDK/compiler output.

## Containerized builds
- Compose services are `build-sdk-linux64` and `build-sdk-linuxarm`.
- Use `FIXUID=$(id -u) FIXGID=$(id -g) docker compose run --rm <service>`.
- Root `README.md` says `FIXGUID`; `compose.yaml` actually uses `FIXGID`.

## OpenCode repo config
- `opencode.json` allows build-script bash commands broadly, but normal edits are only allowed under `cangjie_*`. Root files, `build_scripts`, and `third_party` are protected unless the user explicitly relaxes that.

## Avoid touching generated outputs
- Do not hand-edit build outputs under `software/`, `*/output/`, `*/target/`, `cangjie_tools/*/build/build/`, or `cangjie_runtime/runtime/CMakebuild/`.
