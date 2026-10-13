// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/inference_runtime.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

#include "core/utils/utf8_path.h"
#include "session_cache.h"

namespace thespeon {
InferenceRuntime::InferenceRuntime(std::filesystem::path binaries,
                                   metagraph::SessionOptionsConfig options)
    : binaries_(std::move(binaries)),
      sessions_(std::make_shared<metagraph::SessionCache>(options)),
      graphs_(std::make_shared<metagraph::GraphCache>()) {}

void InferenceRuntime::Preload(const std::filesystem::path& graph,
                               const metagraph::PreloadProgress& on_progress,
                               const metagraph::CancelToken& cancel) {
  metagraph::MetaGraphRunner(binaries_, sessions_, graphs_)
      .PreloadGraph(graph, on_progress, cancel);
}

metagraph::Tensor InferenceRuntime::Execute(
    InferenceRequest request, const metagraph::CancelToken& cancel) {
  return metagraph::MetaGraphRunner(binaries_, sessions_, graphs_)
      .ExecuteGraphWithResult(request.graph, std::move(request.inputs), cancel);
}

void InferenceRuntime::ExecuteWithCallbacks(
    InferenceRequest request, metagraph::CallbackHandler handler,
    const metagraph::CancelToken& cancel) {
  metagraph::MetaGraphRunner(binaries_, sessions_, graphs_)
      .ExecuteGraphWithCallbacks(request.graph, std::move(request.inputs),
                                 std::move(handler), cancel);
}

void InferenceRuntime::Unload(const std::filesystem::path& graph) {
  if (!std::filesystem::exists(graph)) return;
  const auto parsed = graphs_->Load(graph);
  for (const auto* node : metagraph::PreloadOrder(*parsed)) {
    sessions_->Evict(binaries_ / (node->model_path() + ".onnx"),
                     static_cast<int>(node->preferred_device()));
  }
}

std::vector<SessionKey> InferenceRuntime::Sessions(
    const std::filesystem::path& graph) const {
  std::ifstream input(graph, std::ios::binary);
  if (!input)
    throw std::runtime_error("Could not open graph: " + PathToUtf8(graph));
  metaonnx::MetaGraph parsed;
  if (!parsed.ParseFromIstream(&input))
    throw std::runtime_error("Could not parse graph: " + PathToUtf8(graph));
  std::vector<SessionKey> result;
  for (const auto* node : metagraph::PreloadOrder(parsed)) {
    result.push_back({binaries_ / (node->model_path() + ".onnx"),
                      static_cast<int>(node->preferred_device())});
  }
  return result;
}

bool InferenceRuntime::IsResident(
    const std::vector<SessionKey>& sessions) const {
  return std::all_of(sessions.begin(), sessions.end(), [&](const auto& key) {
    return sessions_->TryGet(key.model, key.device) != nullptr;
  });
}

void InferenceRuntime::Clear() {
  sessions_->Clear();
  graphs_->Clear();
}

void InferenceRuntime::ClearGraphs() { graphs_->Clear(); }

std::uintmax_t InferenceRuntime::ResidentBytes() const {
  return sessions_->ResidentBytes();
}

}  // namespace thespeon
