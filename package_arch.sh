#!/bin/bash
# Perform a clean release build, build an Arch Linux package, and then install it.
#
# IMPORTANT: this is not meant for package maintainers.
#
set -e
rm -rf build
if [[ $1 == debug ]]
then
    meson build --prefix=/usr --buildtype=debug
else
    meson build --prefix=/usr --buildtype=release
fi
cd build
# `makepkg -srif` to include automatic installation
makepkg -srf
target=`cat arch_targetfile`
echo "Package Built: '${target}'"
if [[ $1 == install ]] || [[ $1 == debug ]]
then
    sudo pacman -U $target
fi
