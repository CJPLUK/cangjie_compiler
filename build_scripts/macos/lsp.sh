set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# LSP
cd $WORKSPACE/cangjie_tools/cangjie-language-server/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash $WORKSPACE/build_scripts/macos/clone_thirdparty.sh
python3 build.py build -t "$LSP_TARGET";
python3 build.py install;
