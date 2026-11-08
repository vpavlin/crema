// crypto.hpp — C++ mirror of loam-sync/src/crypto.ts (ADR 0011). The shared household
// AEAD envelope: domain-parameterised HKDF key schedule + a DETERMINISTIC id-derived
// nonce, so a re-sealed immutable event is byte-identical (the fleet store dedups it).
// Byte-compatible with the TS reference and with each app's legacy derivation.
//
//   K     = HKDF-SHA256(ikm=S, salt="<domain>-pair-v1", info="",                   32)
//   Ke    = HKDF-SHA256(ikm=K, salt="",                 info="<domain>/payload/v1",32)
//   topic = "/<domain>/1/" + hex(HMAC-SHA256(K,"<domain>/topic/v1|"+epoch)[0..15]) + "/proto"
//   nonce = HMAC-SHA256(Ke, "<domain>/nonce/v1|"+eventId)[0..11]      (12B, deterministic)
//   wire  = nonce(12) ‖ ChaCha20-Poly1305(Ke, nonce, plaintext, aad=topic)  (ct‖tag)
#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>

namespace logos_sync {
namespace crypto {

using Bytes = std::vector<unsigned char>;

inline Bytes sbytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string hexs(const Bytes& b) {
    static const char* H = "0123456789abcdef";
    std::string o; o.reserve(b.size() * 2);
    for (unsigned char x : b) { o += H[x >> 4]; o += H[x & 15]; }
    return o;
}

inline Bytes hkdf(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t len) {
    Bytes out(len);
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!c) throw std::runtime_error("hkdf ctx");
    bool ok = EVP_PKEY_derive_init(c) == 1
        && EVP_PKEY_CTX_set_hkdf_md(c, EVP_sha256()) == 1
        && EVP_PKEY_CTX_set1_hkdf_key(c, ikm.data(), (int)ikm.size()) == 1;
    // Empty salt/info: skip the setter (OpenSSL then uses a zero-length default, which
    // matches @noble's empty-salt/empty-info behaviour — both reduce to a zero HMAC key).
    if (ok && !salt.empty()) ok = EVP_PKEY_CTX_set1_hkdf_salt(c, salt.data(), (int)salt.size()) == 1;
    if (ok && !info.empty()) ok = EVP_PKEY_CTX_add1_hkdf_info(c, info.data(), (int)info.size()) == 1;
    size_t outlen = len;
    ok = ok && EVP_PKEY_derive(c, out.data(), &outlen) == 1;
    EVP_PKEY_CTX_free(c);
    if (!ok || outlen != len) throw std::runtime_error("hkdf derive");
    return out;
}

inline Bytes hmac256(const Bytes& key, const Bytes& data) {
    Bytes out(32); unsigned int n = 32;
    HMAC(EVP_sha256(), key.data(), (int)key.size(), data.data(), data.size(), out.data(), &n);
    out.resize(n);
    return out;
}

struct Identity { Bytes secret, K, Ke, fpBytes; };

inline Identity deriveIdentity(const Bytes& secret, const std::string& domain) {
    if (secret.size() != 32) throw std::runtime_error("household secret must be 32 bytes");
    Identity id;
    id.secret = secret;
    id.K  = hkdf(secret, sbytes(domain + "-pair-v1"), {}, 32);
    id.Ke = hkdf(id.K, {}, sbytes(domain + "/payload/v1"), 32);
    // fp = sha256(K)[0..2]
    Bytes h(EVP_MAX_MD_SIZE); unsigned int hl = 0;
    EVP_Digest(id.K.data(), id.K.size(), h.data(), &hl, EVP_sha256(), nullptr);
    id.fpBytes = Bytes(h.begin(), h.begin() + 3);
    return id;
}

inline std::string topicFor(const Identity& id, const std::string& domain, int epoch = 0) {
    Bytes t = hmac256(id.K, sbytes(domain + "/topic/v1|" + std::to_string(epoch)));
    t.resize(16);
    return "/" + domain + "/1/" + hexs(t) + "/proto";
}

inline Bytes nonceFor(const Identity& id, const std::string& domain, const std::string& eventId) {
    Bytes n = hmac256(id.Ke, sbytes(domain + "/nonce/v1|" + eventId));
    n.resize(12);
    return n;
}

// nonce(12) ‖ ct ‖ tag(16). Deterministic nonce → byte-identical for an immutable event.
inline Bytes seal(const Identity& id, const std::string& domain, const std::string& eventId,
                  const Bytes& plaintext, const std::string& topic) {
    Bytes n = nonceFor(id, domain, eventId);
    Bytes aad = sbytes(topic);
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes ct(plaintext.size()); Bytes tag(16); int len = 0;
    bool ok = EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1
        && EVP_EncryptInit_ex(c, nullptr, nullptr, id.Ke.data(), n.data()) == 1
        && EVP_EncryptUpdate(c, nullptr, &len, aad.data(), (int)aad.size()) == 1
        && EVP_EncryptUpdate(c, ct.data(), &len, plaintext.data(), (int)plaintext.size()) == 1;
    int ctlen = len;
    ok = ok && EVP_EncryptFinal_ex(c, ct.data() + ctlen, &len) == 1;
    ctlen += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) throw std::runtime_error("seal");
    ct.resize(ctlen);
    Bytes out; out.reserve(12 + ct.size() + 16);
    out.insert(out.end(), n.begin(), n.end());
    out.insert(out.end(), ct.begin(), ct.end());
    out.insert(out.end(), tag.begin(), tag.end());
    return out;
}

// Inverse of seal(); throws on a bad tag. The nonce rides the wire, so a legacy
// random-nonce message opens unchanged.
inline Bytes open(const Identity& id, const Bytes& sealed, const std::string& topic) {
    if (sealed.size() < 12 + 16) throw std::runtime_error("sealed too short");
    Bytes n(sealed.begin(), sealed.begin() + 12);
    Bytes tag(sealed.end() - 16, sealed.end());
    Bytes ct(sealed.begin() + 12, sealed.end() - 16);
    Bytes aad = sbytes(topic);
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    Bytes pt(ct.size()); int len = 0;
    bool ok = EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1
        && EVP_DecryptInit_ex(c, nullptr, nullptr, id.Ke.data(), n.data()) == 1
        && EVP_DecryptUpdate(c, nullptr, &len, aad.data(), (int)aad.size()) == 1
        && EVP_DecryptUpdate(c, pt.data(), &len, ct.data(), (int)ct.size()) == 1;
    int ptlen = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, tag.data()) == 1;
    ok = ok && EVP_DecryptFinal_ex(c, pt.data() + ptlen, &len) == 1;  // fails if tag mismatch
    ptlen += len;
    EVP_CIPHER_CTX_free(c);
    if (!ok) throw std::runtime_error("open: bad tag / wrong key");
    pt.resize(ptlen);
    return pt;
}

}  // namespace crypto
}  // namespace logos_sync
