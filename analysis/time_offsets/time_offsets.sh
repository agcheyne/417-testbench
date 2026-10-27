#! /bin/bash

FILE="$1"

if [ -z "$FILE" ] ; then

    root --web=off -l time_offsets.C\(0\)
else

    root --web=off -l time_offsets.C\(\"$FILE\"\)
fi
