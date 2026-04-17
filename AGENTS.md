# Cangjie SDK project

## Build instructions

To perform any build of the project as a whole, or a component, you will run commands of the following form
```shell
bash build_scripts/linux/<SCRIPT>.sh <OPTS>
```
Any timeout for the builds should be generous, say one hour and a half.

### Full build and package
The most basic command for a full build of the SDK into a tarball is the following
```shell
bash build_scripts/linux/all.sh
```

However this is only necessary for packaging the SDK to distribute, for general dev purposes one of the other commands here are more appropriate.

### Build all components

```shell
bash build_scripts/linux/all.sh <FLAGS>
```

Where <FLAGS> can be one or more of the following:

--skip-clean: Skip cleaning, for iterative builds this should be safe
--skip-compiler: Skip the compiler component
--skip-runtime: Skip the runtime component
--skip-stdlib: Skip the standard library component
--skip-stdx: Skip the extended standard library
--skip-cjpm: Skip the CJPM project management tool build
--skip-cjfmt: Skip the formatter build
--skip-hyperlang: Skip the hyperlang build
--skip-lsp: Skip the language server build
--skip-bundle: Skip bundling the SDK into a tarball (most likely always use this one unless explicitly building something to be distributed)
--debug: Build in debug mode

### Build individual components

```shell
bash build_scripts/linux/<COMPONENT>.sh <FLAGS>
```

Where <FLAGS> is the same as above, and <COMPONENT> is one of:
- compiler
- runtime
- stdlib
- stdx
- cjpm
- cjfmt
- hyperlang
- lsp
