# dark_count: dark pulses of one run (standalone, no pmonitor); the plot is a ROOT macro (ACLiC)
TB_ROOT_DIR = $(abspath ../..)
CXXFLAGS = -O2 -I$(TB_ROOT_DIR)/lib -DTB_ROOT_DIR=\"$(TB_ROOT_DIR)\" $(shell root-config --cflags) -I$(ONLINE_MAIN)/include
LDFLAGS = $(shell root-config --libs) -Wl,--no-as-needed -L$(ONLINE_MAIN)/lib -lEvent -lNoRootEvent -lmessage

dark_count : dark_count.cc $(wildcard $(TB_ROOT_DIR)/lib/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f dark_count
