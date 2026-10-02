// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "metagraph_runner.h"

namespace thespeon {

struct InferenceRequest {
  std::filesystem::path graph;
  metagraph::TensorMap inputs;
};

struct SessionKey {
  std::filesystem::path model;
  int device = 0;
};

struct RuntimeOptions {
  int intra_op_threads = 0;
  bool allow_spinning = true;
  bool low_memory = false;
};

class InferenceRuntime {
 public:
  explicit InferenceRuntime(std::filesystem::path binaries,
                            RuntimeOptions options = {});

  metagraph::Tensor Execute(InferenceRequest request,
                            const metagraph::CancelToken& cancel = {});
  void ExecuteWithCallbacks(
      InferenceRequest request, metagraph::CallbackHandler handler,
      const metagraph::CancelToken& cancel = {});
  void Preload(const std::filesystem::path& graph,
               const metagraph::PreloadProgress& on_progress = {},
               const metagraph::CancelToken& cancel = {});
  void Unload(const std::filesystem::path& graph);
  // Parses the graph without adding it to the graph cache.
  std::vector<SessionKey> Sessions(const std::filesystem::path& graph) const;
  bool IsResident(const std::vector<SessionKey>& sessions) const;
  void ClearGraphs();
  void Clear();
  std::uintmax_t ResidentBytes() const;

 private:
  std::filesystem::path binaries_;
  std::shared_ptr<metagraph::SessionCache> sessions_;
  std::shared_ptr<metagraph::GraphCache> graphs_;
};

}  // namespace thespeon
