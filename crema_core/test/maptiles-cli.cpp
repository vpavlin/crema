// maptiles-cli.cpp - run the in-core map maker on a saved Overpass answer: maptiles-cli IN.json OUT.pmtiles w s e n
#include <cstdio>
#include <fstream>
#include <iostream>
#include "../src/maptiles.hpp"
int main(int argc, char** argv) {
    if (argc < 7) { std::fprintf(stderr, "usage: %s overpass.json out.pmtiles w s e n\n", argv[0]); return 2; }
    std::ifstream f(argv[1]); auto doc = maptiles::json::parse(f);
    auto feats = maptiles::featuresFromOverpass(doc);
    maptiles::Box box{atof(argv[3]), atof(argv[4]), atof(argv[5]), atof(argv[6])};
    auto r = maptiles::buildPmtiles(feats, box, 12, 16);
    if (!r.error.empty()) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
    std::ofstream(argv[2], std::ios::binary) << r.bytes;
    std::printf("%zu features -> %d tiles, %zu bytes\n", feats.size(), r.tiles, r.bytes.size());
}
