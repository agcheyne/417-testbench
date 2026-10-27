#! /bin/bash

FILE="$1"

if [ -z "$FILE" ] ; then

    root --web=off -l gain_hv.C\(0\)
else

    root --web=off -l gain_hv.C\(\"$FILE\"\)
fi
