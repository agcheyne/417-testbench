// tb_spe.h - single-photoelectron charge-spectrum fit for gain and mean photoelectrons (ROOT).
//
//   TH1 *h = ...;                       // charge spectrum [pC], pedestal included
//   tb::SpeFit f = tb::fit_spe(h);
//   f.gain, f.mu, f.q1 ...              // see the struct
//
// Models (all: pedestal Gaus(Q0, s0) + Poisson-weighted n-photoelectron terms, n = 0 .. nmax):
//   SPE_GAUSS        n p.e. = Gaus(Q0 + n Q1, sqrt(s0^2 + n s1^2))
//   SPE_POLYA        (default) n p.e. = gamma (Polya) of q - Q0 with mean n Q1, variance n s1^2 + s0^2:
//                    the right-skewed shape of an MCP-PMT's single-photoelectron charge
//   SPE_POLYA_UNDER  (on request) SPE_POLYA, with a share w of the single photoelectrons
//                    under-amplified (backscattered at the MCP top, a late start of the avalanche):
//                    an exponential of slope 1/a from the pedestal, smeared by its noise (Bellamy et al.);
//                    1/a is held between max(0.2 Q1, 3 s0) and Q1, else it fits the pedestal's tail instead
// The Gaussian model is fitted first, as the start; the Polya fit from it is kept whenever it
// converges (the Gaussian only if it does not). With model = SPE_POLYA_UNDER that is tried before
// and kept only if the data ask for it: >= 1000 photoelectron events, a likelihood-ratio chi2 lower
// than the Gaussian's by more than 4 (2 per extra parameter, AIC), and the share and slope off their
// limits. (Photonis run 261, 6 px: Polya and Polya + under differ by 15 in chi2, Q1 0.304 / 0.308 pC,
// but mean_q 0.304 / 0.279: the under-amplified share is not yet established, so Polya is the default.)
// Q1 is the mean charge of ONE fully amplified photoelectron above the pedestal, so gain = Q1 / e;
// mean_q is the mean charge per photoelectron with the under-amplified ones, (1 - w) Q1 + w / a.
// (Photonis run 252, ch 20-23 summed: Gauss Q1 0.281 pC chi2/ndf 0.89; Polya 0.302, 0.57; Polya +
// under-amplified 0.311 pC with w 0.21, slope 0.17 pC, mean_q 0.281 pC, 0.53.)
// (MCP_Scan's ChargeSpectrumFit.C printed (mean - Q0)/e = mu * gain as "Gain", and used a
// 2 V range for the 1 V V1742; neither is repeated here.)
// Starting values: pedestal from a Gaussian around the highest bin, mu from the fraction of
// events in the pedestal, mu = -ln(N_ped / N); if the fit fails it is retried with nudged
// Q1 / s1 / mu, as in the MCP_Scan macro.
#ifndef TB_SPE_H
#define TB_SPE_H

#include <TF1.h>
#include <TFitResult.h>
#include <TH1.h>
#include <TMath.h>
#include <cmath>

namespace tb {

const double E_CHARGE_PC = 1.602176634e-7;   // electron charge in pC

enum SpeModel { SPE_GAUSS, SPE_POLYA, SPE_POLYA_UNDER };

struct SpeFit {
  SpeModel model = SPE_POLYA;
  bool ok = false;                                                  // fit converged
  bool resolved = false;                                            // Q1 > 3 pedestal sigmas: a real photoelectron peak
  double q0 = 0, s0 = 0, q1 = 0, s1 = 0, mu = 0, norm = 0;          // pC, pC, pC, pC, -, events
  double eq0 = 0, es0 = 0, eq1 = 0, es1 = 0, emu = 0;
  double gain = 0, egain = 0;                                       // Q1 / e
  double w = 0, ew = 0, slope = 0;                                  // under-amplified share, its exponential slope [pC]
  double mean_q = 0;                                                // mean charge per photoelectron [pC]
  double chi2ndf = 0;
  TF1 *f = nullptr;                                                 // the fitted function (owned by caller), 9 parameters
  double par[9] = {};                                               // its parameters, for spe_function()
};

// one term: n photoelectrons (n = 0: the pedestal), not yet times N; p as in spe_function()
inline double spe_term(double q, const double *p, int n, SpeModel m)
{
  const double pn = TMath::Poisson(n, p[4]), s0 = p[1];
  if (n == 0) return pn * TMath::Gaus(q, p[0], s0, kTRUE);
  if (m == SPE_GAUSS) return pn * TMath::Gaus(q, p[0] + n * p[2], std::sqrt(s0 * s0 + n * p[3] * p[3]), kTRUE);
  const double x = q - p[0], mean = n * p[2], var = n * p[3] * p[3] + s0 * s0;
  double v = x > 0 ? TMath::GammaDist(x, mean * mean / var, 0, var / mean) : 0;
  if (m == SPE_POLYA_UNDER && n == 1 && p[7] > 0) {   // exponential from the pedestal, smeared by its noise
    const double a = p[8];
    double emg = 0.5 * a * std::exp(0.5 * a * a * s0 * s0 - a * x) * TMath::Erfc((a * s0 * s0 - x) / (std::sqrt(2.0) * s0));
    v = (1 - p[7]) * v + p[7] * emg;
  }
  return pn * v;
}

// parameters: Q0, s0, Q1, s1, mu, N (events x bin width), nmax, w, a
inline TF1 *spe_function(const char *name, double lo, double hi, SpeModel m = SPE_POLYA, int only_n = -1)
{
  auto *f = new TF1(name, [m, only_n](double *x, double *p) {
    double sum = 0;
    for (int n = 0; n <= int(p[6]); n++) if (only_n < 0 || n == only_n) sum += spe_term(x[0], p, n, m);
    return p[5] * sum;
  }, lo, hi, 9);
  f->SetParNames("Q0", "s0", "Q1", "s1", "mu", "N", "nmax", "w", "a");
  f->SetNpx(1000);
  return f;
}

inline double spe_model(double *x, double *p)   // the Gaussian model (7 parameters)
{
  const int nmax = int(p[6]);
  double sum = 0;
  for (int n = 0; n <= nmax; n++)
    sum += TMath::Poisson(n, p[4]) *
           TMath::Gaus(x[0], p[0] + n * p[2], std::sqrt(p[1] * p[1] + n * p[3] * p[3]), kTRUE);
  return p[5] * sum;
}

inline SpeFit fit_spe(TH1 *h, int nmax = 8, double xmin = NAN, double xmax = NAN, SpeModel model = SPE_POLYA)
{
  SpeFit r;
  r.model = SPE_GAUSS;
  if (!h || h->GetEntries() < 100) return r;
  if (std::isnan(xmin)) xmin = h->GetXaxis()->GetXmin();
  if (std::isnan(xmax)) xmax = h->GetXaxis()->GetXmax();
  const double bw = h->GetBinWidth(1);

  // pedestal: Gaussian around the highest bin
  double qp = h->GetBinCenter(h->GetMaximumBin());
  double w = 5 * bw;
  if (h->Integral(h->FindBin(qp - w), h->FindBin(qp + w)) < 20) return r;   // no pedestal peak to fit
  TF1 g("tb_ped", "gaus", qp - w, qp + w);
  h->Fit(&g, "RQ0N");
  double q0 = g.GetParameter(1), s0 = std::fabs(g.GetParameter(2));
  if (!(s0 > 0)) s0 = bw;

  double ntot = h->Integral(h->FindBin(xmin), h->FindBin(xmax));
  double nped = g.Integral(q0 - 3 * s0, q0 + 3 * s0) / bw;
  double mu = (nped > 0 && nped < ntot) ? -std::log(nped / ntot) : 0.1;
  double mean = h->GetMean();
  double q1 = mu > 0.01 ? (mean - q0) / mu : 5 * s0;
  if (!(q1 > 2 * s0)) q1 = 5 * s0;

  TF1 *f = new TF1("tb_spe", spe_model, xmin, xmax, 7);
  f->SetParNames("Q0", "s0", "Q1", "s1", "mu", "N", "nmax");
  f->SetParameters(q0, s0, q1, 0.5 * q1, mu, ntot * bw, nmax);
  f->FixParameter(6, nmax);
  f->SetParLimits(1, 0.1 * s0, 5 * s0);
  f->SetParLimits(2, 0.5 * s0, 50 * q1);
  f->SetParLimits(3, 0.01 * q1, 5 * q1);
  f->SetParLimits(4, 1e-3, nmax);
  f->SetNpx(1000);

  for (int i = 0; i < 8; i++) {
    TFitResultPtr fr = h->Fit(f, "SRLQ0N");
    if (fr.Get() && fr->IsValid()) { r.ok = true ; break ; }
    f->SetParameter(2, q1 * (1.0 + 0.15 * (i + 1)));
    f->SetParameter(3, 0.5 * q1 * (0.8 + 0.1 * i));
    f->SetParameter(4, mu * (0.7 + 0.15 * i));
  }
  // the Polya models, started from the Gaussian fit: Polya + under-amplified (only if asked for,
  // and kept only if the data ask for it), then plain Polya (kept if it converges)
  const double npe = ntot * (1 - std::exp(-f->GetParameter(4)));   // events with photoelectrons
  for (SpeModel m : {SPE_POLYA_UNDER, SPE_POLYA}) {
    if (m > model || !r.ok || (m == SPE_POLYA_UNDER && npe < 1000)) continue;
    static int count = 0;
    const double q1 = f->GetParameter(2);
    double a_max = 0;
    TF1 *g = spe_function(Form("tb_spe_polya_%d", count++), xmin, xmax, m);
    for (int k = 0; k < 7; k++) g->SetParameter(k, f->GetParameter(k));
    g->FixParameter(6, nmax);
    g->SetParLimits(1, 0.1 * s0, 5 * s0);
    g->SetParLimits(2, 0.5 * q1, 3 * q1);
    g->SetParLimits(3, 0.05 * q1, 3 * q1);
    g->SetParLimits(4, 1e-3, nmax);
    if (m == SPE_POLYA_UNDER) {
      g->SetParameter(7, 0.1) ; g->SetParLimits(7, 0, 0.6);
      // slope 1/a between max(0.2 Q1, 3 pedestal sigmas) and Q1: shorter, it only mimics the pedestal's tail
      a_max = 1 / std::max(0.2 * q1, 3 * f->GetParameter(1));
      g->SetParameter(8, std::min(3 / q1, 0.9 * a_max)) ; g->SetParLimits(8, 1 / q1, a_max);
    } else { g->FixParameter(7, 0) ; g->FixParameter(8, 1) ; }
    TFitResultPtr fr = h->Fit(g, "SRLQ0N");
    // AIC: 2 more parameters than the Gaussian must lower the likelihood-ratio chi2 by more than 4
    bool better = fr.Get() && fr->IsValid() && (m == SPE_POLYA || g->GetChisquare() < f->GetChisquare() - 4);
    if (better && m == SPE_POLYA_UNDER) {   // the under-amplified term pinned to a limit: not measured
      double w = g->GetParameter(7), a = g->GetParameter(8);
      better = w > 0.005 && w < 0.59 && a > 1.01 / q1 && a < 0.99 * a_max;
    }
    if (better) { delete f ; f = g ; r.model = m ; break ; }
    delete g;
  }
  if (r.model == SPE_GAUSS) {   // as a 9-parameter function, like the others
    static int count = 0;
    TF1 *g = spe_function(Form("tb_spe_gauss_%d", count++), xmin, xmax, SPE_GAUSS);
    for (int k = 0; k < 7; k++) { g->SetParameter(k, f->GetParameter(k)) ; g->SetParError(k, f->GetParError(k)) ; }
    g->SetParameter(7, 0) ; g->SetParameter(8, 1);
    g->SetChisquare(f->GetChisquare()) ; g->SetNDF(f->GetNDF());
    delete f;
    f = g;
  }
  r.f = f;
  for (int k = 0; k < 9; k++) r.par[k] = f->GetParameter(k);
  r.w = f->GetParameter(7) ; r.ew = f->GetParError(7) ; r.slope = r.w > 0 ? 1 / f->GetParameter(8) : 0;
  r.q0 = f->GetParameter(0) ; r.eq0 = f->GetParError(0);
  r.s0 = f->GetParameter(1) ; r.es0 = f->GetParError(1);
  r.q1 = f->GetParameter(2) ; r.eq1 = f->GetParError(2);
  r.s1 = f->GetParameter(3) ; r.es1 = f->GetParError(3);
  r.mu = f->GetParameter(4) ; r.emu = f->GetParError(4);
  r.norm = f->GetParameter(5);
  r.gain = r.q1 / E_CHARGE_PC ; r.egain = r.eq1 / E_CHARGE_PC;
  r.mean_q = (1 - r.w) * r.q1 + r.w * r.slope;
  r.chi2ndf = f->GetNDF() > 0 ? f->GetChisquare() / f->GetNDF() : 0;
  r.resolved = r.q1 > 3 * r.s0;
  return r;
}

} // namespace tb

#endif // TB_SPE_H
