# Shared bash functions for the 417 test bench. Source it, don't run it:
#   . "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
#
# Gives the caller:
#   TB_ROOT            the repository folder
#   TB_DB, TB_STATE, TB_LOGS
#   tb_die / tb_warn / tb_info / tb_log
#   tb_conf FILE       read KEY=value lines into shell variables (no code is run)
#   tb_site            load db/testbench.conf (data paths, stage limits, rest position)
#   tb_active          set TB_DETECTOR and TB_DAQ from state/active.conf (or $TB_DETECTOR / $TB_DAQ)
#   tb_detector_dir, tb_daq_file, tb_detector_names, tb_daq_names
#   tb_table FILE      print a whitespace table without comments or blank lines
#   tb_fcmp A OP B     floating-point compare (OP: < <= > >= == !=), exit status
#   tb_calc EXPR       floating-point arithmetic through awk

TB_ROOT=$(dirname "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")")
TB_DB=$TB_ROOT/db
TB_STATE=$TB_ROOT/state       # run-time selection and scan context, not in git
TB_LOGS=$TB_ROOT/logs         # not in git

tb_die()  { echo "ERROR: $*" >&2 ; exit 1 ; }
tb_warn() { echo "WARNING: $*" >&2 ; }
tb_info() { echo "$*" ; }

# one line to logs/testbench.log: time, user, calling script, message
tb_log() {
    mkdir -p "$TB_LOGS" 2> /dev/null
    printf '%s %s %s: %s\n' "$(date '+%F %T')" "$USER" "$(basename "$0")" "$*" >> "$TB_LOGS/testbench.log"
}

# KEY=value lines; # starts a comment; values may be "quoted". Only names made of
# A-Z 0-9 _ are accepted, and nothing is evaluated, so a conf file cannot run code.
tb_conf() {
    local file=$1 line key val
    [ -r "$file" ] || tb_die "cannot read $file"
    while IFS= read -r line || [ -n "$line" ] ; do
        line=${line%%#*}
        [[ $line =~ ^[[:space:]]*([A-Z_][A-Z0-9_]*)[[:space:]]*=[[:space:]]*(.*[^[:space:]])?[[:space:]]*$ ]] || continue
        key=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]}
        [[ $val =~ ^\"(.*)\"$ ]] && val=${BASH_REMATCH[1]}
        printf -v "$key" '%s' "$val"
    done < "$file"
}

tb_site() { tb_conf "$TB_DB/testbench.conf" ; }

tb_active() {
    local det=${TB_DETECTOR-} daq=${TB_DAQ-} DETECTOR= DAQ=
    [ -r "$TB_STATE/active.conf" ] && tb_conf "$TB_STATE/active.conf"
    TB_DETECTOR=${det:-$DETECTOR}
    TB_DAQ=${daq:-$DAQ}
    [ -n "$TB_DETECTOR" ] || tb_die "no detector selected: testbench.sh use detector NAME"
    [ -n "$TB_DAQ" ]      || tb_die "no DAQ profile selected: testbench.sh use daq PROFILE"
    [ -d "$(tb_detector_dir)" ] || tb_die "detector '$TB_DETECTOR' is not in $TB_DB/detectors"
    [ -r "$(tb_daq_file)" ]     || tb_die "DAQ profile '$TB_DAQ' is not in $TB_DB/daq"
}

tb_detector_dir() { echo "$TB_DB/detectors/${1:-$TB_DETECTOR}" ; }
tb_daq_file()     { echo "$TB_DB/daq/${1:-$TB_DAQ}.conf" ; }
tb_detector_names() { ls "$TB_DB/detectors" | grep -v '^_' ; }
tb_daq_names()      { ls "$TB_DB/daq" | sed -n 's/\.conf$//p' ; }

tb_table() { sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$1" ; }

tb_fcmp() { awk -v a="$1" -v b="$3" -v op="$2" 'BEGIN {
    r = (op == "<") ? a < b : (op == "<=") ? a <= b : (op == ">") ? a > b :
        (op == ">=") ? a >= b : (op == "==") ? a == b : (op == "!=") ? a != b : -1
    if (r < 0) exit 2 ; exit !r }' ; }

tb_calc() { awk "BEGIN { printf \"%.6g\n\", $* }" ; }
