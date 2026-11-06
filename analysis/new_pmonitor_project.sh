#! /bin/bash
#
# Start a pmonitor project for the 417 test bench: the files writePmonitorProject.sh writes, plus
# what every project here needs, so nothing has to be added by hand:
#   - the Makefile finds lib/ (tb_v1742.h, tb_pulse.h, tb_spe.h, tb_db.h) and passes TB_ROOT_DIR
#   - the library is rebuilt when a header in lib/ changes
#   - process_event() starts from tb::V1742Event (calibrated, baseline-subtracted, despiked)
#
#   new_pmonitor_project.sh DIR        e.g. online_monitor/waveforms or analysis/my_study
#
# DIR's last part is the package name: it names the library, the ROOT macro and its function, so it
# must be a C++ name (letters, digits, _; not starting with a digit). DIR may exist, but none of the
# files it would write. Then: cd DIR ; make ; ./NAME.sh [FILE]

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

[ $# -eq 1 ] && [[ $1 != -* ]] || { sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
DIR=${1%/}
P=$(basename "$DIR")
[[ $P =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] ||
    tb_die "'$P' cannot name the ROOT macro's function: use letters, digits and _, not starting with a digit (e.g. two_board_test)"
mkdir -p "$DIR" || exit 1
for f in $P.cc $P.h $P.C $P.sh ${P}LinkDef.h $P.Makefile Makefile ; do
    [ -e "$DIR/$f" ] || [ -L "$DIR/$f" ] && tb_die "$DIR/$f exists: not overwriting anything"
done
UP=$(tr a-z A-Z <<< "$P")
REL=$(realpath --relative-to="$DIR" "$TB_ROOT")   # ../.. in a folder two levels down

cat > "$DIR/$P.Makefile" << EOF
PACKAGE = $P

ROOTFLAGS = \$(shell root-config --cflags)
ROOTLIBS = \$(shell root-config --glibs)


# lib/ of the repository: tb_v1742.h, tb_pulse.h, tb_spe.h, tb_db.h
TB_ROOT_DIR = \$(abspath $REL)
CXXFLAGS = -I. -I\$(TB_ROOT_DIR)/lib -DTB_ROOT_DIR=\\"\$(TB_ROOT_DIR)\\" \$(ROOTFLAGS) -I\$(ONLINE_MAIN)/include -I\$(OFFLINE_MAIN)/include
RCFLAGS = -I.  -I\$(ONLINE_MAIN)/include -I\$(OFFLINE_MAIN)/include

LDFLAGS = -Wl,--no-as-needed  -L\$(ONLINE_MAIN)/lib -L\$(OFFLINE_MAIN)/lib -lpmonitor -lEvent -lNoRootEvent -lmessage  \$(ROOTLIBS) -fPIC



HDRFILES = \$(PACKAGE).h
LINKFILE = \$(PACKAGE)LinkDef.h


ADDITIONAL_SOURCES =
ADDITIONAL_LIBS =


SO = lib\$(PACKAGE).so

\$(SO) : \$(PACKAGE).cc \$(PACKAGE)_dict.C \$(ADDITIONAL_SOURCES) \$(LINKFILE) \$(wildcard \$(TB_ROOT_DIR)/lib/*.h)
	\$(CXX) \$(CXXFLAGS) -o \$@ -shared  \$<  \$(ADDITIONAL_SOURCES) \$(PACKAGE)_dict.C \$(LDFLAGS)  \$(ADDITIONAL_LIBS)


\$(PACKAGE)_dict.C : \$(HDRFILES) \$(LINKFILE)
	rootcint -f \$@  -c \$(RCFLAGS) \$^


.PHONY: clean

clean:
	rm -f \$(SO) \$(PACKAGE)_dict.C \$(PACKAGE)_dict.h *.pcm

EOF
ln -s $P.Makefile "$DIR/Makefile"

cat > "$DIR/$P.h" << EOF
#ifndef __${UP}_H__
#define __${UP}_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

#endif /* __${UP}_H__ */
EOF

cat > "$DIR/${P}LinkDef.h" << EOF
#ifdef __CINT__

#pragma link C++ defined_in "$P.h";

#endif /* __CINT__ */
EOF

cat > "$DIR/$P.cc" << EOF
// $DIR - (what it shows)
//
//   ./$P.sh FILE     then   root [1] prun(1000)

#include "$P.h"
#include "tb_v1742.h"

#include <pmonitor/pmonitor.h>

#include <TH1.h>

static tb::V1742Event ev;      // big: keep it static
static tb::RunInfo info;
static TH1F *h_trig = nullptr;

int pinit()
{
  if (h_trig) return 1;
  h_trig = new TH1F("trig", "group 0 trigger time;t [ns];events", 200, 0, 200);
  return 0;
}

int process_event(Event *e)
{
  if (e->getEvtType() == 9) { info.parse(e) ; ev.begin_run(e) ; return 0 ; }
  if (!ev.fill(e)) return 0;   // calibrated, baseline-subtracted [mV], despiked, group triggers found
  h_trig->Fill(ev.trig[0]);
  return 0;
}
EOF

cat > "$DIR/$P.C" << EOF
#include "$P.h"
R__LOAD_LIBRARY(lib$P.so)

void $P(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
EOF

cat > "$DIR/$P.sh" << EOF
#! /bin/bash

FILE="\$1"

if [ -z "\$FILE" ] ; then

    root --web=off -l $P.C\\(0\\)
else

    root --web=off -l $P.C\\(\\"\$FILE\\"\\)
fi
EOF
chmod +x "$DIR/$P.sh"

echo "wrote $DIR: $P.cc $P.h $P.C $P.sh ${P}LinkDef.h $P.Makefile (Makefile -> $P.Makefile)"
echo "next: cd $DIR ; make ; ./$P.sh FILE"
