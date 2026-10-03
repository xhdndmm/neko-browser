// neko::ui::RunSteadyState — the GUI cadence driver (see steady_state.h).

#include "neko/ui/steady_state.h"

#include <QCoreApplication>
#include <thread>

namespace neko::ui {

SteadyStateResult RunSteadyState(BrowserWorker& worker, const SteadyStateOptions& options)
{
  if (options.raf_per_turn.has_value()) {
    worker.SetRafPerTurnLimit(options.raf_per_turn.value());
  }
  const auto call_start = std::chrono::steady_clock::now();
  SteadyStateResult result;

  // Wait for the page to settle first (bounded): the load is one posted task on
  // the worker, so pumps queued while it runs are not served until it finishes,
  // and a window starting at navigation would measure the load.
  const auto ready_deadline = call_start + options.ready_timeout;
  while (std::chrono::steady_clock::now() < ready_deadline) {
    QCoreApplication::processEvents();
    const browser::TabSnapshot tab = worker.SnapshotActiveTab();
    const bool has_content = tab.page != nullptr || tab.image != nullptr || tab.pdf != nullptr ||
                             tab.audio != nullptr || tab.raw_text != nullptr ||
                             tab.error != nullptr;
    if (has_content && !tab.loading) {
      result.ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  result.ready_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - call_start)
                        .count();

  const auto deadline = std::chrono::steady_clock::now() + options.duration;
  auto next_pump = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    // Qt events first: this is what delivers the worker's frame snapshots to the
    // widgets, and what a real session spends most of its idle time doing.
    QCoreApplication::processEvents();
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_pump) {
      worker.PumpScriptTimers();
      next_pump += options.interval;
      if (next_pump <= now) {
        // Fell behind (a slow pump or a loaded machine): resynchronise instead of
        // firing a burst of pumps to catch up, which would not resemble a session.
        next_pump = now + options.interval;
      }
    }
    // 1 ms granularity keeps the cadence honest without busy-waiting.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  QCoreApplication::processEvents();
  return result;
}

} // namespace neko::ui
