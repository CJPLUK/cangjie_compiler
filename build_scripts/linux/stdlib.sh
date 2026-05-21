set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# STD
cd $WORKSPACE/cangjie_runtime/stdlib;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash ../build_scripts/linux/clone_thirdparty.sh
python3 build.py build -t release --target-lib=$WORKSPACE/cangjie_runtime/runtime/output
python3 build.py install;
rsync -a $WORKSPACE/cangjie_runtime/stdlib/output/ $WORKSPACE/cangjie_compiler/output/;

