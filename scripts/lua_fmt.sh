#!/bin/bash

# We enforce strict limits on path naming rules for lua directories because they'll
# be collapsed into the name of the lua bytecode file in releases. This means
# Windows can bite us in the ass if various special characters are used. We also
# enforce lowercase naming due to some filesystems not having case sensitivity.

if ! [[ -z `echo "$1" | grep -P -n '[^a-z|0-9|_|/|-]'` ]]
then
    echo "Directories must only contain lowercase ANSI characters"
    exit 1
fi
if ! [[ -z `echo "$1" | grep -P -n '__'` ]]
then
    echo "Directories must not contain `__`"
    exit 1
fi
echo -n $1 | sed 's:/:__:g'
