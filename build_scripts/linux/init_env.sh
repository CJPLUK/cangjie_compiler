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
      --debug) TARGET=debug; shift ;;
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
  : "${TARGET:=release}"

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
  export TARGET
  
  : "${CANGJIE_VERSION:=unofficial}"
  : "${SDK_NAME:=stdx}"
  export CANGJIE_VERSION
  export SDK_NAME
  export WORKSPACE=$(realpath $(dirname $0)/../..)
  echo $WORKSPACE

  export cjbuildenv=1
fi
