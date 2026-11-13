#!/bin/sh

if [ $# -lt 3 ] ; then
    echo "usage: $(basename "$0") <cnf folder> <output folder> <num workers>"
    echo "in the specified output folder a new folder with all the results will be created"
    exit 1
fi

TIMES=3
RUN_ID=$(date +"%Y-%m-%d_%T")
CNF_PATH="$1"
OUT_PATH="$2"
WORKERS="$3"
RUN_PATH="$OUT_PATH/RUN_$RUN_ID"

ICADICAL_PATH=~/repos/icadical/build/icadical
CADICAL_PATH=~/repos/cadical/build/cadical
ILINGELING_PATH=~/repos/lingeling/ilingeling

if [ ! -d "$CNF_PATH" ] ; then
    echo "cnf folder must be a folder" >&2
    exit 1
fi

if ! ( set -- "$CNF_PATH"/*.icnf; [ -e "$1" ] ) ; then
    echo "no .cnf files in $IN_DIR" >&2
    exit 1
fi

if [ ! -d "$OUT_PATH" ] ; then
    echo "output dir $OUT_PATH not existing. creating."
    mkdir -p "$OUT_PATH" || exit 1
fi

echo "creating run directory $RUN_PATH"
mkdir -p "$RUN_PATH" || exit 1

if [ ! -x $ICADICAL_PATH ] ; then
    echo "icadical couldnt be found at $ICADICAL_PATH"
    exit 1
fi

if [ ! -x $CADICAL_PATH ] ; then
    echo "cadical couldnt be found at $CADICAL_PATH"
    exit 1
fi

if [ ! -x $ILINGELING_PATH ] ; then
    echo "ilingeling couldnt be found at $ILINGELING_PATH"
    exit 1
fi

RESULTS_FILE="$RUN_PATH/results.out"
touch "$RESULTS_FILE" || exit 1

i=1
while [ "$i" -le "$TIMES" ]; do
    echo "STARTING RUN $i" | tee "$RESULTS_FILE"
    for icnf in "$CNF_PATH"/*.icnf; do
        if [ ! -f "$icnf" ] ; then
            echo "$icnf not a file" | tee "$RESULTS_FILE"
            continue
        fi
        echo "  - SOLVING FILE $icnf" | tee "$RESULTS_FILE"
        echo "    - SOLVING FILE $icnf WITH CADICAL" | tee "$RESULTS_FILE"
        echo "    - START TIME CADICAL: $(date)" | tee "$RESULTS_FILE"
        START_TIME=$(date +%s)
        # RUN CADICAL
        $CADICAL_PATH $icnf > "$RUN_PATH/out_cadical_$(basename "$icnf" .icnf)_$i.out" 2>&1
        CADICAL_RESULT=$?
        END_TIME=$(date +%s)
        echo "    - CADICAL RESULT: $CADICAL_RESULT" | tee "$RESULTS_FILE"
        echo "    - END TIME CADICAL: $(date)" | tee "$RESULTS_FILE"
        echo "    - SECONDS NEEDED: $((END_TIME - START_TIME))"

        # RUN ICADICAL
        echo "    - SOLVING FILE $icnf WITH ICADICAL" | tee "$RESULTS_FILE"
        START_TIME=$(date +%s)
        $ICADICAL_PATH --num-workers $WORKERS $icnf > "$RUN_PATH/out_icadical_$(basename "$icnf" .icnf)_$i.out" 2>&1
        ICADICAL_RESULT=$?
        END_TIME=$(date +%s)
        echo "    - ICADICAL RESULT: $ICADICAL_RESULT" | tee "$RESULTS_FILE"
        echo "    - END TIME ICADICAL: $(date)" | tee "$RESULTS_FILE"
        echo "    - SECONDS NEEDED: $((END_TIME - START_TIME))"
        if [ $ICADICAL_RESULT -ne $CADICAL_RESULT ] ; then
            echo "    - WARNING: THIS DIFFERS FROM CADICAL RESULT $ICADICAL_RESULT" | tee "$RESULTS_FILE"
        fi

        # RUN ILINGELING
        echo "    - SOLVING FILE $icnf WITH ILINGELING" | tee "$RESULTS_FILE"
        START_TIME=$(date +%s)
        $ILINGELING_PATH $icnf $WORKERS > "$RUN_PATH/out_ilingeling_$(basename "$icnf" .icnf)_$i.out" 2>&1
        ILINGELING_RESULT=$?
        END_TIME=$(date +%s)
        echo "    - ILINGELING RESULT: $ILINGELING_RESULT" | tee "$RESULTS_FILE"
        echo "    - END TIME ILINGELING: $(date)" | tee "$RESULTS_FILE"
        echo "    - SECONDS NEEDED: $((END_TIME - START_TIME))"
        if [ $ILINGELING_RESULT -ne $CADICAL_RESULT ] ; then
            echo "    - WARNING: THIS DIFFERS FROM CADICAL RESULT $ILINGELING_RESULT" | tee "$RESULTS_FILE"
        fi
    done

    i=$((i + 1))
done
