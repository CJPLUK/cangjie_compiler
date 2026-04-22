set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# Cangjie Runtime
#
cd $WORKSPACE/cangjie_runtime/runtime;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t $TARGET -v ${CANGJIE_VERSION};
python3 build.py install;
rsync -a $WORKSPACE/cangjie_runtime/runtime/output/common/linux_${TARGET}_${ARCH}/lib/ $WORKSPACE/cangjie_compiler/output/lib/;
rsync -a $WORKSPACE/cangjie_runtime/runtime/output/common/linux_${TARGET}_${ARCH}/runtime/ $WORKSPACE/cangjie_compiler/output/runtime/;

