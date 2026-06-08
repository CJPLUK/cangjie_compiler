# Must be sourced from the original working directory it was invoked from
set -o xtrace
if test "0$cjbuildenv" -ne "01"; then
  while [ $# -gt 0 ]; do
    case "$1" in
      --skip-clean) SKIP_CLEAN=1; shift ;;
      --skip-compiler) SKIP_COMPILER=1; shift ;;
      --skip-runtime) SKIP_RUNTIME=1; shift ;;
      --skip-stdlib) SKIP_STDLIB=1; shift ;;
      --skip-stdx) SKIP_STDX=1; shift ;;
      --skip-cjpm) SKIP_CJPM=1; shift ;;
      --skip-cjfmt) SKIP_CJFMT=1; shift ;;
      --skip-hyperlang) SKIP_HYPERLANG=1; shift ;;
      --skip-lsp) SKIP_LSP=1; shift ;;
      --skip-bundle) SKIP_BUNDLE=1; shift ;;
      --bundle-link-dest) BUNDLE_LINK_DEST=1; shift ;;
      --compiler-target=*) COMPILER_TARGET=${1#*=}; shift ;;
      --runtime-target=*) RUNTIME_TARGET=${1#*=}; shift ;;
      --stdlib-target=*) STDLIB_TARGET=${1#*=}; shift ;;
      --stdx-target=*) STDX_TARGET=${1#*=}; shift ;;
      --cjpm-target=*) CJPM_TARGET=${1#*=}; shift ;;
      --cjfmt-target=*) CJFMT_TARGET=${1#*=}; shift ;;
      --hyperlang-target=*) HYPERLANG_TARGET=${1#*=}; shift ;;
      --lsp-target=*) LSP_TARGET=${1#*=}; shift ;;
      *) shift ;;
    esac
  done

  : "${SKIP_CLEAN:=0}"
  : "${SKIP_COMPILER:=0}"
  : "${SKIP_RUNTIME:=0}"
  : "${SKIP_STDLIB:=0}"
  : "${SKIP_STDX:=0}"
  : "${SKIP_CJPM:=0}"
  : "${SKIP_CJFMT:=0}"
  : "${SKIP_HYPERLANG:=0}"
  : "${SKIP_LSP:=0}"
  : "${SKIP_BUNDLE:=0}"
  : "${BUNDLE_LINK_DEST:=0}"
  # Per-component build targets default independently to release.
  : "${COMPILER_TARGET:=release}"
  : "${RUNTIME_TARGET:=release}"
  : "${STDLIB_TARGET:=release}"
  : "${STDX_TARGET:=release}"
  : "${CJPM_TARGET:=release}"
  : "${CJFMT_TARGET:=release}"
  : "${HYPERLANG_TARGET:=release}"
  : "${LSP_TARGET:=release}"

  export SKIP_CLEAN
  export SKIP_COMPILER
  export SKIP_RUNTIME
  export SKIP_STDLIB
  export SKIP_STDX
  export SKIP_CJPM
  export SKIP_CJFMT
  export SKIP_HYPERLANG
  export SKIP_LSP
  export SKIP_BUNDLE
  export BUNDLE_LINK_DEST
  export COMPILER_TARGET
  export RUNTIME_TARGET
  export STDLIB_TARGET
  export STDX_TARGET
  export CJPM_TARGET
  export CJFMT_TARGET
  export HYPERLANG_TARGET
  export LSP_TARGET
  
  : "${CANGJIE_VERSION:=unofficial}"
  : "${SDK_NAME:=stdx}"
  export CANGJIE_VERSION
  export SDK_NAME
  export WORKSPACE=$(realpath $(dirname $0)/../..)
  echo $WORKSPACE

  export cjbuildenv=1
  CPU_COUNT=$(sysctl -n hw.logicalcpu)
  export CMAKE_BUILD_PARALLEL_LEVEL=$((CPU_COUNT > 16 ? 16 : CPU_COUNT))
  unset CPU_COUNT
fi
