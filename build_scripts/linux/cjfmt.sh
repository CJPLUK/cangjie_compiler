set -e
set -o xtrace
. $(dirname $0)/init_env.sh

. $(dirname $0)/envsetup.sh

# cjfmt
cd $WORKSPACE/cangjie_tools/cjfmt/build;
[ "$SKIP_CLEAN" -eq 1 ] || python3 build.py clean;
bash ../build_scripts/linux/clone_thirdparty.sh
python3 build.py build -t release;
python3 build.py install;

