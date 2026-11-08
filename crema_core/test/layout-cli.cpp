#include <fstream>
#include <iostream>
#include <sstream>
#include "../src/maptiles.hpp"
int main(int c, char** v) {
    std::ifstream f(v[1], std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    auto L = maptiles::pmtilesLayout(ss.str());
    if (!L.error.empty()) { std::cout << "error: " << L.error << "\n"; return 1; }
    std::cout << "size " << ss.str().size() << " dirEnd " << L.dirEnd << " zooms " << L.minzoom << "-" << L.maxzoom << "\n";
    for (auto& [z, e] : L.zoomEnds) std::cout << "  z" << z << " complete at " << e << " (" << (e + 61439) / 61440 << " pieces)\n";
}
