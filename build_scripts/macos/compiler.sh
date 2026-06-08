set -e
set -o xtrace
. $(dirname $0)/init_env.sh

# Cangjie Compiler
#
cd $WORKSPACE/cangjie_compiler;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash $WORKSPACE/build_scripts/macos/clone_thirdparty.sh
python3 build.py build -t "$COMPILER_TARGET" --no-tests;
python3 build.py install;

# Quick test
. output/envsetup.sh
cjc -v
