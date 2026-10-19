# Profile for the TrimUI Brick

GCC profiles recorded on a TrimUI Brick (Allwinner A133P, Cortex-A53, Knulli) with a
`MELONDSDS_PGO_GENERATE=/userdata/system/melondsds-pgo` build: New Super Mario Bros.
(world map and World 1-1) and the Animal Crossing: Wild World intro, about five minutes each.
They contain only execution counts; there is no game, save or user data in them.

Recorded with melonDS DS at `0e11c4e` (melonDS `95074f9`). Code that changed since then
is built without profile data, which is harmless but loses the optimisation for it.

GCC finds the profile of each object file by its path, which is encoded in the file names,
so the optimised build must use the same build directory as the recording build:

```sh
cmake -S . -B /home/user/build/pgo-a64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=<aarch64 toolchain> -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPENGL=OFF -DENABLE_ZLIB=OFF \
    -DMELONDSDS_PGO_USE=$PWD/pgo/trimui-brick \
    "-DCMAKE_MODULE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
ninja -C /home/user/build/pgo-a64
```

On the Brick, this build was about 10% faster than the same code built without the profile.
