// tb_db.h - read the 417 test-bench database (db/) from C++ / ROOT analyses (header only).
//
//   tb::Db db;
//   db.load();                          // detector selected with testbench.sh use detector
//   db.load("HRPPD_26");            // or a named one, e.g. from tb::detector_in_910()
//   int row, col;
//   if (db.pixel(2000, ch, row, col)) ...   // packet + channel -> anode pixel (mapping + cabling)
//   db.num("PITCH_X_MM", 0)             // any detector.conf value
//
// The repository is found from $TB_ROOT, else from TB_ROOT_DIR given at compile time
// (the analysis Makefiles pass -DTB_ROOT_DIR=<repo>).
#ifndef TB_DB_H
#define TB_DB_H

#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tb {

inline std::string repo_root()
{
  if (const char *e = std::getenv("TB_ROOT")) return e;
#ifdef TB_ROOT_DIR
  return TB_ROOT_DIR;
#else
  return ".";
#endif
}

// KEY=value file (same rules as lib/tb.sh tb_conf)
inline std::map<std::string, std::string> read_conf(const std::string &file)
{
  std::map<std::string, std::string> m;
  std::ifstream in(file);
  std::string line;
  static const std::regex re("^\\s*([A-Z_][A-Z0-9_]*)\\s*=\\s*(.*?)\\s*$");
  while (std::getline(in, line)) {
    line = line.substr(0, line.find('#'));
    std::smatch s;
    if (!std::regex_match(line, s, re)) continue;
    std::string v = s[2];
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    m[s[1]] = v;
  }
  return m;
}

// the detector named in begin-run packet 910 ("detector NAME, DAQ profile ..."), or ""
inline std::string detector_in_910(const std::string &text)
{
  std::smatch s;
  static const std::regex re("detector ([A-Za-z0-9_.-]+), DAQ profile");
  return std::regex_search(text, s, re) ? std::string(s[1]) : std::string();
}

// V1742 inputs to leave out (pickup or dead): list "1 4 6"; "-" = MASKED_CHANNELS in db/testbench.conf
inline std::set<int> masked_channels(const std::string &list = "-")
{
  std::string l = list == "-" ? read_conf(repo_root() + "/db/testbench.conf")["MASKED_CHANNELS"] : list;
  if (l.size() >= 2 && l.front() == '"') l = l.substr(1, l.size() - 2);
  std::set<int> m;
  std::istringstream is(l);
  for (int ch; is >> ch;) m.insert(ch);
  return m;
}

struct Db {
  std::string root, detector;
  std::map<std::string, std::string> conf;              // detector.conf
  std::map<std::pair<int, int>, std::pair<int, int>> map_slot;   // (slot, ch) -> (row, col)
  std::map<std::pair<int, int>, std::pair<int, int>> by_packet; // (packet, ch) -> (row, col), via cabling.txt
  struct HvRow { std::string electrode ; int unit = -1, ch = -1 ; };   // unit = board serial, -1 = not cabled
  std::vector<HvRow> hv;                                 // hv.conf, in file order

  bool load(const std::string &name = "")
  {
    root = repo_root();
    detector = name;
    if (detector.empty()) detector = read_conf(root + "/state/active.conf")["DETECTOR"];
    if (detector.empty()) return false;
    std::string d = root + "/db/detectors/" + detector;
    conf = read_conf(d + "/detector.conf");
    if (conf.empty()) return false;
    map_slot.clear();
    by_packet.clear();
    hv.clear();
    std::ifstream m(d + "/mapping.txt"), c(d + "/cabling.txt"), h(d + "/hv.conf");
    std::string line;
    while (std::getline(m, line)) {
      std::istringstream is(line.substr(0, line.find('#')));
      int slot, ch, row, col;
      if (is >> slot >> ch >> row >> col) map_slot[{slot, ch}] = {row, col};
    }
    while (std::getline(c, line)) {
      std::istringstream is(line.substr(0, line.find('#')));
      int packet, slot;
      if (!(is >> packet >> slot)) continue;
      for (auto &e : map_slot)          // a packet may read several slots (channels must not overlap)
        if (e.first.first == slot) by_packet[{packet, e.first.second}] = e.second;
    }
    // hv.conf: electrode unit ch ...; unit is a board serial or an alias from HV_UNITS (testbench.conf)
    std::map<std::string, int> alias;
    std::istringstream units(read_conf(root + "/db/testbench.conf")["HV_UNITS"]);
    for (std::string u; units >> u;) {
      auto k = u.find(':');
      if (k != std::string::npos) alias[u.substr(0, k)] = std::atoi(u.c_str() + k + 1);
    }
    while (std::getline(h, line)) {
      std::istringstream is(line.substr(0, line.find('#')));
      std::string e, unit, ch;
      if (!(is >> e >> unit >> ch)) continue;
      HvRow r;
      r.electrode = e;
      if (alias.count(unit)) r.unit = alias[unit];
      else if (unit != "-") r.unit = std::atoi(unit.c_str());
      r.ch = ch == "-" ? -1 : std::atoi(ch.c_str());
      hv.push_back(r);
    }
    return true;
  }

  // the CAEN unit (board serial) and channel feeding an electrode; "" = the only cabled one
  bool hv_channel(const std::string &electrode, int &unit, int &ch) const
  {
    int n = 0;
    for (auto &r : hv)
      if (r.unit >= 0 && r.ch >= 0 && (electrode.empty() || r.electrode == electrode)) { unit = r.unit ; ch = r.ch ; n++ ; }
    return n == 1;
  }

  // packet + channel -> pixel; false if the cabling or mapping does not cover it
  bool pixel(int packet, int ch, int &row, int &col) const
  {
    auto m = by_packet.find({packet, ch});
    if (m == by_packet.end()) return false;
    row = m->second.first;
    col = m->second.second;
    return true;
  }

  std::string str(const std::string &key, const std::string &def = "") const
  {
    auto f = conf.find(key);
    return (f == conf.end() || f->second.empty()) ? def : f->second;
  }
  double num(const std::string &key, double def = 0) const
  {
    std::string s = str(key);
    return s.empty() ? def : std::atof(s.c_str());
  }
};

} // namespace tb

#endif // TB_DB_H
