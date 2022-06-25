#!/bin/bash

touch "$1/$2"

# This incredibly ugly hack will break if:
# * meson updates and changes how it names/handles these build rules
# * ninja updates and changes some behavior involved here (unlikely)
# * a different meson backend is used
ln=`grep -Fn "build build.ninja: REGENERATE_BUILD" "$1/build.ninja" | cut -f1 -d:`
if ! [[ -z "$ln" ]]
then
    sed -i "${ln}s|.*|build build.ninja: REGENERATE_BUILD ../meson.build ../meson_options.txt meson-private/coredata.dat|" "$1/build.ninja"
fi
