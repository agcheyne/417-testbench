PACKAGE = timing_xtalk

ROOTFLAGS = $(shell root-config --cflags)
ROOTLIBS = $(shell root-config --glibs)


# lib/ of the repository: tb_*.h helpers
TB_ROOT_DIR = $(abspath ../..)
CXXFLAGS = -I. -I$(TB_ROOT_DIR)/lib -DTB_ROOT_DIR=\"$(TB_ROOT_DIR)\" $(ROOTFLAGS) -I$(ONLINE_MAIN)/include -I$(OFFLINE_MAIN)/include
RCFLAGS = -I.  -I$(ONLINE_MAIN)/include -I$(OFFLINE_MAIN)/include

LDFLAGS = -Wl,--no-as-needed  -L$(ONLINE_MAIN)/lib -L$(OFFLINE_MAIN)/lib -lpmonitor -lEvent -lNoRootEvent -lmessage  $(ROOTLIBS) -fPIC



HDRFILES = $(PACKAGE).h
LINKFILE = $(PACKAGE)LinkDef.h


ADDITIONAL_SOURCES =
ADDITIONAL_LIBS =


SO = lib$(PACKAGE).so

$(SO) : $(PACKAGE).cc $(PACKAGE)_dict.C $(ADDITIONAL_SOURCES) $(LINKFILE) $(wildcard $(TB_ROOT_DIR)/lib/*.h)
	$(CXX) $(CXXFLAGS) -o $@ -shared  $<  $(ADDITIONAL_SOURCES) $(PACKAGE)_dict.C $(LDFLAGS)  $(ADDITIONAL_LIBS)


$(PACKAGE)_dict.C : $(HDRFILES) $(LINKFILE)
	rootcint -f $@  -c $(RCFLAGS) $^


.PHONY: clean

clean:
	rm -f $(SO) $(PACKAGE)_dict.C $(PACKAGE)_dict.h *.pcm

