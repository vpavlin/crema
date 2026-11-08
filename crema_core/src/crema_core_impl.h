#pragma once
// CremaCoreImpl - Crema's Basecamp core (universal authoring: the public methods below ARE the module's
// API; the glue is generated from this header). Comments go ABOVE a method, never after it on the
// same line (the parser would skip the method). At most 4 arguments, no defaults, strings only.
//
// The organizer's side of the coffee passport: create an event, import cafés (each gets its own
// Ed25519 key), sign and publish the café list, show the join QR, each café's setup QR, café mode
// on this laptop, and the live leaderboard. Syncs over loam_core (plain relay) with the phones;
// crema.hpp is the C++ twin of the phone's protocol + engine (test/parity.cpp checks them).
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "logos_module_context.h"
#include "crema.hpp"

class QTimer;

class CremaCoreImpl : public LogosModuleContext {
public:
    ~CremaCoreImpl() override;

    // Everything the view shows: {ok, status, me, current, events:[{eventId,name,organizer}], event:{...}}
    std::string snapshot();
    // A new event; this desktop is its organizer. startDate "YYYY-MM-DD" (local midnight), days 1..60
    std::string createEvent(std::string name, std::string city, std::string startDate, std::string days);
    // Add cafés from a JSON list [{id,name,latE6,lonE6,address?,radiusM?}] (e.g. data/brno-cafes.json); each gets a key
    std::string importCafes(std::string eventId, std::string listJson);
    // Add or replace one café: {id,name,latE6,lonE6,address?,radiusM?}
    std::string addCafe(std::string eventId, std::string cafeJson);
    // Remove a café from the draft (publish to make it count)
    std::string removeCafe(std::string eventId, std::string cafeId);
    // Sign the draft café list as the next version and send it to the players
    std::string publishCatalog(std::string eventId);
    // The event's join QR: {ok, link, n, cells}
    std::string joinQr(std::string eventId);
    // A café's SETUP QR (carries its secret key - show it only in person): {ok, link, n, cells}
    std::string cafeSetupQr(std::string eventId, std::string cafeId);
    // Café mode on this laptop: the café's current stamp QR {ok, text, n, cells, secondsLeft}
    std::string cafeStamp(std::string eventId, std::string cafeId);
    // Follow an event someone else organizes (as a spectator: leaderboard, map)
    std::string joinEvent(std::string link);
    // Which event the view shows
    std::string selectEvent(std::string eventId);

protected:
    void onContextReady() override;

logos_events:
    void stateChanged(const std::string& snapshotJson);

private:
    struct Ev {
        crema::Join join;
        bool organizer = false;
        crema::json draft;                              // organizer: {name, city, start, end, cafes:[...]}
        std::map<std::string, std::string> cafeKeys;    // organizer: cafeId -> secret hex
        std::unique_ptr<crema::Game> game;
        bool joined = false;
    };
    struct Life { std::mutex mu; CremaCoreImpl* self = nullptr; };

    void setupDataDir();
    void loadAll();
    void saveIndex();
    void saveEvent(const Ev& e);
    Ev* find(const std::string& eventId);
    void startTransport();
    void onStatus(const std::string& st);
    void onFrame(const std::string& topic, const std::string& payloadB64);
    void joinTopic(Ev& e);
    void sendFrames(const Ev& e, const std::vector<crema::Bytes>& frames);
    void helloAll();
    void publish();
    crema::json eventJson(const Ev& e);
    static void withSelf(const std::shared_ptr<Life>& life, std::function<void(CremaCoreImpl*)> fn);

    std::recursive_mutex m_mtx;
    std::shared_ptr<Life> m_life;
    QTimer* m_timer = nullptr;
    std::string m_dataDir, m_status = "Starting...", m_current, m_organizerSecret;
    crema::Bytes m_playerSecret;
    std::map<std::string, std::unique_ptr<Ev>> m_events;
    bool m_starting = false, m_ready = false;
    int m_startFailures = 0, m_helloRounds = 0;
    long long m_lastStartMs = 0, m_lastPollMs = 0, m_lastHelloMs = 0;
};
