// tb_pulse.h - waveform helpers for 417 test-bench analyses (header only, no ROOT needed).
//
// All functions work on one trace: t[] [ns] and v[] [mV], n samples, baseline already
// subtracted, pulses negative-going (pass a negated copy for positive pulses).
// Use the calibrated per-cell time axis (caen_correction::caen_time) for t[] - the V1742
// samples are not evenly spaced, so "sample * 0.2 ns" is only an approximation.
//
// Methods carried over from the MCP_Scan analysis (processScans.cxx, the Photek gain and
// afterpulse notebooks) and from glasgow.cc, with their known bugs fixed.
#ifndef TB_PULSE_H
#define TB_PULSE_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace tb {

const double NaN = std::numeric_limits<double>::quiet_NaN();

// median of v[lo..hi) - a baseline that a pulse in the window cannot drag far
inline double median(const double *v, int lo, int hi)
{
  std::vector<double> s(v + lo, v + hi);
  if (s.empty()) return NaN;
  std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
  double m = s[s.size() / 2];
  if (s.size() % 2 == 0) m = 0.5 * (m + *std::max_element(s.begin(), s.begin() + s.size() / 2));
  return m;
}

// rms of v[lo..hi) around its mean - the noise, e.g. for a threshold in sigmas
inline double rms(const double *v, int lo, int hi)
{
  if (hi - lo < 2) return NaN;
  double m = 0, q = 0;
  for (int i = lo; i < hi; i++) m += v[i];
  m /= (hi - lo);
  for (int i = lo; i < hi; i++) q += (v[i] - m) * (v[i] - m);
  return std::sqrt(q / (hi - lo));
}

// index of the minimum of v[lo..hi)
inline int min_index(const double *v, int lo, int hi)
{
  return int(std::min_element(v + lo, v + hi) - v);
}

// time where v first falls through `level` in [lo, hi), linearly interpolated; NaN if never
inline double crossing_time(const double *t, const double *v, int lo, int hi, double level)
{
  for (int i = std::max(lo, 1); i < hi; i++)
    if (v[i - 1] > level && v[i] <= level)
      return t[i - 1] + (level - v[i - 1]) / (v[i] - v[i - 1]) * (t[i] - t[i - 1]);
  return NaN;
}

// constant-fraction time: where the leading edge crosses `fraction` of the pulse minimum in
// [lo, hi). Takes the last crossing before the minimum (so noise earlier in the window is
// skipped), interpolated. NaN if the minimum is not below `min_amplitude` (negative, mV).
inline double cfd_time(const double *t, const double *v, int lo, int hi,
                       double fraction = 0.5, double min_amplitude = 0)
{
  int im = min_index(v, lo, hi);
  if (!(v[im] < min_amplitude)) return NaN;
  double level = fraction * v[im];
  for (int i = im; i > lo; i--)
    if (v[i - 1] > level && v[i] <= level)
      return t[i - 1] + (level - v[i - 1]) / (v[i] - v[i - 1]) * (t[i] - t[i - 1]);
  return NaN;
}

// like cfd_time, but from a straight line fitted through all leading-edge samples between
// `from` and `to` of the minimum (the time where the line crosses `fraction`). Averages the
// per-sample timing errors of the DRS4 over the edge: on the TR traces (run 70, 3 Nov 2026) the
// two-sample CFD has a 13-21 ps random error per group, the fit ~0-12 ps, same mean (< 1 ps).
// Falls back to cfd_time when fewer than 3 samples lie on the edge.
inline double edge_fit_time(const double *t, const double *v, int lo, int hi, double fraction = 0.5,
                            double min_amplitude = 0, double from = 0.2, double to = 0.8)
{
  int im = min_index(v, lo, hi);
  if (!(v[im] < min_amplitude)) return NaN;
  int i = im;
  while (i > lo && v[i - 1] <= from * v[im]) i--;   // back to the start of the edge
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  int n = 0;
  for (int j = i; j <= im; j++)
    if (v[j] <= from * v[im] && v[j] >= to * v[im]) { sx += t[j] ; sy += v[j] ; sxx += t[j] * t[j] ; sxy += t[j] * v[j] ; n++ ; }
  double d = n * sxx - sx * sx;
  if (n < 3 || d == 0) return cfd_time(t, v, lo, hi, fraction, min_amplitude);
  double b = (n * sxy - sx * sy) / d, a = (sy - b * sx) / n;
  return (fraction * v[im] - a) / b;
}

// charge [pC] into 50 ohm of v over [lo, hi): trapezoid in mV*ns, / 50 ohm (1 mV*ns/ohm = 1 pC).
// Positive for a negative pulse.
inline double charge_pC(const double *t, const double *v, int lo, int hi, double ohm = 50.0)
{
  double s = 0;
  for (int i = std::max(lo, 0); i + 1 < hi; i++) s += 0.5 * (v[i] + v[i + 1]) * (t[i + 1] - t[i]);
  return -s / ohm;
}

// window [lo, hi) of samples whose times lie in [t0 + from_ns, t0 + to_ns)
inline void window_around(const double *t, int n, double t0, double from_ns, double to_ns, int &lo, int &hi)
{
  lo = int(std::lower_bound(t, t + n, t0 + from_ns) - t);
  hi = int(std::lower_bound(t, t + n, t0 + to_ns) - t);
}

// pulses in [lo, hi): each local minimum below `threshold` (negative, mV), at least
// `min_sep_ns` after the previous one and separated from it by a return above threshold/2.
// For late pulses / afterpulses: search from a few ns after the main pulse.
struct Pulse { double time, amplitude ; int index ; };
inline std::vector<Pulse> find_pulses(const double *t, const double *v, int lo, int hi,
                                      double threshold, double min_sep_ns = 2.0)
{
  std::vector<Pulse> out;
  bool armed = true;
  for (int i = std::max(lo, 1); i + 1 < hi; i++) {
    if (v[i] > 0.5 * threshold) armed = true;
    if (!armed || v[i] > threshold || v[i] > v[i - 1] || v[i] > v[i + 1]) continue;
    int j = i;                                         // walk to the bottom of this pulse
    while (j + 1 < hi && v[j + 1] <= v[j]) j++;
    if (!out.empty() && t[j] - out.back().time < min_sep_ns) { i = j ; continue ; }
    out.push_back({t[j], v[j], j});
    armed = false;
    i = j;
  }
  return out;
}

// DRS4 spike removal (from glasgow.cc). A bad storage cell gives a 1-2 sample spike at the
// same sample on all 9 channels of one chip (8 signals + its trigger trace); a real pulse is
// on one channel. ch9[] = the 9 traces of one group, each n samples, in mV.
inline void despike_group(double *ch9[9], int n, double threshold_mv = 10.0, int min_channels = 5)
{
  for (int s = 1; s + 1 < n; s++) {
    int count = 0;
    for (int k = 0; k < 9; k++) {
      double dp = ch9[k][s] - ch9[k][s - 1], dm = ch9[k][s] - ch9[k][s + 1];
      if (dp * dm > 0 && std::fabs(dp) > threshold_mv && std::fabs(dm) > threshold_mv) count++;
    }
    if (count >= min_channels)
      for (int k = 0; k < 9; k++) ch9[k][s] = 0.5 * (ch9[k][s - 1] + ch9[k][s + 1]);
  }
  for (int s = 1; s + 2 < n; s++) {
    int count = 0;
    for (int k = 0; k < 9; k++) {
      double mid = 0.5 * (ch9[k][s] + ch9[k][s + 1]);
      double dp = mid - ch9[k][s - 1], dm = mid - ch9[k][s + 2];
      if (dp * dm > 0 && std::fabs(dp) > threshold_mv && std::fabs(dm) > threshold_mv) count++;
    }
    if (count >= min_channels)
      for (int k = 0; k < 9; k++) {
        double lo = ch9[k][s - 1], hi = ch9[k][s + 2];
        ch9[k][s] = lo + (hi - lo) / 3.0;
        ch9[k][s + 1] = lo + 2.0 * (hi - lo) / 3.0;
      }
  }
}

} // namespace tb

#endif // TB_PULSE_H
