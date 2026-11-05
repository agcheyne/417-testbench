# check_align: V1742 board alignment per run from the group trigger time tags (standalone, no pmonitor, no ROOT)
CXXFLAGS = -O2 -I$(ONLINE_MAIN)/include
LDFLAGS = -Wl,--no-as-needed -L$(ONLINE_MAIN)/lib -lEvent -lNoRootEvent -lmessage

check_align : check_align.cc
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f check_align
