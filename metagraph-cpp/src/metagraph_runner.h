#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cancel_token.h"

namespace metagraph {

enum class DType { Int64, Float32 };

class Tensor {
 public:
  Tensor() = default;

  static Tensor Int64(std::vector<std::int64_t> shape,
                      std::vector<std::int64_t> values);
  static Tensor Float32(std::vector<std::int64_t> shape,
                        std::vector<float> values);
  static Tensor FullInt64(std::vector<std::int64_t> shape,
                          std::int64_t value);
  static Tensor FullFloat32(std::vector<std::int64_t> shape, float value);

  DType dtype() const { return dtype_; }
  const std::vector<std::int64_t>& shape() const { return shape_; }
  std::size_t size() const;
  const std::int64_t* int64_data() const;
  const float* float32_data() const;
  std::int64_t* mutable_int64_data();
  float* mutable_float32_data();
  Tensor DeepCopy() const;

 private:
  using IntStorage = std::shared_ptr<std::vector<std::int64_t>>;
  using FloatStorage = std::shared_ptr<std::vector<float>>;

  DType dtype_ = DType::Float32;
  std::vector<std::int64_t> shape_;
  std::variant<IntStorage, FloatStorage> storage_ =
      std::make_shared<std::vector<float>>();
};

using TensorMap = std::unordered_map<std::string, Tensor>;

enum class CallbackType : std::uint8_t {
  Unspecified = 0,
  Error = 1,
  Audio = 2,
  TriggerSample = 3,
};

using CallbackMetadataValue =
    std::variant<std::int64_t, float, bool, std::string>;
using CallbackMetadata =
    std::unordered_map<std::string, CallbackMetadataValue>;
using CallbackPayload =
    std::variant<std::vector<float>, std::vector<std::int64_t>>;

struct CallbackPacket {
  CallbackType type = CallbackType::Unspecified;
  CallbackPayload payload = std::vector<float>{};
  CallbackMetadata metadata;
};

using CallbackHandler = std::function<void(CallbackPacket)>;

// Thrown when the graph emits an error callback; what() is
// "<callback_name>: <message>".
class GraphError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class SessionCache;
class GraphCache;

// Progress is reported as (loaded, total); returning false cancels.
using PreloadProgress = std::function<bool(std::size_t, std::size_t)>;

class MetaGraphRunner {
 public:
  explicit MetaGraphRunner(std::filesystem::path models_path);
  MetaGraphRunner(std::filesystem::path models_path,
                  std::shared_ptr<SessionCache> sessions,
                  std::shared_ptr<GraphCache> graphs);
  ~MetaGraphRunner();

  MetaGraphRunner(MetaGraphRunner&&) noexcept;
  MetaGraphRunner& operator=(MetaGraphRunner&&) noexcept;
  MetaGraphRunner(const MetaGraphRunner&) = delete;
  MetaGraphRunner& operator=(const MetaGraphRunner&) = delete;

  // cancel stops the run by terminating the ONNX call in flight, and the run
  // then throws Cancelled. An error callback in the graph throws GraphError.
  Tensor ExecuteGraphWithResult(const std::filesystem::path& graph_path,
                                TensorMap input_tensors,
                                const CancelToken& cancel = {});
  void ExecuteGraphWithCallbacks(
      const std::filesystem::path& graph_path, TensorMap input_tensors,
      CallbackHandler callback_handler, const CancelToken& cancel = {});

  // Blocks until every model the graph references is loaded. Callers own their
  // threading; this creates none.
  // Throws Cancelled when cancel trips or on_progress returns false. ORT
  // cannot interrupt a session build, so a cancel lands between models.
  void PreloadGraph(const std::filesystem::path& graph_path,
                    const PreloadProgress& on_progress = {},
                    const CancelToken& cancel = {});

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace metagraph
