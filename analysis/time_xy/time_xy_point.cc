// Hit times of every channel in one run (one scan point): CFD time of the biggest pulse in 10-45 ns
// after its DRS4 group trigger, less the input's db time offset. Writes OUT.root with
//   TH2F "tc"   channel x time (50 ps bins)
//   TH1D "nev"  bin 1 = events read
//   TH1I "row", "col"  the anode pixel of each input (bin ch + 1; -1 = none), from the db's mapping.txt
//               and the cabling recorded in the run's begin-run packet 910 (so an old scan keeps its own)
//   TNamed "detector"  the detector named in 910
// Usually run by time_xy.sh on every point of a scan.
//   time_xy_point FILE OUT.root [EVENTS]
// EVENTS = 0: only (re)write row, col and detector into an existing OUT.root (parts made before they were stored).
#include "tb_v1742.h"
#include "tb_db.h"
#include <Event/fileEventiterator.h>
#include <TFile.h>
#include <TH1D.h>
#include <TH1I.h>
#include <TH2F.h>
#include <TNamed.h>
#include <cstdio>
#include <cstdlib>

// "--- detectors/<name>/cabling.txt" section of the 910 text -> packet, slot pairs
static std::vector<std::pair<int, int>> cabling_in_910(const std::string &text)
{
  std::vector<std::pair<int, int>> v;
  std::istringstream is(text);
  std::string line;
  bool in = false;
  while (std::getline(is, line)) {
    if (line.rfind("---", 0) == 0) { in = line.find("/cabling.txt") != std::string::npos ; continue ; }
    if (!in) continue;
    std::istringstream ls(line);
    int packet, slot;
    if (ls >> packet >> slot) v.push_back({packet, slot});
  }
  return v;
}

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: time_xy_point FILE OUT.root [EVENTS]\n") ; return 2 ; }
  long nmax = argc > 3 ? atol(argv[3]) : 20000, n = 0;
  static tb::V1742Event ev;
  static tb::RunInfo info;
  tb::TimeOffsets toff;
  bool loaded = false;
  TH2F *tc = new TH2F("tc", ";channel;time - trigger [ns]", 32, -0.5, 31.5, 700, 10, 45);
  TH1D *nev = new TH1D("nev", "", 1, 0, 1);
  TH1I *hrow = new TH1I("row", "pixel row per input (-1 = none)", 32, -0.5, 31.5);
  TH1I *hcol = new TH1I("col", "pixel column per input (-1 = none)", 32, -0.5, 31.5);
  for (TH1 *h : {(TH1 *)tc, (TH1 *)nev, (TH1 *)hrow, (TH1 *)hcol}) h->SetDirectory(nullptr);
  for (int ch = 0; ch < 32; ch++) { hrow->SetBinContent(ch + 1, -1) ; hcol->SetBinContent(ch + 1, -1) ; }
  int status;
  fileEventiterator it(argv[1], status);
  if (status) { fprintf(stderr, "cannot open %s\n", argv[1]) ; return 1 ; }
  Event *e;
  while ((n < nmax || (nmax == 0 && !info.seen)) && (e = it.getNextEvent())) {
    if (e->getEvtType() == 9) { info.parse(e) ; ev.serial = info.serial ; delete e ; continue ; }
    if (!ev.fill(e)) { delete e ; continue ; }
    delete e;
    if (!loaded) { toff.load(ev.serial, ev.freq) ; loaded = true ; }
    n++;
    for (int ch = 0; ch < 32; ch++) {
      tb::Cuts c;
      c.sig_from = 10 + toff.ns[ch] ; c.sig_to = 45 + toff.ns[ch];
      tb::ChannelHit x = tb::measure(ev, ch, c);
      if (!x.valid || !x.hit || std::isnan(x.t_cfd)) continue;
      tc->Fill(ch, x.t_cfd - toff.ns[ch]);
    }
  }
  nev->SetBinContent(1, n);
  tb::Db db;
  if (!info.detector.empty() && db.load(info.detector)) {
    for (auto &[packet, slot] : cabling_in_910(info.text910))
      for (auto &m : db.map_slot)
        if (m.first.first == slot && packet == ev.packet) {
          hrow->SetBinContent(m.first.second + 1, m.second.first);
          hcol->SetBinContent(m.first.second + 1, m.second.second);
        }
  }
  TFile f(argv[2], nmax ? "RECREATE" : "UPDATE");
  if (f.IsZombie()) return 1;
  if (nmax) { tc->Write() ; nev->Write() ; }
  hrow->Write("", TObject::kOverwrite) ; hcol->Write("", TObject::kOverwrite);
  TNamed("detector", info.detector.c_str()).Write("", TObject::kOverwrite);
  f.Close();
  return 0;
}
