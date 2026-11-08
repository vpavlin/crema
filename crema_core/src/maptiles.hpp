#pragma once
// maptiles.hpp - an event's own map, made inside crema_core (ADR 0002, amended for 0.3.0).
//
// OpenStreetMap DATA for the event's box (one Overpass request, JSON with geometry) -> Web Mercator ->
// clipped per tile -> Mapbox Vector Tiles (MVT 2.1) -> gzip -> one PMTiles v3 file. No tile server, no
// external program. The layers/classes are the subset of the OpenMapTiles schema the Crema map style
// draws (tilemaker's profile writes the same), so an imported tilemaker file and a made one look alike:
//   transportation{class}, building, water, waterway, park, landcover{class}, landuse{class}
// Data (c) OpenStreetMap contributors, ODbL.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <zlib.h>

namespace maptiles {

using json = nlohmann::json;

struct LL { double lon, lat; };
struct Box { double w, s, e, n; };
struct Feature {
    std::string layer, cls;              // cls "" = no class attribute
    bool polygon = false;
    std::vector<std::vector<LL>> parts;  // polygon: rings (first outer; holes after); line: polylines
    std::vector<int> outerCount;         // polygon: per polygon, how many rings (outer + its holes)
    int minzoom = 12;
    Box bbox{180, 90, -180, -90};
};

// ── the Overpass query: only what the style draws ──
inline std::string overpassQuery(const Box& b) {
    char bb[128];
    snprintf(bb, sizeof bb, "%.5f,%.5f,%.5f,%.5f", b.s, b.w, b.n, b.e);
    return std::string("[out:json][timeout:180][bbox:") + bb + "];("
        "way[highway];way[railway~\"^(rail|tram|light_rail|subway|narrow_gauge)$\"];way[building];"
        "way[waterway~\"^(river|canal|stream)$\"];way[natural~\"^(water|wood|scrub|grassland)$\"];"
        "way[landuse~\"^(forest|grass|meadow|cemetery|recreation_ground|village_green|allotments)$\"];"
        "way[leisure~\"^(park|garden|pitch|playground)$\"];"
        "rel[type=multipolygon][natural~\"^(water|wood)$\"];rel[type=multipolygon][landuse~\"^(forest|grass|cemetery)$\"];"
        "rel[type=multipolygon][leisure~\"^(park|garden)$\"];rel[type=multipolygon][building];"
        ");out geom qt;";
}

// ── tags -> layer/class/minzoom ──
inline bool classify(const json& t, Feature& f, bool closed) {
    auto tag = [&](const char* k) { return t.contains(k) && t[k].is_string() ? t[k].get<std::string>() : std::string(); };
    const std::string hw = tag("highway"), rw = tag("railway"), bd = tag("building"), ww = tag("waterway"),
                      nat = tag("natural"), lu = tag("landuse"), le = tag("leisure"), area = tag("area");
    if (!hw.empty() && area != "yes") {
        static const std::map<std::string, std::pair<std::string, int>> H = {
            {"motorway", {"motorway", 12}}, {"motorway_link", {"motorway", 12}}, {"trunk", {"trunk", 12}}, {"trunk_link", {"trunk", 12}},
            {"primary", {"primary", 12}}, {"primary_link", {"primary", 12}}, {"secondary", {"secondary", 12}}, {"secondary_link", {"secondary", 12}},
            {"tertiary", {"tertiary", 13}}, {"tertiary_link", {"tertiary", 13}}, {"residential", {"minor", 13}}, {"unclassified", {"minor", 13}},
            {"living_street", {"minor", 13}}, {"road", {"minor", 13}}, {"pedestrian", {"minor", 13}}, {"service", {"service", 14}},
            {"footway", {"path", 14}}, {"path", {"path", 14}}, {"cycleway", {"path", 14}}, {"steps", {"path", 15}}, {"bridleway", {"path", 14}},
            {"track", {"track", 14}}};
        auto it = H.find(hw);
        if (it == H.end()) return false;
        f.layer = "transportation"; f.cls = it->second.first; f.minzoom = it->second.second; f.polygon = false;
        return true;
    }
    if (!rw.empty()) { f.layer = "transportation"; f.cls = rw == "rail" || rw == "narrow_gauge" ? "rail" : "transit"; f.minzoom = 12; f.polygon = false; return true; }
    if (!ww.empty() && !closed) { f.layer = "waterway"; f.cls = ww; f.minzoom = ww == "stream" ? 14 : 12; f.polygon = false; return true; }
    if (!closed) return false;
    f.polygon = true;
    if (!bd.empty() && bd != "no") { f.layer = "building"; f.minzoom = 14; return true; }   // 14: keeps the first pieces light
    if (nat == "water") { f.layer = "water"; f.minzoom = 12; return true; }
    if (nat == "wood" || lu == "forest") { f.layer = "landcover"; f.cls = "wood"; return true; }
    if (nat == "scrub" || nat == "grassland" || lu == "grass" || lu == "meadow" || lu == "village_green" || lu == "recreation_ground" || lu == "allotments") {
        f.layer = "landcover"; f.cls = "grass"; return true;
    }
    if (le == "park" || le == "garden") { f.layer = "park"; f.cls = le; return true; }
    if (lu == "cemetery" || le == "pitch" || le == "playground") { f.layer = "landuse"; f.cls = lu == "cemetery" ? "cemetery" : le; f.minzoom = 13; return true; }
    return false;
}

inline std::vector<LL> geomOf(const json& g) {
    std::vector<LL> out;
    if (!g.is_array()) return out;
    out.reserve(g.size());
    for (const auto& p : g) if (p.is_object() && p.contains("lat") && p.contains("lon")) out.push_back({p["lon"].get<double>(), p["lat"].get<double>()});
    return out;
}
inline bool samePt(const LL& a, const LL& b) { return std::fabs(a.lon - b.lon) < 1e-9 && std::fabs(a.lat - b.lat) < 1e-9; }

// join a relation's member ways into closed rings (greedy endpoint matching)
inline std::vector<std::vector<LL>> assembleRings(std::vector<std::vector<LL>> segs) {
    std::vector<std::vector<LL>> rings;
    while (!segs.empty()) {
        std::vector<LL> cur = std::move(segs.back()); segs.pop_back();
        bool grew = true;
        while (!cur.empty() && !samePt(cur.front(), cur.back()) && grew) {
            grew = false;
            for (size_t i = 0; i < segs.size(); i++) {
                auto& s = segs[i];
                if (s.empty()) continue;
                if (samePt(cur.back(), s.front())) cur.insert(cur.end(), s.begin() + 1, s.end());
                else if (samePt(cur.back(), s.back())) cur.insert(cur.end(), s.rbegin() + 1, s.rend());
                else if (samePt(cur.front(), s.back())) cur.insert(cur.begin(), s.begin(), s.end() - 1);
                else if (samePt(cur.front(), s.front())) cur.insert(cur.begin(), s.rbegin(), s.rend() - 1);
                else continue;
                segs.erase(segs.begin() + i); grew = true; break;
            }
        }
        if (cur.size() >= 4 && samePt(cur.front(), cur.back())) rings.push_back(std::move(cur));
    }
    return rings;
}

inline void growBox(Feature& f) {
    for (const auto& p : f.parts) for (const auto& q : p) {
        f.bbox.w = std::min(f.bbox.w, q.lon); f.bbox.e = std::max(f.bbox.e, q.lon);
        f.bbox.s = std::min(f.bbox.s, q.lat); f.bbox.n = std::max(f.bbox.n, q.lat);
    }
}

// Overpass JSON (out geom) -> features
inline std::vector<Feature> featuresFromOverpass(const json& doc) {
    std::vector<Feature> out;
    if (!doc.is_object() || !doc.contains("elements") || !doc["elements"].is_array()) return out;
    for (const auto& el : doc["elements"]) {
        const std::string type = el.value("type", "");
        const json tags = el.value("tags", json::object());
        if (type == "way") {
            auto g = geomOf(el.value("geometry", json::array()));
            if (g.size() < 2) continue;
            Feature f;
            const bool closed = g.size() >= 4 && samePt(g.front(), g.back());
            if (!classify(tags, f, closed)) continue;
            f.parts.push_back(std::move(g));
            if (f.polygon) f.outerCount.push_back(1);
            growBox(f);
            out.push_back(std::move(f));
        } else if (type == "relation") {
            Feature f;
            if (!classify(tags, f, true) || !f.polygon) continue;
            std::vector<std::vector<LL>> outer, inner;
            for (const auto& m : el.value("members", json::array())) {
                if (m.value("type", "") != "way") continue;
                auto g = geomOf(m.value("geometry", json::array()));
                if (g.size() < 2) continue;
                (m.value("role", "") == "inner" ? inner : outer).push_back(std::move(g));
            }
            auto O = assembleRings(std::move(outer)), I = assembleRings(std::move(inner));
            if (O.empty()) continue;
            // holes go with the first outer ring that contains their first point's box (good enough at city scale)
            for (auto& o : O) {
                Box ob{180, 90, -180, -90};
                for (const auto& p : o) { ob.w = std::min(ob.w, p.lon); ob.e = std::max(ob.e, p.lon); ob.s = std::min(ob.s, p.lat); ob.n = std::max(ob.n, p.lat); }
                f.parts.push_back(o);
                int n = 1;
                for (auto& h : I) {
                    if (h.empty()) continue;
                    if (h[0].lon >= ob.w && h[0].lon <= ob.e && h[0].lat >= ob.s && h[0].lat <= ob.n) { f.parts.push_back(std::move(h)); h.clear(); n++; }
                }
                f.outerCount.push_back(n);
            }
            growBox(f);
            out.push_back(std::move(f));
        }
    }
    return out;
}

// ── projection + clipping ──
constexpr int EXTENT = 4096, BUFFER = 64;
struct P { double x, y; };
inline P project(const LL& p, int z, int tx, int ty) {
    const double n = std::ldexp(1.0, z);
    const double lat = std::max(-85.05112878, std::min(85.05112878, p.lat)) * M_PI / 180.0;
    const double x = (p.lon + 180.0) / 360.0 * n, y = (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / M_PI) / 2.0 * n;
    return {(x - tx) * EXTENT, (y - ty) * EXTENT};
}
inline int lon2tile(double lon, int z) { return (int)std::floor((lon + 180.0) / 360.0 * std::ldexp(1.0, z)); }
inline int lat2tile(double lat, int z) {
    const double r = lat * M_PI / 180.0;
    return (int)std::floor((1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * std::ldexp(1.0, z));
}

// Sutherland-Hodgman against the buffered tile square
inline std::vector<P> clipPolygon(const std::vector<P>& in) {
    const double lo = -BUFFER, hi = EXTENT + BUFFER;
    std::vector<P> poly = in;
    for (int edge = 0; edge < 4 && !poly.empty(); edge++) {
        auto inside = [&](const P& p) { switch (edge) { case 0: return p.x >= lo; case 1: return p.x <= hi; case 2: return p.y >= lo; default: return p.y <= hi; } };
        auto cross = [&](const P& a, const P& b) {
            double t;
            switch (edge) {
                case 0: t = (lo - a.x) / (b.x - a.x); return P{lo, a.y + t * (b.y - a.y)};
                case 1: t = (hi - a.x) / (b.x - a.x); return P{hi, a.y + t * (b.y - a.y)};
                case 2: t = (lo - a.y) / (b.y - a.y); return P{a.x + t * (b.x - a.x), lo};
                default: t = (hi - a.y) / (b.y - a.y); return P{a.x + t * (b.x - a.x), hi};
            }
        };
        std::vector<P> out;
        for (size_t i = 0; i < poly.size(); i++) {
            const P& cur = poly[i]; const P& prev = poly[(i + poly.size() - 1) % poly.size()];
            const bool ci = inside(cur), pi = inside(prev);
            if (ci) { if (!pi) out.push_back(cross(prev, cur)); out.push_back(cur); }
            else if (pi) out.push_back(cross(prev, cur));
        }
        poly.swap(out);
    }
    return poly;
}
// Liang-Barsky per segment, re-joined into polylines
inline std::vector<std::vector<P>> clipLine(const std::vector<P>& in) {
    const double lo = -BUFFER, hi = EXTENT + BUFFER;
    std::vector<std::vector<P>> out;
    std::vector<P> cur;
    for (size_t i = 0; i + 1 < in.size(); i++) {
        P a = in[i], b = in[i + 1];
        double t0 = 0, t1 = 1, dx = b.x - a.x, dy = b.y - a.y;
        bool ok = true;
        auto test = [&](double p, double q) {
            if (p == 0) { if (q < 0) ok = false; return; }
            const double r = q / p;
            if (p < 0) { if (r > t1) ok = false; else if (r > t0) t0 = r; }
            else { if (r < t0) ok = false; else if (r < t1) t1 = r; }
        };
        test(-dx, a.x - lo); test(dx, hi - a.x); test(-dy, a.y - lo); test(dy, hi - a.y);
        if (!ok) { if (cur.size() >= 2) out.push_back(cur); cur.clear(); continue; }
        P ca{a.x + t0 * dx, a.y + t0 * dy}, cb{a.x + t1 * dx, a.y + t1 * dy};
        if (cur.empty()) cur.push_back(ca);
        else if (t0 > 0) { if (cur.size() >= 2) out.push_back(cur); cur.clear(); cur.push_back(ca); }
        cur.push_back(cb);
        if (t1 < 1) { if (cur.size() >= 2) out.push_back(cur); cur.clear(); }
    }
    if (cur.size() >= 2) out.push_back(cur);
    return out;
}

// ── protobuf (just what MVT needs) ──
struct PB {
    std::string b;
    void varint(uint64_t v) { while (v >= 0x80) { b.push_back(char((v & 0x7f) | 0x80)); v >>= 7; } b.push_back(char(v)); }
    void key(int field, int wire) { varint((uint64_t(field) << 3) | wire); }
    void bytes(int field, const std::string& s) { key(field, 2); varint(s.size()); b += s; }
    void u32(int field, uint32_t v) { key(field, 0); varint(v); }
    void packed(int field, const std::vector<uint32_t>& v) { PB t; for (auto x : v) t.varint(x); bytes(field, t.b); }
};
inline uint32_t zz(int32_t n) { return (uint32_t(n) << 1) ^ uint32_t(n >> 31); }
inline uint32_t cmd(int id, int count) { return (id & 7) | (uint32_t(count) << 3); }

// int-quantize + drop repeats; returns false if too short
inline bool quantize(const std::vector<P>& in, std::vector<std::pair<int, int>>& out, bool ring) {
    out.clear();
    for (const auto& p : in) {
        const int x = (int)std::lround(p.x), y = (int)std::lround(p.y);
        if (!out.empty() && out.back().first == x && out.back().second == y) continue;
        out.push_back({x, y});
    }
    if (ring) { while (out.size() > 1 && out.front() == out.back()) out.pop_back(); return out.size() >= 3; }
    return out.size() >= 2;
}
inline double ringArea(const std::vector<std::pair<int, int>>& r) {
    double a = 0;
    for (size_t i = 0; i < r.size(); i++) { auto& p = r[i]; auto& q = r[(i + 1) % r.size()]; a += double(p.first) * q.second - double(q.first) * p.second; }
    return a / 2;
}

struct LayerBuilder {
    std::string name;
    std::vector<std::string> keys{"class"};
    std::vector<std::string> values;
    std::map<std::string, uint32_t> valIdx;
    std::vector<std::string> features;
    void add(const std::string& cls, bool polygon, const std::vector<uint32_t>& geom) {
        PB f;
        if (!cls.empty()) {
            auto it = valIdx.find(cls);
            uint32_t vi;
            if (it == valIdx.end()) { vi = (uint32_t)values.size(); values.push_back(cls); valIdx[cls] = vi; } else vi = it->second;
            f.packed(2, {0, vi});
        }
        f.u32(3, polygon ? 3 : 2);
        f.packed(4, geom);
        features.push_back(f.b);
    }
    std::string encode() const {
        PB l;
        l.u32(15, 2);
        l.bytes(1, name);
        for (const auto& f : features) l.bytes(2, f);
        for (const auto& k : keys) l.bytes(3, k);
        for (const auto& v : values) { PB val; val.bytes(1, v); l.bytes(4, val.b); }
        l.u32(5, EXTENT);
        return l.b;
    }
};

inline std::string gzip(const std::string& in) {
    z_stream s{};
    if (deflateInit2(&s, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return "";
    std::string out(deflateBound(&s, in.size()) + 32, '\0');
    s.next_in = (Bytef*)in.data(); s.avail_in = (uInt)in.size();
    s.next_out = (Bytef*)out.data(); s.avail_out = (uInt)out.size();
    const int r = deflate(&s, Z_FINISH);
    out.resize(s.total_out);
    deflateEnd(&s);
    return r == Z_STREAM_END ? out : "";
}

// Douglas-Peucker in tile units: coarse zooms carry far fewer vertices (the first Storage piece
// should already hold a usable overview)
inline void simplifyDP(const std::vector<P>& in, double tol, std::vector<P>& out) {
    out.clear();
    if (in.size() <= 2 || tol <= 0) { out = in; return; }
    std::vector<char> keep(in.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> st{{0, in.size() - 1}};
    const double t2 = tol * tol;
    while (!st.empty()) {
        auto [a, b] = st.back(); st.pop_back();
        double best = -1; size_t bi = 0;
        const double dx = in[b].x - in[a].x, dy = in[b].y - in[a].y, l2 = dx * dx + dy * dy;
        for (size_t i = a + 1; i < b; i++) {
            double d;
            if (l2 == 0) { const double ex = in[i].x - in[a].x, ey = in[i].y - in[a].y; d = ex * ex + ey * ey; }
            else { const double c = (in[i].x - in[a].x) * dy - (in[i].y - in[a].y) * dx; d = c * c / l2; }
            if (d > best) { best = d; bi = i; }
        }
        if (best > t2) { keep[bi] = 1; st.push_back({a, bi}); st.push_back({bi, b}); }
    }
    for (size_t i = 0; i < in.size(); i++) if (keep[i]) out.push_back(in[i]);
}
inline double tolFor(int z) { return z >= 16 ? 1.0 : std::ldexp(1.0, 16 - z); }   // z16: 1, z15: 2, ... z12: 16 units of 4096

// one tile: every feature that touches it, clipped and encoded; "" when empty
inline std::string makeTile(const std::vector<Feature>& feats, const std::vector<size_t>& cand, int z, int x, int y) {
    std::map<std::string, LayerBuilder> layers;
    std::vector<std::pair<int, int>> q;
    for (size_t idx : cand) {
        const Feature& f = feats[idx];
        if (z < f.minzoom) continue;
        std::vector<uint32_t> g;
        if (f.polygon) {
            size_t base = 0;
            int curX = 0, curY = 0;   // MVT deltas run across the whole feature
            for (int cnt : f.outerCount) {
                const size_t first = base;
                base += (size_t)cnt;   // the next polygon starts here whatever happens to this one's rings
                for (int k = 0; k < cnt && first + k < f.parts.size(); k++) {
                    const size_t ri = first + (size_t)k;
                    std::vector<P> pts; pts.reserve(f.parts[ri].size());
                    for (const auto& p : f.parts[ri]) pts.push_back(project(p, z, x, y));
                    std::vector<P> sp; simplifyDP(pts, tolFor(z), sp);
                    auto c = clipPolygon(sp);
                    if (!quantize(c, q, true)) { if (k == 0) break; continue; }    // no outer ring here: skip its holes too
                    const double a = ringArea(q);
                    if (std::fabs(a) < 2.0) { if (k == 0) break; continue; }   // smaller than a pixel or so
                    if ((k == 0 && a < 0) || (k > 0 && a > 0)) std::reverse(q.begin(), q.end());       // outer: positive area (y down)
                    g.push_back(cmd(1, 1)); g.push_back(zz(q[0].first - curX)); g.push_back(zz(q[0].second - curY)); curX = q[0].first; curY = q[0].second;
                    g.push_back(cmd(2, (int)q.size() - 1));
                    for (size_t i = 1; i < q.size(); i++) { g.push_back(zz(q[i].first - curX)); g.push_back(zz(q[i].second - curY)); curX = q[i].first; curY = q[i].second; }
                    g.push_back(cmd(7, 1));
                }
            }
        } else {
            int curX = 0, curY = 0;
            for (const auto& part : f.parts) {
                std::vector<P> pts; pts.reserve(part.size());
                for (const auto& p : part) pts.push_back(project(p, z, x, y));
                std::vector<P> sp; simplifyDP(pts, tolFor(z), sp);
                for (const auto& seg : clipLine(sp)) {
                    if (!quantize(seg, q, false)) continue;
                    g.push_back(cmd(1, 1)); g.push_back(zz(q[0].first - curX)); g.push_back(zz(q[0].second - curY)); curX = q[0].first; curY = q[0].second;
                    g.push_back(cmd(2, (int)q.size() - 1));
                    for (size_t i = 1; i < q.size(); i++) { g.push_back(zz(q[i].first - curX)); g.push_back(zz(q[i].second - curY)); curX = q[i].first; curY = q[i].second; }
                }
            }
        }
        if (g.empty()) continue;
        auto& L = layers[f.layer];
        L.name = f.layer;
        L.add(f.cls, f.polygon, g);
    }
    if (layers.empty()) return "";
    PB t;
    for (const auto& [n, L] : layers) t.bytes(3, L.encode());
    return t.b;
}

// ── PMTiles v3 ──
inline void rotate(uint64_t n, uint64_t& x, uint64_t& y, uint64_t rx, uint64_t ry) {
    if (ry == 0) { if (rx == 1) { x = n - 1 - x; y = n - 1 - y; } std::swap(x, y); }
}
inline uint64_t tileId(int z, uint64_t x, uint64_t y) {
    uint64_t acc = 0;
    for (int t = 0; t < z; t++) acc += uint64_t(1) << (2 * t);
    const uint64_t n = uint64_t(1) << z;
    uint64_t d = 0;
    for (uint64_t s = n / 2; s > 0; s /= 2) {
        const uint64_t rx = (x & s) > 0, ry = (y & s) > 0;
        d += s * s * ((3 * rx) ^ ry);
        rotate(s, x, y, rx, ry);
    }
    return acc + d;
}

struct Result { std::string bytes; int tiles = 0, minzoom = 0, maxzoom = 0; std::string error; };

// features -> a .pmtiles image (tiles gzip'd; directory + metadata uncompressed)
inline Result buildPmtiles(const std::vector<Feature>& feats, const Box& box, int minzoom, int maxzoom,
                           const std::function<void(int done, int total)>& progress = nullptr) {
    Result R; R.minzoom = minzoom; R.maxzoom = maxzoom;
    struct T { uint64_t id; std::string data; };
    std::vector<T> tiles;
    int total = 0;
    for (int z = minzoom; z <= maxzoom; z++)
        total += (lon2tile(box.e, z) - lon2tile(box.w, z) + 1) * (lat2tile(box.s, z) - lat2tile(box.n, z) + 1);
    if (total > 4000) { R.error = "The area is too big for one map (" + std::to_string(total) + " tiles)"; return R; }
    int done = 0;
    for (int z = minzoom; z <= maxzoom; z++) {
        const int x0 = lon2tile(box.w, z), x1 = lon2tile(box.e, z), y0 = lat2tile(box.n, z), y1 = lat2tile(box.s, z);
        for (int x = x0; x <= x1; x++) for (int y = y0; y <= y1; y++) {
            // tile bounds in degrees (+ the buffer, roughly) to pick candidate features
            const double n = std::ldexp(1.0, z), pad = double(BUFFER) / EXTENT;
            const double w = (x - pad) / n * 360.0 - 180.0, e = (x + 1 + pad) / n * 360.0 - 180.0;
            auto latOf = [&](double ty) { return std::atan(std::sinh(M_PI * (1 - 2 * ty / n))) * 180.0 / M_PI; };
            const double nn = latOf(y - pad), ss = latOf(y + 1 + pad);
            std::vector<size_t> cand;
            for (size_t i = 0; i < feats.size(); i++) {
                const auto& b = feats[i].bbox;
                if (z >= feats[i].minzoom && b.e >= w && b.w <= e && b.n >= ss && b.s <= nn) cand.push_back(i);
            }
            std::string raw = makeTile(feats, cand, z, x, y);
            done++;
            if (progress) progress(done, total);
            if (raw.empty()) continue;
            std::string gz = gzip(raw);
            if (gz.empty()) { R.error = "gzip failed"; return R; }
            tiles.push_back({tileId(z, x, y), std::move(gz)});
        }
    }
    if (tiles.empty()) { R.error = "No map data in the area"; return R; }
    std::sort(tiles.begin(), tiles.end(), [](const T& a, const T& b) { return a.id < b.id; });
    // root directory: every tile, consecutive offsets
    PB dir;
    dir.varint(tiles.size());
    uint64_t last = 0;
    for (const auto& t : tiles) { dir.varint(t.id - last); last = t.id; }
    for (size_t i = 0; i < tiles.size(); i++) dir.varint(1);
    for (const auto& t : tiles) dir.varint(t.data.size());
    for (size_t i = 0; i < tiles.size(); i++) dir.varint(i == 0 ? 1 : 0);   // offset+1 for the first, then "follows the previous"
    if (127 + dir.b.size() > 16384) { R.error = "Too many tiles for one directory - make the area smaller"; return R; }
    const json meta = {
        {"name", "Crema event map"}, {"attribution", "© OpenStreetMap contributors"}, {"description", "(c) OpenStreetMap contributors, ODbL. Made by crema_core."},
        {"vector_layers", json::array({{{"id", "transportation"}, {"fields", {{"class", "String"}}}}, {{"id", "building"}, {"fields", json::object()}},
                                       {{"id", "water"}, {"fields", json::object()}}, {{"id", "waterway"}, {"fields", {{"class", "String"}}}},
                                       {{"id", "park"}, {"fields", {{"class", "String"}}}}, {{"id", "landcover"}, {"fields", {{"class", "String"}}}},
                                       {{"id", "landuse"}, {"fields", {{"class", "String"}}}}})}};
    const std::string md = meta.dump();
    uint64_t dataLen = 0;
    for (const auto& t : tiles) dataLen += t.data.size();
    std::string h(127, '\0');
    auto u64 = [&](int o, uint64_t v) { for (int i = 0; i < 8; i++) h[o + i] = char((v >> (8 * i)) & 0xff); };
    auto i32 = [&](int o, int32_t v) { for (int i = 0; i < 4; i++) h[o + i] = char((uint32_t(v) >> (8 * i)) & 0xff); };
    std::memcpy(&h[0], "PMTiles", 7); h[7] = 3;
    const uint64_t rootOff = 127, mdOff = rootOff + dir.b.size(), dataOff = mdOff + md.size();
    u64(8, rootOff); u64(16, dir.b.size()); u64(24, mdOff); u64(32, md.size()); u64(40, dataOff); u64(48, 0);   // no leaves (offset must still be set)
    u64(56, dataOff); u64(64, dataLen); u64(72, tiles.size()); u64(80, tiles.size()); u64(88, tiles.size());
    h[96] = 1;   // clustered
    h[97] = 1;   // internal compression: none
    h[98] = 2;   // tile compression: gzip
    h[99] = 1;   // MVT
    h[100] = (char)minzoom; h[101] = (char)maxzoom;
    i32(102, (int32_t)std::lround(box.w * 1e7)); i32(106, (int32_t)std::lround(box.s * 1e7));
    i32(110, (int32_t)std::lround(box.e * 1e7)); i32(114, (int32_t)std::lround(box.n * 1e7));
    h[118] = (char)std::min(maxzoom, 14);
    i32(119, (int32_t)std::lround((box.w + box.e) / 2 * 1e7)); i32(123, (int32_t)std::lround((box.s + box.n) / 2 * 1e7));
    R.bytes.reserve(dataOff + dataLen);
    R.bytes = h + dir.b + md;
    for (const auto& t : tiles) R.bytes += t.data;
    R.tiles = (int)tiles.size();
    return R;
}

// ── PMTiles layout: where the directories end and where each zoom level is complete ──
// A clustered PMTiles file stores tiles by tile id = by zoom first, so a prefix of the file holds the
// directories plus every zoom up to some level. Pieces of the file (Storage blocks) fetched in order
// therefore light the map up coarse-to-fine (ADR 0002, 0.3.0).
struct Layout { uint64_t dirEnd = 0; int minzoom = 0, maxzoom = 0; std::vector<std::pair<int, uint64_t>> zoomEnds; std::string error; };

inline bool gunzip(const std::string& in, std::string& out) {
    z_stream s{};
    if (inflateInit2(&s, 15 + 32) != Z_OK) return false;
    s.next_in = (Bytef*)in.data(); s.avail_in = (uInt)in.size();
    char buf[65536]; int r;
    out.clear();
    do {
        s.next_out = (Bytef*)buf; s.avail_out = sizeof buf;
        r = inflate(&s, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&s); return false; }
        out.append(buf, sizeof buf - s.avail_out);
        if (out.size() > 64u * 1024 * 1024) { inflateEnd(&s); return false; }
    } while (r != Z_STREAM_END);
    inflateEnd(&s);
    return true;
}
inline int zoomOfTileId(uint64_t id) {
    uint64_t acc = 0;
    for (int z = 0; z < 32; z++) { const uint64_t n = uint64_t(1) << (2 * z); if (id < acc + n) return z; acc += n; }
    return 31;
}
inline Layout pmtilesLayout(const std::string& f) {
    Layout L;
    if (f.size() < 127 || std::memcmp(f.data(), "PMTiles", 7) != 0 || f[7] != 3) { L.error = "Not a PMTiles v3 file"; return L; }
    auto u64 = [&](int o) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | (unsigned char)f[o + i]; return v; };
    const uint64_t rootOff = u64(8), rootLen = u64(16), mdOff = u64(24), mdLen = u64(32), leafOff = u64(40), leafLen = u64(48), dataOff = u64(56);
    const int ic = (unsigned char)f[97];
    // an unclustered file (e.g. tilemaker's) still works, it just lights up only at the end
    if (ic != 1 && ic != 2) { L.error = "Unsupported directory compression in the map file"; return L; }
    L.minzoom = (unsigned char)f[100]; L.maxzoom = (unsigned char)f[101];
    L.dirEnd = std::max({rootOff + rootLen, mdOff + mdLen, leafLen ? leafOff + leafLen : 0});
    if (L.dirEnd > f.size()) { L.error = "Truncated map file"; return L; }
    std::map<int, uint64_t> ends;
    std::function<bool(uint64_t, uint64_t, int)> walk = [&](uint64_t off, uint64_t len, int depth) -> bool {
        if (depth > 3 || off + len > f.size()) return false;
        std::string d = f.substr(off, len), raw;
        if (ic == 2) { if (!gunzip(d, raw)) return false; } else raw = d;
        size_t i = 0;
        auto vr = [&](uint64_t& v) { v = 0; int sh = 0; while (i < raw.size()) { const unsigned char b = raw[i++]; v |= uint64_t(b & 0x7f) << sh; if (!(b & 0x80)) return true; sh += 7; if (sh > 63) return false; } return false; };
        uint64_t n;
        if (!vr(n) || n > 10000000) return false;
        std::vector<uint64_t> ids(n), runs(n), lens(n), offs(n);
        uint64_t last = 0;
        for (auto& x : ids) { uint64_t v; if (!vr(v)) return false; last += v; x = last; }
        for (auto& x : runs) if (!vr(x)) return false;
        for (auto& x : lens) if (!vr(x)) return false;
        for (uint64_t k = 0; k < n; k++) { uint64_t v; if (!vr(v)) return false; offs[k] = (v == 0 && k > 0) ? offs[k - 1] + lens[k - 1] : v - 1; }
        for (uint64_t k = 0; k < n; k++) {
            if (runs[k] == 0) { if (!walk(leafOff + offs[k], lens[k], depth + 1)) return false; continue; }   // a leaf directory
            const int z = zoomOfTileId(ids[k]);
            const uint64_t end = dataOff + offs[k] + lens[k];
            ends[z] = std::max(ends[z], end);
        }
        return true;
    };
    if (!walk(rootOff, rootLen, 0)) { L.error = "Can't read the map file's directory"; return L; }
    uint64_t run = L.dirEnd;
    for (const auto& [z, e] : ends) { run = std::max(run, e); L.zoomEnds.push_back({z, run}); }   // complete up to z once `run` bytes are in
    return L;
}

} // namespace maptiles

