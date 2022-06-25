#!/bin/bash

while test $# -gt 0
do
    case "$1" in
        --win64)
            echo "Creating debug standalone build for 64-bit Windows"
            rm -rf build
            meson build --buildtype=debug -Doptimization=0 -Dstandalone=true --cross-file win64_host.ini
            ;;
        --win64-gdb)
            echo "Creating debug standalone build for 64-bit Windows"
            cd build
            ln -s /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll libwinpthread-1.dll
            ninja -C . iheaders && ninja -C .
            echo "target extended-remote localhost:12345" > 'gdb_startup'
            wine Z:/usr/x86_64-w64-mingw32/bin/gdbserver.exe localhost:12345 kuadron.exe &
            wine /usr/x86_64-w64-mingw32/bin/gdb.exe --command=gdb_startup
            ;;
        --linux)
            echo "Creating debug standalone build for 64-bit Linux"
            rm -rf build
            meson build --buildtype=debug -Doptimization=0 -Dstandalone=true
            ;;
        --*)
            echo "bad option $1"
            ;;
        *)
            echo "unknown argument $1"
            ;;
    esac
    shift
done

pkill -P $$
exit 0
