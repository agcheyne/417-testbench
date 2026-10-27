#! /bin/bash

FILE="$1"

if [ -z "$FILE" ] ; then

    root --web=off -l scan_map.C\(0\)
else

    root --web=off -l scan_map.C\(\"$FILE\"\)
fi
