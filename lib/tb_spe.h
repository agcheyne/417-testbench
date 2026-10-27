// tb_spe.h - single-photoelectron charge-spectrum fit for gain and mean photoelectrons (ROOT).
//
//   TH1 *h = ...;                       // charge spectrum [pC], pedestal included
//   tb::SpeFit f = tb::fit_spe(h);
//   f.gain, f.mu, f.q1 ...              // see the struct
//
// Model: pedestal + Poisson-weighted Gaussians, n = 0 .. nmax photoelectrons
//   f(q) = N * sum_n P(n; mu) * Gaus(q; Q0 + n*Q1, sqrt(s0^2 + n*s1^2))
// Q1 is the charge of ONE photoelectron above the pedestal, so gain = Q1 / e.
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

struct SpeFit {
  bool ok = false;                                                  // fit converged
  bool resolved = false;                                            // Q1 > 3 pedestal sigmas: a real photoelectron peak
  double q0 = 0, s0 = 0, q1 = 0, s1 = 0, mu = 0, norm = 0;          // pC, pC, pC, pC, -, events
  double eq0 = 0, es0 = 0, eq1 = 0, es1 = 0, emu = 0;
  double gain = 0, egain = 0;                                       // Q1 / e
  double chi2ndf = 0;
  TF1 *f = nullptr;                                                 // the fitted function (owned by caller)
};

inline double spe_model(double *x, double *p)
{
  const int nmax = int(p[6]);
  double sum = 0;
  for (int n = 0; n <= nmax; n++)
    sum += TMath::Poisson(n, p[4]) *
           TMath::Gaus(x[0], p[0] + n * p[2], std::sqrt(p[1] * p[1] + n * p[3] * p[3]), kTRUE);
  return p[5] * sum;
}

inline SpeFit fit_spe(TH1 *h, int nmax = 8, double xmin = NAN, double xmax = NAN)
{
  SpeFit r;
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
  r.f = f;
  r.q0 = f->GetParameter(0) ; r.eq0 = f->GetParError(0);
  r.s0 = f->GetParameter(1) ; r.es0 = f->GetParError(1);
  r.q1 = f->GetParameter(2) ; r.eq1 = f->GetParError(2);
  r.s1 = f->GetParameter(3) ; r.es1 = f->GetParError(3);
  r.mu = f->GetParameter(4) ; r.emu = f->GetParError(4);
  r.norm = f->GetParameter(5);
  r.gain = r.q1 / E_CHARGE_PC ; r.egain = r.eq1 / E_CHARGE_PC;
  r.chi2ndf = f->GetNDF() > 0 ? f->GetChisquare() / f->GetNDF() : 0;
  r.resolved = r.q1 > 3 * r.s0;
  return r;
}

} // namespace tb

#endif // TB_SPE_H
