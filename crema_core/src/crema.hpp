// crema.hpp - Crema's protocol and engine in C++: the twin of packages/protocol (the café's rotating
// stamp, the organizer-signed catalog, the geofence, the links) and packages/engine (one event as a
// loam-sync log: admission rules, the fold, sealed frames, catch-up). The phone runs the JS side;
// both must produce and accept the same bytes (test/parity.cpp checks the shared vectors).
#pragma once
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include "logos_sync/event.hpp"
#include "logos_sync/merge.hpp"
#include "logos_sync/signing.hpp"
#include "logos_sync/crypto.hpp"
#include "logos_sync/catchup.hpp"

namespace crema {
using json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
using logos_sync::Event;

static const char* DOMAIN = "crema";
static const int STEP_S = 30;
static const int SKEW_STEPS = 1;
static const long long CLAIM_LATE_MS = 2LL * STEP_S * 1000;
static const int DEFAULT_RADIUS_M = 75;
static const size_t MAX_FRAME = 16 * 1024;
static const char* QR_PREFIX = "CREMA1";

// ── encoding ──
inline std::string hex(const Bytes& b) { return logos_sync::toHexS(b.data(), b.size()); }
inline Bytes unhex(const std::string& s) { return logos_sync::fromHexB(s); }
inline Bytes sbytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
static const char* B64U = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
inline std::string b64url(const Bytes& b) {
    std::string s; size_t i = 0;
    for (; i + 2 < b.size(); i += 3) {
        unsigned n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        s += B64U[n >> 18]; s += B64U[(n >> 12) & 63]; s += B64U[(n >> 6) & 63]; s += B64U[n & 63];
    }
    if (i < b.size()) {
        unsigned n = (b[i] << 16) | ((i + 1 < b.size() ? b[i + 1] : 0) << 8);
        s += B64U[n >> 18]; s += B64U[(n >> 12) & 63];
        if (i + 1 < b.size()) s += B64U[(n >> 6) & 63];
    }
    return s;
}
inline bool unb64url(const std::string& s, Bytes& out) {
    out.clear(); unsigned buf = 0; int bits = 0;
    for (char c : s) {
        const char* p = strchr(B64U, c);
        if (!c || !p) return false;
        buf = (buf << 6) | unsigned(p - B64U); bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((buf >> bits) & 255); }
    }
    return true;
}
// standard base64 (for loam_core's sendSealed / received)
inline std::string b64std(const Bytes& b) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s; size_t i = 0;
    for (; i + 2 < b.size(); i += 3) { unsigned n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2]; s += T[n >> 18]; s += T[(n >> 12) & 63]; s += T[(n >> 6) & 63]; s += T[n & 63]; }
    if (i < b.size()) { unsigned n = (b[i] << 16) | ((i + 1 < b.size() ? b[i + 1] : 0) << 8); s += T[n >> 18]; s += T[(n >> 12) & 63]; s += (i + 1 < b.size()) ? T[(n >> 6) & 63] : '='; s += '='; }
    return s;
}
// decodes standard and URL-safe base64, padded or not
inline bool unb64any(const std::string& s, Bytes& out) {
    out.clear(); unsigned buf = 0; int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52; else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63; else if (c == '=' || c == '\n' || c == '\r') continue; else return false;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((buf >> bits) & 255); }
    }
    return true;
}
inline Bytes randomBytes(size_t n) { Bytes b(n); RAND_bytes(b.data(), (int)n); return b; }
// Same as the JS canonical(): sorted keys, JSON.stringify strings, integers (loam-sync's cjson)
inline std::string canonical(const json& v) { return logos_sync::cjson(v); }

// ── Ed25519 (cafés and organizers) ──
struct Key { std::string secretKey, publicKey; };
inline std::string ed25519Public(const std::string& secretHex) {
    Bytes sk = unhex(secretHex);
    if (sk.size() != 32) return "";
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, sk.data(), sk.size());
    if (!k) return "";
    Bytes pub(32); size_t n = 32;
    EVP_PKEY_get_raw_public_key(k, pub.data(), &n);
    EVP_PKEY_free(k);
    return hex(pub);
}
inline Key newKey() {
    Bytes sk = randomBytes(32);
    return { hex(sk), ed25519Public(hex(sk)) };
}
inline std::string ed25519Sign(const std::string& secretHex, const std::string& msg) {
    Bytes sk = unhex(secretHex);
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, sk.data(), sk.size());
    if (!k) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    Bytes sig(64); size_t n = 64;
    bool ok = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestSign(ctx, sig.data(), &n, (const unsigned char*)msg.data(), msg.size()) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(k);
    return ok ? hex(sig) : "";
}
inline bool ed25519Verify(const std::string& pubHex, const std::string& msg, const std::string& sigHex) {
    Bytes pk = unhex(pubHex), sig = unhex(sigHex);
    if (pk.size() != 32 || sig.size() != 64) return false;
    EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pk.data(), pk.size());
    if (!k) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestVerify(ctx, sig.data(), sig.size(), (const unsigned char*)msg.data(), msg.size()) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(k);
    return ok;
}

// ── the café's rotating stamp ──
inline long long stepOf(long long nowMs) { return nowMs / 1000 / STEP_S; }
inline long long stepTimeMs(long long step) { return step * STEP_S * 1000; }
inline std::string stampMessage(const std::string& cafeId, long long step) { return "crema/stamp/v1|" + cafeId + "|" + std::to_string(step); }
inline bool validCafeId(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}
inline std::string base36(long long v) {
    if (v == 0) return "0";
    std::string s; while (v > 0) { int d = v % 36; s += char(d < 10 ? '0' + d : 'a' + d - 10); v /= 36; }
    std::reverse(s.begin(), s.end()); return s;
}
inline std::string makeStamp(const std::string& cafeId, const std::string& cafeSecretHex, long long nowMs) {
    const long long step = stepOf(nowMs);
    Bytes sig = unhex(ed25519Sign(cafeSecretHex, stampMessage(cafeId, step)));
    return std::string(QR_PREFIX) + "." + cafeId + "." + base36(step) + "." + b64url(sig);
}
inline bool verifyStampSig(const std::string& cafeId, long long step, const std::string& sigHex, const std::string& cafePubHex) {
    return ed25519Verify(cafePubHex, stampMessage(cafeId, step), sigHex);
}

// ── the organizer's signed catalog ──
inline std::string catalogMessage(json body) { body.erase("sig"); return "crema/catalog/v1|" + canonical(body); }
inline json signCatalog(json catalog, const std::string& organizerSecretHex) {
    catalog.erase("sig");
    catalog["organizer"] = ed25519Public(organizerSecretHex);
    catalog["sig"] = ed25519Sign(organizerSecretHex, catalogMessage(catalog));
    return catalog;
}
inline bool verifyCatalog(const json& c, const std::string& expectedOrganizer) {
    if (!c.is_object() || !c.contains("sig") || !c.contains("organizer") || !c.contains("cafes") || !c["cafes"].is_array()) return false;
    if (!c["sig"].is_string() || !c["organizer"].is_string()) return false;
    if (!expectedOrganizer.empty() && c["organizer"].get<std::string>() != expectedOrganizer) return false;
    return ed25519Verify(c["organizer"].get<std::string>(), catalogMessage(c), c["sig"].get<std::string>());
}

// ── links (no URL library: the same tiny encoder as the JS side) ──
inline std::string pctEncode(const std::string& s) {
    // encodeURIComponent: keep A-Z a-z 0-9 - _ . ! ~ * ' ( )
    std::string o; char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-_.!~*'()", c)) o += char(c);
        else { snprintf(buf, sizeof buf, "%%%02X", c); o += buf; }
    }
    return o;
}
inline std::string pctDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) { o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}
inline std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> m; size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!kv.empty()) { size_t eq = kv.find('='); m[pctDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : pctDecode(kv.substr(eq + 1)); }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return m;
}
// insertion order matters (it's what the JS side produces): e, o, k, n
inline std::string makeJoinLink(const std::string& eventId, const std::string& organizer, const std::string& secretHex, const std::string& name) {
    return "crema:join?e=" + pctEncode(eventId) + "&o=" + pctEncode(organizer) + "&k=" + pctEncode(b64url(unhex(secretHex))) + "&n=" + pctEncode(name);
}
inline std::string makeCafeLink(const std::string& cafeId, const std::string& secretHex, const std::string& name, const std::string& eventName) {
    return "crema:cafe?c=" + pctEncode(cafeId) + "&k=" + pctEncode(b64url(unhex(secretHex))) + "&n=" + pctEncode(name) + "&e=" + pctEncode(eventName);
}
struct Join { std::string eventId, organizer, secretHex, name; bool ok = false; };
inline Join parseJoinLink(const std::string& text) {
    Join j;
    const std::string pre = "crema:join?";
    if (text.rfind(pre, 0) != 0) return j;
    auto q = parseQuery(text.substr(pre.size()));
    j.eventId = q["e"]; j.organizer = q["o"]; j.name = q.count("n") && !q["n"].empty() ? q["n"] : q["e"];
    Bytes s;
    if (j.eventId.empty() || j.organizer.size() != 64 || !unb64url(q["k"], s) || s.size() != 32) return j;
    j.secretHex = hex(s); j.ok = true;
    return j;
}

// ── the engine: one event as a loam-sync log ──
inline std::string stampId(const std::string& author, const std::string& cafeId, long long step) { return "st-" + author + "-" + cafeId + "-" + std::to_string(step); }
inline std::string catalogId(long long version) { char b[16]; snprintf(b, sizeof b, "%08lld", version); return std::string("cat-") + b; }

inline bool validCatalogEvent(const Event& e, const Join& join, json* out = nullptr) {
    if (e.type != "catalog" || !e.payload.is_object() || !e.payload.contains("catalog")) return false;
    const json& c = e.payload["catalog"];
    if (!c.is_object() || c.value("eventId", std::string()) != join.eventId || !c.contains("version") || !c["version"].is_number_integer()) return false;
    if (e.id != catalogId(c["version"].get<long long>())) return false;
    if (!verifyCatalog(c, join.organizer)) return false;
    if (out) *out = c;
    return true;
}
inline bool validStamp(const Event& e, const json& catalog) {
    if (e.type != "stamp" || catalog.is_null() || !logos_sync::verifyEvent(DOMAIN, e)) return false;
    const json& p = e.payload;
    if (!p.is_object() || !p.contains("cafeId") || !p["cafeId"].is_string() || !p.contains("step") || !p["step"].is_number_integer() || !p.contains("cafeSig") || !p["cafeSig"].is_string()) return false;
    const std::string cafeId = p["cafeId"]; const long long step = p["step"];
    if (e.id != stampId(e.dev, cafeId, step)) return false;
    std::string pub;
    for (const auto& c : catalog["cafes"]) if (c.value("id", std::string()) == cafeId) pub = c.value("pubkey", std::string());
    if (pub.empty() || !verifyStampSig(cafeId, step, p["cafeSig"], pub)) return false;
    const long long t = stepTimeMs(step);
    if (t < catalog.value("start", 0LL) || t > catalog.value("end", 0LL)) return false;
    return std::llabs(e.hlc.wall - t) <= CLAIM_LATE_MS;
}
inline bool validProfile(const Event& e) {
    return e.type == "profile" && logos_sync::verifyEvent(DOMAIN, e) && e.payload.is_object() && e.payload.contains("name") && e.payload["name"].is_string() && e.payload["name"].get<std::string>().size() <= 40 * 4;
}

struct Standing { std::string author, name; int cafes = 0, stamps = 0; long long reachedAt = 0; std::vector<std::string> visited; };
struct Folded { json catalog; std::vector<Standing> leaderboard; std::map<std::string, std::string> names; };

inline Folded fold(const std::vector<Event>& log, const Join& join) {
    Folded f; f.catalog = nullptr;
    for (const auto& e : log) {
        json c;
        if (validCatalogEvent(e, join, &c) && (f.catalog.is_null() || c["version"].get<long long>() > f.catalog["version"].get<long long>())) f.catalog = c;
    }
    std::vector<Event> sorted = log;
    std::sort(sorted.begin(), sorted.end(), [](const Event& a, const Event& b) { return logos_sync::compareHlc(a.hlc, b.hlc) < 0; });
    struct P { std::set<std::string> cafes, days; int stamps = 0; long long reachedAt = 0; std::vector<std::string> order; };
    std::map<std::string, P> players;
    for (const auto& e : sorted) {
        if (validProfile(e)) { std::string n = e.payload["name"]; n.erase(0, n.find_first_not_of(" \t")); while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back(); f.names[e.dev] = n; continue; }
        if (!validStamp(e, f.catalog)) continue;
        P& p = players[e.dev];
        const std::string cafeId = e.payload["cafeId"]; const long long step = e.payload["step"];
        const std::string day = cafeId + "@" + std::to_string((step * STEP_S) / 86400);
        if (p.days.count(day)) continue;
        p.days.insert(day); p.stamps++;
        if (!p.cafes.count(cafeId)) { p.cafes.insert(cafeId); p.order.push_back(cafeId); p.reachedAt = stepTimeMs(step); }
    }
    for (auto& [author, p] : players) {
        Standing s; s.author = author; s.name = f.names.count(author) ? f.names[author] : ""; s.cafes = (int)p.cafes.size(); s.stamps = p.stamps; s.reachedAt = p.reachedAt; s.visited = p.order;
        f.leaderboard.push_back(s);
    }
    std::sort(f.leaderboard.begin(), f.leaderboard.end(), [](const Standing& a, const Standing& b) {
        if (a.cafes != b.cafes) return a.cafes > b.cafes;
        if (a.reachedAt != b.reachedAt) return a.reachedAt < b.reachedAt;
        return a.author < b.author;
    });
    return f;
}

// One event as seen from this desktop: its log, the sealing key, and frames in/out.
class Game {
public:
    Join join;
    std::vector<Event> log;
    std::string topic, author;

    Game(const Join& j, const logos_sync::Bytes& playerSecret)
        : join(j), m_signer(playerSecret), m_group(logos_sync::crypto::deriveIdentity(unhex(j.secretHex), DOMAIN)), m_clock("") {
        topic = logos_sync::crypto::topicFor(m_group, DOMAIN);
        author = logos_sync::address(m_signer.publicKey());
        m_clock = logos_sync::Clock(author);
    }

    bool admit(const Event& e) {
        if (e.id.empty()) return false;
        if (e.type == "catalog") { if (!validCatalogEvent(e, join)) return false; }
        else if (e.type == "stamp" || e.type == "profile") { if (!logos_sync::verifyEvent(DOMAIN, e)) return false; }
        else return false;
        const bool added = logos_sync::mergeOne(log, e);
        if (added) m_clock.receive(e.hlc);
        return added;
    }
    void primeClock() { for (const auto& e : log) m_clock.receive(e.hlc); }

    Bytes frame(const json& obj, const std::string& id) const {
        const std::string text = obj.dump();
        return logos_sync::crypto::seal(m_group, DOMAIN, id, Bytes(text.begin(), text.end()), topic);
    }
    Bytes eventFrame(const Event& e) const { return frame(json{{"t", "ev"}, {"e", logos_sync::eventToJson(e)}}, e.id); }
    Bytes catchupFrame(const json& m) const { return frame(json{{"t", "cu"}, {"m", m}}, "cu-" + hex(randomBytes(12))); }
    Bytes hello() const { return catchupFrame(logos_sync::catchup::buildInitial(log, author)); }

    // A frame from the topic: { changed, out frames }
    bool receive(const Bytes& bytes, std::vector<Bytes>& out) {
        if (bytes.empty() || bytes.size() > MAX_FRAME) return false;
        json msg;
        try {
            Bytes plain = logos_sync::crypto::open(m_group, bytes, topic);
            msg = json::parse(std::string(plain.begin(), plain.end()));
        } catch (...) { return false; }
        if (!msg.is_object()) return false;
        const std::string t = msg.value("t", std::string());
        if (t == "ev" && msg.contains("e") && msg["e"].is_object()) return admit(logos_sync::eventFromJson(msg["e"]));
        if (t == "cu" && msg.contains("m") && msg["m"].is_object() && msg["m"].value("from", std::string()) != author) {
            auto step = logos_sync::catchup::respond(log, msg["m"], author);
            for (const auto& r : step.replies) out.push_back(catchupFrame(r));
            for (const auto& e : step.serve) out.push_back(eventFrame(e));
        }
        return false;
    }

    // The organizer publishes a (signed) catalog version
    bool publishCatalog(const json& signedCatalog, long long nowMs, std::vector<Bytes>& out) {
        Event e; e.v = 1; e.id = catalogId(signedCatalog.value("version", 0LL)); e.type = "catalog";
        e.hlc = m_clock.send(nowMs); e.dev = author; e.payload = json{{"catalog", signedCatalog}};
        if (!admit(e)) return false;
        out.push_back(eventFrame(e));
        return true;
    }
    Folded state() const { return fold(log, join); }

private:
    logos_sync::SoftwareSigner m_signer;
    logos_sync::crypto::Identity m_group;
    logos_sync::Clock m_clock;
};

} // namespace crema
