#pragma once
// logos_sync/signing.hpp — the OPTIONAL event-authenticity layer (docs/adr/0008).
//
// Proves WHO authored an event (secp256k1 / OpenSSL), so roles become real
// write-authorization instead of a forgeable author claim. Orthogonal to the app's
// seal/open (confidentiality stays app-side, docs/adr/0006). Verification needs only
// PUBLIC keys; signing is delegated through an injected `Signer`, so the library never
// holds a private key — which is how a software key today and a **Keycard** tomorrow
// are the same code path (they differ only inside signDigest()).
//
//   canonical = "<domain>-sig-v1|"+type+"|"+wall+"|"+ctr+"|"+dev+"|"+id+"|"+cjson(payload)
//   cjson     = compact JSON, object keys sorted lexicographically, no spaces
//   address   = "0x" + hex(sha256(pub_compressed_33B))[24:64]        (SHA-256, not keccak)
//   digest    = sha256(utf8(canonical))
//   sig       = secp256k1 ECDSA over digest, compact r||s (64B) hex, LOW-S
//
// The ONLY per-app parameter is the `domain` tag ("scala", "qaku", …). Parity: the
// TS mirror is src/signing.ts; keep the two byte-identical (docs/PARITY.md, adr/0005).
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include "event.hpp"

namespace logos_sync {
using Bytes = std::vector<unsigned char>;

// ── small self-contained helpers ─────────────────────────────────────────────
inline Bytes sha256b(const Bytes& in) { Bytes o(32); SHA256(in.data(), in.size(), o.data()); return o; }
inline Bytes strBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string toHexS(const unsigned char* b, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string o; o.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { o += H[b[i] >> 4]; o += H[b[i] & 0xf]; }
    return o;
}
inline Bytes fromHexB(const std::string& s) {
    Bytes out; out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

// ── canonical form (JSON.stringify(string) + sorted-key compact JSON) ────────
inline std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); o += buf; }
                else o += (char)c;
        }
    }
    return o + "\"";
}
inline std::string cjson(const json& v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_string()) return jsonString(v.get<std::string>());
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    // ADR 0017: pin one number rule matching TS `String(n)`. JS has no int/float
    // distinction, so a whole-valued float (5.0) must canonicalise as "5", not "5.0".
    // Signed payloads should use integers (the money domain is integer milli); a
    // non-integral float falls back to a shortest round-trip decimal.
    if (v.is_number_float()) {
        double d = v.get<double>();
        long long ll = (long long)d;
        if ((double)ll == d) return std::to_string(ll);
        return json(v).dump();
    }
    if (v.is_array()) {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; o += cjson(v[i]); }
        return o + "]";
    }
    if (v.is_object()) {
        std::vector<std::string> keys;
        for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        std::string o = "{";
        for (size_t i = 0; i < keys.size(); i++) { if (i) o += ","; o += jsonString(keys[i]) + ":" + cjson(v.at(keys[i])); }
        return o + "}";
    }
    return "null";
}

inline std::string canonicalMessage(const std::string& domain, const Event& e) {
    const std::string& dev = !e.hlc.dev.empty() ? e.hlc.dev : e.dev;
    return domain + "-sig-v1|" + e.type + "|" + std::to_string(e.hlc.wall) + "|"
         + std::to_string(e.hlc.ctr) + "|" + dev + "|" + e.id + "|" + cjson(e.payload);
}

inline std::string address(const Bytes& pub33) {
    if (pub33.size() != 33) return "";
    Bytes h = sha256b(pub33);
    return "0x" + toHexS(h.data(), 32).substr(24, 40);
}

// ── ECDSA (low-S compact) ────────────────────────────────────────────────────
inline Bytes ecdsaSignLowS(const Bytes& priv, const Bytes& digest32) {
    Bytes out;
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM* bn = BN_bin2bn(priv.data(), (int)priv.size(), nullptr);
    if (key && bn && EC_KEY_set_private_key(key, bn) == 1) {
        ECDSA_SIG* sig = ECDSA_do_sign(digest32.data(), (int)digest32.size(), key);
        if (sig) {
            const BIGNUM* r; const BIGNUM* s; ECDSA_SIG_get0(sig, &r, &s);
            const EC_GROUP* grp = EC_KEY_get0_group(key);
            BN_CTX* ctx = BN_CTX_new();
            BIGNUM* order = BN_new(); EC_GROUP_get_order(grp, order, ctx);
            BIGNUM* half = BN_new(); BN_rshift1(half, order);
            BIGNUM* sN = BN_dup(s);
            if (BN_cmp(sN, half) > 0) BN_sub(sN, order, sN);   // low-S normalize
            out.assign(64, 0);
            BN_bn2binpad(r, out.data(), 32);
            BN_bn2binpad(sN, out.data() + 32, 32);
            BN_free(sN); BN_free(half); BN_free(order); BN_CTX_free(ctx);
            ECDSA_SIG_free(sig);
        }
    }
    BN_free(bn); EC_KEY_free(key);
    return out;
}
inline bool ecdsaVerify(const Bytes& pub33, const Bytes& digest32, const Bytes& sig64) {
    if (pub33.size() != 33 || sig64.size() != 64) return false;
    bool ok = false;
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new();
    const EC_GROUP* grp = EC_KEY_get0_group(key);
    EC_POINT* pt = EC_POINT_new(grp);
    if (EC_POINT_oct2point(grp, pt, pub33.data(), 33, ctx) == 1 && EC_KEY_set_public_key(key, pt) == 1) {
        ECDSA_SIG* sig = ECDSA_SIG_new();
        BIGNUM* r = BN_bin2bn(sig64.data(), 32, nullptr);
        BIGNUM* s = BN_bin2bn(sig64.data() + 32, 32, nullptr);
        ECDSA_SIG_set0(sig, r, s);
        ok = ECDSA_do_verify(digest32.data(), (int)digest32.size(), sig, key) == 1;
        ECDSA_SIG_free(sig);
    }
    EC_POINT_free(pt); BN_CTX_free(ctx); EC_KEY_free(key);
    return ok;
}

// Derive the 33B compressed public key from a 32B scalar (empty on invalid scalar).
inline Bytes pubFromPriv(const Bytes& priv) {
    Bytes pubc;
    if (priv.size() != 32) return pubc;
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* bn = BN_bin2bn(priv.data(), 32, nullptr);
    if (key && bn && EC_KEY_set_private_key(key, bn) == 1) {
        const EC_GROUP* grp = EC_KEY_get0_group(key);
        EC_POINT* pub = EC_POINT_new(grp);
        if (EC_POINT_mul(grp, pub, bn, nullptr, nullptr, ctx) == 1) {
            Bytes tmp(33);
            if (EC_POINT_point2oct(grp, pub, POINT_CONVERSION_COMPRESSED, tmp.data(), 33, ctx) == 33) pubc = tmp;
        }
        EC_POINT_free(pub);
    }
    BN_free(bn); BN_CTX_free(ctx); EC_KEY_free(key);
    return pubc;
}

// ── the Signer seam (docs/adr/0008) ──────────────────────────────────────────
// The library sees a Signer, never a key. `signEvent` reaches a secret ONLY through it,
// so a SoftwareSigner (key in the app's keystore) and a future KeycardSigner (key on a
// secure element, signing over NFC/PC-SC) are drop-in — the digest is the same 32 bytes.
struct Signer {
    virtual ~Signer() = default;
    virtual Bytes publicKey() const = 0;              // 33B compressed
    virtual Bytes signDigest(const Bytes& digest32) const = 0; // 64B low-S compact
};

struct SoftwareSigner : Signer {
    Bytes priv;   // 32B scalar (the app owns storage + RNG; the library just holds it while signing)
    Bytes pub;    // 33B compressed
    bool valid = false;
    explicit SoftwareSigner(const Bytes& priv32) : priv(priv32) { pub = pubFromPriv(priv); valid = pub.size() == 33; }
    Bytes publicKey() const override { return pub; }
    Bytes signDigest(const Bytes& d) const override { return ecdsaSignLowS(priv, d); }
};

// Generate a fresh 32B scalar via OpenSSL RAND (desktop). Mobile/RN supplies its own
// platform RNG and constructs a SoftwareSigner from those bytes.
inline Bytes generatePrivateKey() {
    Bytes priv(32);
    for (int i = 0; i < 8; i++) { RAND_bytes(priv.data(), 32); if (pubFromPriv(priv).size() == 33) return priv; }
    return {};
}

// ── the public API ────────────────────────────────────────────────────────────
// Stamp the event with the signer's address as author (dev + hlc.dev) and sign. Mutates ev.
inline void signEvent(const Signer& signer, const std::string& domain, Event& e) {
    Bytes pub = signer.publicKey();
    std::string addr = address(pub);
    e.dev = addr;
    e.hlc.dev = addr;
    Bytes digest = sha256b(strBytes(canonicalMessage(domain, e)));
    Bytes sig = signer.signDigest(digest);
    e.pub = toHexS(pub.data(), pub.size());
    e.sig = toHexS(sig.data(), sig.size());
}

// True iff the event is well-signed by the key whose address it claims. Pure, public-key
// only, never throws — the fold decides what to do with the result (docs/adr/0007).
inline bool verifyEvent(const std::string& domain, const Event& e) {
    if (e.pub.empty() || e.sig.empty() || e.type.empty() || e.id.empty()) return false;
    const std::string& dev = !e.hlc.dev.empty() ? e.hlc.dev : e.dev;
    if (dev.empty()) return false;
    Bytes pub = fromHexB(e.pub);
    if (pub.size() != 33) return false;
    if (address(pub) != dev) return false;
    Bytes sig = fromHexB(e.sig);
    if (sig.size() != 64) return false;
    Bytes digest = sha256b(strBytes(canonicalMessage(domain, e)));
    return ecdsaVerify(pub, digest, sig);
}

// An event is "legacy" (pre-signing) when it carries no signature — admitted for backward
// compat, but never counts as an authenticated author.
inline bool isSigned(const Event& e) { return !e.sig.empty(); }

// ── delegation certs (docs/adr/0009 custody; ADR 0017 signing parity) ─────────
// C++ mirror of the TS layer in signing.ts. The card identity (idPub) signs — on card —
// a cert authorising an ephemeral delegate key to author events; a verifier checks the
// idSig over canonicalCert and expiry. maxSigs/scope are FOLD-enforced (docs/adr/0007).
struct DelegationCert {
    std::string delegatePub;  // hex compressed 33B — the key that signs events
    std::string idPub;        // hex compressed 33B — the card identity key
    long long   notAfter = 0; // unix ms; 0 = never expires
    long long   maxSigs  = 0; // 0 = unlimited (fold-enforced)
    std::string scope;        // "" = any container (fold-enforced)
    std::string idSig;        // hex 64B ECDSA by idPub over canonicalCert
};

inline std::string canonicalCert(const std::string& domain, const DelegationCert& c) {
    return domain + "-deleg-v1|" + c.delegatePub + "|" + c.idPub + "|"
         + std::to_string(c.notAfter) + "|" + std::to_string(c.maxSigs) + "|" + c.scope;
}

// Verify the cert's identity signature and non-expiry at `atMs`. Does NOT check
// maxSigs/scope — those are count/scope-dependent and belong in the fold.
inline bool verifyCert(const std::string& domain, const DelegationCert& c, long long atMs) {
    if (c.delegatePub.empty() || c.idPub.empty() || c.idSig.empty()) return false;
    Bytes idPub = fromHexB(c.idPub);
    if (idPub.size() != 33) return false;
    if (c.notAfter != 0 && atMs > c.notAfter) return false;
    Bytes digest = sha256b(strBytes(canonicalCert(domain, c)));
    Bytes sig = fromHexB(c.idSig);
    if (sig.size() != 64) return false;
    return ecdsaVerify(idPub, digest, sig);
}

// Issue a cert: the identity signer signs the canonical cert (SoftwareSigner or an
// on-card signer via the Signer seam).
inline DelegationCert issueCert(const Signer& idSigner, const std::string& domain,
                                const std::string& delegatePub, long long notAfter = 0,
                                long long maxSigs = 0, const std::string& scope = "") {
    DelegationCert c;
    c.delegatePub = delegatePub;
    Bytes idPub = idSigner.publicKey();
    c.idPub = toHexS(idPub.data(), idPub.size());
    c.notAfter = notAfter; c.maxSigs = maxSigs; c.scope = scope;
    Bytes digest = sha256b(strBytes(canonicalCert(domain, c)));
    Bytes sig = idSigner.signDigest(digest);
    c.idSig = toHexS(sig.data(), sig.size());
    return c;
}

// Round-trip the cert through an event's `cert` JSON field (byte-compatible with TS).
inline bool certFromJson(const json& j, DelegationCert& c) {
    if (!j.is_object()) return false;
    c.delegatePub = j.value("delegatePub", std::string());
    c.idPub       = j.value("idPub", std::string());
    c.notAfter    = j.value("notAfter", (long long)0);
    c.maxSigs     = j.value("maxSigs", (long long)0);
    c.scope       = j.value("scope", std::string());
    c.idSig       = j.value("idSig", std::string());
    return !c.delegatePub.empty();
}
inline json certToJson(const DelegationCert& c) {
    return json{{"delegatePub", c.delegatePub}, {"idPub", c.idPub}, {"notAfter", c.notAfter},
                {"maxSigs", c.maxSigs}, {"scope", c.scope}, {"idSig", c.idSig}};
}

} // namespace logos_sync
