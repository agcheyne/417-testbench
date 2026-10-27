// tb_fit.h - small fits shared by the 417 test-bench analyses (ROOT).
#ifndef TB_FIT_H
#define TB_FIT_H

#include <TF1.h>
#include <TH1.h>
#include <cmath>

namespace tb {

// Core width of a timing peak: Gaussian over +-0.3 ns around the highest bin, then refitted
// over +-2 sigma. Returns sigma [histogram units], NaN with < 50 entries or a failed fit.
inline double core_sigma(TH1 *h, double &mean, double *esigma = nullptr)
{
  mean = NAN;
  if (!h || h->GetEntries() < 50) return NAN;
  double m = h->GetBinCenter(h->GetMaximumBin()), s = 0.1;
  TF1 g("tb_core", "gaus", m - 0.3, m + 0.3);
  for (int i = 0; i < 2; i++) {
    g.SetRange(m - (i ? 2 * s : 0.3), m + (i ? 2 * s : 0.3));
    if (h->Fit(&g, "RQ0N") != 0) return NAN;
    m = g.GetParameter(1);
    s = std::fabs(g.GetParameter(2));
  }
  mean = m;
  if (esigma) *esigma = g.GetParError(2);
  return s;
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
