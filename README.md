# Custom deferred effect handler SDK

This repository is used to pin down versions of relevant repos to build a fork of the Cangjie SDK with deferred effect handlers, including the versions of some third party dependencies to keep the build system reproducible (not time-dependent).

A byproduct is to build the custom SDK to host on the Cangjie UK website. The goal is to set up containerized builds for all the supported platforms. Maybe later this could also include CI testing pinned down to specific revisions of cangjie_test and cangjie_test_framework.

## Cloning
This repo makes use of git submodules so make sure to `git clone` with the `--recurse-submodules`... or if you forgot to do that the first time, you can run `git submodule update --init`.

## Build (plain)

With the environment set up as dictated by cangjie_build, you can build the SDK with:
```shell
// linux:
sh build_scripts/linux.sh // observed to work on linux/x64
// mac:
sh build_scripts/mac.sh // observed to work on macos/aarch64


## Build (containerized)

There are services defined in `compose.yaml` which will build the SDK for different targets. These will appear as a .tar.gz file under the software/cangjie directory. 

### Linux/x86_64
```shell
docker-compose run --rm build-sdk-linux64
```

### Linux/aarch64
```shell
docker-compose run --rm build-sdk-linuxarm
```

### Others
TODO (with cross compilation instructions)

For cross compilation to windows, it appears the docker images have the necessary toolchain already, it may work with the following env var:
```shell
MINGW_PATH=/opt/buildtools/llvm-mingw-w64
```
