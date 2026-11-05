# time_xy_point: hit times of one scan point (standalone, no pmonitor); the plots are ROOT macros (ACLiC)
TB_ROOT_DIR = $(abspath ../..)
CXXFLAGS = -O2 -I$(TB_ROOT_DIR)/lib -DTB_ROOT_DIR=\"$(TB_ROOT_DIR)\" $(shell root-config --cflags) -I$(ONLINE_MAIN)/include
LDFLAGS = $(shell root-config --libs) -Wl,--no-as-needed -L$(ONLINE_MAIN)/lib -lEvent -lNoRootEvent -lmessage

time_xy_point : time_xy_point.cc $(wildcard $(TB_ROOT_DIR)/lib/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f time_xy_point
