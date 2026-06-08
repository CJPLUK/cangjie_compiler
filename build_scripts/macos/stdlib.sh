set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# STD
cd $WORKSPACE/cangjie_runtime/stdlib;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash $WORKSPACE/build_scripts/macos/clone_thirdparty.sh
python3 build.py build -t "$STDLIB_TARGET" --target-lib=$WORKSPACE/cangjie_runtime/runtime/output
python3 build.py install;
rsync -a --link-dest="$WORKSPACE/cangjie_runtime/stdlib/output" "$WORKSPACE/cangjie_runtime/stdlib/output/" "$WORKSPACE/cangjie_compiler/output/";
