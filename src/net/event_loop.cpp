#include "net/event_loop.h"

#include "util/log.h"

namespace hermit::net {

EventLoop::EventLoop(Listener& listener, const Config& cfg) : listener_(listener), cfg_(cfg) {}

EventLoop::~EventLoop() = default;

LoopStatus EventLoop::run() {
  // ==== CHECKPOINT 2: YOUR CODE ====
  (void)listener_;
  (void)cfg_;
  (void)stop_requested_;
  logging::error("EventLoop::run() is a stub — implement CHECKPOINT 2 (checkpoints/CP2.md)");
  return LoopStatus::kNotImplemented;
  // ==== END CHECKPOINT 2 ====
}

}  // namespace hermit::net
