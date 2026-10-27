// tb_v1742.h - event preparation shared by the 417 test-bench pmonitor analyses (needs the
// Event library: Event, Packet, caen_correction).
//
//   static tb::V1742Event ev;           // big (two 36 x 1024 arrays): keep it static / on the heap
//   static tb::RunInfo info;
//   int process_event(Event *e) {
//     if (e->getEvtType() == 9) { info.parse(e) ; ev.serial = info.serial ; return 0 ; }
//     if (!ev.fill(e)) return 0;        // calibrated, baseline-subtracted, despiked, triggers found
//     tb::ChannelHit h = tb::measure(ev, ch, cuts);
//
// V1742Event::fill(): DRS4 calibration picked from the board's own FREQUENCY code and the serial
// (begin-run packet 921), per-event baseline (median of early samples; this also absorbs the
// -1329 count shift a whole DRS4 board shows in ~1.4 % of events), spike removal per DRS4 group,
// and the trigger time of each group (50 % CFD on its TR trace). Times of a channel are only
// comparable to its OWN group's trigger: the two DRS4 boards (groups 0/1 and 2/3) are not
// aligned (3.4 ns rms).
#ifndef TB_V1742_H
#define TB_V1742_H

#include "tb_db.h"
#include "tb_pulse.h"

#include <Event/Event.h>
#include <Event/caen_correction.h>

#include <cmath>
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
  int serial = -1;                         // V1742 serial for the calibration file (packet 921)

  // --- this event ---
  int ns = 0;                              // samples per channel
  int freq = -1;                           // FREQUENCY code: 0 = 5, 1 = 2.5, 2 = 1 GS/s
  double v[NCH][MAXS];                     // mV, baseline subtracted, pulses negative
  double t[NCH][MAXS];                     // ns, calibrated per-cell time axis
  double trig[NTRIG];                      // group trigger times [ns], NaN if not found
  long n_events = 0;

  double trigger_of(int ch) const { return trig[(ch % NSIG) / 8]; }
  double trace_ns() const { return ns > 1 ? t[0][ns - 1] - t[0][0] : 0; }

  bool fill(Event *e)
  {
    if (e->getEvtType() != 1) return false;
    Packet *p = e->getPacket(packet);
    if (!p) return false;
    std::unique_ptr<Packet> guard(p);
    int f = p->iValue(0, "FREQUENCY");
    if (f != calib_freq || serial != calib_serial) load_calibration(f);
    if (cc) cc->init(p);
    ns = std::min(p->iValue(0, "SAMPLES"), MAXS);
    if (ns < 10) return false;
    freq = f;
    for (int ch = 0; ch < NCH; ch++) {
      for (int s = 0; s < ns; s++) {
        v[ch][s] = cc ? cc->caen_corrected(s, ch) : p->iValue(s, ch);
        t[ch][s] = cc ? cc->caen_time(s, ch) : s * ns_per_sample;
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
    for (int g = 0; g < NTRIG; g++) trig[g] = cfd_time(t[NSIG + g], v[NSIG + g], 0, ns, trig_fraction, trig_min_mv);
    n_events++;
    return true;
  }

  // --- calibration ---
  std::unique_ptr<caen_correction> cc;
  int calib_freq = -1, calib_serial = -2;
  double ns_per_sample = 0.2;
  std::string calib_file;

  void load_calibration(int f)
  {
    static const char *speed[] = {"5G", "2G", "1G"};
    static const double nsps[] = {0.2, 0.4, 1.0};
    calib_freq = f;
    calib_serial = serial;
    cc.reset();
    calib_file.clear();
    if (f < 0 || f > 2) { printf("tb_v1742: unknown FREQUENCY code %d - raw data, nominal 0.2 ns\n", f) ; return ; }
    ns_per_sample = nsps[f];
    std::string dir = read_conf(repo_root() + "/db/testbench.conf")["CALIB_DIR"];
    if (dir.empty()) dir = "/scratch2/data/v1742.db";
    if (serial >= 0) {
      char b[64];
      snprintf(b, sizeof b, "/calib_%04d_%s.dat", serial, speed[f]);
      if (std::ifstream(dir + b)) calib_file = dir + b;
    }
    if (calib_file.empty())   // serial unknown (no begin-run event seen): any file for this speed
      if (DIR *d = opendir(dir.c_str())) {
        std::string suffix = std::string("_") + speed[f] + ".dat";
        while (dirent *de = readdir(d)) {
          std::string n = de->d_name;
          if (n.rfind("calib_", 0) == 0 && n.size() > suffix.size() &&
              n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) { calib_file = dir + "/" + n ; break ; }
        }
        closedir(d);
      }
    if (calib_file.empty()) { printf("tb_v1742: no %s calibration in %s - raw data\n", speed[f], dir.c_str()) ; return ; }
    cc.reset(new caen_correction(calib_file.c_str()));
    printf("tb_v1742: calibration %s\n", calib_file.c_str());
  }
};

// --- begin-run information ------------------------------------------------------------------
struct RunInfo {
  int run = 0;
  int serial = -1;                         // 921: V1742 serial
  std::string detector, text910;           // 910: detector selected when the run was taken
  int scan_id = 0, point = -1, npoints = 0;   // 911
  double target_x = NAN, target_y = NAN;   // 911 [mm]
  double x = NAN, y = NAN;                 // 932 [mm] (else 931, whole mm)
  bool x_whole_mm = false;                 // x, y came from 931: cut to whole mm
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
    if (Packet *p = e->getPacket(932)) { x = p->iValue(0) / 1000.0 ; y = p->iValue(1) / 1000.0 ; delete p ; }
    else if (Packet *p = e->getPacket(931)) { y = p->iValue(0) ; x = p->iValue(1) ; x_whole_mm = true ; delete p ; }
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
