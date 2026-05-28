set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# STDX
cd $WORKSPACE/cangjie_stdx;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t "$STDX_TARGET" --include=$WORKSPACE/cangjie_compiler/include
python3 build.py install;
# export CANGJIE_STDX_PATH=$WORKSPACE/cangjie_stdx/target/darwin_${ARCH}_cjnative/static/stdx;
