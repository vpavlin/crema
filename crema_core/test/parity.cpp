// parity.cpp - crema.hpp against the JS side's vectors (packages/engine/test/make-vectors.js):
// identical Ed25519 bytes, the JS catalog verifies, the JS stamps verify, JS-sealed frames open, and
// the C++ fold ranks the same players. Build: see crema_core/test/run.sh
#include <cstdio>
#include <fstream>
#include <iostream>
#include "../src/crema.hpp"

static int fails = 0, checks = 0;
#define CHECK(cond, what) do { checks++; if (!(cond)) { fails++; std::printf("FAIL: %s\n", what); } else std::printf("ok   %s\n", what); } while (0)

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "vectors.json";
    std::ifstream f(path);
    auto v = crema::json::parse(f);
    using namespace crema;

    // keys and signatures: Ed25519 is deterministic, so the bytes must match the JS side exactly
    CHECK(ed25519Public(v["organizer"]["secretKey"]) == v["organizer"]["publicKey"].get<std::string>(), "Ed25519 public key from the secret matches @noble");
    CHECK(catalogMessage(v["catalog"]) == v["catalogMessage"].get<std::string>(), "canonical catalog message is byte-identical (incl. non-ASCII names)");
    CHECK(verifyCatalog(v["catalog"], v["organizer"]["publicKey"]), "the JS-signed catalog verifies in C++");
    json resigned = signCatalog(v["catalog"], v["organizer"]["secretKey"]);
    CHECK(resigned["sig"] == v["catalog"]["sig"], "C++ signs the catalog to the same bytes");
    json tampered = v["catalog"]; tampered["cafes"][0]["name"] = "Fake";
    CHECK(!verifyCatalog(tampered, v["organizer"]["publicKey"]), "a tampered catalog fails");

    const long long T0 = v["T0"];
    CHECK(makeStamp("kafec", v["cafes"]["kafec"]["secretKey"], T0) == v["stampAtT0"]["kafec"].get<std::string>(), "C++ makes the same stamp QR as the phone");

    // links
    Join j = parseJoinLink(v["joinLink"]);
    CHECK(j.ok && j.eventId == "brno-test" && j.secretHex == v["secretHex"].get<std::string>(), "the JS join link parses");
    CHECK(makeJoinLink(j.eventId, j.organizer, j.secretHex, j.name) == v["joinLink"].get<std::string>(), "C++ writes the same join link");
    CHECK(makeCafeLink("kafec", v["cafes"]["kafec"]["secretKey"], "Kafec", "Brno Coffee Week ☕") == v["cafeLink"].get<std::string>(), "C++ writes the same café setup link");

    // frames: a desktop that joined the event opens the phones' frames and folds the same ranking
    Game g(j, logos_sync::fromHexB(std::string(64, '9')));
    CHECK(g.topic == v["topic"].get<std::string>(), "same Delivery topic from the event secret");
    int admitted = 0;
    for (const auto& b64 : v["framesB64"]) {
        Bytes bytes; unb64any(b64.get<std::string>(), bytes);
        std::vector<Bytes> out;
        if (g.receive(bytes, out)) admitted++;
    }
    CHECK(admitted == (int)v["framesB64"].size(), "every JS-sealed frame opens and its event is admitted");
    auto st = g.state();
    CHECK(!st.catalog.is_null() && st.catalog["version"] == 1, "the catalog event folds");
    const auto& want = v["leaderboard"];
    bool same = st.leaderboard.size() == want.size();
    for (size_t i = 0; same && i < want.size(); i++)
        same = st.leaderboard[i].author == want[i]["author"].get<std::string>() && st.leaderboard[i].cafes == want[i]["cafes"].get<int>()
            && st.leaderboard[i].stamps == want[i]["stamps"].get<int>() && st.leaderboard[i].name == want[i]["name"].get<std::string>();
    CHECK(same, "the C++ fold ranks exactly like the JS fold");

    // the other way: a C++ frame (catalog v2) re-opens and a C++-signed catalog verifies
    json c2 = v["catalog"]; c2["version"] = 2;
    c2 = signCatalog(c2, v["organizer"]["secretKey"]);
    std::vector<Bytes> out;
    CHECK(g.publishCatalog(c2, T0 + 1000, out) && out.size() == 1, "C++ publishes a catalog v2 frame");
    Game g2(j, logos_sync::fromHexB(std::string(64, '8')));
    std::vector<Bytes> o2;
    CHECK(g2.receive(out[0], o2), "another C++ device opens it");
    std::ofstream w("cpp-frame.b64"); w << b64std(out[0]);
    // catch-up: a fresh C++ device's hello gets answered with what it lacks
    std::vector<Bytes> answers;
    g.receive(g2.hello(), answers);
    CHECK(!answers.empty(), "a hello from a behind device gets catch-up replies");

    std::printf("\n%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
