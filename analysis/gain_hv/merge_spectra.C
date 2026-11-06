// Copy the histograms whose names match a pattern from a list of ROOT files into one file: used by
// fit_gain_map.sh to keep the spot and all-channel spectra of a finished gain map
// (<scan>_gain_map_spectra.root, read by gain_map_gif.C) before it removes the chunk files.
//   root -b -q 'merge_spectra.C("files.txt", "out.root", "^q_run[0-9]+_(spot|all)$")'
// files.txt: one ROOT file per line. Names must be unique across the files (q_run<N>_... are).
#include <fstream>
#include "TFile.h"
#include "TKey.h"
#include "TH1.h"
#include "TPRegexp.h"

int merge_spectra(const char *list, const char *out, const char *pattern)
{
  std::ifstream in(list);
  if (!in) { printf("merge_spectra: no %s\n", list) ; return 1 ; }
  TPRegexp re(pattern);
  TFile fo(out, "RECREATE");
  if (fo.IsZombie()) { printf("merge_spectra: cannot write %s\n", out) ; return 1 ; }
  std::string name;
  long n = 0;
  while (std::getline(in, name)) {
    if (name.empty()) continue;
    TFile fi(name.c_str());
    if (fi.IsZombie()) { printf("merge_spectra: cannot read %s\n", name.c_str()) ; return 1 ; }
    for (auto *k : *fi.GetListOfKeys()) {
      TKey *key = (TKey *) k;
      if (!re.MatchB(key->GetName())) continue;
      TH1 *h = dynamic_cast<TH1 *>(key->ReadObj());
      if (!h) continue;
      fo.cd();
      h->Write(key->GetName());
      delete h;
      n++;
    }
  }
  fo.Close();
  printf("merge_spectra: %ld histograms in %s\n", n, out);
  return n > 0 ? 0 : 1;
}
