// tb_fit.h - small fits shared by the 417 test-bench analyses (ROOT).
#ifndef TB_FIT_H
#define TB_FIT_H

#include <TF1.h>
#include <TH1.h>
#include <cmath>
#include <memory>

namespace tb {

// Timing peak of an MCP-PMT: a sharp main peak, a near shoulder on its right (run 70: +100-230 ps,
// a few %; run 252: +180 ps, ~10 %), and a separate late peak (run 70: ~+0.78 ns, 1-2 %, the same
// at every pulse height), on a flat background (dark counts, pickup):
//   f(t) = A1 G(t; m, s1) + A2 G(t; m + d2, s2) + A3 G(t; m + d3, s3) + c
// 1. The main peak (m, s1) is a Gaussian fitted to its left side and top only (m - 3 s .. m + 0.5 s,
//    iterated, likelihood): everything else sits on its right, and a fit over the whole peak blends
//    the shoulder in (run 252 ch 22: 103 ps for the whole peak, 72 ps for its left side and top).
// 2. With m and s1 fixed, the rest is fitted: d2 in [max(s1, 0.08), 0.45] ns, s2 in [0.5, 1.5] s1,
//    A2 <= A1 / 2; d3 in [0.45, 1.5] ns, s3 <= 2.5 s1. The shoulder only with >= 1000 entries, the
//    late peak only where there is a significant excess after the main peak (>= 10 entries and
//    3 sigma over 5 bins); else left out (sh_dt / late_dt NaN, share 0).
// "rest" = the fitted shoulder and late peak: their share of the pulses and their rms about the
// main peak. With fewer than 200 entries only a core Gaussian
// (+-0.3 ns around the highest bin, refitted over +-2 sigma) is fitted (two = false).
struct TimingFit {
  bool ok = false;                         // a peak was fitted
  bool two = false;                        // with the shoulder and late peak
  double mean = NAN, emean = NAN, sigma = NAN, esigma = NAN;   // main peak
  double sh_dt = NAN, sh_sigma = NAN, sh_frac = NAN;          // near shoulder: delay after the main peak, width, share of all three
  double late_dt = NAN, late_sigma = NAN, late_frac = NAN;    // late peak
  double rest_frac = NAN, rest_rms = NAN;  // everything but the main peak and background: share, rms about the main peak
  double lo = NAN, hi = NAN;               // fit range
  double par[10] = {};                     // for timing_function()
};

// the fitted model, e.g. to draw: tb::timing_function("f")->SetParameters(fit.par)
inline TF1 *timing_function(const char *name, double lo = 0, double hi = 1)
{
  return new TF1(name, "[0]*exp(-0.5*((x-[1])/[2])^2) + [3]*exp(-0.5*((x-[1]-[4])/[5])^2) + [6]*exp(-0.5*((x-[1]-[7])/[8])^2) + [9]",
                 lo, hi);
}

inline TimingFit timing_fit(TH1 *h)
{
  TimingFit r;
  if (!h || h->GetEntries() < 50) return r;
  double m = h->GetBinCenter(h->GetMaximumBin()), s = 0.1, es = 0, em = 0;
  TF1 g("tb_core", "gaus", m - 0.3, m + 0.3);
  for (int i = 0; i < 2; i++) {
    g.SetRange(m - (i ? 2 * s : 0.3), m + (i ? 2 * s : 0.3));
    if (h->Fit(&g, "RQ0N") != 0) return r;
    m = g.GetParameter(1) ; s = std::fabs(g.GetParameter(2)) ; em = g.GetParError(1) ; es = g.GetParError(2);
  }
  if (h->GetEntries() >= 200) {             // the main peak: its left side and top
    TF1 gl("tb_left", "gaus", m - 3 * s, m + 0.5 * s);
    gl.SetParameters(g.GetParameter(0), m, s);
    double ml = m, sl = s;
    bool ok = true;
    for (int i = 0; i < 4 && ok; i++) {
      gl.SetRange(ml - 3 * sl, ml + 0.5 * sl);
      ok = h->Fit(&gl, "RQ0NL") == 0 && std::fabs(gl.GetParameter(2)) > 0;
      if (ok) { ml = gl.GetParameter(1) ; sl = std::fabs(gl.GetParameter(2)) ; }
    }
    if (ok && sl < 2 * s && std::fabs(ml - m) < s) {
      m = ml ; s = sl ; em = gl.GetParError(1) ; es = gl.GetParError(2);
      g.SetParameter(0, gl.GetParameter(0));
    }
  }
  r.ok = true ; r.mean = m ; r.sigma = s ; r.emean = em ; r.esigma = es;
  r.lo = m - 2 * s ; r.hi = m + 2 * s;
  double a = g.GetParameter(0);
  r.par[0] = a ; r.par[1] = m ; r.par[2] = s ; r.par[5] = r.par[8] = s ; r.par[4] = 0.2 ; r.par[7] = 0.8;
  if (h->GetEntries() < 200) return r;

  const double bw = h->GetBinWidth(1);
  double lo = m - std::max(5 * s, 0.4), hi = m + 2.0;
  double side = 0;                         // background: mean bin content 1-2 ns before the peak
  int b0 = h->FindBin(m - 2), b1 = h->FindBin(m - 1);
  for (int b = b0; b <= b1; b++) side += h->GetBinContent(b);
  side /= std::max(1, b1 - b0 + 1);
  // the late peak: where the histogram (3 bins summed) is furthest above the main Gaussian
  double d3 = 0.8, r_max = 0, excess = 0, expect = 0;
  for (int b = h->FindBin(m + 0.45) + 2; b < h->FindBin(m + 1.5) - 1; b++) {
    double rr = 0, ex = 0;
    for (int k = -2; k <= 2; k++) {
      double e = side + a * std::exp(-0.5 * std::pow((h->GetBinCenter(b + k) - m) / s, 2));
      rr += h->GetBinContent(b + k) - e ; ex += e;
    }
    if (rr / 5 > r_max) { r_max = rr / 5 ; d3 = h->GetBinCenter(b) - m ; excess = rr ; expect = ex ; }
  }
  double d2_min = std::max(s, 0.08), wmin = std::max(0.3 * s, 0.5 * bw);
  std::unique_ptr<TF1> f(timing_function("tb_timing3", lo, hi));
  double p0[10] = {a, m, s, 0.05 * a, std::max(d2_min, 0.2), s, std::max(r_max, 0.0), d3, s, side};
  f->SetParameters(p0);
  f->SetParLimits(0, 0, 3 * a);
  f->FixParameter(1, m) ; f->FixParameter(2, s);   // the main peak from step 1
  f->SetParLimits(3, 0, 0.5 * a);
  f->SetParLimits(4, d2_min, std::max(0.45, d2_min + 0.05));
  f->SetParLimits(5, std::max(0.5 * s, wmin), 1.5 * s);
  f->SetParLimits(6, 0, a);
  f->SetParLimits(7, 0.45, 1.5);
  f->SetParLimits(8, wmin, 2.5 * s);
  f->SetParLimits(9, 0, std::max(a, 1.0));
  const bool near = h->GetEntries() >= 1000, late = excess >= 10 && excess >= 3 * std::sqrt(expect + 1);
  if (!near) { f->FixParameter(3, 0) ; f->FixParameter(4, p0[4]) ; f->FixParameter(5, p0[5]) ; }
  if (!late) { f->FixParameter(6, 0) ; f->FixParameter(7, p0[7]) ; f->FixParameter(8, p0[8]) ; }
  if (h->Fit(f.get(), "RQ0NL") != 0) return r;
  double w1 = f->GetParameter(0) * f->GetParameter(2), w2 = f->GetParameter(3) * f->GetParameter(5), w3 = f->GetParameter(6) * f->GetParameter(8);
  double wt = w1 + w2 + w3;
  r.two = true;
  r.sh_dt = near ? f->GetParameter(4) : NAN ; r.sh_sigma = near ? f->GetParameter(5) : NAN ; r.sh_frac = wt > 0 ? w2 / wt : 0;
  r.late_dt = late ? f->GetParameter(7) : NAN ; r.late_sigma = late ? f->GetParameter(8) : NAN ; r.late_frac = wt > 0 ? w3 / wt : 0;
  r.lo = lo ; r.hi = hi;
  for (int k = 0; k < 10; k++) r.par[k] = f->GetParameter(k);
  // the rest: the shoulder and late peak as fitted (their share, and rms about the main peak);
  // from the model, not data - fit, so the noise of the tails does not enter
  double v2 = std::isnan(r.sh_dt) ? 0 : w2, v3 = std::isnan(r.late_dt) ? 0 : w3;
  r.rest_frac = wt > 0 ? (v2 + v3) / wt : 0;
  if (v2 + v3 > 0)
    r.rest_rms = std::sqrt((v2 * (r.par[4] * r.par[4] + r.par[5] * r.par[5]) + v3 * (r.par[7] * r.par[7] + r.par[8] * r.par[8])) / (v2 + v3));
  return r;
}

// width of the main timing peak (timing_fit); mean = its position. NaN with < 50 entries or a failed fit.
inline double core_sigma(TH1 *h, double &mean, double *esigma = nullptr)
{
  TimingFit f = timing_fit(h);
  mean = f.mean;
  if (esigma) *esigma = f.esigma;
  return f.sigma;
}

// fraction of entries more than n_sigma core widths after the core mean (late tail)
inline double tail_fraction(TH1 *h, double mean, double sigma, double n_sigma = 3.0)
{
  if (!h || h->GetEntries() <= 0 || std::isnan(mean) || std::isnan(sigma)) return NAN;
  int b = h->FindBin(mean + n_sigma * sigma);
  return h->Integral(b, h->GetNbinsX() + 1) / h->Integral(0, h->GetNbinsX() + 1);
}

} // namespace tb

#endif // TB_FIT_H
