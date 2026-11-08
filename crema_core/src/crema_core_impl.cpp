#include "crema_core_impl.h"
#include "logos_sdk.h"   // LogosModules (modules().loam_core) + the generated glue

#include <QMetaObject>
#include <QTimer>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
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
        const long long pub = st.catalog.is_null() ? 0 : st.catalog.value("version", 0LL);
        j["publishedVersion"] = pub;
        // unpublished changes: the draft's cafés differ from the published list
        bool dirty = st.catalog.is_null();
        if (!dirty) {
            json a = e.draft.value("cafes", json::array()), b = json::array();
            for (const auto& c : st.catalog["cafes"]) { json x = c; x.erase("pubkey"); b.push_back(x); }
            dirty = a != b || st.catalog.value("name", "") != e.draft.value("name", "");
        }
        j["unpublished"] = dirty;
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
    cat = crema::signCatalog(cat, m_organizerSecret);
    std::vector<crema::Bytes> out;
    if (!e->game->publishCatalog(cat, nowMs(), out)) return fail("Couldn't publish (the signature didn't check)");
    saveEvent(*e);
    sendFrames(*e, out);
    publish();
    return ok({{"version", version}, {"sent", m_ready}});
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
