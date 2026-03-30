set -e
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# Cangjie Runtime
#
cd $WORKSPACE/cangjie_runtime/runtime;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
python3 build.py build -t release -v ${CANGJIE_VERSION};
python3 build.py install;
cp -R $WORKSPACE/cangjie_runtime/runtime/output/common/darwin_release_${ARCH}/{lib,runtime} $WORKSPACE/cangjie_compiler/output;

