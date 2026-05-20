set -e
set -o xtrace
. $(dirname $0)/init_env.sh

cd ${WORKSPACE}
[ -e cangjie_compiler/third_party/llvm-project ] || ln -s ../../third_party/llvm-project cangjie_compiler/third_party/llvm-project
[ -e cangjie_compiler/third_party/flatbuffers ] || ln -s ../../third_party/third_party_flatbuffers cangjie_compiler/third_party/flatbuffers
[ -e cangjie_runtime/stdlib/third_party/flatbuffers ] || ln -s ../../../third_party/third_party_flatbuffers cangjie_runtime/stdlib/third_party/flatbuffers
[ -d cangjie_tools/cangjie-language-server/third_party ] || mkdir cangjie_tools/cangjie-language-server/third_party
[ -e cangjie_tools/cangjie-language-server/third_party/flatbuffers ] || ln -s ../../../third_party/third_party_flatbuffers cangjie_tools/cangjie-language-server/third_party/flatbuffers
[ -e cangjie_stdx/third_party/flatbuffers ] || ln -s ../../third_party/third_party_flatbuffers cangjie_stdx/third_party/flatbuffers
