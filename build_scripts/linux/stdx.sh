set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# STDX
cd $WORKSPACE/cangjie_stdx;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash ../build_scripts/linux/clone_thirdparty.sh
python3 build.py build -t release --include=$WORKSPACE/cangjie_compiler/include
python3 build.py install;
# export CANGJIE_STDX_PATH=$WORKSPACE/cangjie_stdx/target/linux_${ARCH}_cjnative/static/stdx;

