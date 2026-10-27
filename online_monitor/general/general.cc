// online_monitor/general - live view of the V1742 data on the 417 test bench.
//
// Online, while rcdaq takes data:
//   ./general.sh
//   root [1] online()                  // rcdaqopen() + pstart() + draw(): the main canvas, refreshed every 2 s
//   root [2] draw_details() ; draw_noise()     // the other canvases, when wanted
// From a file (testing, or a look at a finished run):
//   ./general.sh /scratch2/data/hpDIRC/photonis/photonis-00000111-0000.evt
//   root [1] pstart() ; draw()         // runs through the file in the background, canvases refresh
//   root [1] prun(5000) ; draw()       // or a fixed number of events first
// Then any time: watch_channel(20) (-1: follow the laser again), set_waveform_mode("average"),
// set_waveform_time(-50, 150), set_cluster(8, 3, 1, 0.5), set_spot_window(1, 2.5), reset(), status(),
// save_plots("run111").
//
// Canvases:
//   main       (draw)          hit rate per pixel; for the watched channel: persistence (every
//                              sample of its hit events, scope style), average waveform with its neighbours and TR,
//                              charge spectrum (every event: pedestal + photoelectrons), hit time
//                              with its core sigma; trends over the last 30 min (10 s steps of
//                              data time, kept across runs): event rate, and for the watched
//                              channel its hits per event, mean pulse height and the mean time of
//                              its in-time hits (each channel's history is kept: a switch redraws it)
//   face       (draw_details)  mean pulse height of the hits per pixel, hit rate per pixel
//   clusters   (draw_details)  the latest cluster, cluster centroids, cluster size, summed charge
//                              (by cluster size), seed time with its core sigma, the neighbour -
//                              seed time of every candidate with the time cut; spot charge: the
//                              watched channel + its neighbours summed in every event (fixed window
//                              around the laser peak, pedestal included) with the single-photoelectron
//                              fit (Q1, gain, mu), log and above the pedestal; time vs pulse height
//                              of the watched channel and of the cluster seeds, with the mean time
//   waveforms  (draw_details)  one pad per DRS4 group: its 8 channels and its trigger trace (TR / 10)
//   fits       (draw_fits)     the two results for the watched channel: its hit-time fit (main peak,
//                              near shoulder, late peak; peak sigma and the rms of the rest; log and linear) and the single-photoelectron fit of its spot
//                              charge (log and above the pedestal), with the numbers; also in status()
//   noise      (draw_noise)    baseline rms per channel (TR0-3 as 32-35), mean of every event per
//                              pixel (pedestal face), trigger found per group, hit rate per channel
//                              (also the unmapped ones), frequency spectrum of the traces without
//                              a hit (every channel, and the noisiest one against the pixels' mean;
//                              fft_channel() picks another): for checks with the laser off
//
// The watched channel follows the laser: the channel with the most in-time hits (within 1 ns
// of the hit-time peak, so pickup and dark counts do not win), re-chosen every 500 events looked at.
// The face uses the detector's mapping + cabling from the db (the detector in the begin-run
// packet 910, else the one selected with testbench.sh use); without them, channels are laid
// out 8 per row, one row per DRS4 group.
// Masked V1742 inputs (MASKED_CHANNELS in db/testbench.conf, or mask_channels()) are left out of
// the faces, clusters and channel picking; the noise canvas still shows them.
// Time offsets: with db/v1742/time_offsets_<serial>_<speed>.conf (analysis/time_offsets), every
// CFD time has its input's offset taken off, so all inputs share one time axis from the first
// event; the group alignment below then only follows what is left (reported if > 0.2 ns).
// Clusters: the seed is the highest pulse (>= seed threshold) within 2 ns of the laser's timing
// peak (so pickup and dark counts do not seed); neighbours are hits within the
// range of the seed (pixels, square) and in time with it. Times are relative to each channel's
// own group trigger, and the two DRS4 boards still differ by ~2 ns there, so each group is first
// shifted to put its hit-time peak at 0 (from the data: every 200 events at first, then every 1000).
// The event loop (pstart) only fills; the canvases are redrawn by a timer in ROOT's main thread.

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "general.h"

#include "tb_canvas.h"
#include "tb_db.h"
#include "tb_fit.h"
#include "tb_pulse.h"
#include "tb_spe.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TF1.h>
#include <TGraph.h>
#include <TH1.h>
#include <TH2.h>
#include <TLatex.h>
#include <TLegend.h>
#include <TLine.h>
#include <TPaveText.h>
#include <TProfile.h>
#include <TProfile2D.h>
#include <TROOT.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TTimer.h>

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <mutex>
#include <sstream>
#include <vector>

using namespace std;
using tb::NCH;
using tb::NSIG;
using tb::NTRIG;

int init_done = 0;

static const int PACKET = 2000;
static const double TR_SCALE = 0.1;      // trigger traces (NIM, ~ -800 mV) drawn / 10
static const int TREND_EVERY = 10, TREND_KEEP = 1800;            // s of data time
static const int TREND_N = TREND_KEEP / TREND_EVERY + 2;         // points per trend graph

// --- settings ---------------------------------------------------------------------------
static tb::Cuts cuts = [] { tb::Cuts c ; c.sig_from = 17 ; c.sig_to = 35 ; return c ; }();
static double seed_mv = 5, neighbour_mv = 2, cluster_dt = 1.0, seed_window = 2.0;   // run 252: diagonals stay at chance down to 2 mV
static string masked_list = "-";          // "-": MASKED_CHANNELS from db/testbench.conf
static int cluster_range = 1;
static bool wave_average = false;
static double wave_min = -100, wave_max = 20;
static double wave_from = NAN, wave_to = NAN;   // ns from the group trigger; NaN = -10 .. signal window + 20
static bool reset_each_run = true;
static int watch_fixed = -1;                    // channel on the main canvas; -1 = follow the laser

static tb::V1742Event ev;
static tb::RunInfo info;
static tb::Db db;
static bool db_ok = false;

// --- face layout: channel -> pixel ---------------------------------------------------------------
static int pix_row[NSIG], pix_col[NSIG];
static int nrows = 4, ncols = 8;
static bool by_pixel = false;

static void layout()
{
  by_pixel = false;
  int maxr = 0, maxc = 0;
  for (int ch = 0; ch < NSIG; ch++) {
    pix_row[ch] = pix_col[ch] = -1;
    int r, c;
    if (db_ok && db.pixel(PACKET, ch, r, c)) { pix_row[ch] = r ; pix_col[ch] = c ; by_pixel = true ; maxr = max(maxr, r) ; maxc = max(maxc, c) ; }
  }
  if (by_pixel) {
    nrows = max(int(db.num("NROWS", 0)), maxr + 1);
    ncols = max(int(db.num("NCOLS", 0)), maxc + 1);
  } else {
    for (int ch = 0; ch < NSIG; ch++) { pix_row[ch] = ch / 8 ; pix_col[ch] = ch % 8 ; }
    nrows = 4 ; ncols = 8;
  }
  // masked inputs: off the face, out of clusters and channel picking
  for (int ch : tb::masked_channels(masked_list)) if (ch >= 0 && ch < NSIG) pix_row[ch] = pix_col[ch] = -1;
}

// --- histograms -------------------------------------------------------------------------------
static bool booked = false;
static TProfile2D *face_amp = nullptr, *face_occ = nullptr, *face_all = nullptr;
static TH2F *clu_last = nullptr, *clu_centroid = nullptr;
// time vs pulse height (pulses down to the neighbour threshold): per channel, the watched one's
// copy, and the cluster seeds; with the mean time per pulse-height bin
static TH2F *h_tamp[NSIG], *w_tamp = nullptr, *clu_tamp = nullptr;
static TProfile *w_tamp_px = nullptr, *clu_tamp_px = nullptr;
static TH1F *clu_size = nullptr, *clu_q = nullptr, *clu_t = nullptr, *clu_dt = nullptr;
static TH1F *clu_qn[3];                  // summed charge by cluster size: 1, 2, 3+ pixels
// spot charge: the watched channel and its neighbours summed in every event, in a fixed window
// around each group's laser peak (pedestal included), fitted with lib/tb_spe.h's model
static double spot_before = 0.8, spot_after = 2.0;   // window [ns] before / after the laser's CFD peak
static TH1F *spot_q = nullptr;
static TF1 *spot_fn = nullptr;           // drawn; the fit's parameters are copied in
static TF1 *spot_term[4] = {};           // its 0 (pedestal), 1, 2, 3 photoelectron terms, dashed
static TLegend *spot_leg = nullptr;
static tb::SpeFit spot_fit;
static tb::SpeModel spot_model = tb::SPE_POLYA;   // the model spot_fn draws
static void make_spot_terms()            // (again when the model changes)
{
  static const int col[4] = {kGray + 2, kBlue + 1, kGreen + 2, kMagenta + 1};
  for (int n = 0; n < 4; n++) {
    delete spot_term[n];
    spot_term[n] = tb::spe_function(Form("spot_spe_%dpe", n), -0.2, 8.0, spot_model, n);
    spot_term[n]->SetNpx(3000) ; spot_term[n]->SetLineColor(col[n]) ; spot_term[n]->SetLineStyle(2) ; spot_term[n]->SetLineWidth(2);
  }
  if (!spot_leg) {
    spot_leg = new TLegend(0.70, 0.38, 0.90, 0.72);
    spot_leg->SetBorderSize(0) ; spot_leg->SetFillStyle(0);
  }
  spot_leg->Clear();
  spot_leg->AddEntry(spot_fn, "fit", "l");
  const char *name[4] = {"pedestal", "1 p.e.", "2 p.e.", "3 p.e."};
  for (int n = 0; n < 4; n++) spot_leg->AddEntry(spot_term[n], name[n], "l");
}
static TH1F *h_tgroup[NTRIG];            // hit times per group, to line the groups up
static double grp_off[NTRIG], t_peak = NAN;   // group offsets; hit-time peak after lining up
static bool grp_ok[NTRIG];               // offset measured (the group has seen laser light), or known from the db
// per-input time offsets from db/v1742 (analysis/time_offsets), taken off every CFD time; the
// group alignment then only measures what is left (should be ~0: else cables changed)
static tb::TimeOffsets toff;
static int toff_serial = -2, toff_freq = -2;
static bool toff_warned[NTRIG];
static TGraph *g_wave[NCH];              // latest event
static TProfile *p_wave[NCH];            // average of the hit events
static TH1F *wave_frame[NTRIG];
static TH1F *h_qch[NSIG], *h_tch[NSIG];  // per channel: charge of every event, hit time
// hit times for the timing pads and fit: every hit, or with set_timing_amplitude() only pulses of
// those heights (bigger pulses come later: a narrow window gives the sharper peak, for a closer look)
static TH1F *h_tcut[NSIG];
static double tamp_lo = 0, tamp_hi = INFINITY;   // no window: every hit above the hit threshold
static string tamp_text() { return std::isinf(tamp_hi) ? Form("hits > %g mV", cuts.threshold) : Form("pulses %g-%g mV", tamp_lo, tamp_hi) ; }
static long n_events = 0, n_clusters = 0;
static chrono::steady_clock::time_point last_wave, last_cluster;

// main canvas: the watched channel (copies of its histograms, refreshed from the event loop)
static const int PICK_EVERY = 500;       // events looked at between choices of the watched channel
static double intime[NSIG];              // in-time hits per channel, halved at each choice
static int watched = -1;
static bool placeholder = false;         // watched is only a stand-in until the laser is found
static bool watched_changed = false;
static TH2F *persist = nullptr;          // copy of the watched channel's p_persist
static TH2F *p_persist[NSIG];            // per channel: every sample of its hit events
static TH1F *w_q = nullptr, *w_t = nullptr, *w_frame = nullptr;
static const int NW = 10;                // watched + up to 8 neighbours + its TR
static TProfile *w_avg[NW];
static int w_avg_ch[NW];

// trends: one point every TREND_EVERY s of data time
static TGraph *trend[4];
static TH1F *trend_frame[4];
struct TrendPt { double t, rate ; float occ[NSIG], amp[NSIG], time[NSIG] ; };   // per channel: hits / event, mean pulse height, mean in-time hit time
static vector<TrendPt> trend_pts;
static TLatex *trend_label[4];
// events = the ones looked at; daq_events = the ones taken (from the event numbers: online, the
// monitor only sees a sample when the DAQ is faster than it)
static struct Interval { time_t start = 0 ; long events = 0, daq_events = 0, hits[NSIG] = {}, timed[NSIG] = {} ; double amp[NSIG] = {}, time[NSIG] = {} ; } iv;
static int last_seq = -1;
static double last_rate = NAN;
static time_t last_time = 0;             // data time of the latest event

// noise canvas
static TProfile *noise_rms = nullptr, *noise_hits = nullptr, *trig_found = nullptr;
// frequency spectrum of the traces without a hit (every FFT_EVERY-th event): mean amplitude of
// each frequency [mV], Hann window, the first fft_n samples (a power of 2)
static const int FFT_EVERY = 10;
static int fft_n = 0, fft_ch = -1;       // fft_ch: channel drawn on its own; -1 = the noisiest
static double fft_df = 0;                // MHz per bin
static TProfile2D *noise_fft = nullptr;
static TH1F *fft_one = nullptr, *fft_mean = nullptr;

static double trig_of(int ch) { return ch < NSIG ? ev.trigger_of(ch) : ev.trig[ch - NSIG]; }
static void wave_xrange();

static const char *xtitle() { return by_pixel ? "col" : "channel % 8"; }
static const char *ytitle() { return by_pixel ? "row" : "DRS4 group"; }

static void face_titles()
{
  face_amp->SetTitle(Form("%s: mean pulse height;%s;%s;mV", db_ok ? db.detector.c_str() : "?", xtitle(), ytitle()));
  face_occ->SetTitle(Form("hit rate (> %.0f mV);%s;%s;hits / event", cuts.threshold, xtitle(), ytitle()));
  noise_hits->SetTitle(Form("hit rate (> %.0f mV);channel;hits / event", cuts.threshold));
}

static void cluster_titles()
{
  clu_size->SetTitle(Form("cluster size (seed %g, neighbours %g mV);pixels;clusters", seed_mv, neighbour_mv));
}

// trace_ns: trace length; wl..wh: waveform range relative to the group trigger [ns]; nsps: ns per sample
static void book(double trace_ns, double wl, double wh, double nsps)
{
  layout();
  const char *xt = xtitle(), *yt = ytitle();
  double xl = -0.5, xh = ncols - 0.5, yl = -0.5, yh = nrows - 0.5;
  face_amp = new TProfile2D("face_amp", "", ncols, xl, xh, nrows, yl, yh);
  face_occ = new TProfile2D("face_occ", "", ncols, xl, xh, nrows, yl, yh);
  face_all = new TProfile2D("face_all", Form("mean pulse height, every event;%s;%s;mV", xt, yt),
                            ncols, xl, xh, nrows, yl, yh);
  noise_hits = new TProfile("noise_hits", "", NSIG, -0.5, NSIG - 0.5);
  face_titles();
  clu_last = new TH2F("cluster_last", Form("latest cluster;%s;%s;mV", xt, yt), ncols, xl, xh, nrows, yl, yh);
  clu_last->SetMarkerSize(2.5);   // size of the mV numbers
  clu_centroid = new TH2F("cluster_centroid", Form("cluster centroids;%s;%s;clusters", xt, yt),
                          4 * ncols, xl, xh, 4 * nrows, yl, yh);
  clu_size = new TH1F("cluster_size", "", 9, 0.5, 9.5);
  cluster_titles();
  clu_q = new TH1F("cluster_charge", "cluster charge;charge [pC];clusters", 205, -0.05, 4.05);
  for (int i = 0; i < 3; i++) clu_qn[i] = new TH1F(Form("cluster_charge_%d", i + 1), "", 205, -0.05, 4.05);
  spot_q = new TH1F("spot_charge", "", 1640, -0.2, 8.0);   // 5 fC bins; multi-photon runs reach several pC
  spot_fn = tb::spe_function("spot_spe", -0.2, 8.0, spot_model);
  spot_fn->SetNpx(3000) ; spot_fn->SetLineColor(kRed) ; spot_fn->SetLineWidth(2);
  spot_fn->SetParameters(0, 0.01, 0.3, 0.1, 0.1, 0, 8);   // N = 0: flat until the first fit
  spot_fn->SetParameter(7, 0) ; spot_fn->SetParameter(8, 1);
  make_spot_terms();
  clu_q->SetLineColor(kBlack);
  clu_qn[0]->SetLineColor(kGreen + 2) ; clu_qn[1]->SetLineColor(kAzure + 1) ; clu_qn[2]->SetLineColor(kRed + 1);
  double tr = trace_ns, tlo = max(cuts.sig_from, -tr), thi = min(cuts.sig_to, tr);
  clu_t = new TH1F("cluster_time", "seed time;t [ns];clusters",
                   max(10, int((thi - tlo) / 0.05)), tlo, thi);
  clu_dt = new TH1F("cluster_dt", "neighbour - seed time;#Deltat [ns];neighbours", 200, -5, 5);
  for (int g = 0; g < NTRIG; g++) {
    h_tgroup[g] = new TH1F(Form("tgroup_%d", g), "", max(10, int((thi - tlo) / 0.05)), tlo, thi);
    h_tgroup[g]->SetDirectory(nullptr);
    grp_off[g] = 0;
  }
  for (int ch = 0; ch < NSIG; ch++) {
    h_qch[ch] = new TH1F(Form("q_%02d", ch), Form("ch %d charge, every event;charge [pC];events", ch), 400, -0.2, 3.0);
    h_tch[ch] = new TH1F(Form("t_%02d", ch), Form("ch %d hit time - group trigger;t [ns];hits", ch), max(10, int((thi - tlo) / 0.01)), tlo, thi);
    h_tcut[ch] = new TH1F(Form("tcut_%02d", ch), "", max(10, int((thi - tlo) / 0.01)), tlo, thi);
    h_tcut[ch]->SetDirectory(nullptr);
  }

  // waveforms: time relative to the group trigger
  int nb = max(10, int((wh - wl) / nsps));
  static const int colors[8] = {kRed + 1, kOrange + 7, kYellow + 2, kGreen + 2, kCyan + 2, kBlue + 1, kViolet + 1, kMagenta + 1};
  for (int ch = 0; ch < NCH; ch++) {
    int col = ch < NSIG ? colors[ch % 8] : kBlack;
    g_wave[ch] = new TGraph(max(1, int(trace_ns / nsps + 0.5)));
    g_wave[ch]->SetLineColor(col);
    g_wave[ch]->SetLineWidth(2);
    p_wave[ch] = new TProfile(Form("wave_avg_%02d", ch), "", nb, wl, wh);
    p_wave[ch]->SetLineColor(col);
    p_wave[ch]->SetLineWidth(2);
  }
  for (int g = 0; g < NTRIG; g++) {
    wave_frame[g] = new TH1F(Form("wave_frame_%d", g), "", 100, wl, wh);
    wave_frame[g]->SetMinimum(wave_min);
    wave_frame[g]->SetMaximum(wave_max);
  }

  int ntb = max(10, int((thi - tlo) / 0.02));
  for (int ch = 0; ch < NSIG; ch++) {
    h_tamp[ch] = new TH2F(Form("tamp_%02d", ch), "", 80, 0, 80, ntb, tlo, thi);
    h_tamp[ch]->SetDirectory(nullptr);
  }
  w_tamp = new TH2F("watch_tamp", "", 80, 0, 80, ntb, tlo, thi);
  clu_tamp = new TH2F("cluster_tamp", "seed time vs pulse height;pulse height [mV];t [ns];clusters", 80, 0, 80, ntb, tlo, thi);
  w_tamp_px = new TProfile("watch_tamp_px", "", 80, 0, 80);
  clu_tamp_px = new TProfile("cluster_tamp_px", "", 80, 0, 80);
  for (auto *p : {w_tamp_px, clu_tamp_px}) { p->SetLineColor(kBlack) ; p->SetMarkerColor(kBlack) ; p->SetMarkerStyle(20) ; p->SetMarkerSize(0.5) ; }

  // main canvas
  persist = new TH2F("persistence", "", nb, wl, wh, 150, wave_min, wave_max);
  for (int ch = 0; ch < NSIG; ch++) {
    p_persist[ch] = new TH2F(Form("persistence_%02d", ch), "", nb, wl, wh, 150, wave_min, wave_max);
    p_persist[ch]->SetDirectory(nullptr);
  }
  w_q = new TH1F("watch_q", "", 400, -0.2, 3.0);
  w_t = new TH1F("watch_t", "", max(10, int((thi - tlo) / 0.01)), tlo, thi);
  w_frame = new TH1F("watch_frame", "", 100, wl, wh);
  w_frame->SetMinimum(wave_min);
  w_frame->SetMaximum(wave_max);
  for (int k = 0; k < NW; k++) { w_avg[k] = new TProfile(Form("watch_avg_%d", k), "", nb, wl, wh) ; w_avg_ch[k] = -1 ; }
  for (int i = 0; i < 4; i++) {
    trend_frame[i] = new TH1F(Form("trend_frame_%d", i), "", 1, 0, 1);
    trend_frame[i]->SetDirectory(nullptr);
    TAxis *a = trend_frame[i]->GetXaxis();
    a->SetTimeDisplay(1) ; a->SetTimeFormat("%H:%M") ; a->SetTimeOffset(0, "local");
    a->SetLabelSize(0.13) ; trend_frame[i]->GetYaxis()->SetLabelSize(0.13) ; trend_frame[i]->GetYaxis()->SetNdivisions(504);
    trend[i] = new TGraph(TREND_N);       // fixed size, never reallocated while drawn
    trend[i]->SetLineColor(kBlue + 1) ; trend[i]->SetMarkerColor(kBlue + 1) ; trend[i]->SetMarkerStyle(20) ; trend[i]->SetMarkerSize(0.4);
  }

  // noise canvas
  noise_rms = new TProfile("noise_rms", "baseline rms (32-35: TR0-3);channel;mV", NCH, -0.5, NCH - 0.5);
  trig_found = new TProfile("trigger_found", "trigger found;DRS4 group;fraction of events", NTRIG, -0.5, NTRIG - 0.5);
  for (fft_n = 2; 2 * fft_n <= int(trace_ns / nsps + 0.5); fft_n *= 2) {}
  fft_df = 1000.0 / (fft_n * nsps);
  int nf = fft_n / 2;
  noise_fft = new TProfile2D("noise_fft", "noise spectrum;frequency [MHz];channel;mean amplitude [mV]", nf, 0.5 * fft_df, (nf + 0.5) * fft_df, NSIG, -0.5, NSIG - 0.5);
  fft_one = new TH1F("noise_fft_one", "", nf, 0.5 * fft_df, (nf + 0.5) * fft_df);
  fft_mean = new TH1F("noise_fft_mean", "", nf, 0.5 * fft_df, (nf + 0.5) * fft_df);
  fft_one->SetLineColor(kRed + 1) ; fft_one->SetLineWidth(2) ; fft_mean->SetLineColor(kGray + 2);
  booked = true;
  wave_xrange();
}

// book once, from whichever comes first: the first event (event loop) or a canvas (prompt)
static std::mutex book_mutex;
static void ensure_booked(double trace_ns, double wl, double wh, double nsps)
{
  std::lock_guard<std::mutex> lock(book_mutex);
  if (booked) return;
  if (!db_ok) {
    db_ok = db.load("");
    printf("general: no begin-run event seen: detector %s (testbench.sh use)\n", db_ok ? db.detector.c_str() : "unknown");
  }
  book(trace_ns, wl, wh, nsps);
}

// before any data: the trace from the selected DAQ profile, and a waveform range wide enough for
// any trigger position (the canvases are empty until events come in)
static void book_from_profile()
{
  string root = tb::repo_root(), daq = tb::read_conf(root + "/state/active.conf")["DAQ"];
  auto prof = tb::read_conf(root + "/db/daq/" + daq + ".conf");
  int f = atoi(prof["FREQ_CODE"].c_str()), n = atoi(prof["RECORD_LENGTH"].c_str());
  static const double nsps[] = {0.2, 0.4, 1.0};
  double dt = nsps[(f >= 0 && f <= 2) ? f : 0], tr = (n > 0 ? n : 1024) * dt;
  ensure_booked(tr, -0.5 * tr, tr, dt);
  printf("general: no events yet: canvases set up from DAQ profile %s (%.0f ns traces); they fill as data come in\n",
         daq.empty() ? "?" : daq.c_str(), tr);
}

static void wave_xrange()
{
  double lo = std::isnan(wave_from) ? -10 : wave_from, hi = std::isnan(wave_to) ? cuts.sig_to + 20 : wave_to;
  for (int g = 0; g < NTRIG; g++) wave_frame[g]->GetXaxis()->SetRangeUser(lo, hi);
  w_frame->GetXaxis()->SetRangeUser(lo, hi);
  persist->GetXaxis()->SetRangeUser(lo, hi);
}

static void wave_titles()
{
  for (int g = 0; g < NTRIG; g++)
    wave_frame[g]->SetTitle(Form("ch %d-%d + TR%d / 10 (%s);t - group trigger [ns];mV", 8 * g, 8 * g + 7, g, wave_average ? "average" : "latest"));
}

void reset()
{
  if (!booked) return;
  face_amp->Reset() ; face_occ->Reset() ; face_all->Reset();
  clu_last->Reset() ; clu_centroid->Reset() ; clu_size->Reset() ; clu_q->Reset() ; for (auto *h : clu_qn) h->Reset() ; spot_q->Reset() ; spot_fit = tb::SpeFit() ; clu_t->Reset() ; clu_dt->Reset();
  for (int ch = 0; ch < NCH; ch++) p_wave[ch]->Reset();
  for (int ch = 0; ch < NSIG; ch++) { h_qch[ch]->Reset() ; h_tch[ch]->Reset() ; h_tcut[ch]->Reset() ; }
  persist->Reset() ; w_q->Reset() ; w_t->Reset();
  for (int ch = 0; ch < NSIG; ch++) { p_persist[ch]->Reset() ; h_tamp[ch]->Reset() ; }
  w_tamp->Reset() ; clu_tamp->Reset();
  for (int k = 0; k < NW; k++) w_avg[k]->Reset();
  noise_rms->Reset() ; noise_hits->Reset() ; trig_found->Reset() ; noise_fft->Reset();
  n_events = n_clusters = 0;                // the trends and the group time alignment go on
}

// two-peak timing fit (lib/tb_fit.h) of a time histogram: the model and its main peak are kept
// for drawing on it, the axis zoomed onto the peak; returns the numbers for the title
struct TimeFitDraw { TF1 *all = nullptr, *main = nullptr ; tb::TimingFit fit ; };
static TimeFitDraw wt_fit, ct_fit;
static string apply_timing_fit(TH1 *h, TimeFitDraw &d)
{
  if (!d.all) {
    d.all = tb::timing_function(Form("%s_fit", h->GetName())) ; d.main = tb::timing_function(Form("%s_main", h->GetName()));
    d.all->SetLineColor(kRed) ; d.all->SetLineWidth(2) ; d.all->SetNpx(600);
    d.main->SetLineColor(kBlue + 1) ; d.main->SetLineStyle(2) ; d.main->SetLineWidth(2) ; d.main->SetNpx(600);
  }
  tb::TimingFit f = tb::timing_fit(h);
  d.fit = f;
  if (!f.ok) {
    for (TF1 *fn : {d.all, d.main}) for (int k = 0; k < 10; k++) fn->SetParameter(k, k == 2 || k == 5 || k == 8 ? 1 : 0);
    h->GetXaxis()->SetRange();
    return "";
  }
  d.all->SetRange(f.lo, f.hi) ; d.all->SetParameters(f.par);
  d.main->SetRange(f.lo, f.hi) ; d.main->SetParameters(f.par) ; d.main->SetParameter(3, 0) ; d.main->SetParameter(6, 0);   // main peak + background
  h->GetXaxis()->SetRangeUser(f.mean - 1, f.mean + 3);
  if (!f.two) return Form("#sigma %.0f ps (one Gaussian)", 1000 * f.sigma);
  return Form("#sigma %.0f ps, rest %.0f %% (rms %.0f ps)", 1000 * f.sigma, 100 * f.rest_frac, 1000 * f.rest_rms);
}

// --- the watched channel ---------------------------------------------------------------------
// the event thread (pstart) only raises this flag; the copies and the fit are made in the main
// thread (timer, draw_...(), status, save_plots): ROOT's fitting is not safe from two threads
static std::atomic<bool> watched_dirty{false};
static void refresh_watched()
{
  watched_dirty = false;
  if (watched < 0) return;
  persist->Reset() ; persist->Add(p_persist[watched]);
  w_tamp->Reset() ; w_tamp->Add(h_tamp[watched]);
  w_tamp->SetTitle(Form("ch %d time vs pulse height;pulse height [mV];t [ns];pulses", watched));
  w_q->Reset() ; w_q->Add(h_qch[watched]);
  w_t->Reset() ; w_t->Add(h_tcut[watched]);
  for (int k = 0; k < NW; k++) if (w_avg_ch[k] >= 0) { w_avg[k]->Reset() ; w_avg[k]->Add(p_wave[w_avg_ch[k]]) ; }
  const char *axis = toff.ok ? "t - group trigger - db offset [ns]" : "t - group trigger [ns]";
  apply_timing_fit(w_t, wt_fit);            // drawn on the fits canvas only; here it just zooms
  w_t->SetTitle(Form("ch %d hit time, %s;%s;hits", watched, tamp_text().c_str(), axis));
}

static void set_watched(int ch)
{
  if (ch == watched) return;
  watched = ch;
  int k = 0;
  w_avg_ch[k++] = ch;
  for (int n = 0; n < NSIG && k < NW - 1; n++)   // neighbours on the face
    if (n != ch && pix_row[n] >= 0 && pix_row[ch] >= 0 && abs(pix_row[n] - pix_row[ch]) <= 1 && abs(pix_col[n] - pix_col[ch]) <= 1)
      w_avg_ch[k++] = n;
  w_avg_ch[k++] = NSIG + ch / 8;                // its group's trigger trace
  for (; k < NW; k++) w_avg_ch[k] = -1;
  spot_fit = tb::SpeFit();                      // another set of pixels
  if (!spot_q) { watched_changed = true ; return ; }
  spot_q->Reset();
  int npx = 0;
  for (int c : w_avg_ch) if (c >= 0 && c < NSIG) npx++;
  spot_q->SetTitle(Form("spot charge (ch %d + %d);charge [pC];events", ch, npx - 1));
  static const int colors[8] = {kOrange + 7, kYellow + 2, kGreen + 2, kCyan + 2, kBlue + 1, kViolet + 1, kMagenta + 1, kGray + 1};
  for (k = 0; k < NW; k++) {
    int c = w_avg_ch[k];
    w_avg[k]->SetLineColor(k == 0 ? kRed + 1 : c >= NSIG ? kBlack : colors[(k - 1) % 8]);
    w_avg[k]->SetLineWidth(k == 0 ? 3 : 2);
  }
  string px = pix_row[ch] >= 0 && by_pixel ? Form(" (pixel %d,%d)", pix_row[ch], pix_col[ch]) : "";
  persist->SetTitle(Form("ch %d%s hit waveforms;t - group trigger [ns];mV;samples", ch, px.c_str()));
  w_frame->SetTitle(Form("ch %d + neighbours, mean waveform;t - group trigger [ns];mV", ch));
  w_q->SetTitle(Form("ch %d charge;charge [pC];events", ch));
  watched_changed = true;                   // the timer redraws the waveform pad and its legend
  watched_dirty = true;                     // and refreshes the copies
}

static void choose_watched()
{
  if (watch_fixed >= 0) { set_watched(watch_fixed) ; return ; }
  if (std::isnan(t_peak)) return;           // no laser peak yet: in time means nothing
  int best = -1;
  for (int ch = 0; ch < NSIG; ch++)
    if (pix_row[ch] >= 0 && (best < 0 || intime[ch] > intime[best])) best = ch;
  // switch only on a clear winner: two pixels sharing the light (a corner) would otherwise swap
  // back and forth, and each swap restarts the spot charge
  double lead = watched >= 0 ? intime[best] - intime[watched] : 0;
  if (best >= 0 && intime[best] >= 5 &&
      (watched < 0 || placeholder || (lead > 0.2 * intime[watched] && lead > 3 * sqrt(intime[best] + intime[watched])))) {
    placeholder = false;
    if (best != watched) { set_watched(best) ; printf("general: main canvas on ch %d (most in-time hits)\n", best) ; }
  }
  for (int ch = 0; ch < NSIG; ch++) intime[ch] *= 0.5;   // a memory of ~1000 events
}

void fft_channel(int ch) { fft_ch = (ch >= 0 && ch < NSIG) ? ch : -1 ; }

void watch_channel(int ch)
{
  if (ch >= NSIG) { printf("general: channels are 0-%d\n", NSIG - 1) ; return ; }
  watch_fixed = ch;
  if (booked && ch >= 0) set_watched(ch);
  else if (booked) watched_changed = true;
  printf("general: main canvas %s\n", ch >= 0 ? Form("on ch %d", ch) : "follows the laser (most in-time hits)");
}

// --- trends ---------------------------------------------------------------------------------
// the graphs: the event rate, and the watched channel's history (redrawn when it changes)
static void rebuild_trends()
{
  int ch = watched >= 0 ? watched : 0;
  static const char *tt[4] = {"event rate [Hz]", "ch %d: hits per event", "ch %d: mean pulse height of its hits [mV]",
                              "ch %d: mean time of its in-time hits [ns]"};
  for (int i = 0; i < 4; i++) if (trend_label[i]) trend_label[i]->SetTitle(Form((string(tt[i]) + ", last %d min").c_str(), i ? ch : TREND_KEEP / 60, TREND_KEEP / 60));
  if (trend_pts.empty() || !trend[0]) return;
  double now = trend_pts.back().t, x0 = trend_pts.front().t, x1 = max(now, x0 + 60);
  for (int i = 0; i < 4; i++) {
    int k = 0;
    double lo = 1e300, hi = -1e300, lx = 0, ly = 0;
    for (auto &p : trend_pts) {
      double y = i == 0 ? p.rate : i == 1 ? p.occ[ch] : i == 2 ? p.amp[ch] : p.time[ch];
      if (std::isnan(y)) continue;
      lx = p.t ; ly = y;
      trend[i]->SetPoint(k++, lx, ly);
      lo = min(lo, ly) ; hi = max(hi, ly);
    }
    if (!k) continue;
    for (; k < TREND_N; k++) trend[i]->SetPoint(k, lx, ly);   // unused points sit on the last one
    double m = hi > lo ? 0.15 * (hi - lo) : max(0.05 * fabs(hi), 1e-3);
    trend_frame[i]->SetMinimum(lo - m);
    trend_frame[i]->SetMaximum(hi + m);
    trend_frame[i]->GetXaxis()->SetLimits(x0 - 5, x1 + 5);
  }
}

static void trend_point(time_t now)
{
  double dt = now - iv.start;
  last_rate = iv.daq_events / dt;
  TrendPt p;
  p.t = now ; p.rate = last_rate;
  for (int ch = 0; ch < NSIG; ch++) {
    p.occ[ch] = iv.events ? double(iv.hits[ch]) / iv.events : NAN;
    p.amp[ch] = iv.hits[ch] ? iv.amp[ch] / iv.hits[ch] : NAN;
    p.time[ch] = iv.timed[ch] ? iv.time[ch] / iv.timed[ch] : NAN;
  }
  trend_pts.push_back(p);
  while (trend_pts.size() > TREND_N || trend_pts.front().t < now - TREND_KEEP) trend_pts.erase(trend_pts.begin());
  rebuild_trends();
}

// --- settings ----------------------------------------------------------------------------
void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; if (booked) { face_titles() ; printf("general: the time histograms keep their range: restart for a new one\n") ; } }
void set_threshold(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; if (booked) { face_titles() ; cluster_titles() ; } }
void set_charge_window(double a, double b) { cuts.q_from = a ; cuts.q_to = b ; }
void set_cluster(double seed, double neighbour, int range, double dt, double seed_window_ns)
{
  seed_mv = seed ; neighbour_mv = neighbour ; cluster_range = range ; cluster_dt = dt ; seed_window = seed_window_ns;
  if (neighbour < cuts.threshold) printf("general: neighbours also need the hit threshold, %.1f mV (set_threshold)\n", cuts.threshold);
  if (booked) cluster_titles();
}
void set_reset_each_run(bool on) { reset_each_run = on ; }
void set_timing_amplitude(double lo, double hi)
{
  tamp_lo = lo ; tamp_hi = hi > lo ? hi : INFINITY;   // (0, 0): every hit
  if (booked) for (auto *h : h_tcut) h->Reset();   // collected again with the new window
  watched_dirty = true;
}

void mask_channels(const char *list)
{
  masked_list = list;
  if (booked) layout();      // from the next event on; reset() clears what masked channels filled before
  printf("general: masked V1742 inputs: %s\n", masked_list == "-" ? "MASKED_CHANNELS from db/testbench.conf" : (*list ? list : "none"));
}

// --- per event ----------------------------------------------------------------------------
int pinit()
{
  if (init_done) return 1;
  init_done = 1;
  return 0;
}

static void begin_run(Event *e)
{
  info.parse(e);
  ev.serial = info.serial;   // calibration file of this run's digitiser
  if (!db_ok || (!info.detector.empty() && info.detector != db.detector)) {
    tb::Db d;
    bool ok = d.load(info.detector);
    if (booked && ok && d.detector != db.detector)
      printf("general: run %d is detector %s, the face was made for %s: restart the monitor\n", info.run, d.detector.c_str(), db.detector.c_str());
    else { db = d ; db_ok = ok ; }
  }
  if (booked && reset_each_run) reset();
  printf("general: run %d, detector %s\n", info.run, db_ok ? db.detector.c_str() : "unknown");
}

// the 0.25 ns window of a hit-time histogram holding the most hits: laser light is sharp in time,
// noise and pickup are spread over the signal window. Returns the hits in it, its centre in at_ns.
static double peak_window(TH1 *h, double &at_ns)
{
  int nw = max(1, int(0.25 / h->GetBinWidth(1) + 0.5));
  double w = 0;
  at_ns = NAN;
  for (int b = 1; b + nw - 1 <= h->GetNbinsX(); b++) {
    double s = h->Integral(b, b + nw - 1);
    if (s > w) { w = s ; at_ns = h->GetXaxis()->GetBinLowEdge(b) + 0.125 ; }
  }
  return w;
}

// amplitude spectrum [mV] of one trace: the first n (power of 2) samples, mean removed, Hann window
static void amplitude_spectrum(const double *v, int n, vector<double> &amp)
{
  vector<complex<double>> x(n);
  double mean = 0, wsum = 0;
  for (int i = 0; i < n; i++) mean += v[i];
  mean /= n;
  for (int i = 0; i < n; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)) ; x[i] = w * (v[i] - mean) ; wsum += w ; }
  for (int i = 1, j = 0; i < n; i++) {                 // bit reversal
    int b = n >> 1;
    for (; j & b; b >>= 1) j ^= b;
    j ^= b;
    if (i < j) swap(x[i], x[j]);
  }
  for (int len = 2; len <= n; len <<= 1) {            // radix-2 butterflies
    complex<double> wl = polar(1.0, -2 * M_PI / len);
    for (int i = 0; i < n; i += len) {
      complex<double> w = 1;
      for (int k = 0; k < len / 2; k++) { complex<double> a = x[i + k], b = w * x[i + k + len / 2] ; x[i + k] = a + b ; x[i + k + len / 2] = a - b ; w *= wl ; }
    }
  }
  amp.resize(n / 2);
  for (int k = 1; k <= n / 2; k++) amp[k - 1] = 2 * abs(x[k]) / wsum;   // a sine of amplitude A gives A
}

static void spot_charge()
{
  if (watched < 0 || placeholder || std::isnan(t_peak)) return;
  double sum = 0;
  for (int k = 0; k < NW; k++) {
    int ch = w_avg_ch[k];
    if (ch < 0 || ch >= NSIG) continue;
    double tg = ev.trigger_of(ch);
    if (std::isnan(tg) || !grp_ok[ch / 8]) return;     // a group without its laser peak: no window yet
    int lo, hi;
    double pk = t_peak + grp_off[ch / 8] + toff.ns[ch];   // back on the input's own time axis
    tb::window_around(ev.t[ch], ev.ns, tg, pk - spot_before, pk + spot_after, lo, hi);
    if (hi - lo < 2) return;
    sum += tb::charge_pC(ev.t[ch], ev.v[ch], lo, hi);
  }
  spot_q->Fill(sum);
}

// shift each group so that its hit-time peak sits where the clearest group's does. A group's
// peak counts once its best 0.25 ns window holds >= 30 of its hits and >= 20 % of them;
// groups without one keep their offset. With the db time offsets taken off the times, what is
// left should be ~0; more than 0.2 ns is reported once.
static void align_groups()
{
  double peak[NTRIG], best = 0;
  int ref = -1;
  for (int g = 0; g < NTRIG; g++) {
    TH1F *h = h_tgroup[g];
    double at, w = peak_window(h, at), total = h->Integral();
    peak[g] = (w >= 30 && w >= 0.2 * total) ? at : NAN;
    if (!std::isnan(peak[g]) && w > best) { best = w ; ref = g ; }
  }
  if (ref < 0) return;
  for (int g = 0; g < NTRIG; g++) if (!std::isnan(peak[g])) { grp_off[g] = peak[g] - peak[ref] ; grp_ok[g] = true ; }
  t_peak = peak[ref];
  for (int g = 0; g < NTRIG; g++)
    if (toff.ok && !std::isnan(peak[g]) && fabs(grp_off[g]) > 0.2 && !toff_warned[g]) {
      toff_warned[g] = true;
      printf("general: group %d is %+.2f ns off after the db time offsets: cables or trigger changed? measure again (analysis/time_offsets)\n", g, grp_off[g]);
    }
}

// hit: the channels above the hit threshold (face, trends, waveforms); clusters take their
// neighbours from the times measured down to the neighbour threshold
static void cluster(const tb::ChannelHit *hit)
{
  int seed = -1;
  for (int ch = 0; ch < NSIG; ch++)
    if (hit[ch].hit && pix_row[ch] >= 0 && hit[ch].amp >= seed_mv && (seed < 0 || hit[ch].amp > hit[seed].amp) &&
        (std::isnan(t_peak) || fabs(hit[ch].t_cfd - grp_off[ch / 8] - t_peak) <= seed_window))   // in time with the laser
      seed = ch;
  if (seed < 0) return;
  double ts = hit[seed].t_cfd - grp_off[seed / 8];
  vector<int> members{seed};
  for (int ch = 0; ch < NSIG; ch++) {
    if (ch == seed || !hit[ch].hit || pix_row[ch] < 0 || hit[ch].amp < neighbour_mv) continue;
    if (max(abs(pix_row[ch] - pix_row[seed]), abs(pix_col[ch] - pix_col[seed])) > cluster_range) continue;
    double dt = hit[ch].t_cfd - grp_off[ch / 8] - ts;
    clu_dt->Fill(dt);
    if (fabs(dt) <= cluster_dt) members.push_back(ch);
  }
  double sa = 0, sr = 0, sc = 0, sq = 0;
  for (int ch : members) { double a = hit[ch].amp ; sa += a ; sr += a * pix_row[ch] ; sc += a * pix_col[ch] ; sq += hit[ch].q ; }
  clu_size->Fill(members.size());
  clu_q->Fill(sq);
  clu_qn[min<int>(members.size(), 3) - 1]->Fill(sq);
  clu_t->Fill(ts);
  clu_tamp->Fill(hit[seed].amp, ts);
  clu_centroid->Fill(sc / sa, sr / sa);
  n_clusters++;
  auto now = chrono::steady_clock::now();
  if (now - last_cluster >= chrono::seconds(1)) {   // the display shows one cluster a second
    last_cluster = now;
    clu_last->Reset();
    for (int ch : members) clu_last->Fill(pix_col[ch], pix_row[ch], hit[ch].amp);
    clu_last->SetTitle(Form("last cluster: ch %d, %zu px, %.2f pC;%s;%s;mV", seed, members.size(), sq, xtitle(), ytitle()));
  }
}

static void waveforms(const tb::ChannelHit *hit)
{
  for (int ch = 0; ch < NCH; ch++) {              // average: every hit, and every trigger trace
    if (ch < NSIG && !hit[ch].hit) continue;
    double tg = trig_of(ch), sc = ch < NSIG ? 1 : TR_SCALE;
    if (std::isnan(tg)) continue;
    for (int s = 0; s < ev.ns; s++) p_wave[ch]->Fill(ev.t[ch][s] - tg, sc * ev.v[ch][s]);
  }
  for (int ch = 0; ch < NSIG; ch++) {             // persistence: every hit event
    double tg = trig_of(ch);
    if (!hit[ch].hit || std::isnan(tg)) continue;
    for (int s = 0; s < ev.ns; s++) p_persist[ch]->Fill(ev.t[ch][s] - tg, ev.v[ch][s]);
  }
  auto now = chrono::steady_clock::now();
  if (now - last_wave < chrono::seconds(1)) return;   // latest: one event a second
  last_wave = now;
  for (int ch = 0; ch < NCH; ch++) {
    double tg = trig_of(ch), sc = ch < NSIG ? 1 : TR_SCALE;
    if (std::isnan(tg)) tg = 0;
    if (g_wave[ch]->GetN() != ev.ns) g_wave[ch]->Set(ev.ns);
    for (int s = 0; s < ev.ns; s++) g_wave[ch]->SetPoint(s, ev.t[ch][s] - tg, sc * ev.v[ch][s]);
  }
}

static void load_time_offsets()   // once per V1742 serial and sampling speed
{
  if (ev.serial == toff_serial && ev.freq == toff_freq) return;
  toff_serial = ev.serial ; toff_freq = ev.freq;
  for (bool &w : toff_warned) w = false;
  if (toff.load(ev.serial, ev.freq)) {
    printf("general: time offsets %s\n", toff.file.c_str());
    for (int g = 0; g < NTRIG; g++) if (toff.group_known[g]) grp_ok[g] = true;   // lined up from the start
  } else printf("general: no time offsets in db/v1742 for this V1742 and speed: groups lined up from the data only\n");
}

int process_event(Event *e)
{
  if (e->getEvtType() == 9) { begin_run(e) ; return 0 ; }
  if (!ev.fill(e)) return 0;
  load_time_offsets();
  if (!booked) {
    double tg = std::isnan(ev.trig[0]) ? 0 : ev.trig[0];
    ensure_booked(ev.trace_ns(), ev.t[0][0] - tg, ev.t[0][ev.ns - 1] - tg, ev.ns_per_sample);
  }
  n_events++;
  time_t now = e->getTime();
  if (now <= 0) now = time(nullptr);
  if (n_events == 1)
    for (int i = 0; i < 4; i++) trend_frame[i]->GetXaxis()->SetLimits(now - 5, now + 65);
  if (!iv.start) iv.start = now;
  last_time = now;
  iv.events++;
  int seq = e->getEvtSequence();
  iv.daq_events += (last_seq >= 0 && seq > last_seq) ? seq - last_seq : 1;   // 1 at a new run
  last_seq = seq;

  for (int g = 0; g < NTRIG; g++) trig_found->Fill(g, std::isnan(ev.trig[g]) ? 0 : 1);
  int bl_hi = min(ev.bl_last, ev.ns);
  for (int ch = 0; ch < NCH; ch++) noise_rms->Fill(ch, tb::rms(ev.v[ch], ev.bl_first, bl_hi));

  tb::Cuts tcuts = cuts;                   // times down to the neighbour threshold, for the clusters
  tcuts.threshold = tcuts.le_threshold = min(cuts.threshold, neighbour_mv);
  tb::ChannelHit hit[NSIG], timed[NSIG];
  for (int ch = 0; ch < NSIG; ch++) {
    timed[ch] = tb::measure(ev, ch, tcuts);
    timed[ch].t_cfd -= toff.ns[ch] ; timed[ch].t_le -= toff.ns[ch];   // 0 without db offsets
    hit[ch] = timed[ch];
    if (timed[ch].hit && timed[ch].amp >= max(tamp_lo, cuts.threshold) && timed[ch].amp < tamp_hi) h_tcut[ch]->Fill(timed[ch].t_cfd);
    if (timed[ch].hit && !std::isnan(timed[ch].t_cfd)) h_tamp[ch]->Fill(timed[ch].amp, timed[ch].t_cfd - grp_off[ch / 8]);
    if (hit[ch].hit && hit[ch].amp < cuts.threshold) { hit[ch].hit = false ; hit[ch].t_cfd = hit[ch].t_le = hit[ch].t_abs = NAN ; }
    const tb::ChannelHit &h = hit[ch];
    if (!h.valid) continue;
    noise_hits->Fill(ch, h.hit ? 1 : 0);
    h_qch[ch]->Fill(h.q);
    if (h.hit) {
      h_tch[ch]->Fill(h.t_cfd);
      double ta = h.t_cfd - grp_off[ch / 8];
      if (!std::isnan(t_peak) && fabs(ta - t_peak) < 1.0) intime[ch]++;
    }
    if (pix_row[ch] < 0) continue;
    face_occ->Fill(pix_col[ch], pix_row[ch], h.hit ? 1 : 0);
    face_all->Fill(pix_col[ch], pix_row[ch], h.amp);
    if (!h.hit) continue;
    face_amp->Fill(pix_col[ch], pix_row[ch], h.amp);
    h_tgroup[ch / 8]->Fill(h.t_cfd);
    iv.hits[ch]++ ; iv.amp[ch] += h.amp;
    double ta = h.t_cfd - grp_off[ch / 8];   // in time with the laser: dark counts and late pulses left out
    if (!std::isnan(ta) && !std::isnan(t_peak) && fabs(ta - t_peak) < 1.0) { iv.time[ch] += ta ; iv.timed[ch]++ ; }
  }
  if (n_events % FFT_EVERY == 0 && fft_n >= 8 && ev.ns >= fft_n) {
    vector<double> amp;
    for (int ch = 0; ch < NSIG; ch++) {
      if (!timed[ch].valid || timed[ch].hit) continue;
      amplitude_spectrum(ev.v[ch], fft_n, amp);
      for (int k = 0; k < fft_n / 2; k++) noise_fft->Fill((k + 1) * fft_df, ch, amp[k]);
    }
  }
  if (n_events % (n_events < 2000 ? 200 : 1000) == 0) align_groups();
  cluster(timed);
  spot_charge();
  waveforms(hit);

  if (n_events % PICK_EVERY == 0) choose_watched();
  if (n_events % 300 == 0) watched_dirty = true;
  if (now - iv.start >= TREND_EVERY) {
    trend_point(now);
    iv = Interval();
    iv.start = now;
  }
  return 0;
}

// --- display: drawn in ROOT's main thread --------------------------------------------------------
static TCanvas *c_main = nullptr, *c_face = nullptr, *c_clusters = nullptr, *c_waves = nullptr, *c_noise = nullptr;
static TCanvas *c_fits = nullptr;
static TLine *dt_cut_line[2] = {};
static TPaveText *fits_box[2] = {};      // the result boxes: timing, single-photoelectron
// a canvas window closed by hand: forget it and what was drawn on it (its next draw_...() makes a
// new one); called before every use, so nothing draws into a deleted canvas
static void forget_closed()
{
  if (!tb::canvas_open(c_main)) for (auto &l : trend_label) l = nullptr;
  if (!tb::canvas_open(c_clusters)) for (auto &l : dt_cut_line) l = nullptr;
  if (!tb::canvas_open(c_fits)) for (auto &b : fits_box) b = nullptr;
  tb::canvas_open(c_face) ; tb::canvas_open(c_waves) ; tb::canvas_open(c_noise);
}

static void style()
{
  gStyle->SetOptStat(0);
  gStyle->SetNumberContours(64);
  gStyle->SetPaintTextFormat(".0f");
}

// no channel chosen yet (too few events for the in-time count): the one with the sharpest-timed
// hits so far, i.e. laser light rather than pickup
static bool pick_sharpest()
{
  int best = -1;
  double bw = 0, at;
  for (int ch = 0; ch < NSIG; ch++) {
    double w = pix_row[ch] >= 0 ? peak_window(h_tch[ch], at) : 0;
    if (w > bw) { bw = w ; best = ch ; }
  }
  if (best < 0 || bw < 5) return false;
  placeholder = false;
  set_watched(best);
  printf("general: main canvas on ch %d (sharpest hit times so far)\n", best);
  return true;
}

static void ready()   // a canvas before any data: set up from the DAQ profile, filled later
{
  if (!booked) book_from_profile();
}

static void draw_watch_pad()
{
  static TLegend *leg = nullptr;
  c_main->cd(3);
  gPad->Clear();
  w_frame->Draw();
  delete leg;
  leg = new TLegend(0.78, 0.12, 0.98, 0.55);
  for (int k = 0; k < NW; k++) {
    int ch = w_avg_ch[k];
    if (ch < 0) continue;
    w_avg[k]->Draw("hist same");
    leg->AddEntry(w_avg[k], ch < NSIG ? Form("ch %d", ch) : Form("TR%d / 10", ch - NSIG), "l");
  }
  leg->Draw();
}

static void draw_waveforms()
{
  static TLegend *leg[NTRIG] = {};
  wave_titles();
  for (int g = 0; g < NTRIG; g++) {
    c_waves->cd(g + 1);
    gPad->Clear();
    wave_frame[g]->Draw();
    if (!leg[g]) {
      leg[g] = new TLegend(0.80, 0.12, 0.98, 0.52);
      for (int k = 0; k < 9; k++) {
        int ch = k < 8 ? 8 * g + k : NSIG + g;
        leg[g]->AddEntry(g_wave[ch], k < 8 ? Form("ch %d", ch) : Form("TR%d / 10", g), "l");
      }
    }
    for (int k = 0; k < 9; k++) {
      int ch = k < 8 ? 8 * g + k : NSIG + g;
      if (wave_average) p_wave[ch]->Draw("hist same");
      else if (g_wave[ch]->GetN() > 0) g_wave[ch]->Draw("L same");
    }
    leg[g]->Draw();
  }
  c_waves->Modified();
  c_waves->Update();
}

static void modify_all(TVirtualPad *p)
{
  p->Modified();
  TIter next(p->GetListOfPrimitives());
  while (TObject *o = next()) if (o->InheritsFrom(TVirtualPad::Class())) modify_all((TVirtualPad *)o);
}

static void refresh_clusters()   // zoom the cluster pads to what is in them
{
  if (!c_clusters) return;
  double qhi = 0.6;
  if (clu_q->GetEntries() >= 20) {
    double pr = 0.995, q;
    clu_q->GetQuantiles(1, &q, &pr);
    qhi = max(qhi, 1.3 * q);
    double half = 0.5, med;
    clu_q->GetQuantiles(1, &med, &half);
    clu_q->SetTitle(Form("cluster charge (median %.2f pC);charge [pC];clusters", med));
  }
  clu_q->GetXaxis()->SetRangeUser(-0.05, min(qhi, 4.0));
  for (auto hp : {make_pair(w_tamp, w_tamp_px), make_pair(clu_tamp, clu_tamp_px)}) {   // mean time per pulse height, zoom
    TH2F *h = hp.first;
    TProfile *px = hp.second;
    px->Reset();
    int xmax = 1;
    for (int i = 1; i <= h->GetNbinsX(); i++)
      for (int j = 1; j <= h->GetNbinsY(); j++) {
        double w = h->GetBinContent(i, j);
        if (w <= 0) continue;
        double y = h->GetYaxis()->GetBinCenter(j);
        if (!std::isnan(t_peak) && fabs(y - t_peak) > 2) continue;   // the mean of the laser peak, not of dark counts
        px->Fill(h->GetXaxis()->GetBinCenter(i), y, w);
        xmax = max(xmax, i);
      }
    h->GetXaxis()->SetRange(1, min(h->GetNbinsX(), int(1.1 * xmax) + 2));
    if (!std::isnan(t_peak)) h->GetYaxis()->SetRangeUser(t_peak - 1, t_peak + 3);
  }
  string fit = apply_timing_fit(clu_t, ct_fit);
  (void)fit;
  clu_t->SetTitle("seed time;t (groups lined up) [ns];clusters");
  double dw = max(2.0, 2 * cluster_dt);
  clu_dt->GetXaxis()->SetRangeUser(-dw, dw);
  for (int i = 0; i < 2; i++)
    if (dt_cut_line[i]) { double x = i ? cluster_dt : -cluster_dt ; dt_cut_line[i]->SetX1(x) ; dt_cut_line[i]->SetX2(x) ; dt_cut_line[i]->SetY2(1.05 * clu_dt->GetMaximum()) ; }
}

static void set_spot_function(const tb::SpeFit &f)   // the drawn curves: the fitted model and parameters
{
  if (f.model != spot_model) {
    delete spot_fn;
    spot_model = f.model;
    spot_fn = tb::spe_function("spot_spe", -0.2, 8.0, f.model);
    spot_fn->SetNpx(3000) ; spot_fn->SetLineColor(kRed) ; spot_fn->SetLineWidth(2);
    make_spot_terms();
  }
  spot_fn->SetParameters(f.par);
  for (TF1 *t : spot_term) t->SetParameters(f.par);
}

static void refresh_spot()   // SPE fit of the spot charge, every 10 s
{
  static int calls = 0;
  if ((!c_clusters && !c_fits) || calls++ % 5 || spot_q->GetEntries() < 2000) return;
  tb::SpeFit f = tb::fit_spe(spot_q);
  if (!f.f) return;
  set_spot_function(f);
  delete f.f;
  f.f = nullptr;
  spot_fit = f;
}

// an SPE fit means something only with a resolved photoelectron peak and a pedestal to anchor it
static const char *spe_problem(const tb::SpeFit &f)
{
  if (f.mu > 3) return "no SPE fit: mu > 3, no pedestal";
  if (!f.resolved) return "no SPE fit: no resolved p.e. peak";
  return nullptr;
}
static double spot_hi()       // upper end of the spot pads: the 99.5 % quantile, at least 1.2 pC
{
  double q = 1.2, pr = 0.995;
  if (spot_q->GetEntries() >= 100) spot_q->GetQuantiles(1, &q, &pr);
  return min(8.0, max(1.2, 1.2 * q));
}

static void spot_text()       // fit results on the spot pads
{
  const tb::SpeFit &f = spot_fit;
  if (f.norm <= 0) return;
  if (const char *why = spe_problem(f)) {
    auto *t = new TLatex(0.4, 0.84, why);
    t->SetNDC() ; t->SetTextSize(0.04) ; t->SetTextColor(kRed + 1) ; t->SetBit(kCanDelete) ; t->Draw();
    t = new TLatex(0.4, 0.78, Form("mean %.2f pC", spot_q->GetMean()));
    t->SetNDC() ; t->SetTextSize(0.04) ; t->SetBit(kCanDelete) ; t->Draw();
    return;
  }
  string line[2] = {Form("Q_{1} %.3f pC, gain %.2g", f.q1, f.gain),
                    f.model == tb::SPE_POLYA_UNDER ? Form("under-amp. %.0f %%, #mu %.3f", 100 * f.w, f.mu) : Form("#sigma_{1}/Q_{1} %.0f %%, #mu %.3f", 100 * f.s1 / f.q1, f.mu)};
  for (int i = 0; i < 2; i++) {
    auto *t = new TLatex(0.5, 0.84 - 0.06 * i, line[i].c_str());
    t->SetNDC() ; t->SetTextSize(0.04) ; t->SetBit(kCanDelete) ; t->Draw();   // the next Clear() deletes it
  }
}

static void refresh_noise()      // the frequency spectrum of one channel, and the mean of the pixels
{
  if (!c_noise) return;
  int ch = fft_ch;
  if (ch < 0)                    // the noisiest signal channel
    for (int c = 0; c < NSIG; c++) if (ch < 0 || noise_rms->GetBinContent(c + 1) > noise_rms->GetBinContent(ch + 1)) ch = c;
  int npix = 0;
  fft_mean->Reset() ; fft_one->Reset();
  for (int c = 0; c < NSIG; c++) {
    if (pix_row[c] < 0 || noise_fft->GetBinEntries(noise_fft->GetBin(1, c + 1)) == 0) continue;
    npix++;
    for (int k = 1; k <= fft_mean->GetNbinsX(); k++) fft_mean->AddBinContent(k, noise_fft->GetBinContent(k, c + 1));
  }
  if (npix) fft_mean->Scale(1.0 / npix);
  int top = 1;
  for (int k = 1; k <= fft_one->GetNbinsX(); k++) {
    fft_one->SetBinContent(k, noise_fft->GetBinContent(k, ch + 1));
    if (fft_one->GetBinContent(k) > fft_one->GetBinContent(top)) top = k;
  }
  fft_one->SetTitle(Form("ch %d (red) vs %d px mean: peak %.0f MHz;frequency [MHz];mean amplitude [mV]", ch, npix, fft_one->GetBinCenter(top)));
  // from the contents: GetMaximum() would return the maximum set last time (and it doubled every update)
  fft_one->SetMaximum(2 * max(fft_one->GetBinContent(fft_one->GetMaximumBin()), fft_mean->GetBinContent(fft_mean->GetMaximumBin())));
  fft_one->SetMinimum(0.003);
}

static void draw_spot_pads(TCanvas *c, int pad_log, int pad_lin);
static void refresh_fits();
static void tick()   // redraw: from the timer, and before saving
{
  forget_closed();
  if (watched_dirty) refresh_watched();
  refresh_clusters();
  refresh_spot();
  draw_spot_pads(c_clusters, 4, 9);
  draw_spot_pads(c_fits, 2, 4);
  refresh_fits();
  refresh_noise();
  if (placeholder && watch_fixed < 0) pick_sharpest();
  if (watched_changed && c_main) { watched_changed = false ; draw_watch_pad() ; rebuild_trends() ; }
  for (TCanvas *c : {c_main, c_face, c_clusters, c_waves, c_noise, c_fits})
    if (c) { modify_all(c) ; c->Update() ; }
}

struct Ticker : public TTimer {
  Ticker() : TTimer(2000, kTRUE) {}
  Bool_t Notify() override { tick() ; Reset() ; return kTRUE ; }
};

static void start_ticker()
{
  static Ticker *t = nullptr;
  if (t || gROOT->IsBatch()) return;
  t = new Ticker;
  t->TurnOn();
}

void set_waveform_mode(const char *m)
{
  string s = m;
  if (s != "latest" && s != "average") { printf("general: waveform mode is \"latest\" or \"average\"\n") ; return ; }
  wave_average = s == "average";
  if (c_waves) draw_waveforms();
}

void set_waveform_range(double lo, double hi)
{
  wave_min = lo ; wave_max = hi;
  if (!booked) return;
  for (int g = 0; g < NTRIG; g++) { wave_frame[g]->SetMinimum(lo) ; wave_frame[g]->SetMaximum(hi) ; }
  w_frame->SetMinimum(lo) ; w_frame->SetMaximum(hi);
  if (c_waves) draw_waveforms();
}

void set_waveform_time(double from_ns, double to_ns)
{
  wave_from = from_ns ; wave_to = to_ns;
  if (!booked) return;
  wave_xrange();
  if (c_waves) draw_waveforms();
}

void draw()
{
  forget_closed();
  ready();
  style();
  if (watched < 0 && watch_fixed >= 0) set_watched(watch_fixed);
  if (watched < 0 && !pick_sharpest()) { set_watched(0) ; placeholder = true ; printf("general: main canvas on ch 0 until the laser is found\n") ; }
  if (!c_main) { c_main = new TCanvas("c_main", "417 test bench monitor", 1500, 860) ; c_main->Divide(3, 2) ; }
  c_main->cd(1) ; gPad->SetRightMargin(0.15) ; face_occ->Draw("colz");
  c_main->cd(2) ; gPad->SetRightMargin(0.15) ; gPad->SetLogz() ; persist->Draw("colz");
  draw_watch_pad();
  watched_changed = false;
  c_main->cd(4) ; gPad->SetLogy() ; w_q->Draw();
  c_main->cd(5) ; w_t->Draw("hist");
  c_main->cd(6);
  gPad->Divide(1, 4, 0.002, 0.002);
  for (int i = 0; i < 4; i++) {
    c_main->cd(6)->cd(i + 1);
    gPad->SetTopMargin(0.22) ; gPad->SetBottomMargin(0.2) ; gPad->SetLeftMargin(0.14) ; gPad->SetRightMargin(0.03) ; gPad->SetGridy();
    trend_frame[i]->Draw("axis");
    trend[i]->Draw("LP same");
    trend_label[i] = new TLatex(0.14, 0.86, "");
    trend_label[i]->SetNDC() ; trend_label[i]->SetTextSize(0.14) ; trend_label[i]->Draw();
  }
  rebuild_trends();
  c_main->Update();
  start_ticker();
}

// the spot charge (log, with the fit) and above its pedestal, on two pads of a canvas
static void draw_spot_pads(TCanvas *c, int pad_log, int pad_lin)
{
  if (!c) return;
  TVirtualPad *keep = gPad;
  double hi = spot_hi(), cmax = 0;
  for (int b = spot_q->FindBin(-0.15); b <= spot_q->FindBin(hi); b++) cmax = max(cmax, spot_q->GetBinContent(b));
  c->cd(pad_log) ; gPad->Clear() ; gPad->SetLogy(cmax > 0);   // nothing in range yet: linear 0..1 (log of nothing: ROOT complains)
  spot_q->SetMinimum(cmax > 0 ? 0.5 : 0) ; spot_q->SetMaximum(cmax > 0 ? 2 * cmax : 1);
  spot_q->GetXaxis()->SetRangeUser(-0.15, hi);
  spot_q->Draw("hist");
  bool fitted = spot_fit.norm > 0 && !spe_problem(spot_fit);
  if (fitted) { for (TF1 *t : spot_term) t->Draw("same") ; spot_fn->Draw("same") ; }
  if (c != c_fits) spot_text();
  c->cd(pad_lin) ; gPad->Clear();
  TH1 *hz = spot_q->DrawCopy("hist");
  hz->SetTitle("spot charge (linear);charge [pC];events");
  double x0 = spot_fit.norm > 0 && !spe_problem(spot_fit) ? spot_fit.q0 + 3 * spot_fit.s0 : 0.05;
  hz->GetXaxis()->SetRangeUser(x0, hi);
  double ymax = 0;
  for (int b = hz->FindBin(x0); b <= hz->FindBin(hi); b++) ymax = max(ymax, hz->GetBinContent(b));
  hz->SetMinimum(0) ; hz->SetMaximum(1.3 * ymax + 1);
  if (fitted) { for (TF1 *t : spot_term) t->Draw("same") ; spot_fn->Draw("same") ; spot_leg->Draw() ; }
  if (c != c_fits) spot_text();
  if (c == c_fits && fits_box[1]) { c->cd(pad_log) ; fits_box[1]->Draw() ; }
  if (keep) keep->cd();
}

void set_spot_window(double before_ns, double after_ns) { spot_before = before_ns ; spot_after = after_ns ; if (booked) spot_q->Reset() ; }

// --- the two results: timing and single-photoelectron charge -----------------------------------
static void refresh_fits()     // the numbers in the result boxes (and refits the timing)
{
  if (!c_fits || !fits_box[0]) return;
  refresh_watched();
  c_fits->cd(1)->SetLogy(w_t->GetMaximum() > 0);
  const tb::TimingFit &t = wt_fit.fit;
  TPaveText *b = fits_box[0];
  b->Clear();
  b->AddText(Form("%.0f %s", w_t->GetEntries(), tamp_text().c_str()));
  if (!t.ok && h_tch[watched]->GetEntries() > 200 && w_t->GetEntries() < 50) {
    b->AddText("few hits in the pulse-height window:")->SetTextColor(kRed + 1);
    b->AddText("widen it, or set_timing_amplitude(0, 0) = all")->SetTextColor(kRed + 1);
  } else if (!t.ok) b->AddText("no timing peak yet");
  else {
    b->AddText(Form("peak %.4f ns, #sigma %.1f #pm %.1f ps", t.mean, 1000 * t.sigma, 1000 * t.esigma));
    if (t.two) {
      b->AddText(t.rest_frac > 0 ? Form("rest %.1f %%, rms %.0f ps", 100 * t.rest_frac, 1000 * t.rest_rms) : "rest: none");
      b->AddText(std::isnan(t.sh_dt) ? "shoulder: < 1000 hits" : Form("shoulder +%.0f ps, %.1f %%", 1000 * t.sh_dt, 100 * t.sh_frac));
      b->AddText(std::isnan(t.late_dt) ? "late peak: none" : Form("late +%.0f ps, %.1f %%", 1000 * t.late_dt, 100 * t.late_frac));
    } else b->AddText("(one Gaussian: < 200 hits)");
  }
  const tb::SpeFit &f = spot_fit;
  b = fits_box[1];
  b->Clear();
  static const char *model[] = {"Gauss", "Polya", "Polya + under-amp."};
  b->AddText(f.norm > 0 ? Form("%.0f events, %s", spot_q->GetEntries(), model[f.model]) : Form("%.0f events", spot_q->GetEntries()));
  if (f.norm <= 0) b->AddText(spot_q->GetEntries() < 2000 ? "fit after 2000 events" : "no fit");
  else if (const char *why = spe_problem(f)) {
    b->AddText(why)->SetTextColor(kRed + 1);
    b->AddText(Form("mean %.3f pC", spot_q->GetMean()));
  } else {
    b->AddText(Form("Q_{1} %.4f #pm %.4f pC", f.q1, f.eq1));
    b->AddText(Form("gain %.3g #pm %.1g", f.gain, f.egain));
    if (f.model == tb::SPE_POLYA_UNDER) b->AddText(Form("under-amplified %.0f %%, mean %.3f pC", 100 * f.w, f.mean_q));
    b->AddText(Form("#sigma_{1}/Q_{1} %.0f %%, #mu %.3f", 100 * f.s1 / f.q1, f.mu));
    b->AddText(Form("#sigma_{0} %.3f pC, #chi^{2}/ndf %.2f%s", f.s0, f.chi2ndf, f.ok ? "" : " (no conv.)"));
  }
}

void draw_fits()
{
  forget_closed();
  ready();
  style();
  if (!c_fits) { c_fits = new TCanvas("c_fits", "timing and single-photoelectron fits", 1400, 900) ; c_fits->Divide(2, 2) ; }
  refresh_watched();
  c_fits->cd(1) ; gPad->Clear() ; gPad->SetLogy(w_t->GetMaximum() > 0) ; w_t->Draw("hist");
  if (wt_fit.all) { wt_fit.all->Draw("same") ; wt_fit.main->Draw("same") ; }
  c_fits->cd(3) ; gPad->Clear() ; w_t->Draw("hist");
  if (wt_fit.all) { wt_fit.all->Draw("same") ; wt_fit.main->Draw("same") ; }
  for (int i = 0; i < 2; i++) {
    c_fits->cd(i ? 2 : 1);
    fits_box[i] = new TPaveText(0.55, 0.58, 0.9, 0.9, "NDC");
    fits_box[i]->SetFillColor(0) ; fits_box[i]->SetBorderSize(1) ; fits_box[i]->SetTextAlign(12);
    fits_box[i]->SetTextSize(0);              // 0: ROOT scales the text to fit the box
  }
  draw_spot_pads(c_fits, 2, 4);
  c_fits->cd(1) ; fits_box[0]->Draw();
  refresh_fits();
  c_fits->Update();
  start_ticker();
}

void draw_details()
{
  forget_closed();
  ready();
  style();
  if (!c_face) { c_face = new TCanvas("c_face", "detector face", 1300, 560) ; c_face->Divide(2, 1) ; }
  for (int i = 1; i <= 2; i++) { c_face->cd(i) ; gPad->SetRightMargin(0.15) ; }
  c_face->cd(1) ; face_amp->Draw("colz");
  c_face->cd(2) ; face_occ->Draw("colz");
  c_face->Update();

  if (!c_clusters) { c_clusters = new TCanvas("c_clusters", "clusters", 2200, 820) ; c_clusters->Divide(5, 2) ; }
  c_clusters->cd(1) ; gPad->SetRightMargin(0.15) ; clu_last->Draw("colz text");
  c_clusters->cd(2) ; gPad->SetRightMargin(0.15) ; clu_centroid->Draw("colz");
  c_clusters->cd(3) ; clu_size->Draw("hist text0");
  c_clusters->cd(5) ; gPad->SetRightMargin(0.15) ; gPad->SetLogz() ; w_tamp->Draw("colz") ; w_tamp_px->Draw("same");
  c_clusters->cd(10) ; gPad->SetRightMargin(0.15) ; gPad->SetLogz() ; clu_tamp->Draw("colz") ; clu_tamp_px->Draw("same");
  c_clusters->cd(6) ; clu_q->Draw("hist");
  for (auto *h : clu_qn) h->Draw("hist same");
  {
    auto *l = new TLegend(0.6, 0.62, 0.88, 0.88);
    l->AddEntry(clu_q, "all", "l") ; l->AddEntry(clu_qn[0], "1 pixel", "l") ; l->AddEntry(clu_qn[1], "2 pixels", "l") ; l->AddEntry(clu_qn[2], "3+ pixels", "l");
    l->Draw();
  }
  c_clusters->cd(7) ; clu_t->Draw("hist");
  refresh_clusters();
  if (ct_fit.all) { ct_fit.all->Draw("same") ; ct_fit.main->Draw("same") ; }
  c_clusters->cd(8) ; clu_dt->Draw("hist");
  for (auto *&l : dt_cut_line) { l = new TLine(0, 0, 0, 1) ; l->SetLineStyle(2) ; l->SetLineColor(kRed + 1) ; l->Draw() ; }
  refresh_clusters();
  draw_spot_pads(c_clusters, 4, 9);
  c_clusters->Update();

  if (!c_waves) { c_waves = new TCanvas("c_waves", "waveforms", 1400, 820) ; c_waves->Divide(2, 2) ; }
  draw_waveforms();
  start_ticker();
}

void draw_noise()
{
  forget_closed();
  ready();
  style();
  if (!c_noise) { c_noise = new TCanvas("c_noise", "noise and triggers", 1600, 820) ; c_noise->Divide(3, 2) ; }
  c_noise->cd(1) ; gPad->SetGridy() ; noise_rms->SetMinimum(0) ; noise_rms->Draw("hist");
  c_noise->cd(2) ; gPad->SetRightMargin(0.15) ; face_all->Draw("colz");
  c_noise->cd(3) ; gPad->SetGridy() ; trig_found->SetMinimum(0) ; trig_found->SetMaximum(1.05) ; trig_found->Draw("hist");
  c_noise->cd(4) ; gPad->SetGridy() ; gPad->SetLogy() ; noise_hits->Draw("hist");
  c_noise->cd(5) ; gPad->SetRightMargin(0.15) ; gPad->SetLogx() ; gPad->SetLogz() ; noise_fft->Draw("colz");
  c_noise->cd(6) ; gPad->SetLogx() ; gPad->SetLogy() ; gPad->SetGridx();
  refresh_noise();
  fft_one->Draw("hist") ; fft_mean->Draw("hist same");
  c_noise->Update();
  start_ticker();
}

void online()
{
  if (rcdaqopen()) printf("general: no new rcdaq stream opened (already open, or no rcdaq_server)\n");
  pstart();
  draw();
}

void status()
{
  bool partial = std::isnan(last_rate) && last_time > iv.start;   // not one full trend step yet
  printf("general: run %d, detector %s, %ld events looked at, %ld clusters, DAQ rate %.0f Hz (%s)\n", info.run, db_ok ? db.detector.c_str() : "unknown",
         n_events, n_clusters, partial ? iv.daq_events / double(last_time - iv.start) : last_rate,
         partial ? "since the start" : Form("last %d s", TREND_EVERY));
  printf("  signal window %.1f to %.1f ns, hit threshold %.1f mV, waveforms %s\n", cuts.sig_from, cuts.sig_to, cuts.threshold,
         wave_average ? "average" : "latest");
  printf("  clusters: seed > %.1f mV, neighbours > %.1f mV within %d pixel(s) and %.2f ns\n", seed_mv, neighbour_mv,
         cluster_range, cluster_dt);
  printf("  main canvas: ch %d (%s)\n", watched, watch_fixed >= 0 ? "fixed" : "follows the laser");
  if (watched >= 0 && booked) {
    refresh_watched();
    const tb::TimingFit &t = wt_fit.fit;
    if (!t.ok && h_tch[watched]->GetEntries() > 200)
      printf("  timing ch %d: too few %s (set_timing_amplitude)\n", watched, tamp_text().c_str());
    if (t.ok) {
      string more = " (one Gaussian: < 200 hits)";
      if (t.two) {
        more = t.rest_frac > 0 ? Form("; rest %.1f %% (rms %.0f ps)", 100 * t.rest_frac, 1000 * t.rest_rms) : "; rest 0 %";
        more += std::isnan(t.sh_dt) ? "; shoulder not fitted (< 2000 hits)" : Form("; shoulder +%.0f ps (sigma %.0f ps, %.1f %%)", 1000 * t.sh_dt, 1000 * t.sh_sigma, 100 * t.sh_frac);
        more += std::isnan(t.late_dt) ? "; late peak not fitted (< 1000 hits)" : Form("; late peak +%.0f ps (sigma %.0f ps, %.1f %%)", 1000 * t.late_dt, 1000 * t.late_sigma, 100 * t.late_frac);
      }
      printf("  timing ch %d (%s): peak %.4f +- %.4f ns, sigma %.1f +- %.1f ps%s\n", watched, tamp_text().c_str(), t.mean, t.emean,
             1000 * t.sigma, 1000 * t.esigma, more.c_str());
    }
    if (spot_fit.norm <= 0 && spot_q->GetEntries() >= 2000) {   // no canvas has asked for it yet
      tb::SpeFit f = tb::fit_spe(spot_q);
      if (f.f) { set_spot_function(f) ; delete f.f ; f.f = nullptr ; spot_fit = f ; }
    }
    if (spot_fit.norm > 0 && spe_problem(spot_fit))
      printf("  SPE ch %d + neighbours: no fit, %s; mean spot charge %.3f pC\n", watched, spe_problem(spot_fit), spot_q->GetMean());
    else if (spot_fit.norm > 0) printf("  SPE ch %d + neighbours (%s): Q1 %.4f +- %.4f pC, gain %.3g, under-amplified %.0f %% (mean %.4f pC per p.e.), sigma1/Q1 %.0f %%, mu %.3f, chi2/ndf %.2f\n",
                                  watched, spot_fit.model == tb::SPE_GAUSS ? "Gauss" : spot_fit.model == tb::SPE_POLYA ? "Polya" : "Polya + under-amplified",
                                  spot_fit.q1, spot_fit.eq1, spot_fit.gain, 100 * spot_fit.w, spot_fit.mean_q, 100 * spot_fit.s1 / spot_fit.q1,
                                  spot_fit.mu, spot_fit.chi2ndf);
  }
  if (toff.ok) printf("  time offsets: %s\n", toff.file.c_str());
  else printf("  time offsets: none in db/v1742 (groups lined up from the data only)\n");
  if (booked) {
    printf("  group time offsets%s:", toff.ok ? " left after the db ones" : "");
    for (int g = 0; g < NTRIG; g++) printf(grp_ok[g] ? " %.2f" : " -", grp_off[g]);
    printf(" ns (- = no laser light in that group yet)\n");
  }
}

void save_plots(const char *prefix)
{
  forget_closed();
  if (!c_main) draw();
  if (!c_face || !c_clusters || !c_waves) draw_details();
  if (!c_noise) draw_noise();
  if (!c_main) return;
  refresh_watched();
  tick();
  for (auto cp : {make_pair(c_main, "main"), make_pair(c_face, "face"), make_pair(c_clusters, "clusters"), make_pair(c_waves, "waveforms"),
                  make_pair(c_noise, "noise"), make_pair(c_fits, "fits")})
    if (cp.first) cp.first->SaveAs(Form("%s_%s.png", prefix, cp.second));
}
