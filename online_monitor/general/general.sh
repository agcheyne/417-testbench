#! /bin/bash

FILE="$1"

if [ -z "$FILE" ] ; then

    root --web=off -l general.C\(0\)
else

    root --web=off -l general.C\(\"$FILE\"\)
fi
