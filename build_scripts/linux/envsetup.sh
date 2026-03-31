set -o xtrace
. $(dirname $0)/init_env.sh

if test "0$cjenvsetup" -ne "01"; then
  . ${WORKSPACE}/cangjie_compiler/output/envsetup.sh
  # Note STDX might not be built yet
  export CANGJIE_STDX_PATH=$WORKSPACE/cangjie_stdx/target/linux_${ARCH}_cjnative/static/stdx;
fi
export cjenvsetup=1
