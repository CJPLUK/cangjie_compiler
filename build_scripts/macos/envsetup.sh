. $(dirname $0)/init_env.sh

if test "0$cjenvsetup" -ne "01"; then;
  . ${WORKSPACE}/cangjie_compiler/output/envsetup.sh
fi
cjenvsetup=1
