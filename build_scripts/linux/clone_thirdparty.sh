set -e
set -o xtrace
. $(dirname $0)/init_env.sh

cd ${WORKSPACE}
[ -d cangjie_compiler/third_party/llvm-project ] || git clone third_party/llvm-project cangjie_compiler/third_party/llvm-project
[ -d cangjie_compiler/third_party/flatbuffers ] || git clone third_party/third_party_flatbuffers cangjie_compiler/third_party/flatbuffers
