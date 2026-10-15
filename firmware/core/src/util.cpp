#include "ghostwave/util.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace gw {

double distanceM(const Position& a, const Position& b) {
  constexpr double kR = 6371000.0, kD = M_PI / 180.0;
  double dlat = (b.lat - a.lat) * kD, dlon = (b.lon - a.lon) * kD;
  double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(a.lat * kD) * std::cos(b.lat * kD) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2 * kR * std::asin(std::sqrt(h));
}

Position lerp(const Position& a, const Position& b, double f) {
  return {a.lat + (b.lat - a.lat) * f, a.lon + (b.lon - a.lon) * f, true};
}

uint32_t airtimeMs(const Phy& phy, size_t bytes) {
  double tsym = std::pow(2.0, phy.sf) / phy.bwHz;
  int de = tsym > 0.016 ? 1 : 0;
  double num = 8.0 * bytes - 4.0 * phy.sf + 28 + 16;
  double n = 8 + std::max(std::ceil(num / (4.0 * (phy.sf - 2 * de))) * (phy.cr + 4), 0.0);
  return static_cast<uint32_t>(std::ceil(((phy.preamble + 4.25) + n) * tsym * 1000.0));
}

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',') {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

bool coord(const std::string& v, const std::string& hemi, int degDigits, double& out) {
  if (v.size() < static_cast<size_t>(degDigits) + 2 || hemi.empty()) return false;
  double deg = std::atof(v.substr(0, degDigits).c_str());
  double min = std::atof(v.substr(degDigits).c_str());
  out = deg + min / 60.0;
  if (hemi == "S" || hemi == "W") out = -out;
  return true;
}

}  // namespace

bool parseNmea(const std::string& raw, Position& out) {
  std::string line = raw;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  if (line.size() < 10 || line[0] != '$') return false;
  size_t star = line.rfind('*');
  if (star == std::string::npos || star + 3 > line.size()) return false;
  uint8_t sum = 0;
  for (size_t i = 1; i < star; ++i) sum ^= static_cast<uint8_t>(line[i]);
  if (std::strtoul(line.substr(star + 1, 2).c_str(), nullptr, 16) != sum) return false;
  auto f = split(line.substr(1, star - 1));
  if (f.empty() || f[0].size() < 5) return false;
  std::string kind = f[0].substr(2);
  double lat, lon;
  if (kind == "GGA" && f.size() > 6) {
    if (f[6].empty() || f[6] == "0") return false;
    if (!coord(f[2], f[3], 2, lat) || !coord(f[4], f[5], 3, lon)) return false;
  } else if (kind == "RMC" && f.size() > 6) {
    if (f[2] != "A") return false;
    if (!coord(f[3], f[4], 2, lat) || !coord(f[5], f[6], 3, lon)) return false;
  } else {
    return false;
  }
  out = {lat, lon, true};
  return true;
}

}  // namespace gw
