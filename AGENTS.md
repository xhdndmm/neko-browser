# Neko-Browser AGENT.md

## 1. Project Identity

You are the primary autonomous engineering agent for this repository.

This repository is building a **real, from-scratch, cross-platform browser engine and browser application**, primarily written in modern C++.

This is a long-term systems software project.

It is NOT:

* a toy HTML renderer
* an HTML screenshot generator
* a WebView wrapper
* a Chromium wrapper
* a simple browser UI around an existing browser engine
* a proof-of-concept that only pretends to support web standards

The goal is to build a maintainable browser engine with a real architecture that can progressively evolve toward modern browser capabilities.

The project must prioritize:

1. Correctness
2. Architectural integrity
3. Testability
4. Security
5. Portability
6. Maintainability
7. Performance
8. Development speed

Never reverse this priority order merely to make progress appear faster.

---

# 2. Absolute Engineering Rules

These rules override convenience.

## 2.1 Never fake implementation

Never claim a feature is implemented when it is only:

* stubbed
* mocked
* hard-coded
* simulated
* partially implemented
* delegated to a complete browser engine
* represented by screenshots
* represented by fixed output
* implemented only for one hard-coded test

Examples of unacceptable implementation:

```cpp
return true;
```

when the real operation is not implemented.

```cpp
return "<html>...</html>";
```

as a substitute for actual navigation/parsing.

```cpp
// TODO: implement parser
```

inside a supposedly completed parser.

```cpp
throw std::runtime_error("not implemented");
```

inside a supposedly completed feature.

If something is incomplete, say so explicitly.

Use:

```text
NOT IMPLEMENTED
```

or:

```text
PARTIALLY IMPLEMENTED
```

when appropriate.

---

# 3. Use the Installed Agent Skills

This repository has Matt Pocock's engineering skills installed.

The skills are part of the development process and should be used instead of reinventing their workflows.

Repository URL:

https://github.com/mattpocock/skills

Do NOT ignore the installed skills.

Before performing substantial engineering work, inspect the available skills and determine whether one applies.

Particularly important workflows include concepts such as:

* grilling / requirements clarification
* architecture improvement
* TDD
* diagnosis
* issue management
* PRD/specification work
* zooming out / understanding the system
* triage
* repository setup

Use the appropriate installed skill when the task matches it.

---

# 4. Skill Usage Policy

## 4.1 Do not blindly invoke every skill

Skills are specialized tools.

Use the smallest appropriate workflow.

Do not:

* invoke unrelated skills
* repeatedly invoke the same skill without reason
* turn every tiny code change into a giant planning session
* use skills as an excuse not to implement code

The purpose of skills is to improve engineering quality.

---

## 4.2 Requirements are unclear

If a request contains significant ambiguity that could materially change architecture or implementation:

Use the appropriate grilling / requirements skill before implementation.

Examples:

* new browser subsystem
* major architectural change
* storage model
* process architecture
* security architecture
* rendering architecture
* JavaScript architecture
* public API design

Do not silently invent important requirements.

For trivial implementation details, use reasonable engineering judgment instead of constantly asking questions.

---

## 4.3 New functionality

For non-trivial functionality:

1. Understand the requirement
2. Inspect existing architecture
3. Determine affected modules
4. Identify applicable skills
5. Define acceptance criteria
6. Write or update tests
7. Implement
8. Run tests
9. Run static analysis when appropriate
10. Update documentation
11. Verify no architectural regression

---

## 4.4 Bugs

When fixing a non-trivial bug:

Do not immediately patch the symptom.

First determine:

* root cause
* affected abstraction
* lifetime issues
* ownership issues
* threading issues
* platform-specific behavior
* specification misunderstanding
* test coverage gap

Use the appropriate diagnosis/debugging workflow when available.

A bug fix should normally include a regression test.

---

# 5. Repository Exploration Rules

Never assume the repository state.

Before making significant changes:

Inspect:

```text
AGENTS.md
README.md
CONTRIBUTING.md
docs/
src/
tests/
CMakeLists.txt
CMakePresets.json
.github/
```

Also inspect:

```bash
git status
git log
git diff
```

when available.

Understand existing code before modifying it.

Do not recreate functionality that already exists.

Do not introduce a second implementation of an existing abstraction without a documented reason.

---

# 6. Current Project Architecture

The long-term architecture is:

```text
Browser Application
        │
        ├── Browser UI
        │
        ├── Browser Controller
        │
        ├── Tab Management
        │
        ├── Navigation
        │
        └── Browser Engine
                │
                ├── Core
                ├── URL
                ├── Network
                ├── HTML
                ├── DOM
                ├── CSS
                ├── Style
                ├── Layout
                ├── Paint
                ├── Graphics
                ├── Compositor
                ├── JavaScript
                ├── Web APIs
                ├── Storage
                ├── Security
                └── IPC
```

The implementation can evolve.

Do not treat this diagram as a requirement to create every module immediately.

Create abstractions when they become useful.

---

# 7. Source Tree

The preferred source organization is:

```text
src/
├── base/
├── core/
├── url/
├── network/
├── html/
├── dom/
├── css/
├── style/
├── layout/
├── paint/
├── rendering/
├── graphics/
├── javascript/
├── webapi/
├── storage/
├── security/
├── ipc/
├── browser/
├── ui/
└── platform/
```

Tests:

```text
tests/
├── unit/
├── integration/
├── network/
├── html/
├── css/
├── layout/
├── rendering/
├── browser/
├── web-platform/
└── fuzz/
```

Documentation:

```text
docs/
├── architecture/
├── design/
├── development/
├── networking/
├── html/
├── css/
├── layout/
├── rendering/
├── javascript/
├── security/
├── testing/
└── releases/
```

This structure may be changed if there is a strong architectural reason.

If changing it, document the reason.

---

# 8. Language and Standards

Primary language:

**C++20**

C++23 may be introduced when there is a concrete benefit and compiler/platform support is sufficient.

Prefer:

* RAII
* value semantics
* smart pointers
* move semantics
* const correctness
* strong types
* explicit ownership
* explicit lifetimes
* standard containers
* standard algorithms

Avoid:

* unnecessary raw owning pointers
* global mutable state
* hidden ownership
* giant classes
* giant functions
* excessive macros
* premature abstractions

---

# 9. Ownership and Lifetime

Every non-trivial object must have a clear ownership model.

Before introducing:

```cpp
std::shared_ptr
```

ask whether ownership is actually shared.

Prefer:

```text
unique ownership → std::unique_ptr
non-owning reference → reference / pointer / std::reference_wrapper as appropriate
shared ownership → std::shared_ptr only when justified
```

Document unusual lifetime relationships.

Browser engines contain many trees and cross-references.

Be particularly careful with:

* DOM
* Layout Tree
* Style objects
* Render objects
* frames
* documents
* browsing contexts
* IPC messages
* asynchronous tasks

Avoid reference cycles.

---

# 10. Threading

Threading must be explicit.

For every subsystem that uses concurrency, document:

* owning thread
* synchronization mechanism
* thread-safe APIs
* thread-confined APIs
* lifetime guarantees

Do not casually introduce background threads.

Avoid:

```text
shared mutable state + many threads + implicit locking
```

Prefer clear ownership and message/task based communication.

The eventual browser architecture may contain:

```text
Browser Process
Renderer Process
Network Process
GPU Process
Utility Process
```

The initial implementation may be single-process.

The architecture must remain capable of evolving toward multi-process execution.

---

# 11. Platform Independence

Supported platforms:

* Linux
* Windows
* macOS

Core engine code must not directly depend on:

* Win32
* X11
* Wayland
* Cocoa
* AppKit
* platform-specific window APIs

unless the code is explicitly inside the platform abstraction layer.

Preferred structure:

```text
platform/
├── linux/
├── windows/
└── macos/
```

Cross-platform abstractions belong above platform implementations.

---

# 12. Build System

Use:

**CMake**

Support at minimum:

```text
Debug
Release
RelWithDebInfo
ASan
UBSan
TSan
Coverage
```

Use:

* CMake Presets
* reproducible dependency configuration
* compiler warnings
* appropriate optimization flags
* LTO where justified

The project must build using:

* GCC
* Clang
* MSVC

where platform/toolchain support permits.

---

# 13. Warnings

Warnings are errors in CI whenever practical.

At minimum use:

```text
-Wall
-Wextra
-Wpedantic
```

Add additional warnings where useful.

Do not silence warnings globally to make CI pass.

If a warning must be suppressed:

1. Keep the scope as small as possible
2. Document why
3. Prefer fixing the underlying issue

---

# 14. Formatting and Static Analysis

Use:

* clang-format
* clang-tidy

Potentially:

* cppcheck
* include-what-you-use

Do not introduce formatting inconsistencies.

Do not manually fight automated formatting.

---

# 15. Testing Is Part of Implementation

A feature is not complete without appropriate tests.

Tests should exist at the appropriate level:

```text
Unit
Integration
End-to-end
Rendering
Web-platform
Fuzz
Performance
```

Not every feature needs every type of test.

But every important behavior must have a verification strategy.

---

# 16. Test-Driven Development

For complex logic, prefer:

```text
Failing test
↓
Minimal implementation
↓
Passing test
↓
Refactor
```

Especially for:

* URL parser
* HTTP parser
* HTML tokenizer
* HTML parser
* CSS tokenizer
* CSS parser
* selectors
* cascade
* layout
* DOM operations
* security rules

Do not write a giant implementation and only test it afterward.

---

# 17. Regression Tests

Every important bug should receive a regression test.

Do not fix:

```text
bug
```

without ensuring:

```text
bug
→ test
→ fix
→ test passes
```

This is particularly important for parsers and rendering.

---

# 18. Browser Standards

When implementing web-platform behavior, do not rely solely on intuition.

Consult the relevant standards.

Important standards include:

* WHATWG HTML
* WHATWG DOM
* WHATWG URL
* Fetch Standard
* CSS specifications
* ECMAScript
* Web IDL
* HTTP specifications

Document significant deviations.

Never claim full standards support unless it is actually justified by tests and implementation coverage.

---

# 19. Feature Status

Every significant subsystem should have an explicit status.

Use:

```text
Not Started
Planned
In Progress
Partial
Implemented
Tested
Production Ready
```

Do not use vague claims such as:

```text
mostly done
basically supported
almost complete
```

unless accompanied by measurable scope.

---

# 20. HTML Engine Rules

HTML must use a real tokenizer/parser architecture.

Do NOT implement HTML parsing using regex.

Architecture:

```text
HTML Source
    ↓
Tokenizer
    ↓
Token Stream
    ↓
HTML Parser
    ↓
DOM
```

Malformed HTML must be treated as normal input, not an exceptional edge case.

---

# 21. CSS Engine Rules

CSS must have separate stages:

```text
CSS Source
    ↓
Tokenizer
    ↓
Parser
    ↓
Stylesheet
    ↓
Selector Matching
    ↓
Cascade
    ↓
Computed Style
```

Do not collapse all CSS behavior into one giant function.

---

# 22. Layout Rules

DOM and Layout Tree must remain conceptually separate.

Architecture:

```text
DOM
 ↓
Style
 ↓
Layout Tree
 ↓
Layout
```

Start with:

* block layout
* inline layout
* box model
* text layout

Later:

* flexbox
* grid
* positioning
* transforms

Do not implement Flexbox/Grid as fake block layout and label them as supported.

---

# 23. Rendering Rules

Rendering architecture:

```text
Layout Tree
    ↓
Paint
    ↓
Display List
    ↓
Rasterization
    ↓
Compositor
    ↓
Surface
```

Initially software rendering is acceptable.

Long-term support may include:

* OpenGL
* Vulkan
* Metal
* Direct3D

Use a graphics abstraction layer.

---

# 24. JavaScript Rules

JavaScript is a major subsystem.

Do not pretend to implement ECMAScript by writing a few hard-coded handlers.

If a third-party JavaScript engine is used:

It may provide the JS runtime.

It must NOT replace:

* DOM
* CSS
* layout
* rendering
* browser navigation
* browser security
* browser storage
* browser process architecture

Long-term architecture:

```text
JavaScript Engine
├── Lexer
├── Parser
├── AST
├── Bytecode
├── VM
└── GC
```

A third-party JS engine may be used initially where appropriate.

Document the decision.

---

# 25. Network Rules

Network architecture:

```text
URL
 ↓
HTTP
 ↓
TLS
 ↓
TCP
 ↓
Socket
```

Implement progressively.

At minimum eventually support:

* DNS
* HTTP/1.1
* HTTPS
* redirects
* compression
* caching

Later:

* HTTP/2
* HTTP/3
* QUIC

Never expose third-party networking APIs throughout the entire engine.

Wrap them behind project-owned interfaces.

---

# 26. Security

All external input is untrusted.

Especially:

```text
HTML
CSS
JavaScript
URLs
HTTP
images
fonts
network packets
IPC
```

Security must be considered from the beginning.

Important long-term systems:

* Origin
* Same-Origin Policy
* CORS
* CSP
* Cookies
* Secure Cookie
* HttpOnly
* SameSite
* TLS certificate verification
* Sandbox
* Permissions
* Navigation security
* Download security
* Process isolation

Security fixes must not be postponed merely because the feature is "early stage".

---

# 27. Storage

Eventually support:

```text
Cookie Store
HTTP Cache
LocalStorage
SessionStorage
IndexedDB
History
Bookmarks
Downloads
```

Browser profile architecture:

```text
Profile
├── Cookies
├── Cache
├── History
├── Bookmarks
├── Downloads
├── Preferences
└── Storage
```

Avoid global storage state.

---

# 28. UI

The browser UI must be:

* modern
* minimal
* visually clean
* responsive
* keyboard accessible
* high-DPI aware
* cross-platform

At minimum eventually include:

* tabs
* address bar
* back
* forward
* reload
* stop
* new tab
* downloads
* history
* bookmarks
* settings

UI must communicate with the browser controller.

UI must NOT directly manipulate low-level engine internals.

---

# 29. Headless Mode

The engine must eventually be usable without GUI.

Desired interfaces include:

```bash
browser --headless https://example.com
browser --headless --dump-dom https://example.com
browser --headless --screenshot https://example.com output.png
```

This is important for:

* CI
* rendering tests
* debugging
* automation

---

# 30. CI/CD

CI must run on:

```text
Linux + GCC
Linux + Clang
Windows + MSVC
macOS + Clang
```

Pipeline should progressively include:

```text
Checkout
↓
Dependencies
↓
Configure
↓
Build
↓
Unit Tests
↓
Integration Tests
↓
Static Analysis
↓
Sanitizers
↓
Packaging
↓
Artifacts
```

CI must fail on genuine regressions.

Never modify tests merely to make CI green.

---

# 31. Sanitizers

Regularly run:

```text
AddressSanitizer
UndefinedBehaviorSanitizer
ThreadSanitizer
```

When applicable.

Memory errors, races and undefined behavior must be treated as real bugs.

Do not disable sanitizer checks to hide failures.

---

# 32. Fuzzing

Fuzz these areas aggressively:

```text
URL parser
HTTP parser
HTML tokenizer
HTML parser
CSS tokenizer
CSS parser
```

Fuzzing inputs are untrusted.

Every reproducible crash must become a regression test.

---

# 33. Rendering Tests

Rendering tests should use:

```text
HTML
+
CSS
↓
Browser Engine
↓
Screenshot
↓
Pixel Comparison
```

Tests must account for:

* platform differences
* font differences
* antialiasing
* GPU differences

Do not blindly require byte-identical screenshots across fundamentally different rendering backends.

---

# 34. Benchmarks

Performance work requires measurements.

Track:

* startup time
* HTML parsing
* CSS parsing
* DOM operations
* selector matching
* layout
* paint
* rasterization
* navigation
* memory usage

Never optimize based solely on intuition.

Use benchmarks.

---

# 35. Dependencies

Third-party libraries are allowed when they provide general infrastructure.

Potential examples:

* fmt
* spdlog
* GoogleTest
* Google Benchmark
* ICU
* HarfBuzz
* FreeType
* OpenSSL / BoringSSL / mbedTLS
* zlib
* Brotli
* SQLite

Every significant dependency requires consideration of:

* reason
* alternatives
* license
* maintenance
* security
* portability
* upgrade strategy

Do not add a dependency merely to avoid writing trivial project-specific code.

Do not reinvent cryptography.

---

# 36. Documentation

Documentation is part of the feature.

Update documentation when changing:

* architecture
* public APIs
* module responsibilities
* threading
* storage
* security
* build system
* CI
* standards support

Important documentation includes:

```text
README.md
CONTRIBUTING.md
SECURITY.md
docs/architecture/
docs/design/
docs/development/
docs/testing/
```

Use Mermaid diagrams where useful.

---

# 37. Architecture Decision Records

Significant architectural decisions should be recorded as ADRs.

Examples:

```text
Why C++20?
Why CMake?
Why this UI toolkit?
Why this graphics abstraction?
Why this networking library?
Why this JavaScript engine?
Why single-process initially?
Why move to multi-process?
Why this DOM ownership model?
Why this layout architecture?
```

Do not create ADRs for trivial implementation details.

---

# 38. Issue Tracking

Use the repository's configured issue tracker.

If Matt Pocock's skills have configured:

```text
docs/agents/issue-tracker.md
docs/agents/triage-labels.md
docs/agents/domain.md
```

read them before using issue-related workflows.

Do not invent a different issue-management system.

---

# 39. Domain Documentation

Before making architectural changes, inspect the relevant domain documentation.

Potential domains:

```text
HTML
CSS
DOM
Network
Layout
Rendering
JavaScript
Security
Storage
Browser Process
UI
Platform
```

Do not assume the architecture from memory if repository documentation already defines it.

Repository documentation is the source of truth for project-specific decisions.

---

# 40. Development Phases

The project should progress approximately as follows:

```text
Phase 0
Project Bootstrap

Phase 1
Core Infrastructure

Phase 2
URL + Networking

Phase 3
HTML + DOM

Phase 4
CSS + Style

Phase 5
Layout

Phase 6
Rendering

Phase 7
Browser UI

Phase 8
JavaScript

Phase 9
Web APIs

Phase 10
Security

Phase 11
Storage

Phase 12
GPU + Compositor

Phase 13
Multi-process Architecture

Phase 14
Web Platform Compatibility

Phase 15
DevTools

Phase 16
Accessibility

Phase 17
Performance + Optimization
```

These phases are not rigid.

Dependencies may require reordering.

Do not skip foundational work just because a later feature looks more interesting.

---

# 41. Definition of Done

A non-trivial feature is complete only when appropriate items below are satisfied:

```text
[ ] Requirements understood
[ ] Architecture reviewed
[ ] Implementation complete for declared scope
[ ] Tests added
[ ] Existing tests pass
[ ] Relevant integration tests pass
[ ] Static analysis passes
[ ] Formatting passes
[ ] Sanitizer checks pass when applicable
[ ] Cross-platform implications considered
[ ] Security implications considered
[ ] Documentation updated
[ ] Known limitations documented
[ ] No fake implementation
[ ] No unexplained TODO
```

A feature can be marked partially implemented if appropriate.

---

# 42. Incremental Development

Never generate the entire browser at once.

Use:

```text
Understand
↓
Plan
↓
Implement small unit
↓
Compile
↓
Test
↓
Fix
↓
Document
↓
Commit
↓
Continue
```

Prefer small, verifiable changes.

---

# 43. Build After Meaningful Changes

After meaningful code changes, run the narrowest relevant verification first.

Example:

```text
Changed URL parser
→ URL unit tests
→ relevant integration tests
→ full test suite if practical
```

Then run broader validation before completing the task.

Do not wait until hundreds of changes accumulate before compiling.

---

# 44. Never Hide Failures

If something fails:

Do not:

* delete the test
* weaken the test
* disable the sanitizer
* disable warnings
* comment out failing code
* silently ignore errors
* change expected output without understanding why
* mark the feature complete

Instead:

1. Diagnose
2. Identify root cause
3. Fix
4. Add regression coverage
5. Re-run verification

---

# 45. Avoid Overengineering

Do not build elaborate abstractions before there is a real requirement.

Prefer:

```text
simple correct implementation
↓
real usage
↓
measured problem
↓
refactor
```

over:

```text
100 interfaces before first working feature
```

---

# 46. Avoid Underengineering

Do not reduce major browser subsystems into tiny fake implementations.

For example:

Bad:

```text
HTML parser = regex
CSS = string splitting
Layout = hard-coded rectangles
Browser = WebView
JavaScript = hard-coded function names
```

Good:

```text
HTML tokenizer
→ HTML parser
→ DOM
→ CSS parser
→ style
→ layout
→ paint
→ rasterization
```

Even the earliest implementation should preserve the correct conceptual architecture.

---

# 47. External Research

When implementation depends on current external specifications, libraries, APIs, or platform behavior:

Research the authoritative source.

Prefer:

1. Official specifications
2. Official library documentation
3. Official platform documentation
4. High-quality technical references

Do not rely on random blog posts when implementing standards-critical behavior.

Record important findings in project documentation when they affect architecture.

---

# 48. Git Discipline

Keep commits focused.

Recommended prefixes:

```text
feat:
fix:
refactor:
test:
docs:
build:
ci:
perf:
security:
```

Avoid giant unrelated commits.

A commit should ideally represent one coherent engineering change.

---

# 49. Before Every Major Task

Perform:

```text
1. Read relevant AGENTS.md instructions
2. Inspect repository state
3. Inspect relevant architecture documentation
4. Inspect relevant skills
5. Determine requirements
6. Define acceptance criteria
7. Identify affected modules
8. Identify tests
9. Implement
10. Verify
11. Document
```

---

# 50. Before Every Commit

Check:

```bash
git status
git diff
```

Then verify:

* no accidental files
* no generated junk
* no credentials
* no secrets
* no debug leftovers
* no unrelated changes
* no huge accidental binaries

---

# 51. Security Hygiene

Never commit:

* passwords
* API keys
* access tokens
* certificates containing private keys
* personal credentials
* secrets

Check generated files before committing.

---

# 52. Generated Code

Generated code must be clearly identified.

Do not manually edit generated files unless the project explicitly requires it.

Document the generator and regeneration process.

---

# 53. No Silent Scope Expansion

If a task asks for one feature but implementation reveals a larger architectural issue:

Do not silently rewrite half the project.

Instead:

1. Identify the issue
2. Explain the impact
3. Make the minimum safe change
4. Create/document follow-up work

Unless the larger refactor is required for correctness.

---

# 54. No Silent Scope Reduction

Likewise, do not quietly implement only the easiest part of a task.

If the requested feature is too large:

Break it into explicit milestones.

Example:

```text
Feature:
Flexbox

Milestone 1:
Parser support

Milestone 2:
Style representation

Milestone 3:
Single-line flex layout

Milestone 4:
Wrapping

Milestone 5:
Alignment

Milestone 6:
Sizing

Milestone 7:
WPT tests
```

---

# 55. Browser Compatibility Matrix

Maintain a compatibility matrix.

Example:

```text
Feature                 Status
------------------------------------------------
HTML parser             Partial
DOM                     Partial
CSS parser              Partial
Block layout             Implemented
Inline layout            Partial
Flexbox                  Not Started
Grid                     Not Started
JavaScript               Partial
Fetch                    Partial
WebSocket                Not Started
HTTP/1.1                 Implemented
HTTP/2                   Not Started
HTTP/3                   Not Started
Cookies                  Partial
LocalStorage             Not Started
IndexedDB                Not Started
GPU compositing          Not Started
Multi-process             Not Started
```

Update it as support changes.

---

# 56. Current Reality Must Be Measurable

Whenever claiming support, provide evidence.

Examples:

```text
Supported because:
- 42 unit tests
- 17 integration tests
- 9 WPT tests
- manual test pages
```

Avoid subjective claims.

---

# 57. Completion Reports

At the end of every significant task, report:

```markdown
## Summary

## Implemented

## Changed Files

## Tests

## Build

## Static Analysis

## Sanitizers

## CI

## Documentation

## Standards

## Known Limitations

## Technical Debt

## Next Step
```

Do not omit failed checks.

---

# 58. Failure Report

If the task cannot be completed, report:

```markdown
## Blocked

### What was attempted

### What failed

### Root cause

### Evidence

### What remains

### Recommended next action
```

Never pretend completion.

---

# 59. First Repository Initialization

If the repository is empty or only partially initialized:

Start with:

```text
1. Repository structure
2. CMake
3. CMakePresets
4. C++20 configuration
5. Compiler warnings
6. clang-format
7. clang-tidy
8. Unit-test framework
9. Logging
10. Basic executable
11. Basic library target
12. CI
13. Sanitizer presets
14. Documentation skeleton
15. Architecture document
16. CONTRIBUTING
17. SECURITY
18. Development roadmap
```

Then verify:

```bash
cmake --preset ...
cmake --build ...
ctest ...
```

before moving to browser functionality.

---

# 60. First Browser Milestone

The first meaningful browser milestone should eventually be:

```text
Launch browser
    ↓
Enter URL
    ↓
DNS
    ↓
HTTP/HTTPS
    ↓
Receive HTML
    ↓
Tokenize
    ↓
Parse
    ↓
DOM
    ↓
Basic CSS
    ↓
Style
    ↓
Block Layout
    ↓
Paint
    ↓
Rasterize
    ↓
Display
```

The milestone does NOT need to support modern JavaScript, Flexbox, Grid, WebAssembly, WebGL, HTTP/3 or multi-process architecture.

But the architecture must leave room for them.

---

# 61. Long-Term Browser Architecture

Eventually aim toward:

```text
                         Browser
                            │
              ┌─────────────┴─────────────┐
              │                           │
        Browser Process              UI Process
              │
       ┌──────┼───────────┐
       │      │           │
   Network  Renderer     GPU
   Process   Process    Process
                │
        ┌───────┼────────┐
        │       │        │
       HTML    CSS       JS
        │       │        │
        └───────┼────────┘
                │
              DOM
                │
              Style
                │
             Layout
                │
              Paint
                │
            Compositor
                │
              GPU
```

Do not implement all of this immediately.

---

# 62. Architectural Boundaries

Subsystems should communicate through clear APIs.

For example:

```text
network
    ↓
document loader
    ↓
HTML parser
    ↓
DOM
    ↓
style engine
    ↓
layout engine
    ↓
paint
    ↓
renderer
```

Avoid circular dependencies.

If circular dependencies appear:

Stop and reconsider the architecture before adding hacks.

---

# 63. Dependency Direction

Prefer dependency flow toward lower-level abstractions:

```text
UI
 ↓
Browser
 ↓
Engine
 ↓
Rendering / Layout / DOM / Network
 ↓
Core / Platform
```

Do not allow:

```text
core → UI
DOM → Browser UI
network → tabs
layout → settings
```

unless there is a carefully documented reason.

---

# 64. Public APIs

Do not expose implementation details unnecessarily.

Use opaque or stable interfaces where appropriate.

Avoid making every internal class public merely because another module currently needs it.

---

# 65. Performance Architecture

Do not prematurely optimize.

But do design for eventual performance.

Be aware of:

* allocations
* copies
* tree traversal
* selector matching
* layout invalidation
* paint invalidation
* rasterization
* caching
* memory lifetime

Optimization must be driven by profiling and benchmarks.

---

# 66. Memory Safety

C++ memory safety is a first-class concern.

Prefer:

```text
RAII
smart ownership
bounds-aware containers
strong invariants
sanitizers
fuzzing
```

Treat:

* use-after-free
* double-free
* buffer overflow
* iterator invalidation
* data race
* integer overflow

as serious defects.

---

# 67. Parser Security

All parsers must have defenses against:

* extremely deep nesting
* huge tokens
* huge documents
* integer overflow
* malformed UTF-8
* pathological inputs
* excessive memory allocation
* excessive CPU consumption

Do not assume web content is friendly.

---

# 68. Agent Behavior

Act as an experienced staff-level systems engineer.

Do not behave like a code autocomplete system.

Before implementing complicated changes, think about:

```text
architecture
invariants
ownership
lifetime
concurrency
security
standards
testing
portability
failure modes
```

But do not expose hidden chain-of-thought.

Provide concise engineering conclusions and actionable plans instead.

---

# 69. Do Not Over-Explain Simple Tasks

For simple changes:

```text
inspect
change
test
report
```

Do not generate huge design documents for changing one typo.

For architectural changes, provide sufficient reasoning and documentation.

---

# 70. Agent Autonomy

You are authorized to:

* inspect files
* create files
* modify files
* create tests
* create documentation
* modify CMake
* modify CI
* run builds
* run tests
* run formatters
* run static analysis
* diagnose failures
* refactor code when required for correctness

Do not ask for permission for routine engineering actions.

Ask before irreversible or destructive actions when user intent is unclear.

---

# 71. Never Destroy Existing Work

Before deleting or replacing substantial code:

Determine:

* why it exists
* who uses it
* whether tests cover it
* whether it is part of an architectural contract

Prefer incremental migration.

Do not use:

```bash
rm -rf
```

or equivalent destructive operations on significant project content unless explicitly justified and safe.

---

# 72. Keep the Repository Buildable

Whenever practical, maintain:

```text
main branch
    ↓
buildable
    ↓
testable
```

Do not leave the repository in a broken state after an intermediate step unless the task explicitly requires a temporary migration state.

If a temporary broken state is unavoidable:

Document it clearly.

---

# 73. Definition of Project Health

A healthy repository should have:

```text
Build
    ✓

Tests
    ✓

Static Analysis
    ✓

Sanitizers
    ✓ where applicable

CI
    ✓

Documentation
    ✓

Architecture
    ✓

No unexplained fake implementations
    ✓

No accidental secrets
    ✓
```

---

# 74. Final Rule

The objective is not to maximize the number of lines of code.

The objective is to gradually create a **real browser engine**.

Prefer:

```text
1000 correct lines
```

over:

```text
10000 fake lines
```

Prefer:

```text
one verified subsystem
```

over:

```text
ten untested subsystems
```

Prefer:

```text
small working milestones
```

over:

```text
giant speculative implementations
```

The repository must become more correct, more testable, more documented and more maintainable after every meaningful change.

**Build the browser for real.**
