#include "crema_core_impl.h"
#include "logos_sdk.h"   // LogosModules (modules().loam_core) + the generated glue

#include <QMetaObject>
#include <QTimer>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
#include <curl/curl.h>
#include <thread>
#include "maptiles.hpp"
#include <pwd.h>
#include <unistd.h>
#include "qrcodegen.hpp"

using crema::json;
namespace fs = std::filesystem;

namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string ok(json j = json::object()) { j["ok"] = true; return j.dump(); }
std::string fail(const std::string& e) { return json{{"ok", false}, {"error", e}}.dump(); }

// Owner-only files: the data dir holds the organizer key and every café's secret key
constexpr fs::perms kPrivateFile = fs::perms::owner_read | fs::perms::owner_write;
bool writeAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
        if (!f) return false;
        std::error_code ec;
        fs::permissions(tmp, kPrivateFile, fs::perm_options::replace, ec);
        f << content; f.flush();
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string realHome() {
    if (struct passwd* pw = getpwuid(getuid())) if (pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    const char* h = std::getenv("HOME");
    return (h && *h == '/') ? h : "";
}
json qrJson(const std::string& text) {
    const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int n = qr.getSize();
    json cells = json::array();
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) cells.push_back(qr.getModule(x, y) ? 1 : 0);
    return json{{"n", n}, {"cells", std::move(cells)}};
}
std::string slug(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c)) o += char(tolower(c));
        else if (!o.empty() && o.back() != '-') o += '-';
        if (o.size() >= 30) break;
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o.empty() ? "event" : o;
}
// "YYYY-MM-DD" -> local midnight in ms (Brno: the event days are local days)
long long localMidnightMs(const std::string& d) {
    struct tm t {};
    if (sscanf(d.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) return -1;
    t.tm_year -= 1900; t.tm_mon -= 1; t.tm_isdst = -1;
    const time_t s = mktime(&t);
    return s < 0 ? -1 : (long long)s * 1000;
}
bool validCafe(const json& c, std::string& why) {
    if (!c.is_object()) { why = "a café must be an object"; return false; }
    const std::string id = c.value("id", std::string());
    if (!crema::validCafeId(id)) { why = "café id '" + id + "': a-z, 0-9 and '-', up to 40"; return false; }
    if (!c.contains("name") || !c["name"].is_string() || c["name"].get<std::string>().empty() || c["name"].get<std::string>().size() > 80) { why = "café " + id + ": a name of 1-80 characters"; return false; }
    if (!c.contains("latE6") || !c["latE6"].is_number_integer() || !c.contains("lonE6") || !c["lonE6"].is_number_integer()) { why = "café " + id + ": latE6/lonE6 as whole microdegrees"; return false; }
    const long long la = c["latE6"], lo = c["lonE6"];
    if (la < -90000000 || la > 90000000 || lo < -180000000 || lo > 180000000) { why = "café " + id + ": position out of range"; return false; }
    return true;
}

// The event map: PMTiles v3 header (127 bytes, little-endian) -> zooms + box (degrees * 1e7)
bool pmtilesInfo(const std::string& path, json& out, std::string& why) {
    std::ifstream f(path, std::ios::binary);
    unsigned char h[127];
    if (!f || !f.read(reinterpret_cast<char*>(h), sizeof h)) { why = "Can't read the file"; return false; }
    if (std::memcmp(h, "PMTiles", 7) != 0 || h[7] != 3) { why = "Not a PMTiles v3 file (make one with tools/make-map.sh)"; return false; }
    if (h[99] != 1) { why = "The map must hold vector tiles (MVT)"; return false; }
    auto i32 = [&](int o) { return (int32_t)((uint32_t)h[o] | (uint32_t)h[o + 1] << 8 | (uint32_t)h[o + 2] << 16 | (uint32_t)h[o + 3] << 24); };
    const long long w = i32(102) / 10, so = i32(106) / 10, e = i32(110) / 10, n = i32(114) / 10;
    if (!(w < e && so < n)) { why = "The map has no area"; return false; }
    out = {{"bboxE6", {w, so, e, n}}, {"minzoom", (int)h[100]}, {"maxzoom", (int)h[101]}};
    return true;
}
std::string sha256File(const std::string& path, long long& size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), nullptr);
    char buf[65536]; size = 0;
    while (f) { f.read(buf, sizeof buf); auto n = f.gcount(); if (n > 0) { EVP_DigestUpdate(c, buf, (size_t)n); size += n; } }
    unsigned char md[32]; unsigned int ml = 0;
    EVP_DigestFinal_ex(c, md, &ml); EVP_MD_CTX_free(c);
    return crema::hex(crema::Bytes(md, md + 32));
}
std::string resultString(const StdLogosResult& r) { return r.value.is_string() ? r.value.get<std::string>() : std::string(); }
json resultJson(const StdLogosResult& r) {
    if (r.value.is_string()) { json j = json::parse(r.value.get<std::string>(), nullptr, false); return j.is_discarded() ? json() : j; }
    return r.value;
}

} // namespace

// ── lifecycle ──
CremaCoreImpl::~CremaCoreImpl() {
    if (m_life) { std::lock_guard<std::mutex> g(m_life->mu); m_life->self = nullptr; }
    if (m_timer) { m_timer->stop(); delete m_timer; m_timer = nullptr; }
}

void CremaCoreImpl::onContextReady() {
    if (!m_life) { m_life = std::make_shared<Life>(); m_life->self = this; }
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    setupDataDir();
    loadAll();
    // Basecamp 0.3 rejects calls to other modules from inside onContextReady(): the timer starts
    // the transport a second later, then keeps catch-up going.
    m_timer = new QTimer();
    QObject::connect(m_timer, &QTimer::timeout, [this] {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        pollStorage();
        if (!m_ready) {
            startTransport();
            if (m_starting && nowMs() - m_lastPollMs >= 3000) {
                m_lastPollMs = nowMs();
                auto life = m_life;
                try { modules().loam_core.statusAsync([life](std::string st) { withSelf(life, [st](CremaCoreImpl* s) { s->onStatus(st); }); }); } catch (...) {}
            }
            return;
        }
        // catch-up: right after connecting, then every 5 minutes
        static const long long kRounds[] = {3000, 10000, 25000};
        const long long due = m_helloRounds < 3 ? kRounds[m_helloRounds] : 300000;
        if (nowMs() - m_lastHelloMs >= due) { m_helloRounds++; helloAll(); }
    });
    m_timer->start(1000);
    publish();
}

void CremaCoreImpl::setupDataDir() {
    if (const char* ov = std::getenv("CREMA_CORE_DATA")) m_dataDir = ov;
    else {
        const std::string home = realHome();
        m_dataDir = (home.empty() ? std::string("/tmp") : home) + "/.crema-core";
    }
    std::error_code ec;
    fs::create_directories(m_dataDir, ec);
    fs::permissions(m_dataDir, fs::perms::owner_all, fs::perm_options::replace, ec);
}

void CremaCoreImpl::loadAll() {
    // this desktop's player identity (secp256k1) and organizer key (Ed25519)
    std::string ps = readFile(m_dataDir + "/player.key");
    m_playerSecret = crema::unhex(ps.substr(0, 64));
    if (m_playerSecret.size() != 32) {
        m_playerSecret = logos_sync::generatePrivateKey();
        writeAtomic(m_dataDir + "/player.key", crema::hex(m_playerSecret));
    }
    m_organizerSecret = readFile(m_dataDir + "/organizer.key").substr(0, 64);
    if (crema::ed25519Public(m_organizerSecret).empty()) {
        m_organizerSecret = crema::newKey().secretKey;
        writeAtomic(m_dataDir + "/organizer.key", m_organizerSecret);
    }
    json idx = json::parse(readFile(m_dataDir + "/events.json"), nullptr, false);
    if (!idx.is_object()) idx = json::object();
    m_current = idx.value("current", std::string());
    for (const auto& id : idx.value("events", json::array())) {
        if (!id.is_string()) continue;
        json e = json::parse(readFile(m_dataDir + "/event-" + id.get<std::string>() + ".json"), nullptr, false);
        if (!e.is_object()) continue;
        auto ev = std::make_unique<Ev>();
        ev->join = crema::parseJoinLink(e.value("join", std::string()));
        if (!ev->join.ok) continue;
        ev->organizer = e.value("organizer", false);
        ev->draft = e.value("draft", json::object());
        // a local, never .items() of a temporary: the proxy outlives it (an endless loop on restart)
        const json keys = e.value("cafeKeys", json::object());
        for (auto it = keys.begin(); it != keys.end(); ++it) if (it->is_string()) ev->cafeKeys[it.key()] = it->get<std::string>();
        ev->game = std::make_unique<crema::Game>(ev->join, m_playerSecret);
        for (const auto& j : e.value("log", json::array())) ev->game->admit(logos_sync::eventFromJson(j));
        ev->game->primeClock();
        m_events[ev->join.eventId] = std::move(ev);
    }
    if (!m_events.count(m_current)) m_current = m_events.empty() ? "" : m_events.begin()->first;
}

void CremaCoreImpl::saveIndex() {
    json ids = json::array();
    for (const auto& [id, e] : m_events) ids.push_back(id);
    writeAtomic(m_dataDir + "/events.json", json{{"current", m_current}, {"events", ids}}.dump());
}
void CremaCoreImpl::saveEvent(const Ev& e) {
    json log = json::array();
    for (const auto& x : e.game->log) log.push_back(logos_sync::eventToJson(x));
    json keys = json::object();
    for (const auto& [k, v] : e.cafeKeys) keys[k] = v;
    const auto& j = e.join;
    writeAtomic(m_dataDir + "/event-" + j.eventId + ".json", json{
        {"join", crema::makeJoinLink(j.eventId, j.organizer, j.secretHex, j.name)},
        {"organizer", e.organizer}, {"draft", e.draft}, {"cafeKeys", keys}, {"log", log}}.dump());
}
CremaCoreImpl::Ev* CremaCoreImpl::find(const std::string& id) {
    auto it = m_events.find(id);
    return it == m_events.end() ? nullptr : it->second.get();
}

// ── transport: loam_core (plain relay; catch-up is ours) ──
void CremaCoreImpl::withSelf(const std::shared_ptr<Life>& life, std::function<void(CremaCoreImpl*)> fn) {
    if (!life) return;
    std::lock_guard<std::mutex> g(life->mu);
    CremaCoreImpl* self = life->self;
    if (!self || !self->m_timer) return;
    QMetaObject::invokeMethod(self->m_timer, [life, fn] {
        CremaCoreImpl* s2 = nullptr;
        { std::lock_guard<std::mutex> g2(life->mu); s2 = life->self; }
        if (s2) { std::lock_guard<std::recursive_mutex> lk(s2->m_mtx); fn(s2); }
    }, Qt::QueuedConnection);
}

void CremaCoreImpl::startTransport() {
    if (m_ready || m_starting) return;
    const long long backoff = std::min<long long>(60000, 5000LL << std::min(m_startFailures, 4));
    if (m_startFailures > 0 && nowMs() - m_lastStartMs < backoff) return;
    m_lastStartMs = nowMs();
    m_starting = true;
    json cfg = {{"mode", "Core"}, {"preset", "logos.test"}, {"useChannels", false}};
    if (const char* ov = std::getenv("CREMA_DELIVERY_CFG")) { auto j = json::parse(ov, nullptr, false); if (j.is_object()) cfg = j; }
    auto life = m_life;
    try {
        // callbacks arrive on the IPC thread: never call a module from inside one - hop to ours
        modules().loam_core.onReceived([life](const std::string& topic, const std::string&, const std::string& payloadB64, int64_t) {
            withSelf(life, [topic, payloadB64](CremaCoreImpl* s) { s->onFrame(topic, payloadB64); });
        });
        modules().loam_core.onStatusChanged([life](const std::string& st) { withSelf(life, [st](CremaCoreImpl* s) { s->onStatus(st); }); });
        modules().loam_core.setSenderIdAsync(crema::hex(m_playerSecret).substr(0, 16), [](std::string) {});
        modules().loam_core.startAsync(cfg.dump(), [life](std::string err) {
            if (err.empty() || err == "{}" || err.find("\"ok\":true") != std::string::npos) return;
            withSelf(life, [err](CremaCoreImpl* s) { if (s->m_ready) return; s->m_starting = false; s->m_startFailures++; s->m_status = "Transport error: " + err; s->publish(); });
        });
        m_status = "Connecting...";
    } catch (const std::exception& e) {
        m_starting = false; m_startFailures++;
        m_status = std::string("loam_core unavailable: ") + e.what();
    }
    publish();
}

void CremaCoreImpl::onStatus(const std::string& st) {
    if (st.empty()) return;
    if (st == "Connected" && !m_ready) {
        m_ready = true; m_startFailures = 0; m_helloRounds = 0;
        for (auto& [id, e] : m_events) joinTopic(*e);
        helloAll();
    } else if (m_ready && st == "Connected" && m_status != "Connected") {
        helloAll();   // back after a stall: catch up what was missed
    }
    m_status = st;
    publish();
}

void CremaCoreImpl::joinTopic(Ev& e) {
    if (!m_ready || e.joined) return;
    e.joined = true;
    auto life = m_life;
    const std::string id = e.join.eventId;
    modules().loam_core.joinAsync(e.game->topic, [life, id](std::string r) {
        if (r.find("\"ok\":false") == std::string::npos && r.find("error") == std::string::npos) return;
        withSelf(life, [id](CremaCoreImpl* s) { if (Ev* e = s->find(id)) e->joined = false; });
    });
}

void CremaCoreImpl::sendFrames(const Ev& e, const std::vector<crema::Bytes>& frames) {
    if (!m_ready) return;   // the log keeps it; catch-up delivers it later
    for (const auto& f : frames) {
        try { modules().loam_core.sendSealedAsync(e.game->topic, crema::b64std(f), [](std::string) {}); } catch (...) {}
    }
}

void CremaCoreImpl::helloAll() {
    m_lastHelloMs = nowMs();
    for (auto& [id, e] : m_events) { joinTopic(*e); sendFrames(*e, {e->game->hello()}); }
}

void CremaCoreImpl::onFrame(const std::string& topic, const std::string& payloadB64) {
    for (auto& [id, e] : m_events) {
        if (e->game->topic != topic) continue;
        crema::Bytes bytes;
        if (!crema::unb64any(payloadB64, bytes)) return;
        // older delivery paths hand a base64 text of the bytes once more: peel one layer if needed
        std::vector<crema::Bytes> out;
        bool changed = e->game->receive(bytes, out);
        if (!changed && out.empty()) {
            crema::Bytes inner;
            if (crema::unb64any(std::string(bytes.begin(), bytes.end()), inner) && !inner.empty()) changed = e->game->receive(inner, out);
        }
        sendFrames(*e, out);
        if (changed) { saveEvent(*e); publish(); }
        return;
    }
}

// ── state for the view ──
json CremaCoreImpl::eventJson(const Ev& e) {
    const auto st = e.game->state();
    json lb = json::array();
    for (const auto& s : st.leaderboard)
        lb.push_back({{"author", s.author}, {"name", s.name}, {"cafes", s.cafes}, {"stamps", s.stamps}, {"visited", s.visited}, {"reachedAt", s.reachedAt}});
    json j = {{"eventId", e.join.eventId}, {"name", st.catalog.is_null() ? e.join.name : st.catalog.value("name", e.join.name)},
              {"organizer", e.organizer}, {"catalog", st.catalog}, {"leaderboard", lb}, {"logSize", e.game->log.size()}};
    if (e.organizer) {
        j["draft"] = e.draft;
        if (j["draft"].contains("map") && j["draft"]["map"].contains("pieces")) {   // the view needs counts, not 30 paths
            json& dm = j["draft"]["map"];
            int done = 0;
            for (const auto& pc : dm["pieces"]) if (!pc.value("cid", "").empty()) done++;
            dm["piecesDone"] = done; dm["piecesTotal"] = dm["pieces"].size(); dm.erase("pieces"); dm.erase("zoomEnds");
        }
        const long long pub = st.catalog.is_null() ? 0 : st.catalog.value("version", 0LL);
        j["publishedVersion"] = pub;
        // unpublished changes: the draft's cafés differ from the published list
        bool dirty = st.catalog.is_null();
        if (!dirty) {
            json a = e.draft.value("cafes", json::array()), b = json::array();
            for (const auto& c : st.catalog["cafes"]) { json x = c; x.erase("pubkey"); b.push_back(x); }
            dirty = a != b || st.catalog.value("name", "") != e.draft.value("name", "");
            const std::string pubMap = st.catalog.contains("map") ? st.catalog["map"].value("cid", "") : "";
            const json dm = e.draft.value("map", json());
            const std::string draftMap = dm.is_object() && dm.value("state", "") == "ready" ? dm.value("cid", "") : "";
            if (pubMap != draftMap && !(dm.is_object() && dm.value("state", "") == "uploading" && pubMap.empty())) dirty = true;
        }
        j["unpublished"] = dirty;
        auto mk = m_making.find(e.join.eventId);
        if (mk != m_making.end()) j["mapMaking"] = mk->second;
    }
    return j;
}

void CremaCoreImpl::publish() { emit stateChanged(snapshot()); }

std::string CremaCoreImpl::snapshot() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    json evs = json::array();
    for (const auto& [id, e] : m_events)
        evs.push_back({{"eventId", id}, {"name", e->join.name}, {"organizer", e->organizer}});
    json out = {{"status", m_status}, {"me", logos_sync::address(logos_sync::pubFromPriv(m_playerSecret))},
                {"current", m_current}, {"events", evs}, {"dataDir", m_dataDir}};
    if (Ev* e = find(m_current)) out["event"] = eventJson(*e);
    return ok(out);
}

// ── the organizer's actions ──
std::string CremaCoreImpl::createEvent(std::string name, std::string city, std::string startDate, std::string days) try {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (name.empty() || name.size() > 80) return fail("A name of 1-80 characters");
    const long long start = localMidnightMs(startDate);
    if (start < 0) return fail("Start date as YYYY-MM-DD");
    int nd = 0; try { nd = std::stoi(days); } catch (...) {}
    if (nd < 1 || nd > 60) return fail("1 to 60 days");
    crema::Join j;
    j.eventId = slug(name) + "-" + crema::hex(crema::randomBytes(3));
    j.organizer = crema::ed25519Public(m_organizerSecret);
    j.secretHex = crema::hex(crema::randomBytes(32));
    j.name = name; j.ok = true;
    auto ev = std::make_unique<Ev>();
    ev->join = j; ev->organizer = true;
    ev->draft = {{"name", name}, {"city", city}, {"start", start}, {"end", start + nd * 86400000LL - 1}, {"cafes", json::array()}};
    ev->game = std::make_unique<crema::Game>(j, m_playerSecret);
    joinTopic(*ev);
    saveEvent(*ev);
    m_current = j.eventId;
    m_events[j.eventId] = std::move(ev);
    saveIndex();
    publish();
    return ok({{"eventId", j.eventId}});
} catch (const std::exception& e) { return fail(e.what()); }

std::string CremaCoreImpl::addCafe(std::string eventId, std::string cafeJson) {
    return importCafes(eventId, "[" + cafeJson + "]");
}

std::string CremaCoreImpl::importCafes(std::string eventId, std::string listJson) try {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer) return fail("Not an event you organize");
    json list = json::parse(listJson, nullptr, false);
    if (list.is_object() && list.contains("cafes")) list = list["cafes"];   // a seed file as a whole
    if (!list.is_array() || list.empty()) return fail("Expected a list of cafés");
    json& cafes = e->draft["cafes"];
    int added = 0, replaced = 0;
    for (const auto& c : list) {
        std::string why;
        if (!validCafe(c, why)) return fail(why);
    }
    for (const auto& c : list) {
        json cafe = {{"id", c["id"]}, {"name", c["name"]}, {"latE6", c["latE6"]}, {"lonE6", c["lonE6"]},
                     {"radiusM", c.value("radiusM", (long long)crema::DEFAULT_RADIUS_M)}};
        if (c.contains("address") && c["address"].is_string() && !c["address"].get<std::string>().empty()) cafe["address"] = c["address"];
        const std::string id = c["id"];
        bool found = false;
        for (auto& x : cafes) if (x.value("id", std::string()) == id) { x = cafe; found = true; replaced++; }
        if (!found) { cafes.push_back(cafe); added++; }
        if (!e->cafeKeys.count(id)) e->cafeKeys[id] = crema::newKey().secretKey;
        if (cafes.size() > 200) return fail("At most 200 cafés in an event");
    }
    saveEvent(*e);
    publish();
    return ok({{"added", added}, {"replaced", replaced}, {"cafes", cafes.size()}});
} catch (const std::exception& e) { return fail(e.what()); }

std::string CremaCoreImpl::removeCafe(std::string eventId, std::string cafeId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer) return fail("Not an event you organize");
    json kept = json::array();
    for (const auto& c : e->draft["cafes"]) if (c.value("id", std::string()) != cafeId) kept.push_back(c);
    e->draft["cafes"] = kept;
    saveEvent(*e);   // the key stays: a café removed and added back keeps its display working
    publish();
    return ok();
}

std::string CremaCoreImpl::publishCatalog(std::string eventId) try {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer) return fail("Not an event you organize");
    if (e->draft["cafes"].empty()) return fail("Add some cafés first");
    const auto st = e->game->state();
    const long long version = (st.catalog.is_null() ? 0 : st.catalog.value("version", 0LL)) + 1;
    json cafes = json::array();
    for (auto c : e->draft["cafes"]) {
        c["pubkey"] = crema::ed25519Public(e->cafeKeys[c["id"].get<std::string>()]);
        cafes.push_back(c);
    }
    json cat = {{"v", 1}, {"eventId", e->join.eventId}, {"name", e->draft.value("name", e->join.name)}, {"city", e->draft.value("city", "")},
                {"version", version}, {"start", e->draft["start"]}, {"end", e->draft["end"]}, {"cafes", cafes}};
    // the map goes in once Storage has its CID; while it uploads, the cafés publish without it
    const json dm = e->draft.value("map", json());
    bool mapIn = false;
    if (dm.is_object() && dm.value("state", "") == "ready" && !dm.value("cid", "").empty()) {
        json pcs = json::array();
        for (const auto& pc : dm.value("pieces", json::array())) pcs.push_back(pc.value("cid", ""));
        cat["map"] = {{"cid", dm["cid"]}, {"sha256", dm["sha256"]}, {"size", dm["size"]}, {"bboxE6", dm["bboxE6"]},
                      {"minzoom", dm.value("minzoom", 0)}, {"maxzoom", dm.value("maxzoom", 16)}, {"providers", dm.value("providers", json::array())}};
        if (!pcs.empty()) {   // the relay-friendly copy: pieces in file order + where each zoom is complete
            cat["map"]["pieces"] = pcs; cat["map"]["pieceSize"] = dm.value("pieceSize", 61440LL);
            cat["map"]["dirEnd"] = dm.value("dirEnd", 0LL); cat["map"]["zoomEnds"] = dm.value("zoomEnds", json::array());
        }
        mapIn = true;
    }
    if (cat.dump().size() > 12000) return fail("The café list and map index are too big for one message - fewer cafés or a smaller map");
    cat = crema::signCatalog(cat, m_organizerSecret);
    std::vector<crema::Bytes> out;
    if (!e->game->publishCatalog(cat, nowMs(), out)) return fail("Couldn't publish (the signature didn't check)");
    saveEvent(*e);
    sendFrames(*e, out);
    publish();
    return ok({{"version", version}, {"sent", m_ready}, {"map", mapIn}, {"mapPending", dm.is_object() && !mapIn}});
} catch (const std::exception& e) { return fail(e.what()); }

std::string CremaCoreImpl::joinQr(std::string eventId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e) return fail("No such event");
    const auto& j = e->join;
    const std::string link = crema::makeJoinLink(j.eventId, j.organizer, j.secretHex, e->draft.value("name", j.name));
    json q = qrJson(link); q["link"] = link;
    return ok(q);
}

std::string CremaCoreImpl::cafeSetupQr(std::string eventId, std::string cafeId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer || !e->cafeKeys.count(cafeId)) return fail("Not a café of an event you organize");
    std::string name = cafeId;
    for (const auto& c : e->draft["cafes"]) if (c.value("id", std::string()) == cafeId) name = c.value("name", cafeId);
    const std::string link = crema::makeCafeLink(cafeId, e->cafeKeys[cafeId], name, e->draft.value("name", e->join.name));
    json q = qrJson(link); q["link"] = link; q["name"] = name;
    return ok(q);
}

std::string CremaCoreImpl::cafeStamp(std::string eventId, std::string cafeId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer || !e->cafeKeys.count(cafeId)) return fail("Not a café of an event you organize");
    const long long now = nowMs();
    const std::string text = crema::makeStamp(cafeId, e->cafeKeys[cafeId], now);
    json q = qrJson(text);
    q["text"] = text;
    q["secondsLeft"] = crema::STEP_S - (now / 1000) % crema::STEP_S;
    return ok(q);
}

std::string CremaCoreImpl::joinEvent(std::string link) try {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    crema::Join j = crema::parseJoinLink(link);
    if (!j.ok) return fail("That isn't a Crema event link");
    if (!find(j.eventId)) {
        auto ev = std::make_unique<Ev>();
        ev->join = j;
        ev->organizer = j.organizer == crema::ed25519Public(m_organizerSecret);
        ev->game = std::make_unique<crema::Game>(j, m_playerSecret);
        joinTopic(*ev);
        saveEvent(*ev);
        m_events[j.eventId] = std::move(ev);
        if (m_ready) sendFrames(*m_events[j.eventId], {m_events[j.eventId]->game->hello()});
    }
    m_current = j.eventId;
    saveIndex();
    publish();
    return ok({{"eventId", j.eventId}});
} catch (const std::exception& e) { return fail(e.what()); }

std::string CremaCoreImpl::selectEvent(std::string eventId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!find(eventId)) return fail("No such event");
    m_current = eventId;
    saveIndex();
    publish();
    return ok();
}

// ── the event map on Logos Storage (docs/adr/0002) ──
// The file goes up twice: as 60 KiB pieces (one Storage block each - the only size that crosses a
// circuit relay, see ADR 0002) and whole (for 0.2 phones). Pieces are in file order, and a PMTiles
// prefix holds the coarse zooms, so a phone lights the map up coarse-to-fine as pieces arrive.
namespace { constexpr long long kPieceSize = 61440; constexpr long long kMaxMapBytes = 6LL * 1024 * 1024; }

std::string CremaCoreImpl::importMap(std::string eventId, std::string path) try {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer) return fail("Not an event you organize");
    if (path.rfind("file://", 0) == 0) path = crema::pctDecode(path.substr(7));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return fail("No such file: " + path);
    const auto sz0 = fs::file_size(path, ec);
    if (ec || sz0 == 0) return fail("Can't read the file");
    if ((long long)sz0 > kMaxMapBytes) return fail("The map is " + std::to_string(sz0 / 1048576) + " MB; an event map can be at most 6 MB (make a smaller area)");
    json info; std::string why;
    if (!pmtilesInfo(path, info, why)) return fail(why);
    const std::string bytes = readFile(path);
    const auto L = maptiles::pmtilesLayout(bytes);
    if (!L.error.empty()) return fail(L.error);
    long long size = 0;
    const std::string sha = sha256File(path, size);
    if (sha.empty()) return fail("Can't read the file");
    const json old = e->draft.value("map", json());
    if (old.is_object() && old.value("sha256", "") == sha && old.value("state", "") == "ready") {   // the same map again
        publish();
        return ok({{"sha256", sha}, {"size", size}, {"state", "ready"}});
    }
    // Storage's manifest carries the file name: name everything by content so the poll can find it again
    const std::string dir = m_dataDir + "/maps/" + sha;
    fs::create_directories(dir, ec);
    const std::string whole = dir + "/" + sha + ".pmtiles";
    if (!fs::exists(whole, ec)) fs::copy_file(path, whole, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("Can't copy the map: " + ec.message());
    json pieces = json::array();
    for (long long off = 0, i = 0; off < size; off += kPieceSize, i++) {
        char nm[16]; snprintf(nm, sizeof nm, ".p%03lld", i);
        const std::string pf = dir + "/" + sha + nm;
        if (!writeAtomic(pf, bytes.substr((size_t)off, (size_t)std::min(kPieceSize, size - off)))) return fail("Can't write the map pieces");
        pieces.push_back({{"file", pf}, {"cid", ""}});
    }
    json zoomEnds = json::array();
    for (const auto& [z, end] : L.zoomEnds) zoomEnds.push_back({z, (long long)end});
    json m = {{"file", whole}, {"sha256", sha}, {"size", size}, {"bboxE6", info["bboxE6"]}, {"minzoom", L.minzoom}, {"maxzoom", L.maxzoom},
              {"dirEnd", (long long)L.dirEnd}, {"zoomEnds", zoomEnds}, {"pieceSize", kPieceSize}, {"pieces", pieces}, {"cid", ""},
              {"state", "uploading"}, {"since", nowMs()}};
    e->draft["map"] = m;
    saveEvent(*e);
    uploadMap(*e);
    publish();
    return ok({{"sha256", sha}, {"size", size}, {"pieces", pieces.size()}, {"bboxE6", info["bboxE6"]}, {"maxzoom", L.maxzoom}, {"state", "uploading"}});
} catch (const std::exception& e) { return fail(e.what()); }

std::string CremaCoreImpl::removeMap(std::string eventId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer) return fail("Not an event you organize");
    e->draft.erase("map");
    for (auto it = m_upSessions.begin(); it != m_upSessions.end();) it = it->second.rfind(eventId + "\t", 0) == 0 ? m_upSessions.erase(it) : std::next(it);
    saveEvent(*e);
    publish();
    return ok();
}

// Basecamp 0.3 / logosctl own the Storage node and configure it from ~/.logos_storage/config.json.
// We never init() it (that would replace the host's config for every module); start() is harmless.
void CremaCoreImpl::ensureStorage() {
    if (m_storageStarted) return;
    m_storageStarted = true;
    auto life = m_life;
    try {
        modules().storage_module.onStorageUploadDone([life](const std::string& payload) {
            withSelf(life, [payload](CremaCoreImpl* s) { s->onUploadDone(payload); });
        });
        modules().storage_module.start();
        fprintf(stderr, "[crema] storage: using the host's node\n");
    } catch (const std::exception& ex) {
        m_storageStarted = false;
        fprintf(stderr, "[crema] storage unavailable: %s\n", ex.what());
    }
}

namespace {
// upload items: 0..n-1 = pieces, -1 = the whole file. Pieces first: they are what relayed phones need.
int nextItem(const json& m) {
    const json& ps = m["pieces"];
    for (size_t i = 0; i < ps.size(); i++) if (ps[i].value("cid", "").empty()) return (int)i;
    return m.value("cid", "").empty() ? -1 : -2;   // -2: everything is up
}
std::string itemFile(const json& m, int idx) { return idx < 0 ? m.value("file", "") : m["pieces"][idx].value("file", ""); }
std::string sessKey(const std::string& eventId, int idx) { return eventId + "\t" + std::to_string(idx); }
}

// one upload at a time, in item order; each done-event (or the manifest poll) starts the next
void CremaCoreImpl::uploadMap(Ev& e) {
    json& m = e.draft["map"];
    if (!m.is_object() || m.value("state", "") != "uploading" || m_storageBusy) return;
    if (!m.contains("pieces")) { m["pieces"] = json::array(); }
    const std::string prefix = e.join.eventId + "\t";
    for (const auto& [sess, key] : m_upSessions) if (key.rfind(prefix, 0) == 0) return;
    const int idx = nextItem(m);
    if (idx == -2) return;
    ensureStorage();
    m_storageBusy = true;
    m["tried"] = nowMs();
    const std::string key = sessKey(e.join.eventId, idx);
    auto life = m_life;
    try {
        modules().storage_module.uploadUrlAsyncResult(itemFile(m, idx), 65536, true,
            [life, key](logos::AsyncResult<StdLogosResult> ar) {
                withSelf(life, [ar, key](CremaCoreImpl* s) {
                    s->m_storageBusy = false;
                    // a timeout may mean "still running": the manifest poll finds it by file name
                    if (!ar.ok()) { s->m_upSessions["?" + key] = key; return; }
                    if (!ar.value.success || resultString(ar.value).empty()) {
                        const std::string id = key.substr(0, key.find('\t'));
                        if (Ev* e = s->find(id)) { e->draft["map"]["error"] = "Storage refused the upload: " + ar.value.error; s->publish(); }
                        return;
                    }
                    s->m_upSessions[resultString(ar.value)] = key;
                });
            }, 60000);
    } catch (const std::exception& ex) {
        m_storageBusy = false;
        m["error"] = std::string("Storage unavailable: ") + ex.what();
    }
}

void CremaCoreImpl::onUploadDone(const std::string& payload) {
    json p = json::parse(payload, nullptr, false);
    if (!p.is_object()) return;
    auto it = m_upSessions.find(p.value("sessionId", ""));
    if (it == m_upSessions.end()) return;
    const std::string key = it->second;
    m_upSessions.erase(it);
    const std::string id = key.substr(0, key.find('\t'));
    const int idx = std::stoi(key.substr(key.find('\t') + 1));
    if (!p.value("success", false) || p.value("cid", "").empty()) {
        if (Ev* e = find(id)) { e->draft["map"]["error"] = "Upload failed: " + p.value("error", std::string("unknown")); e->draft["map"]["tried"] = nowMs(); publish(); }
        return;
    }
    mapUploaded(id, idx, p.value("cid", ""));
}

void CremaCoreImpl::mapUploaded(const std::string& eventId, int idx, const std::string& cid) {
    Ev* e = find(eventId);
    if (!e || !e->draft.contains("map")) return;
    json& m = e->draft["map"];
    if (m.value("state", "") != "uploading") return;
    if (idx >= 0) { if (idx >= (int)m["pieces"].size()) return; m["pieces"][idx]["cid"] = cid; }
    else m["cid"] = cid;
    m.erase("error");
    const std::string key = sessKey(eventId, idx);
    for (auto it = m_upSessions.begin(); it != m_upSessions.end();) it = it->second == key ? m_upSessions.erase(it) : std::next(it);
    if (nextItem(m) != -2) { saveEvent(*e); publish(); uploadMap(*e); return; }
    m["state"] = "ready";
    saveEvent(*e);
    publish();
    fprintf(stderr, "[crema] map %s on Storage: %zu pieces + whole %s\n", m.value("sha256", "").substr(0, 12).c_str(), m["pieces"].size(), m.value("cid", "").c_str());
    // where phones can find it first: this node (Loam dials these before asking the DHT)
    auto life = m_life;
    try {
        modules().storage_module.debugAsyncResult([life, eventId](logos::AsyncResult<StdLogosResult> ar) {
            if (!ar.ok() || !ar.value.success) return;
            json d = resultJson(ar.value);
            if (!d.is_object() || !d.contains("id")) return;
            json addrs = json::array();
            for (const auto& a : d.value("addrs", json::array()))
                if (a.is_string() && a.get<std::string>().find("/127.") == std::string::npos && a.get<std::string>().find("/::1/") == std::string::npos
                    && a.get<std::string>().find("mix-transport") == std::string::npos) addrs.push_back(a);
            json peer = {{"peerId", d["id"]}, {"addrs", addrs}};
            withSelf(life, [eventId, peer](CremaCoreImpl* s) {
                Ev* e = s->find(eventId);
                if (!e || !e->draft.contains("map")) return;
                e->draft["map"]["providers"] = json::array({peer});
                s->saveEvent(*e); s->publish();
            });
        }, 20000);
    } catch (...) {}
}

// Storage done-events can be lost: read the same facts back (our staged files' manifests), and retry
// an upload that failed, stalled or was cut off by a restart.
void CremaCoreImpl::pollStorage() {
    const long long now = nowMs();
    bool pending = false;
    for (auto& [id, e] : m_events) {
        if (!e->organizer || !e->draft.contains("map")) continue;
        json& m = e->draft["map"];
        if (m.value("state", "") != "uploading") continue;
        pending = true;
        bool inFlight = false;
        for (const auto& [sess, key] : m_upSessions) if (key.rfind(id + "\t", 0) == 0) inFlight = true;
        const long long tried = m.value("tried", 0LL);
        if (inFlight && now - tried > 180000)   // stuck: forget it, the manifest poll or a retry settles it
            for (auto it = m_upSessions.begin(); it != m_upSessions.end();) it = it->second.rfind(id + "\t", 0) == 0 ? m_upSessions.erase(it) : std::next(it);
        else if (!inFlight && (m.contains("error") ? now - tried > 60000 : true)) uploadMap(*e);
    }
    if (!pending || m_storageBusy || now - m_lastManifestPollMs < 10000 || !m_storageStarted) return;
    m_lastManifestPollMs = now;
    m_storageBusy = true;
    auto life = m_life;
    try {
        modules().storage_module.manifestsAsyncResult([life](logos::AsyncResult<StdLogosResult> ar) {
            withSelf(life, [ar](CremaCoreImpl* s) {
                s->m_storageBusy = false;
                if (!ar.ok() || !ar.value.success) return;
                json arr = resultJson(ar.value);
                if (!arr.is_array()) return;
                std::map<std::string, std::string> byName;
                for (const auto& m : arr) if (m.is_object() && !m.value("cid", "").empty()) byName[m.value("filename", "")] = m.value("cid", "");
                struct Hit { std::string id; int idx; std::string cid; };
                std::vector<Hit> hits;
                for (auto& [id, e] : s->m_events) {
                    if (!e->draft.contains("map")) continue;
                    const json& m = e->draft["map"];
                    if (m.value("state", "") != "uploading") continue;
                    for (int i = -1; i < (int)m.value("pieces", json::array()).size(); i++) {
                        const std::string have = i < 0 ? m.value("cid", "") : m["pieces"][i].value("cid", "");
                        if (!have.empty()) continue;
                        auto it = byName.find(fs::path(itemFile(m, i)).filename().string());
                        if (it != byName.end()) hits.push_back({id, i, it->second});
                    }
                }
                for (const auto& h : hits) s->mapUploaded(h.id, h.idx, h.cid);
            });
        }, 30000);
    } catch (...) { m_storageBusy = false; }
}

// ── making the map here (maptiles.hpp; ADR 0002 amended) ──
namespace {
// A nix-built libcurl looks for CA certificates under /nix/store: point it at the system's bundle.
const char* caBundle() {
    static const char* paths[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
                                  "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem", "/usr/local/etc/openssl/cert.pem"};
    if (const char* e = std::getenv("SSL_CERT_FILE")) if (*e) return e;
    if (const char* e = std::getenv("CURL_CA_BUNDLE")) if (*e) return e;
    for (const char* p : paths) { std::error_code ec; if (fs::is_regular_file(p, ec)) return p; }
    return nullptr;
}
struct Dl { std::string body; std::function<void(long long)> progress; long long lastReport = 0; };
size_t dlWrite(char* p, size_t sz, size_t n, void* ud) {
    auto* d = static_cast<Dl*>(ud);
    if (d->body.size() + sz * n > 300ull * 1024 * 1024) return 0;   // a city map is tens of MB; stop runaway answers
    d->body.append(p, sz * n);
    if (d->progress && (long long)d->body.size() - d->lastReport > 512 * 1024) { d->lastReport = (long long)d->body.size(); d->progress(d->lastReport); }
    return sz * n;
}
// one POST to one Overpass server; returns "" + error on failure (busy = worth trying elsewhere)
std::string overpassOnce(const std::string& url, const std::string& query, std::string& err, bool& busy, const std::function<void(long long)>& progress) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return ""; }
    busy = false;
    char* q = curl_easy_escape(c, query.c_str(), (int)query.size());
    const std::string form = std::string("data=") + (q ? q : "");
    if (q) curl_free(q);
    Dl d; d.progress = progress;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "crema/0.3 (github.com/vpavlin/crema; event map maker)");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (const char* ca = caBundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { err = std::string("download failed: ") + curl_easy_strerror(rc); busy = true; return ""; }
    if (status == 429 || status == 502 || status == 503 || status == 504) { err = "Overpass is busy (HTTP " + std::to_string(status) + ")"; busy = true; return ""; }
    if (status != 200) { err = "Overpass answered HTTP " + std::to_string(status); return ""; }
    return std::move(d.body);
}
// the public Overpass servers take turns; a busy one is common (HTTP 429/502/504)
std::string overpass(const std::string& query, std::string& err, const std::function<void(long long)>& progress) {
    std::vector<std::string> urls;
    if (const char* env = std::getenv("CREMA_OVERPASS_URL")) if (*env) urls.push_back(env);
    for (const char* u : {"https://overpass-api.de/api/interpreter", "https://overpass.private.coffee/api/interpreter", "https://overpass.kumi.systems/api/interpreter"}) urls.push_back(u);
    std::string last;
    for (int round = 0; round < 2; round++) {
        for (const auto& u : urls) {
            bool busy = false;
            std::string e;
            std::string body = overpassOnce(u, query, e, busy, progress);
            if (!body.empty()) return body;
            fprintf(stderr, "[crema] overpass %s: %s\n", u.c_str(), e.c_str());
            last = e;
            if (!busy) { err = e; return ""; }
        }
        std::this_thread::sleep_for(std::chrono::seconds(20));
    }
    err = last + " - every Overpass server was busy; try again in a few minutes";
    return "";
}
} // namespace

std::string CremaCoreImpl::makeMap(std::string eventId, std::string marginM) try {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    Ev* e = find(eventId);
    if (!e || !e->organizer) return fail("Not an event you organize");
    if (m_making.count(eventId) && m_making[eventId].value("error", "").empty()) return fail("A map is already being made");
    const json cafes = e->draft.value("cafes", json::array());
    if (cafes.empty()) return fail("Add the cafés first: the map covers them");
    double margin = 500;
    try { if (!marginM.empty()) margin = std::stod(marginM); } catch (...) {}
    margin = std::max(100.0, std::min(margin, 3000.0));
    double s = 90, n = -90, w = 180, ea = -180;
    for (const auto& c : cafes) {
        const double la = c.value("latE6", 0LL) / 1e6, lo = c.value("lonE6", 0LL) / 1e6;
        s = std::min(s, la); n = std::max(n, la); w = std::min(w, lo); ea = std::max(ea, lo);
    }
    const double dLat = margin / 111320.0, dLon = margin / (111320.0 * std::cos((s + n) / 2 * M_PI / 180.0));
    const maptiles::Box box{w - dLon, s - dLat, ea + dLon, n + dLat};
    const double kmW = (box.e - box.w) * 111.32 * std::cos((s + n) / 2 * M_PI / 180.0), kmH = (box.n - box.s) * 111.32;
    if (kmW > 20 || kmH > 20) return fail("The cafés span more than 20 km - the map would be too big");
    m_making[eventId] = {{"step", "downloading"}, {"bytes", 0}, {"box", {box.w, box.s, box.e, box.n}}};
    publish();
    auto life = m_life;
    const std::string out = m_dataDir + "/maps/made-" + eventId + ".pmtiles";
    std::thread([life, eventId, box, out] {
        // nothing here touches the module: progress and the result hop back through withSelf
        auto report = [life, eventId](json st) {
            withSelf(life, [eventId, st](CremaCoreImpl* self) {
                auto it = self->m_making.find(eventId);
                if (it == self->m_making.end()) return;
                it->second.update(st);
                self->publish();
            });
        };
        std::string err;
        const std::string body = overpass(maptiles::overpassQuery(box), err, [&](long long b) { report({{"bytes", b}}); });
        if (body.empty()) { withSelf(life, [eventId, err](CremaCoreImpl* s) { s->makeMapDone(eventId, "", err.empty() ? "no map data" : err); }); return; }
        report({{"step", "drawing"}, {"bytes", (long long)body.size()}});
        json doc = json::parse(body, nullptr, false);
        if (!doc.is_object()) { withSelf(life, [eventId](CremaCoreImpl* s) { s->makeMapDone(eventId, "", "Overpass sent something that isn't map data"); }); return; }
        auto feats = maptiles::featuresFromOverpass(doc);
        doc = json();   // free the parsed answer before tiling
        int lastPct = -1;
        auto r = maptiles::buildPmtiles(feats, box, 12, 16, [&](int done, int total) {
            const int pct = total ? done * 100 / total : 0;
            if (pct / 10 != lastPct / 10) { lastPct = pct; report({{"tilesDone", done}, {"tilesTotal", total}}); }
        });
        if (!r.error.empty()) { withSelf(life, [eventId, e = r.error](CremaCoreImpl* s) { s->makeMapDone(eventId, "", e); }); return; }
        std::error_code ec;
        fs::create_directories(fs::path(out).parent_path(), ec);
        { std::ofstream f(out, std::ios::binary | std::ios::trunc); f << r.bytes; }
        fprintf(stderr, "[crema] made a map for %s: %zu features, %d tiles, %zu bytes\n", eventId.c_str(), feats.size(), r.tiles, r.bytes.size());
        withSelf(life, [eventId, out](CremaCoreImpl* s) { s->makeMapDone(eventId, out, ""); });
    }).detach();
    return ok({{"started", true}, {"box", {box.w, box.s, box.e, box.n}}});
} catch (const std::exception& e) { return fail(e.what()); }

void CremaCoreImpl::makeMapDone(const std::string& eventId, const std::string& path, const std::string& error) {
    if (!error.empty()) { m_making[eventId] = {{"step", "failed"}, {"error", error}}; publish(); return; }
    m_making.erase(eventId);
    const std::string r = importMap(eventId, path);   // the same path as an imported file: copy, hash, upload
    json j = json::parse(r, nullptr, false);
    if (!j.is_object() || !j.value("ok", false)) { m_making[eventId] = {{"step", "failed"}, {"error", j.is_object() ? j.value("error", r) : r}}; publish(); }
    std::error_code ec; fs::remove(path, ec);   // importMap kept its own copy (named by sha256)
}
