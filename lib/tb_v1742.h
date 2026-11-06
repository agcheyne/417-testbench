// tb_v1742.h - event preparation shared by the 417 test-bench pmonitor analyses (needs the
// Event library: Event, Packet, caen_calibration, caen_correction).
//
//   static tb::V1742Event ev;           // big (two 36 x 1024 arrays): keep it static / on the heap
//   static tb::RunInfo info;
//   int process_event(Event *e) {
//     if (e->getEvtType() == 9) { info.parse(e) ; ev.begin_run(e) ; return 0 ; }
//     if (!ev.fill(e)) return 0;        // calibrated, baseline-subtracted, despiked, triggers found
//     tb::ChannelHit h = tb::measure(ev, ch, cuts);
//
// V1742Event::begin_run(): the board's factory DRS4 calibration from the begin-run event (packet
// 901, 902, ...: the one written for this data packet, all three speeds). Runs before 6 Nov 2026
// have none: then fill() takes the calib_<serial>_<speed>.dat file in CALIB_DIR (serial: 921).
// V1742Event::fill(): DRS4 calibration for the board's own FREQUENCY code, per-event baseline
// (median of early samples; this also absorbs the -1329 count shift a whole DRS4 board shows in
// ~1.4 % of events), spike removal per DRS4 group, and the trigger time of each group (50 % of its TR trace, from a line fit over the 20-80 %
// edge: edge_fit_time; trig_edge_fit = false for the old two-sample CFD). Times of a channel are only
// comparable to its OWN group's trigger: the two DRS4 boards (groups 0/1 and 2/3) are not
// aligned (3.4 ns rms).
// boards_aligned: the two boards each take the next trigger they are ready for, so above ~2 kHz
// they can record different laser pulses in one event (31 Oct 2026, laser at 5 kHz: groups 2/3 one
// pulse later in ~70 % of events; 2 kHz and below: 100 % aligned). fill() compares the group
// trigger time tags; sums or coincidences across inputs 0-15 and 16-31 need aligned events.
#ifndef TB_V1742_H
#define TB_V1742_H

#include "tb_db.h"
#include "tb_pulse.h"

#include <Event/Event.h>
#include <Event/caen_calibration.h>
#include <Event/caen_correction.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace tb {

const int NSIG = 32, NTRIG = 4, NCH = NSIG + NTRIG, MAXS = 1024;
const double ADC_TO_MV = 1000.0 / 4096.0;   // 12 bit, 1 V range

struct V1742Event {
  // --- settings ---
  int packet = 2000;
  int bl_first = 10, bl_last = 110;        // signal baseline samples (before any pulse)
  bool despike = true;
  double trig_fraction = 0.5, trig_min_mv = -100.0;   // NIM trigger pulse is about -800 mV
  bool trig_edge_fit = true;               // TR time from a line fit over the edge (else CFD on 2 samples)
  int serial = -1;                         // V1742 serial (calibration packet, else packet 921)
  bool any_calib_file = true;              // no calib file for the serial: take any board's for the speed
                                           // (one board); false: raw data (several boards, each its own)

  // --- this event ---
  int ns = 0;                              // samples per channel
  int freq = -1;                           // FREQUENCY code: 0 = 5, 1 = 2.5, 2 = 1 GS/s
  double v[NCH][MAXS];                     // mV, baseline subtracted, pulses negative
  double t[NCH][MAXS];                     // ns, calibrated per-cell time axis
  double trig[NTRIG];                      // group trigger times [ns], NaN if not found
  long long ttt[NTRIG] = {};               // group trigger time tags (30-bit counter, 8.53 ns)
  bool boards_aligned = true;              // all four groups took the same trigger
  long n_events = 0, n_misaligned = 0;

  double trigger_of(int ch) const { return trig[(ch % NSIG) / 8]; }
  double trace_ns() const { return ns > 1 ? t[0][ns - 1] - t[0][0] : 0; }

  bool fill(Event *e)
  {
    if (e->getEvtType() != 1) return false;
    Packet *p = e->getPacket(packet);
    if (!p) return false;
    std::unique_ptr<Packet> guard(p);
    int f = p->iValue(0, "FREQUENCY");
    ns = std::min(p->iValue(0, "SAMPLES"), MAXS);
    if (ns < 10) return false;
    freq = f;
    ns_per_sample = f == 1 ? 0.4 : f == 2 ? 1.0 : 0.2;
    bool from_run = run_cal && run_cal->init(p) == 0;   // fails only for a speed without a table
    if (!from_run) {
      if (f != calib_freq || serial != calib_serial) load_calibration(f);
      if (cc) cc->init(p);
    }
    for (int ch = 0; ch < NCH; ch++) {
      for (int s = 0; s < ns; s++) {
        v[ch][s] = from_run ? run_cal->caen_corrected(s, ch) : cc ? cc->caen_corrected(s, ch) : p->iValue(s, ch);
        t[ch][s] = from_run ? run_cal->caen_time(s, ch) : cc ? cc->caen_time(s, ch) : s * ns_per_sample;
      }
      double b = ch < NSIG ? median(v[ch], bl_first, std::min(bl_last, ns)) : median(v[ch], 1, 21);
      for (int s = 0; s < ns; s++) v[ch][s] = (v[ch][s] - b) * ADC_TO_MV;
    }
    if (despike)
      for (int g = 0; g < NTRIG; g++) {
        double *c9[9];
        for (int k = 0; k < 8; k++) c9[k] = v[8 * g + k];
        c9[8] = v[NSIG + g];
        despike_group(c9, ns, 10.0, 5);
      }
    for (int g = 0; g < NTRIG; g++)
      trig[g] = trig_edge_fit ? edge_fit_time(t[NSIG + g], v[NSIG + g], 0, ns, trig_fraction, trig_min_mv)
                              : cfd_time(t[NSIG + g], v[NSIG + g], 0, ns, trig_fraction, trig_min_mv);
    // same trigger: time tags within 1 count of group 0's, modulo the 30-bit wrap
    const long long WRAP = 1LL << 30;
    boards_aligned = true;
    for (int g = 0; g < NTRIG; g++) {
      ttt[g] = p->lValue(g, "GROUPTRIGGERTIME");
      long long d = ((ttt[g] - ttt[0]) % WRAP + WRAP) % WRAP;
      if (d > 1 && d < WRAP - 1) boards_aligned = false;
    }
    n_events++;
    n_misaligned += !boards_aligned;
    return true;
  }

  // --- calibration ---
  std::unique_ptr<caen_calibration> run_cal;   // from the begin-run event (begin_run)
  std::unique_ptr<caen_correction> cc;         // else from a calib_*.dat file (load_calibration)
  int calib_freq = -1, calib_serial = -2;
  double ns_per_sample = 0.2;
  std::string calib_file;

  // begin-run event: the serial (921) and the calibration packet written for this data packet
  void begin_run(Event *e)
  {
    run_cal.reset();
    calib_freq = -1;                          // a run without one reloads the file
    if (Packet *p = e->getPacket(921)) { serial = p->iValue(0) ; delete p ; }
    for (int id = 901; id <= 909 && !run_cal; id++) {
      Packet *p = e->getPacket(id);
      if (!p) continue;
      if (p->iValue(0, "READOUTID") == packet) {
        run_cal.reset(new caen_calibration(p));
        if (run_cal->isBroken()) run_cal.reset();
        else {
          serial = run_cal->getSerialNumber();
          printf("tb_v1742: calibration from begin-run packet %d (serial %d, data packet %d)\n", id, serial, packet);
        }
      }
      delete p;
    }
  }

  void load_calibration(int f)
  {
    static const char *speed[] = {"5G", "2G", "1G"};
    calib_freq = f;
    calib_serial = serial;
    cc.reset();
    calib_file.clear();
    if (f < 0 || f > 2) { printf("tb_v1742: unknown FREQUENCY code %d - raw data, nominal 0.2 ns\n", f) ; return ; }
    std::string dir = read_conf(repo_root() + "/db/testbench.conf")["CALIB_DIR"];
    if (dir.empty()) dir = "/scratch2/data/v1742.db";
    if (serial >= 0) {
      char b[64];
      snprintf(b, sizeof b, "/calib_%04d_%s.dat", serial, speed[f]);
      if (std::ifstream(dir + b)) calib_file = dir + b;
    }
    if (calib_file.empty() && any_calib_file)   // serial unknown (no begin-run event seen): any file for this speed
      if (DIR *d = opendir(dir.c_str())) {
        std::string suffix = std::string("_") + speed[f] + ".dat";
        while (dirent *de = readdir(d)) {
          std::string n = de->d_name;
          if (n.rfind("calib_", 0) == 0 && n.size() > suffix.size() &&
              n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) { calib_file = dir + "/" + n ; break ; }
        }
        closedir(d);
      }
    if (calib_file.empty()) {
      printf("tb_v1742: no %s calibration for data packet %d (s/n %d) in %s - raw data\n", speed[f], packet, serial, dir.c_str());
      return;
    }
    cc.reset(new caen_correction(calib_file.c_str()));
    printf("tb_v1742: calibration %s\n", calib_file.c_str());
  }
};

// --- CFD time offsets of the inputs -------------------------------------------------------------
// db/v1742/time_offsets_<serial>_<5G|2G|1G>.conf, written by analysis/time_offsets from a scan that
// put the laser on every pixel: OFFSET_00 .. OFFSET_31 [ns] = each input's laser peak (CFD time -
// group trigger) minus the mean of the measured inputs of groups 2 and 3. Subtract it from a CFD
// time to put every input on one time axis: the two DRS4 boards (groups 0/1, 2/3) differ by ~2.3 ns
// (the cabling of their TR inputs), inputs within a board by tens to hundreds of ps. Inputs not
// measured ("-") get the mean of the measured inputs on their board; a board with none, 0
// (group_known false).
//   tb::TimeOffsets off ; off.load(ev.serial, ev.freq) ; t = h.t_cfd - off.ns[ch];
struct TimeOffsets {
  bool ok = false;
  std::string file;
  double ns[NSIG] = {};
  bool measured[NSIG] = {};
  bool group_known[NTRIG] = {};            // its board has measured inputs

  bool load(int serial, int freq_code)
  {
    static const char *speed[] = {"5G", "2G", "1G"};
    ok = false;
    file.clear();
    for (int ch = 0; ch < NSIG; ch++) { ns[ch] = 0 ; measured[ch] = false ; }
    for (bool &k : group_known) k = false;
    if (freq_code < 0 || freq_code > 2) return false;
    std::string dir = repo_root() + "/db/v1742";
    if (serial >= 0) {
      char b[64];
      snprintf(b, sizeof b, "/time_offsets_%04d_%s.conf", serial, speed[freq_code]);
      if (std::ifstream(dir + b)) file = dir + b;
    }
    if (file.empty())   // serial unknown (no begin-run event seen): any file for this speed
      if (DIR *d = opendir(dir.c_str())) {
        std::string suffix = std::string("_") + speed[freq_code] + ".conf";
        while (dirent *de = readdir(d)) {
          std::string n = de->d_name;
          if (n.rfind("time_offsets_", 0) == 0 && n.size() > suffix.size() &&
              n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) { file = dir + "/" + n ; break ; }
        }
        closedir(d);
      }
    if (file.empty()) return false;
    auto m = read_conf(file);
    double sum[NTRIG / 2] = {};             // per DRS4 board: groups 0/1, 2/3
    int n[NTRIG / 2] = {};
    for (int ch = 0; ch < NSIG; ch++) {
      char key[16];
      snprintf(key, sizeof key, "OFFSET_%02d", ch);
      auto it = m.find(key);
      if (it == m.end() || it->second.empty() || it->second == "-") continue;
      ns[ch] = atof(it->second.c_str());
      measured[ch] = true;
      sum[ch / 16] += ns[ch] ; n[ch / 16]++;
    }
    for (int ch = 0; ch < NSIG; ch++) {
      int b = ch / 16;
      group_known[ch / 8] = n[b] > 0;
      if (!measured[ch]) ns[ch] = n[b] ? sum[b] / n[b] : 0;
    }
    ok = true;
    return true;
  }
};

// --- begin-run information ------------------------------------------------------------------
struct RunInfo {
  int run = 0;
  int serial = -1;                         // 921: V1742 serial
  std::string detector, text910;           // 910: detector selected when the run was taken
  int scan_id = 0, point = -1, npoints = 0;   // 911
  double target_x = NAN, target_y = NAN;   // 911 [mm]
  double x = NAN, y = NAN;                 // 931 [mm] (older runs: 932, else hpDIRC 931)
  bool x_whole_mm = false;                 // x, y came from the hpDIRC 931: cut to whole mm
  std::vector<int> hv941;                  // raw 941: per unit serial + 4 x (VSET VMON ISET IMON STAT)
  bool seen = false;

  void parse(Event *e)
  {
    *this = RunInfo();
    seen = true;
    run = e->getRunNumber();
    if (Packet *p = e->getPacket(910)) { std::ostringstream os ; p->dump(os) ; text910 = os.str() ; detector = detector_in_910(text910) ; delete p ; }
    if (Packet *p = e->getPacket(911)) {
      if (p->getDataLength() >= 9) {
        scan_id = p->iValue(4) ; point = p->iValue(5) ; npoints = p->iValue(6);
        if (point >= 0) { target_x = p->iValue(7) / 1000.0 ; target_y = p->iValue(8) / 1000.0 ; }
      }
      delete p;
    }
    if (Packet *p = e->getPacket(921)) { serial = p->iValue(0) ; delete p ; }
    // stage: 931 format 2 (2, X, Y [um]); runs before 6 Nov 2026: 932 (X, Y [um]), else the
    // hpDIRC 931 (2 values: Y, X in whole mm)
    Packet *p931 = e->getPacket(931);
    if (p931 && p931->getDataLength() >= 3 && p931->iValue(0) == 2) { x = p931->iValue(1) / 1000.0 ; y = p931->iValue(2) / 1000.0 ; }
    else if (Packet *p = e->getPacket(932)) { x = p->iValue(0) / 1000.0 ; y = p->iValue(1) / 1000.0 ; delete p ; }
    else if (p931) { y = p931->iValue(0) ; x = p931->iValue(1) ; x_whole_mm = true ; }
    delete p931;
    if (Packet *p = e->getPacket(941)) { for (int i = 0; i < p->getDataLength(); i++) hv941.push_back(p->iValue(i)) ; delete p ; }
  }

  // CAEN N1470 readback of one channel [V / uA]; NaN if that unit was not read
  double hv(int unit_serial, int ch, const char *what = "VMON") const
  {
    static const char *names[] = {"VSET", "VMON", "ISET", "IMON"};
    int k = -1;
    for (int i = 0; i < 4; i++) if (std::string(what) == names[i]) k = i;
    if (k < 0 || ch < 0 || ch > 3) return NAN;
    for (size_t b = 0; b + 21 <= hv941.size(); b += 21)
      if (hv941[b] == unit_serial) return hv941[b + 1 + 5 * ch + k] / (k < 2 ? 10.0 : 100.0);
    return NAN;
  }
  int hv_status(int unit_serial, int ch) const
  {
    for (size_t b = 0; b + 21 <= hv941.size(); b += 21)
      if (hv941[b] == unit_serial) return hv941[b + 1 + 5 * ch + 4];
    return -1;
  }
};

// --- one channel in one event ---------------------------------------------------------------
struct Cuts {
  double sig_from = -1e9, sig_to = 1e9;    // signal window [ns] relative to the group trigger
  double q_from = NAN, q_to = NAN;         // charge window [ns] relative to the trigger; NaN = around the pulse
  double threshold = 5.0;                  // hit: pulse below -threshold [mV] in the signal window
  double cfd_fraction = 0.5;
  double le_threshold = 5.0;               // leading-edge time at -le_threshold [mV]
};

struct ChannelHit {
  bool valid = false;                      // group trigger found and window inside the trace
  bool hit = false;
  double amp = NAN;                        // [mV], positive for a negative pulse
  double q = NAN;                          // [pC]
  double t_cfd = NAN, t_le = NAN;          // [ns] relative to the group trigger
  double t_abs = NAN;                      // CFD time on the trace's own axis [ns] (NaN without a hit)
  int imin = -1;                           // sample of the minimum in the signal window
};

inline ChannelHit measure(const V1742Event &ev, int ch, const Cuts &c)
{
  ChannelHit h;
  double tg = ev.trigger_of(ch);
  if (std::isnan(tg)) return h;
  const double *t = ev.t[ch], *v = ev.v[ch];
  int lo, hi;
  window_around(t, ev.ns, tg, c.sig_from, c.sig_to, lo, hi);
  if (hi - lo < 3) return h;
  h.valid = true;
  h.imin = min_index(v, lo, hi);
  h.amp = -v[h.imin];
  int qlo, qhi;
  if (std::isnan(c.q_from)) window_around(t, ev.ns, t[h.imin], -2.0, 4.0, qlo, qhi);
  else window_around(t, ev.ns, tg, c.q_from, c.q_to, qlo, qhi);
  h.q = charge_pC(t, v, qlo, qhi);
  if (h.amp < c.threshold) return h;
  h.hit = true;
  h.t_abs = cfd_time(t, v, lo, hi, c.cfd_fraction, -c.threshold);
  h.t_cfd = h.t_abs - tg;
  h.t_le = crossing_time(t, v, lo, h.imin + 1, -c.le_threshold) - tg;
  return h;
}

} // namespace tb

#endif // TB_V1742_H
