#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace metagraph {

// One-way stop flag. The running call registers how to stop itself and
// deregisters before that state dies, so a late Cancel is harmless.
class CancelFlag {
 public:
  void Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    if (on_cancel_) on_cancel_();
  }

  bool cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  }

  // True if already cancelled, so the caller can refuse to start.
  bool Register(std::function<void()> on_cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_cancel_ = std::move(on_cancel);
    return cancelled_;
  }

  void Unregister() {
    std::lock_guard<std::mutex> lock(mutex_);
    on_cancel_ = {};
  }

 private:
  mutable std::mutex mutex_;
  bool cancelled_ = false;
  std::function<void()> on_cancel_;
};

using CancelToken = std::shared_ptr<CancelFlag>;

class Cancelled : public std::runtime_error {
 public:
  Cancelled() : std::runtime_error("Synthesis was cancelled") {}
};

// For model loading, which ORT cannot interrupt: granularity is one model.
inline void ThrowIfCancelled(const CancelToken& token) {
  if (token && token->cancelled()) throw Cancelled();
}

// Registration for one graph run. A null token disables cancellation.
class CancelScope {
 public:
  CancelScope(CancelToken token, std::function<void()> on_cancel)
      : token_(std::move(token)) {
    if (token_ && token_->Register(std::move(on_cancel))) {
      token_->Unregister();
      throw Cancelled();
    }
  }
  ~CancelScope() {
    if (token_) token_->Unregister();
  }

  CancelScope(const CancelScope&) = delete;
  CancelScope& operator=(const CancelScope&) = delete;

  bool cancelled() const { return token_ && token_->cancelled(); }

 private:
  CancelToken token_;
};

}  // namespace metagraph
