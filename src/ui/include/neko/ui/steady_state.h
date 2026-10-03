#pragma once

#include "neko/ui/browser_worker.h"

#include <chrono>
#include <cstddef>
#include <optional>

namespace neko::ui {

// How a steady-state run is driven.
struct SteadyStateOptions
{
  // How long to drive the worker.
  std::chrono::milliseconds duration{5000};
  // Pump cadence, matching MainWindow's 50 ms script timer.
  std::chrono::milliseconds interval{50};
  // Opt-in per-turn requestAnimationFrame budget (see javascript::DomBinder).
  // Unset leaves the engine default, which is what a real session uses.
  std::optional<std::size_t> raf_per_turn;
  // How long to wait for the page to finish loading before the timed window
  // starts.  The worker loads a page as one posted task, so pumps queued during
  // the load are not served until it finishes: a window that began at navigation
  // would measure the load instead of the steady state.
  std::chrono::milliseconds ready_timeout{15000};
};

// What a steady-state run observed about its own preconditions.
struct SteadyStateResult
{
  // False when the page never settled inside |ready_timeout|: the measurements
  // then describe a load in progress, which the caller must not read as steady
  // state.
  bool ready = false;
  // Time from the start of the call until the page settled (or the timeout).
  long long ready_ms = 0;
};

// Drives |worker| the way the GUI does for |duration|: process Qt events and call
// PumpScriptTimers once per |interval|.
//
// Why this exists: the headless CLI measures a *load*, and its pump has no frame
// cadence, so every requestAnimationFrame callback accumulated during the load is
// drained in a single turn -- which is exactly what the load-time "longest turn"
// numbers are made of (measured: 38 callbacks, 0.8-4 s).  Whether frames actually
// advance while that work is spread out can only be seen by driving the real
// cadence, so this is the harness that decides whether the rAF budget is worth
// enabling.  The caller reads the outcome from
// BrowserWorker::SnapshotRuntimeStats(): frame count, frame production and
// interval distributions, pump turn durations and what those turns ran.
//
// It bounds nothing by wall clock and asserts nothing about time: it is a
// measurement driver, so a caller decides what (if anything) to gate on.
SteadyStateResult RunSteadyState(BrowserWorker& worker, const SteadyStateOptions& options);

} // namespace neko::ui
