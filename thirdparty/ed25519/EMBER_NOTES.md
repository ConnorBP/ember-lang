# ed25519 — ember notes (F2 .em content authentication)

This directory vendors a small, self-contained Ed25519 implementation that ember
links to sign/verify `.em` modules (F2, `docs/spec/SPEC_AUDIT_2026-07-10.md` F2:
signed raw-x86 `.em` — Ed25519 over the `.em` content, verified before
`alloc_executable_rw`).

## Provenance

Vendored verbatim from **orlp/ed25519** (https://github.com/orlp/ed25519), the
portable public-domain Ed25519 by Orson Peters. The C sources (`fe.c`, `ge.c`,
`sc.c`, `sha512.c`, `keypair.c`, `sign.c`, `verify.c`, `add_scalar.c`,
`key_exchange.c`, and their headers) are unchanged. See `license.txt` (the
zlib-style public-domain release) and `readme.md` (the upstream README — note the
lowercase `readme.md`; on case-insensitive filesystems a separate `README.md`
collides with it, so these ember notes live in `EMBER_NOTES.md`).

## What ember uses

Only `ed25519_sign` / `ed25519_verify` / `ed25519_create_keypair` (the standard
sign/verify/keygen surface). `add_scalar` and `key_exchange` ship in the lib for
completeness but are not called by ember. The C++ wrapper
`ed25519_ember.hpp` gives ember a clean `std::array<uint8_t,32/64>` surface
(`PubKey`, `PrivKey`, `Sig`, `keypair_from_seed`, `sign`, `verify`).

## ED25519_NO_SEED — why `seed.c` is excluded

`seed.c` (the CSPRNG `ed25519_create_seed`) is **excluded** from the build via
`-DED25519_NO_SEED` (see `CMakeLists.txt`). Two reasons:

1. **Portability.** `seed.c` uses `wincrypt.h` (`CryptAcquireContext` /
   `CryptGenRandom`) on Windows — a platform coupling the F2 audit asked ember to
   avoid (ember is meant to be portable; the build uses no crypto lib). Excluding
   it keeps ember dependency-free and cross-platform.
2. **Reproducibility.** The F2 test harness (`examples/em_signed_test.cpp`) needs
   a **deterministic** 32-byte seed so the tamper test is reproducible. A CSPRNG
   seed would make the test non-deterministic.

Production hosts that want a CSPRNG seed provide their own 32-byte seed to
`ember::ed25519::keypair_from_seed` (the wrapper's `keypair_from_seed` calls
`ed25519_create_keypair`, which is the deterministic keygen that hashes the seed
with SHA-512 — no CSPRNG needed inside the lib). The signing key (seed / expanded
priv) stays **OFF the host**; the host gets only the 32-byte verification public
key.

## "Ed25519 over SHA-256" — the scheme note

The F2 audit (`docs/spec/SPEC_AUDIT_2026-07-10.md` F2) says "Ed25519 over
SHA-256." Standard Ed25519 (RFC 8032) signs the message bytes directly
(PureEdDSA) and uses **SHA-512** as its INTERNAL hash as part of the EdDSA
construction — there is no separate SHA-256 pass. A "SHA-256-prehashed Ed25519"
would be a non-standard variant no off-the-shelf keypair/tool verifies, and
would buy nothing here (the vendored impl already hashes the full message into
the signature). The audit's "over SHA-256" is read as "**cryptographic content
authentication via Ed25519 over the `.em` content**" (a real hash, not the FNV1a
identity hash the v2/v3 build_id/abi_hash is). The implementation signs the
canonical content byte range directly; the scheme's internal hash is SHA-512
per the standard. See `ed25519_ember.hpp` and the F2 commit message.
