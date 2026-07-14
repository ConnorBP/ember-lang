// extension_registry.hpp — shared extension-registry contracts.
//
// The reusable error/status/result vocabulary used by Ember's extension
// registries (pass, profile, and — per the companion
// plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md — keyed-dispatch registries).
// Keeping one contract here lets every registry report collisions and malformed
// registrations the same way instead of reimplementing parallel
// error/factory machinery.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §4.3.
//
// Status of this file: the contracts below are the Phase 1 foundation used by
// `EmberPassRegistry`'s configured/collision-aware factories. Later phases
// (seed derivation, checked execution, profiles) build on this contract but
// are not implemented yet; this header deliberately declares only the shared
// result vocabulary, not those later services.

#pragma once

#include <optional>
#include <string>
#include <utility>

namespace ember {

// A structured registration/load error. `registry` identifies which registry
// produced the error (e.g. "ember-pass"), `name` the offending entry (may be
// empty for registry-wide errors), and `message` a human-readable diagnostic.
struct ExtensionError {
    std::string registry;
    std::string name;
    std::string message;
};

// A status-only result: success carries no value, failure carries an error.
// `explicit operator bool()` is true on success.
struct ExtensionStatus {
    std::optional<ExtensionError> error;

    ExtensionStatus() = default;
    explicit ExtensionStatus(ExtensionError e) : error(std::move(e)) {}

    explicit operator bool() const { return !error.has_value(); }
};

// A value-or-error result. Success carries `value` and no error; failure
// carries `error` and no value. `explicit operator bool()` is true only when a
// value is present and no error is set.
template <class T>
struct ExtensionResult {
    std::optional<T> value;
    std::optional<ExtensionError> error;

    ExtensionResult() = default;
    explicit ExtensionResult(T v) : value(std::move(v)) {}
    explicit ExtensionResult(ExtensionError e) : error(std::move(e)) {}

    explicit operator bool() const { return value.has_value() && !error.has_value(); }
};

// ─── Convenience constructors ───
inline ExtensionStatus make_extension_ok() {
    return ExtensionStatus{};
}

inline ExtensionStatus make_extension_error(std::string registry,
                                            std::string name,
                                            std::string message) {
    return ExtensionStatus{ExtensionError{std::move(registry),
                                           std::move(name),
                                           std::move(message)}};
}

template <class T>
inline ExtensionResult<T> make_extension_result_ok(T v) {
    return ExtensionResult<T>{std::move(v)};
}

template <class T>
inline ExtensionResult<T> make_extension_result_error(std::string registry,
                                                      std::string name,
                                                      std::string message) {
    return ExtensionResult<T>{make_extension_error(std::move(registry),
                                                    std::move(name),
                                                    std::move(message)).error.value()};
}

} // namespace ember
