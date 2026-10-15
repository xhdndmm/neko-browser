#pragma once

// Fuzz harness entry points for the engine's untrusted-input parsers.
//
// Each function accepts one raw input and must return normally for every
// input the parser rejects or accepts; crashing, hanging, or leaking is a
// finding.  The functions are shared between two drivers:
//
//   * tests/fuzz/fuzz_*.cpp      libFuzzer executables (Clang, opt-in via
//                                NEKO_BUILD_FUZZERS) for long fuzzing runs;
//   * tests/unit/fuzz/fuzz_regression_test.cpp  a GoogleTest that replays
//                                every corpus file (tests/fuzz/corpus/**)
//                                plus a fixed adversarial battery on every
//                                build, so a crash reproducer committed as a
//                                corpus file is a permanent regression test
//                                on all toolchains (GCC/Clang/MSVC).
//
// See docs/testing/fuzzing.md.

#include <cstddef>
#include <cstdint>

namespace neko::fuzz {

// url::Url::Parse — absolute parsing, serialization round-trip, and relative
// resolution (the input is split in half: first half is the base, second half
// the reference to resolve against it).
void Url(const std::uint8_t* data, std::size_t size);

// network::ParseHttpResponse — raw HTTP/1.1 response head + body framing.
void Http(const std::uint8_t* data, std::size_t size);

// html::Tokenizer drain + html::Parser tree construction + a full DOM walk
// (the tree must be acyclic and finite for every input).
void Html(const std::uint8_t* data, std::size_t size);

// css::Tokenizer + ParseStyleSheet + ParseDeclarationBlock.
void Css(const std::uint8_t* data, std::size_t size);

} // namespace neko::fuzz
