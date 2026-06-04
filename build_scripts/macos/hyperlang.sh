set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# hyperlang
cd $WORKSPACE/cangjie_tools/hyperlangExtension/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash $WORKSPACE/build_scripts/macos/clone_thirdparty.sh
python3 build.py build -t "$HYPERLANG_TARGET";
python3 build.py install;
