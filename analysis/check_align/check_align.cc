// Do the V1742's two DRS4 boards record the same laser pulse? Reads the group trigger time tags of
// each event: aligned = all four groups within 1 count of group 0 (the rule of tb::V1742Event's
// boards_aligned). A misaligned event is also checked against its neighbours: groups 2/3's tag equal
// to groups 0/1's of the next event (the 5 kHz pattern of 31 Oct 2026) or of the previous one.
//
//   check_align [--events N] [--min PCT] file.evt [file.evt ...]
//
// One line per file. --min: exit status 1 if any file has fewer than PCT % aligned events.
#include <Event/Event.h>
#include <Event/fileEventiterator.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const long long WRAP = 1LL << 30;    // the time tag is 30 bits
static const double TTT_NS = 200e3 / 23437.5; // one time-tag count, ns

static bool same_tag(long long a, long long b)
{
  long long d = ((a - b) % WRAP + WRAP) % WRAP;
  return d <= 1 || d >= WRAP - 1;
}

static int check(const char *file, long max_events, double min_pct)
{
  int status;
  fileEventiterator it(file, status);
  if (status) { fprintf(stderr, "%s: cannot open\n", file); return 2; }
  std::vector<long long> g0, g2;
  long n = 0, aligned = 0;
  Event *e;
  while ((max_events <= 0 || n < max_events) && (e = it.getNextEvent())) {
    if (e->getEvtType() == 1)
      if (Packet *p = e->getPacket(2000)) {
        long long t[4];
        bool ok = true;
        for (int g = 0; g < 4; g++) { t[g] = p->lValue(g, "GROUPTRIGGERTIME"); ok = ok && same_tag(t[g], t[0]); }
        aligned += ok;
        g0.push_back(t[0]);
        g2.push_back(t[2]);
        n++;
        delete p;
      }
    delete e;
  }
  if (n == 0) { printf("%s: no data events\n", file); return 2; }
  long next = 0, prev = 0;
  for (long i = 0; i < n; i++) {
    if (same_tag(g2[i], g0[i])) continue;
    if (i + 1 < n && same_tag(g2[i], g0[i + 1])) next++;
    else if (i > 0 && same_tag(g2[i], g0[i - 1])) prev++;
  }
  long long span = 0;
  long steps = 0;
  for (long i = 1; i < n; i++) {
    long long d = ((g0[i] - g0[i - 1]) % WRAP + WRAP) % WRAP;
    if (d > 0 && d < WRAP / 4) { span += d; steps++; }
  }
  double rate = steps ? steps / (span * TTT_NS * 1e-9) : 0;
  double pct = 100.0 * aligned / n;
  printf("%s: %ld events, %.0f events/s; aligned %.1f %%, groups 2/3 on the next event's pulse %.1f %%, previous %.1f %%, "
         "other %.1f %%\n",
         file, n, rate, pct, 100.0 * next / n, 100.0 * prev / n, 100.0 * (n - aligned - next - prev) / n);
  return min_pct >= 0 && pct < min_pct;
}

int main(int argc, char **argv)
{
  long max_events = 0;
  double min_pct = -1;
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--events") && i + 1 < argc) max_events = atol(argv[++i]);
    else if (!strcmp(argv[i], "--min") && i + 1 < argc) min_pct = atof(argv[++i]);
    else { i = argc; break; }
  }
  if (i >= argc) { fprintf(stderr, "usage: check_align [--events N] [--min PCT] file.evt [file.evt ...]\n"); return 2; }
  for (; i < argc; i++) {
    int r = check(argv[i], max_events, min_pct);
    if (r > rc) rc = r;
  }
  return rc;
}
