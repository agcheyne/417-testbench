#! /bin/bash

FILE="$1"

if [ -z "$FILE" ] ; then

    root --web=off -l clusters.C\(0\)
else

    root --web=off -l clusters.C\(\"$FILE\"\)
fi
