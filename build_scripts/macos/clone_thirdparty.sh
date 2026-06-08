set -e
set -o xtrace
. $(dirname $0)/init_env.sh

cd ${WORKSPACE}
# LLVM fork
[ -e cangjie_compiler/third_party/llvm-project ] || ln -s ../../third_party/llvm-project cangjie_compiler/third_party/llvm-project

# flatbuffers
[ -e cangjie_compiler/third_party/flatbuffers ] || ln -s ../../third_party/third_party_flatbuffers cangjie_compiler/third_party/flatbuffers
[ -e cangjie_runtime/stdlib/third_party/flatbuffers ] || ln -s ../../../third_party/third_party_flatbuffers cangjie_runtime/stdlib/third_party/flatbuffers
[ -d cangjie_tools/cangjie-language-server/third_party ] || (
	mkdir cangjie_tools/cangjie-language-server/third_party;
	# leave out flatbuffers for now because there is an extra step to build some headers
	# git clone third_party/third_party_flatbuffers cangjie_tools/cangjie-language-server/third_party/flatbuffers;
	ln -s ../../../third_party/json cangjie_tools/cangjie-language-server/third_party/json-v3.11.3;
	ln -s ../../../third_party/sqlite3 cangjie_tools/cangjie-language-server/third_party/sqlite3;
	cd cangjie_tools/cangjie-language-server/third_party/sqlite3;
	[ -d amalgamation ] || (
		mkdir amalgamation;
		rsync -a --link-dest="$PWD/src" src/shell.c amalgamation/;
		rsync -a --link-dest="$PWD/src" src/sqlite3.c amalgamation/;
		rsync -a --link-dest="$PWD/include" include/sqlite3.h amalgamation/;
		rsync -a --link-dest="$PWD/include" include/sqlite3ext.h amalgamation/;
	)
)
[ -e cangjie_stdx/third_party/flatbuffers ] || ln -s ../../third_party/third_party_flatbuffers cangjie_stdx/third_party/flatbuffers

# boundscheck
[ -e cangjie_compiler/third_party/boundscheck ] || ln -s ../../third_party/third_party_bounds_checking_function/ cangjie_compiler/third_party/boundscheck
[ -e cangjie_runtime/runtime/third_party/third_party_bounds_checking_function ] || ln -s ../../../third_party/third_party_bounds_checking_function/ cangjie_runtime/runtime/third_party/third_party_bounds_checking_function
[ -e cangjie_runtime/stdlib/third_party/boundscheck-v1.1.16 ] || ln -s ../../../third_party/third_party_bounds_checking_function/ cangjie_runtime/stdlib/third_party/boundscheck-v1.1.16
[ -e cangjie_stdx/third_party/boundscheck-v1.1.16 ] || ln -s ../../third_party/third_party_bounds_checking_function/ cangjie_stdx/third_party/boundscheck-v1.1.16
