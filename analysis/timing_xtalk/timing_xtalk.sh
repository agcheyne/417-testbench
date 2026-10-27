#! /bin/bash

FILE="$1"

if [ -z "$FILE" ] ; then

    root --web=off -l timing_xtalk.C\(0\)
else

    root --web=off -l timing_xtalk.C\(\"$FILE\"\)
fi
