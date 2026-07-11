// ed25519_ember.hpp - thin C++17 wrapper over the vendored orlp/ed25519 C
// implementation, giving the ember .em content-authentication path (F2,
// docs/spec/SPEC_AUDIT_2026-07-10.md F2) a clean, no-C-linkage surface.
//
// What this wraps and why:
//   - The vendored ed25519 sources (ed25519.h + sign.c + verify.c + keypair.c
//     + the field/group/scalar/sha512 machinery) are unchanged C99 from
//     orlp/ed25519 (public domain, see thirdparty/ed25519/license.txt). They
//     expose a C API taking raw `unsigned char*` buffers. This header gives
//     ember a `std::array<uint8_t,32>`/`std::array<uint8_t,64>` surface so the
//     .em writer/loader do not hand-roll buffer arithmetic.
//   - ed25519_create_seed (seed.c) is EXCLUDED via -DED25519_NO_SEED because it
//     pulls wincrypt.h on Windows (a platform coupling the audit asked ember to
//     avoid for portability) and because the test harness needs DETERMINISTIC
//     keys (a fixed seed) so a tamper test is reproducible. Production hosts
//     that want a CSPRNG seed provide their own 32-byte seed to
//     `ed25519_keypair_from_seed` below; the signing key stays OFF the host.
//
// Signature scheme note (the audit said "Ed25519 over SHA-256"):
//   Standard Ed25519 (RFC 8032) signs the message bytes directly (PureEdDSA)
//   and uses SHA-512 as its INTERNAL hash as part of the EdDSA construction.
//   There is no separate SHA-256 pass: a "SHA-256-prehashed Ed25519" would be a
//   non-standard variant no off-the-shelf keypair/tool verifies, and would buy
//   nothing here (the vendored impl already hashes the full message into the
//   signature). The audit's "over SHA-256" wording is read as "cryptographic
//   content authentication via the Ed25519 scheme over the .em content" (a
//   real hash, not the FNV1a identity hash). The implementation signs the
//   canonical content byte range directly; the scheme's internal hash is
//   SHA-512 per the standard. See the F2 commit message + SPEC_AUDIT F2.
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

#include "ed25519.h"  // vendored, public domain (orlp/ed25519)

namespace ember::ed25519 {

using PubKey  = std::array<uint8_t, 32>;
using PrivKey = std::array<uint8_t, 64>;  // expanded private key (seed-hash || pub), as orlp emits
using Sig     = std::array<uint8_t, 64>;

// Derive (pub, priv) from a 32-byte seed. The seed is the ONLY secret; the
// expanded `priv` returned here is the SHA-512(seed) || pub concatenation the
// orlp sign path consumes. A host that signs .em artifacts keeps the SEED
// (and/or priv) off the runtime; the runtime gets only `pub`.
inline void keypair_from_seed(const std::array<uint8_t,32>& seed,
                              PubKey& pub, PrivKey& priv) {
    ed25519_create_keypair(pub.data(), priv.data(), seed.data());
}

// Sign `msg` (len bytes) with `pub`+`priv`. Produces a 64-byte Ed25519
// signature over the message bytes (PureEdDSA; SHA-512 is internal to the
// scheme per RFC 8032 — there is no prehash).
inline void sign(const uint8_t* msg, size_t len,
                 const PubKey& pub, const PrivKey& priv, Sig& out) {
    ed25519_sign(out.data(), msg, len, pub.data(), priv.data());
}

// Verify a 64-byte `sig` over `msg` (len bytes) against `pub`. Returns true
// iff the signature is valid for this message under this public key. Constant
// time in the comparison; rejects a tampered message, tampered signature, or
// wrong key with `false` (never throws, never reads past `msg+len`).
inline bool verify(const Sig& sig, const uint8_t* msg, size_t len,
                   const PubKey& pub) {
    return ed25519_verify(sig.data(), msg, len, pub.data()) != 0;
}

} // namespace ember::ed25519
