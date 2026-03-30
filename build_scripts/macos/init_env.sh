if test "0$cjbuildenv" -ne "01"; then
  while [ $# -gt 0 ]; do
    case "$1" in
      --skip-clean) SKIP_CLEAN=1; shift ;;
      *) shift ;;
    esac
  done
  : "${SKIP_CLEAN:=0}"
  export SKIP_CLEAN
  
  : "${CANGJIE_VERSION:=unofficial}"
  : "${SDK_NAME:=stdx}"
  export WORKSPACE=$(realpath $(dirname $0)/../..)
  echo $WORKSPACE
fi
cjbuildenv=1
