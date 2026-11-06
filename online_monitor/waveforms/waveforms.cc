// online_monitor/waveforms - every DRS4 chip of every V1742 board in one canvas, one event at a time.
//
// Online, while rcdaq takes data:
//   ./waveforms.sh
//   root [1] online()                  // rcdaqopen() + pstart() + draw(): redrawn every 2 s
// From a file:
//   ./waveforms.sh /scratch2/data/hpDIRC/junk/junk-00007854-0000.evt
//   root [1] next()                    // one event at a time; next(10): 10 on, show the last
//   root [1] pstart() ; draw()         // or run through the file, redrawn every 2 s
// Then any time: set_laser_aligned(false), set_pulse_only(5), set_range(-200, 20), set_time(0, 100),
// save_plot("run7854").
//
// One pad per DRS4 chip (a V1742 group of 8 inputs): 2 x 2 for one board, a row of 4 per board for
// more. The boards are the data packets the begin-run event has a calibration for (901, 902, ...),
// else the ones found in the data (2000 + 10 x link + node). Every trace is calibrated, baseline-
// subtracted and despiked (tb::V1742Event, lib/tb_v1742.h); the chip's TR trace is drawn / 10.
// Joining a run in the middle (online), the begin-run event comes from the run's first file in
// DATA_ROOT. Each board gets only its own calibration: the one in the begin-run event, else its
// calib_<serial>_<speed>.dat; with several boards, none at all means raw data (and a message).
//
// set_laser_aligned(true), the default: t - the chip's trigger (50 % of its TR edge), minus the
// input's offset from db/v1742/time_offsets_<serial>_<speed>.conf when that board has one, so the
// laser pulses line up within each board. Two boards are only as aligned as their TR cables, and
// above ~2 kHz a board can take the next laser pulse (lib/tb_v1742.h).
// set_laser_aligned(false): the trace's own calibrated time axis; a dashed line marks each chip's trigger.
// persistence(20): a canvas with every sample of input 20 (board 0; persistence(5, 1): board 1's
// input 5) from every event from now on, on the waveforms' time axis; with set_pulse_only only the
// events where that input has a pulse. reset_persistence() starts it again; so do set_laser_aligned
// (another time axis) and set_range.
// set_pulse_only(5): only events with a pulse in some input, i.e. 3 samples below -5 mV after falling by
// 5 mV within 2 ns (not a spike, a slow drift or a glitch at the trace's ends); at low light most are empty;
// next() then steps to the next such event. set_pulse_only(0): every event again.
//
// The event thread (pstart) only copies the latest event when the canvas asks for one; the drawing
// is done by a timer in ROOT's main thread.

#include "waveforms.h"
#include "tb_canvas.h"
#include "tb_v1742.h"

#include <pmonitor/pmonitor.h>

#include <TCanvas.h>
#include <TGraph.h>
#include <TH1F.h>
#include <TH2F.h>
#include <TLatex.h>
#include <TLegend.h>
#include <TLine.h>
#include <TROOT.h>
#include <TStyle.h>
#include <TTimer.h>

#include <Event/fileEventiterator.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <glob.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using tb::NSIG;
using tb::NTRIG;
using tb::NCH;
using tb::MAXS;

// --- event thread ------------------------------------------------------------------------------
struct Board {
  int packet;
  std::unique_ptr<tb::V1742Event> ev;      // big: on the heap
  tb::TimeOffsets off;
  bool off_ok = false;                     // offsets measured for this board's serial
  int off_serial = -2, off_freq = -2;
  explicit Board(int p) : packet(p), ev(new tb::V1742Event) { ev->packet = p ; }
};
static std::vector<std::unique_ptr<Board>> boards;
static bool probed = false;                // looked for boards in a data event of this run
static int run = 0;
static std::vector<int> run_serials;       // 921: one serial per board, in the order of the packets

// the latest event, as the canvas shows it: written by the event thread, read by the main thread
struct Snap {
  int packet = 0, serial = -1, ns = 0;     // ns = 0: no data from this board in the event
  bool boards_aligned = true, off_ok = false;
  double t[NCH][MAXS], v[NCH][MAXS], trig[NTRIG], off[NSIG];
};
static std::mutex snap_mtx;
static std::vector<std::unique_ptr<Snap>> snaps;
static int snap_run = 0, snap_seq = 0;
static std::atomic<bool> want{true}, copy_every{false}, fresh{false};
static std::atomic<long> n_seen{0}, n_events{0}, n_snaps{0};   // events processed, V1742 events, snapshots
static std::atomic<double> pulse_mv{0};    // > 0: only events with a pulse below -pulse_mv in some input

static void begin_run(Event *e)
{
  std::vector<int> packets;
  for (int id = 901; id <= 909; id++)
    if (Packet *p = e->getPacket(id)) { packets.push_back(p->iValue(0, "READOUTID")) ; delete p ; }
  if (packets.empty()) packets.push_back(2000);   // runs before 6 Nov 2026: no calibration packets, one board
  std::sort(packets.begin(), packets.end());
  run_serials.clear();
  if (Packet *p = e->getPacket(921)) { for (int i = 0; i < p->getDataLength(); i++) run_serials.push_back(p->iValue(i)) ; delete p ; }
  boards.clear();
  for (size_t i = 0; i < packets.size(); i++) {
    boards.emplace_back(new Board(packets[i]));
    tb::V1742Event &ev = *boards.back()->ev;
    ev.any_calib_file = packets.size() == 1;   // several boards: never another board's calibration
    ev.begin_run(e);
    if (!ev.run_cal) ev.serial = i < run_serials.size() ? run_serials[i] : -1;   // begin_run took the first board's
  }
  probed = false;
  run = e->getRunNumber();
}

// joined mid-run (online): the begin-run event from the run's first file, for every board's
// calibration and serial; false if there is no such file under DATA_ROOT (db/testbench.conf)
static bool read_begin_run(int r)
{
  std::string root = tb::read_conf(tb::repo_root() + "/db/testbench.conf")["DATA_ROOT"];
  if (root.empty()) root = "/scratch2/data/hpDIRC";
  char pat[64];
  snprintf(pat, sizeof pat, "/*/*-%08d-0000.evt", r);
  std::string file;
  glob_t g;
  if (glob((root + pat).c_str(), 0, nullptr, &g) == 0 && g.gl_pathc > 0) file = g.gl_pathv[0];
  globfree(&g);
  if (file.empty()) { printf("waveforms: run %d: no %s%s to take the begin-run event from\n", r, root.c_str(), pat) ; return false ; }
  int status = 0;
  fileEventiterator it(file.c_str(), status);
  Event *b = status ? nullptr : it.getNextEvent();
  bool ok = b && b->getEvtType() == 9;
  if (ok) { printf("waveforms: joined run %d: begin-run event from %s\n", r, file.c_str()) ; begin_run(b) ; }
  else printf("waveforms: run %d: %s does not start with a begin-run event\n", r, file.c_str());
  delete b;
  return ok;
}

// data packets the begin-run event did not announce (joined mid-run, or no calibration packets)
static void find_boards(Event *e)
{
  for (int link = 0; link < 4; link++)
    for (int node = 0; node < 8; node++) {
      int id = 2000 + 10 * link + node;
      if (std::any_of(boards.begin(), boards.end(), [id](const std::unique_ptr<Board> &b) { return b->packet == id ; })) continue;
      if (Packet *p = e->getPacket(id)) {
        delete p;
        boards.emplace_back(new Board(id));
        printf("waveforms: data packet %d found (no begin-run calibration for it: calibration from file)\n", id);
      }
    }
  std::sort(boards.begin(), boards.end(), [](const std::unique_ptr<Board> &a, const std::unique_ptr<Board> &b) { return a->packet < b->packet ; });
  for (size_t i = 0; i < boards.size(); i++) {
    tb::V1742Event &ev = *boards[i]->ev;
    ev.any_calib_file = boards.size() == 1;
    if (!ev.run_cal && ev.serial < 0 && i < run_serials.size()) ev.serial = run_serials[i];
  }
  probed = true;
}

static void load_offsets(Board &b)
{
  const tb::V1742Event &ev = *b.ev;
  if (ev.serial == b.off_serial && ev.freq == b.off_freq) return;
  b.off_serial = ev.serial ; b.off_freq = ev.freq;
  // TimeOffsets falls back to any board's file: only this board's own offsets count here
  char own[32];
  snprintf(own, sizeof own, "time_offsets_%04d_", ev.serial);
  b.off_ok = ev.serial >= 0 && b.off.load(ev.serial, ev.freq) && b.off.file.find(own) != std::string::npos;
  printf("waveforms: packet %d (s/n %d): %s\n", b.packet, ev.serial,
         b.off_ok ? ("input time offsets " + b.off.file).c_str() : "no input time offsets in db/v1742 (aligned on the trigger only)");
}

int pinit() { return 0; }

// a pulse: 3 samples in a row below -pulse_mv (not a one-sample spike), after falling by pulse_mv within
// 2 ns (not a slow baseline drift), away from the trace's first and last 10 samples (DRS4 edge glitches)
static bool pulse_in(const tb::V1742Event &ev, int ch, double mv)
{
  int back = std::max(1, int(2.0 / ev.ns_per_sample + 0.5));
  for (int k = std::max(10, back); k < ev.ns - 10; k++)
    if (ev.v[ch][k] < -mv && ev.v[ch][k + 1] < -mv && ev.v[ch][k + 2] < -mv && ev.v[ch][k - back] - ev.v[ch][k] > mv) return true;
  return false;
}

static bool has_pulse(const std::vector<bool> &ok)
{
  for (size_t i = 0; i < boards.size(); i++)
    if (ok[i])
      for (int ch = 0; ch < NSIG; ch++) if (pulse_in(*boards[i]->ev, ch, pulse_mv)) return true;
  return false;
}

// --- persistence of one input (persistence()): filled here under persist_mtx, drawn by the timer ---
static std::mutex persist_mtx;
static TH2F *h_persist = nullptr;          // booked at the first event after persistence()
static int p_board = 0, p_ch = -1;         // p_ch -1: none
static bool p_aligned = true;              // its time axis: the waveforms' when it was asked for
static double p_ymin = -100, p_ymax = 20;
static long p_events = 0;                  // events in it

static void fill_persistence(const std::vector<bool> &ok)
{
  std::lock_guard<std::mutex> lock(persist_mtx);
  int b = p_board, ch = p_ch;
  if (ch < 0 || b >= (int)boards.size() || !ok[b]) return;
  const Board &bd = *boards[b];
  const tb::V1742Event &ev = *bd.ev;
  if (pulse_mv > 0 && !pulse_in(ev, ch, pulse_mv)) return;
  double dt = 0;
  if (p_aligned) {
    double tg = ev.trigger_of(ch);
    if (std::isnan(tg)) return;
    dt = tg + (bd.off_ok ? bd.off.ns[ch] : 0);
  }
  if (!h_persist) {   // the whole trace, in whole 5 ns, one bin per sample in time
    double lo = 5 * std::floor((ev.t[ch][0] - dt) / 5), hi = 5 * std::ceil((ev.t[ch][ev.ns - 1] - dt) / 5);
    h_persist = new TH2F("persistence", "", std::max(10, int((hi - lo) / ev.ns_per_sample + 0.5)), lo, hi, 150, p_ymin, p_ymax);
    h_persist->SetDirectory(nullptr);
  }
  for (int k = 0; k < ev.ns; k++) h_persist->Fill(ev.t[ch][k] - dt, ev.v[ch][k]);
  p_events++;
}

int process_event(Event *e)
{
  n_seen++;
  if (e->getEvtType() == 9) { begin_run(e) ; return 0 ; }
  if (e->getEvtType() != 1) return 0;
  if (e->getRunNumber() != run) {          // no begin-run event seen: joined the run in the middle
    run = e->getRunNumber();
    if (!read_begin_run(run)) { boards.clear() ; run_serials.clear() ; probed = false ; }
  }
  if (!probed) find_boards(e);
  std::vector<bool> ok(boards.size());
  bool any = false;
  for (size_t i = 0; i < boards.size(); i++)
    if ((ok[i] = boards[i]->ev->fill(e))) { load_offsets(*boards[i]) ; any = true ; }
  if (!any) return 0;
  n_events++;
  if (p_ch >= 0) fill_persistence(ok);
  if (!want && !copy_every) return 0;
  if (pulse_mv > 0 && !has_pulse(ok)) return 0;

  std::lock_guard<std::mutex> lock(snap_mtx);
  snaps.resize(boards.size());
  for (size_t i = 0; i < boards.size(); i++) {
    if (!snaps[i]) snaps[i].reset(new Snap);
    Snap &s = *snaps[i];
    const Board &b = *boards[i];
    const tb::V1742Event &ev = *b.ev;
    s.packet = b.packet ; s.serial = ev.serial ; s.ns = ok[i] ? ev.ns : 0;
    if (!ok[i]) continue;
    s.boards_aligned = ev.boards_aligned ; s.off_ok = b.off_ok;
    for (int ch = 0; ch < NCH; ch++) {
      std::copy(ev.t[ch], ev.t[ch] + ev.ns, s.t[ch]);
      std::copy(ev.v[ch], ev.v[ch] + ev.ns, s.v[ch]);
    }
    std::copy(ev.trig, ev.trig + NTRIG, s.trig);
    std::copy(b.off.ns, b.off.ns + NSIG, s.off);
  }
  snap_run = run ; snap_seq = e->getEvtSequence();
  want = false ; fresh = true;
  n_snaps++;
  return 0;
}

// --- main thread: the canvas ---------------------------------------------------------------------
static TCanvas *canvas = nullptr;
static int drawn_boards = 0;
static std::vector<TGraph *> graphs;       // [board * NCH + ch]
static std::vector<TH1F *> frames;         // [board * NTRIG + chip]
static std::vector<TLine *> trig_lines;    // [board * NTRIG + chip]
static std::vector<TLatex *> titles;       // [board * NTRIG + chip], above the legend
static bool laser_aligned = true;
static double y_min = -100, y_max = 20, x_from = 0, x_to = 0;
// the whole-trace x range, worked out once (else 0 would move with each event's trigger time); again
// after set_laser_aligned, set_time, a new run or another number of boards
static double auto_lo = 0, auto_hi = 0;
static int auto_run = -1;

static void layout(int nb)
{
  int w = nb > 1 ? 1800 : 1300, h = nb > 1 ? 430 * nb : 900;
  if (!tb::canvas_open(canvas)) canvas = new TCanvas("c_waveforms", "V1742 waveforms", w, h);
  else if (nb != drawn_boards) { canvas->SetCanvasSize(w, h) ; canvas->SetWindowSize(w + 4, h + 28) ; }
  canvas->Clear();
  if (nb > 1) canvas->Divide(NTRIG, nb, 0.002, 0.004);
  else canvas->Divide(2, 2);
  static const int colors[8] = {kRed + 1, kOrange + 7, kYellow + 2, kGreen + 2, kCyan + 2, kBlue + 1, kViolet + 1, kMagenta + 1};
  for (int i = graphs.size(); i < nb * NCH; i++) {
    int ch = i % NCH;
    TGraph *g = new TGraph(1);
    g->SetPoint(0, -1e9, 0);
    g->SetLineColor(ch < NSIG ? colors[ch % 8] : kBlack);
    g->SetLineWidth(ch < NSIG ? 2 : 1);
    graphs.push_back(g);
  }
  for (int i = frames.size(); i < nb * NTRIG; i++) {
    frames.push_back(new TH1F(Form("wave_frame_%d", i), "", 100, 0, 1));
    frames.back()->SetDirectory(nullptr);
    trig_lines.push_back(new TLine());
    trig_lines.back()->SetLineStyle(2);
    trig_lines.back()->SetLineColor(kGray + 2);
    titles.push_back(new TLatex());
    titles.back()->SetNDC() ; titles.back()->SetTextSize(0.05) ; titles.back()->SetTextAlign(13);
  }
  for (int b = 0; b < nb; b++)
    for (int g = 0; g < NTRIG; g++) {
      canvas->cd(b * NTRIG + g + 1);
      gPad->SetRightMargin(0.03) ; gPad->SetLeftMargin(0.12) ; gPad->SetTopMargin(0.20) ; gPad->SetBottomMargin(0.12);
      TH1F *f = frames[b * NTRIG + g];
      f->SetMinimum(y_min) ; f->SetMaximum(y_max);
      f->GetYaxis()->SetTitleOffset(1.1);
      f->Draw("axis");
      trig_lines[b * NTRIG + g]->Draw();
      titles[b * NTRIG + g]->SetX(0.12) ; titles[b * NTRIG + g]->SetY(0.98);
      titles[b * NTRIG + g]->Draw();
      // the legend in two rows above the plot, out of the traces' way
      TLegend *leg = new TLegend(0.12, 0.81, 0.97, 0.92);
      leg->SetNColumns(5) ; leg->SetBorderSize(0) ; leg->SetFillStyle(0) ; leg->SetTextSize(0.045);
      for (int k = 0; k < 9; k++) {
        int ch = k < 8 ? 8 * g + k : NSIG + g;
        TGraph *gr = graphs[b * NCH + ch];
        gr->Draw("L same");
        leg->AddEntry(gr, k < 8 ? Form("ch %d", ch) : "TR / 10", "l");
      }
      leg->Draw();
    }
  drawn_boards = nb;
  auto_run = -1;
}

static void modify_all(TVirtualPad *p)
{
  p->Modified();
  TIter next(p->GetListOfPrimitives());
  while (TObject *o = next()) if (o->InheritsFrom(TVirtualPad::Class())) modify_all((TVirtualPad *)o);
}

// the graphs from the snapshot; with force also without a new event (a setting changed)
static void refresh(bool force = false)
{
  if (!tb::canvas_open(canvas)) { drawn_boards = 0 ; return ; }
  if (!fresh && !force) { want = true ; return ; }
  std::lock_guard<std::mutex> lock(snap_mtx);
  fresh = false;
  int nb = snaps.size();
  if (nb == 0) { want = true ; return ; }
  if (nb != drawn_boards) layout(nb);
  const char *xtitle = laser_aligned ? "t - chip trigger [ns]" : "t [ns]";
  double lo = 1e9, hi = -1e9;
  bool misaligned = false;
  for (int b = 0; b < nb; b++) {
    const Snap &s = *snaps[b];
    misaligned |= s.ns > 0 && !s.boards_aligned;
    for (int g = 0; g < NTRIG; g++) {
      double tg = s.ns > 0 ? s.trig[g] : NAN;
      double shift = laser_aligned && !std::isnan(tg) ? tg : 0;
      for (int k = 0; k < 9; k++) {
        int ch = k < 8 ? 8 * g + k : NSIG + g;
        TGraph *gr = graphs[b * NCH + ch];
        if (s.ns == 0) { gr->Set(1) ; gr->SetPoint(0, -1e9, 0) ; continue ; }
        double dt = shift + (laser_aligned && s.off_ok && ch < NSIG ? s.off[ch] : 0);
        double scale = ch < NSIG ? 1 : 0.1;
        gr->Set(s.ns);
        for (int i = 0; i < s.ns; i++) gr->SetPoint(i, s.t[ch][i] - dt, s.v[ch][i] * scale);
        lo = std::min(lo, s.t[ch][0] - dt) ; hi = std::max(hi, s.t[ch][s.ns - 1] - dt);
      }
      TH1F *f = frames[b * NTRIG + g];
      f->GetXaxis()->SetTitle(xtitle) ; f->GetYaxis()->SetTitle("mV");
      titles[b * NTRIG + g]->SetTitle(Form("board %d (packet %d, s/n %d), chip %d%s", b, s.packet, s.serial, g,
                                           s.ns == 0 ? ": not in this event" : std::isnan(tg) ? ": no trigger found" : ""));
      TLine *l = trig_lines[b * NTRIG + g];
      double x = laser_aligned ? 0 : tg;
      if (std::isnan(x)) x = -1e9;
      l->SetX1(x) ; l->SetX2(x) ; l->SetY1(y_min) ; l->SetY2(y_max);
    }
  }
  if (x_to > x_from) { lo = x_from ; hi = x_to ; }
  else {
    if (auto_run != snap_run && hi > lo) { auto_lo = 5 * std::floor(lo / 5) ; auto_hi = 5 * std::ceil(hi / 5) ; auto_run = snap_run ; }   // whole 5 ns
    lo = auto_lo ; hi = auto_hi;
  }
  if (hi > lo)
    for (int i = 0; i < nb * NTRIG; i++) {
      frames[i]->GetXaxis()->SetLimits(lo, hi);
      frames[i]->SetMinimum(y_min) ; frames[i]->SetMaximum(y_max);
    }
  canvas->SetTitle(Form("V1742 waveforms: run %d, event %d%s%s", snap_run, snap_seq,
                        laser_aligned ? ", laser aligned" : "", misaligned ? " - groups of a board took different laser pulses" : ""));
  want = true;
  modify_all(canvas);
  canvas->Update();
}

static TCanvas *c_persist = nullptr;
static TH2F *p_drawn = nullptr;            // the histogram on c_persist

static void refresh_persistence()
{
  if (!tb::canvas_open(c_persist)) { p_drawn = nullptr ; return ; }
  std::lock_guard<std::mutex> lock(persist_mtx);
  if (!h_persist) return;
  c_persist->cd();
  if (h_persist != p_drawn) {
    gPad->SetLogz() ; gPad->SetRightMargin(0.13) ; gPad->SetLeftMargin(0.09);
    h_persist->Draw("colz");
    p_drawn = h_persist;
  }
  const Board *b = p_board < (int)boards.size() ? boards[p_board].get() : nullptr;
  h_persist->SetTitle(Form("board %d%s, ch %d: %ld events%s;%s;mV;samples", p_board,
                           b ? Form(" (packet %d, s/n %d)", b->packet, b->ev->serial) : "", p_ch, p_events,
                           pulse_mv > 0 ? Form(" with a pulse below -%g mV", double(pulse_mv)) : "",
                           p_aligned ? "t - chip trigger [ns]" : "t [ns]"));
  c_persist->Modified();
  c_persist->Update();
}

struct Ticker : public TTimer {
  Ticker() : TTimer(2000, kTRUE) {}
  Bool_t Notify() override { refresh() ; refresh_persistence() ; Reset() ; return kTRUE ; }
};

static void start_ticker()
{
  static Ticker *t = nullptr;
  if (t || gROOT->IsBatch()) return;
  t = new Ticker;
  t->TurnOn();
}

void draw()
{
  gStyle->SetOptStat(0);
  if (!tb::canvas_open(canvas) || drawn_boards == 0) {
    std::lock_guard<std::mutex> lock(snap_mtx);
    layout(std::max<int>(1, snaps.size()));
  }
  refresh(true);
  start_ticker();
}

void next(int n)
{
  // one event at a time, so that with set_pulse_only() it stops at the n-th event with a pulse
  copy_every = true;
  long goal = n_snaps + std::max(1, n), seen = n_seen, start = n_seen;
  while (n_snaps < goal && n_seen - start < 20000) {
    prun(1);
    if (n_seen == seen) break;   // end of the file
    seen = n_seen;
  }
  copy_every = false;
  if (n_snaps < goal)
    printf("waveforms: %s after %ld events\n", n_seen == seen && seen > start ? "end of the file" : "nothing to show", n_seen - start);
  draw();
  refresh_persistence();
}

void online()
{
  if (rcdaqopen()) printf("waveforms: no new rcdaq stream opened (already open, or no rcdaq_server)\n");
  pstart();
  draw();
}

void save_plot(const char *name)
{
  draw();
  canvas->SaveAs(Form("%s.png", name));
  refresh_persistence();
  if (p_drawn) c_persist->SaveAs(Form("%s_persistence.png", name));
}

void persistence(int ch, int board)
{
  if (ch < 0 || ch >= NSIG || board < 0) { printf("waveforms: persistence(ch 0-%d, board 0, 1, ...)\n", NSIG - 1) ; return ; }
  {
    std::lock_guard<std::mutex> lock(persist_mtx);
    if (tb::canvas_open(c_persist)) c_persist->Clear();
    delete h_persist;
    h_persist = p_drawn = nullptr;
    p_board = board ; p_ch = ch ; p_aligned = laser_aligned ; p_ymin = y_min ; p_ymax = y_max ; p_events = 0;
  }
  gStyle->SetOptStat(0);
  if (!tb::canvas_open(c_persist)) c_persist = new TCanvas("c_persistence", "V1742 persistence", 1100, 700);
  printf("waveforms: persistence of board %d ch %d from the next event on%s\n", board, ch,
         pulse_mv > 0 ? " (only events where it has a pulse: set_pulse_only)" : "");
  start_ticker();
  refresh_persistence();
}

void reset_persistence()
{
  if (p_ch >= 0) persistence(p_ch, p_board);
}

void set_laser_aligned(bool on)
{
  laser_aligned = on;
  auto_run = -1;
  refresh(true);
  if (p_ch >= 0 && p_aligned != on) reset_persistence();   // another time axis
}

void set_pulse_only(double mv)
{
  pulse_mv = std::max(0.0, mv);
  if (mv > 0) printf("waveforms: only events with a pulse below -%g mV in some input\n", mv);
  else printf("waveforms: every event\n");
}

void set_range(double min_mv, double max_mv)
{
  y_min = min_mv ; y_max = max_mv;
  refresh(true);
  if (p_ch >= 0) reset_persistence();   // another mV axis
}

void set_time(double from_ns, double to_ns)
{
  x_from = from_ns ; x_to = to_ns;
  auto_run = -1;
  refresh(true);
}
