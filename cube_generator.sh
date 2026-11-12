#!/bin/sh

if [ "$#" -lt 2 ] ; then
    echo "usage: $(basename "$0") <input dir> <output dir> [march options]"
    echo "input and output folder have to be supplied!"
    exit 1
fi

IN_DIR=$1
OUT_DIR=$2
shift 2 # give the rest to march_cu


if [ ! -d "$IN_DIR" ] ; then
    echo "input path must be a folder" >&2
    exit 1
fi

if ! ( set -- "$IN_DIR"/*.cnf; [ -e "$1" ] ) ; then
    echo "no .cnf files in $IN_DIR" >&2
    exit 1
fi


if [ ! -d "$OUT_DIR" ] ; then
    echo "output dir $OUT_DIR not existing. creating."
    mkdir -p "$OUT_DIR" || exit 1
fi

if ! command -v "march_cu" ; then
    echo "march_cu not found. it needs to be in the directory or the path!"
    exit 1
fi

for cnf in "$IN_DIR"/*.cnf; do
    if [ ! -f "$cnf" ] ; then
        echo "$cnf not a file"
        continue
    fi
    icnf="$OUT_DIR/$(basename "$cnf" ".cnf").icnf"
    echo "$cnf -> $icnf"
    march_cu $cnf -o $OUT_DIR/cubes.temp -cnf $@
    sed -e 's,^p cnf.*,p inccnf,' $cnf > "$icnf"
    cat "$OUT_DIR/cubes.temp" >> $icnf
done

rm "$OUT_DIR/cubes.temp"
