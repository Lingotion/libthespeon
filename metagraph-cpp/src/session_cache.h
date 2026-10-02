#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "meta_graph.pb.h"

namespace metagraph {

struct SessionOptionsConfig {
  int intra_op_threads = 0;
  int inter_op_threads = 1;
  bool use_global_thread_pools = false;
  bool allow_spinning = true;
  bool low_memory = false;
};

struct SessionHandle {
  SessionHandle(Ort::Env& env, const std::filesystem::path& model,
                const Ort::SessionOptions& options);

  Ort::Session session;
  std::vector<std::string> output_names;
  std::vector<const char*> output_name_pointers;
  std::uintmax_t model_bytes = 0;
};

// Owns the Ort::Env and every loaded session. Safe for concurrent use; creates
// no threads of its own.
class SessionCache {
 public:
  explicit SessionCache(SessionOptionsConfig config = {});
  ~SessionCache();

  SessionCache(const SessionCache&) = delete;
  SessionCache& operator=(const SessionCache&) = delete;

  std::shared_ptr<SessionHandle> GetOrCreate(const std::filesystem::path& model,
                                             int device);
  std::shared_ptr<SessionHandle> TryGet(const std::filesystem::path& model,
                                        int device) const;
  void Evict(const std::filesystem::path& model, int device);
  void Clear();
  std::uintmax_t ResidentBytes() const;
  std::size_t Size() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class GraphCache {
 public:
  GraphCache();
  ~GraphCache();

  GraphCache(const GraphCache&) = delete;
  GraphCache& operator=(const GraphCache&) = delete;

  // The runner stores raw pointers into the message, so callers must keep the
  // returned graph alive for the whole execution.
  std::shared_ptr<const metaonnx::MetaGraph> Load(
      const std::filesystem::path& graph_path);
  void Clear();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Every model the graph references, deduplicated, in the order execution first
// demands it: a caller that starts synthesizing mid-preload finds the sessions
// it needs next already built.
std::vector<const metaonnx::Node*> PreloadOrder(const metaonnx::MetaGraph& graph);

}  // namespace metagraph
