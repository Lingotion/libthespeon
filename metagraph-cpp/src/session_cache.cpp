#include "session_cache.h"

#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "metagraph_profile.h"

namespace metagraph {
namespace {

std::string CacheKey(const std::filesystem::path& model, int device) {
  return model.string() + '#' + std::to_string(device);
}

Ort::SessionOptions MakeSessionOptions(const SessionOptionsConfig& config) {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  if (config.intra_op_threads > 0)
    options.SetIntraOpNumThreads(config.intra_op_threads);
  if (config.inter_op_threads > 0)
    options.SetInterOpNumThreads(config.inter_op_threads);
  if (config.use_global_thread_pools) options.DisablePerSessionThreads();
  if (!config.allow_spinning) {
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
    options.AddConfigEntry("session.inter_op.allow_spinning", "0");
  }
  if (config.low_memory) {
    options.DisableCpuMemArena();
    options.DisableMemPattern();
  }
  return options;
}

Ort::Env MakeEnv(const SessionOptionsConfig& config) {
  if (!config.use_global_thread_pools)
    return Ort::Env(ORT_LOGGING_LEVEL_WARNING, "metagraph");
  Ort::ThreadingOptions threading;
  if (config.intra_op_threads > 0)
    threading.SetGlobalIntraOpNumThreads(config.intra_op_threads);
  if (config.inter_op_threads > 0)
    threading.SetGlobalInterOpNumThreads(config.inter_op_threads);
  threading.SetGlobalSpinControl(config.allow_spinning);
  return Ort::Env(threading, ORT_LOGGING_LEVEL_WARNING, "metagraph");
}

void CollectNodes(
    const google::protobuf::RepeatedPtrField<metaonnx::GraphItem>& items,
    const std::unordered_map<std::string, const metaonnx::Loop*>& loops,
    const std::unordered_map<std::string, const metaonnx::Node*>& nodes,
    std::vector<const metaonnx::Node*>& order,
    std::unordered_set<std::string>& seen) {
  for (const auto& item : items) {
    switch (item.kind_case()) {
      case metaonnx::GraphItem::kNodeId: {
        const auto node = nodes.find(item.node_id());
        if (node != nodes.end() && seen.insert(item.node_id()).second)
          order.push_back(node->second);
        break;
      }
      case metaonnx::GraphItem::kLoopId: {
        const auto loop = loops.find(item.loop_id());
        if (loop != loops.end())
          CollectNodes(loop->second->subgraph(), loops, nodes, order, seen);
        break;
      }
      case metaonnx::GraphItem::kConditional:
        CollectNodes(item.conditional().then_flow(), loops, nodes, order, seen);
        CollectNodes(item.conditional().else_flow(), loops, nodes, order, seen);
        break;
      default:
        break;
    }
  }
}

}  // namespace

SessionHandle::SessionHandle(Ort::Env& env, const std::filesystem::path& model,
                             const Ort::SessionOptions& options)
    : session(env, model.c_str(), options),
      model_bytes(std::filesystem::file_size(model)) {
  Ort::AllocatorWithDefaultOptions allocator;
  const auto count = session.GetOutputCount();
  output_names.reserve(count);
  output_name_pointers.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    output_names.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
  for (const auto& name : output_names)
    output_name_pointers.push_back(name.c_str());
}

class SessionCache::Impl {
 public:
  explicit Impl(SessionOptionsConfig config)
      : config_(config), env_(MakeEnv(config)) {}

  std::shared_ptr<SessionHandle> GetOrCreate(const std::filesystem::path& model,
                                             int device) {
    const auto key = CacheKey(model, device);
    std::promise<std::shared_ptr<SessionHandle>> promise;
    std::shared_future<std::shared_ptr<SessionHandle>> pending;
    bool build = false;
    {
      std::lock_guard lock(mutex_);
      if (const auto found = sessions_.find(key); found != sessions_.end())
        return found->second;
      if (const auto found = in_flight_.find(key); found != in_flight_.end()) {
        pending = found->second;
      } else {
        pending = promise.get_future().share();
        in_flight_.emplace(key, pending);
        build = true;
      }
    }
    // Building the larger models takes hundreds of milliseconds, so it happens
    // outside the lock; a concurrent caller waits on the same future instead of
    // blocking every other key.
    if (!build) return pending.get();
    try {
      METAGRAPH_PROFILE_SCOPE("CreateSession");
      auto handle = std::make_shared<SessionHandle>(env_, model,
                                                    MakeSessionOptions(config_));
      {
        std::lock_guard lock(mutex_);
        sessions_.emplace(key, handle);
        in_flight_.erase(key);
      }
      promise.set_value(handle);
      return handle;
    } catch (...) {
      {
        std::lock_guard lock(mutex_);
        in_flight_.erase(key);
      }
      promise.set_exception(std::current_exception());
      throw;
    }
  }

  std::shared_ptr<SessionHandle> TryGet(const std::filesystem::path& model,
                                        int device) const {
    std::lock_guard lock(mutex_);
    const auto found = sessions_.find(CacheKey(model, device));
    return found == sessions_.end() ? nullptr : found->second;
  }

  void Evict(const std::filesystem::path& model, int device) {
    std::lock_guard lock(mutex_);
    sessions_.erase(CacheKey(model, device));
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    sessions_.clear();
  }

  std::uintmax_t ResidentBytes() const {
    std::lock_guard lock(mutex_);
    std::uintmax_t total = 0;
    for (const auto& [key, handle] : sessions_) total += handle->model_bytes;
    return total;
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
  }

 private:
  SessionOptionsConfig config_;
  Ort::Env env_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<SessionHandle>> sessions_;
  std::unordered_map<std::string,
                     std::shared_future<std::shared_ptr<SessionHandle>>>
      in_flight_;
};

SessionCache::SessionCache(SessionOptionsConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
SessionCache::~SessionCache() = default;

std::shared_ptr<SessionHandle> SessionCache::GetOrCreate(
    const std::filesystem::path& model, int device) {
  return impl_->GetOrCreate(model, device);
}

std::shared_ptr<SessionHandle> SessionCache::TryGet(
    const std::filesystem::path& model, int device) const {
  return impl_->TryGet(model, device);
}

void SessionCache::Evict(const std::filesystem::path& model, int device) {
  impl_->Evict(model, device);
}

void SessionCache::Clear() { impl_->Clear(); }
std::uintmax_t SessionCache::ResidentBytes() const {
  return impl_->ResidentBytes();
}
std::size_t SessionCache::Size() const { return impl_->Size(); }

class GraphCache::Impl {
 public:
  std::shared_ptr<const metaonnx::MetaGraph> Load(
      const std::filesystem::path& graph_path) {
    std::error_code error;
    const Stamp stamp{std::filesystem::file_size(graph_path, error),
                      std::filesystem::last_write_time(graph_path, error)};
    const auto key = graph_path.string();
    {
      std::lock_guard lock(mutex_);
      if (const auto found = graphs_.find(key);
          found != graphs_.end() && found->second.stamp == stamp)
        return found->second.graph;
    }

    std::ifstream input(graph_path, std::ios::binary);
    if (!input)
      throw std::runtime_error("Could not open graph: " + graph_path.string());
    auto graph = std::make_shared<metaonnx::MetaGraph>();
    if (!graph->ParseFromIstream(&input))
      throw std::runtime_error("Could not parse graph: " + graph_path.string());

    std::lock_guard lock(mutex_);
    graphs_[key] = Entry{graph, stamp};
    return graph;
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    graphs_.clear();
  }

 private:
  struct Stamp {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified;
    friend bool operator==(const Stamp&, const Stamp&) = default;
  };
  struct Entry {
    std::shared_ptr<const metaonnx::MetaGraph> graph;
    Stamp stamp;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> graphs_;
};

GraphCache::GraphCache() : impl_(std::make_unique<Impl>()) {}
GraphCache::~GraphCache() = default;

std::shared_ptr<const metaonnx::MetaGraph> GraphCache::Load(
    const std::filesystem::path& graph_path) {
  return impl_->Load(graph_path);
}

void GraphCache::Clear() { impl_->Clear(); }

std::vector<const metaonnx::Node*> PreloadOrder(
    const metaonnx::MetaGraph& graph) {
  std::unordered_map<std::string, const metaonnx::Loop*> loops;
  for (const auto& loop : graph.loops()) loops[loop.id()] = &loop;
  std::unordered_map<std::string, const metaonnx::Node*> nodes;
  for (const auto& node : graph.nodes()) nodes[node.id()] = &node;

  std::vector<const metaonnx::Node*> order;
  std::unordered_set<std::string> seen;
  CollectNodes(graph.graph(), loops, nodes, order, seen);
  return order;
}

}  // namespace metagraph
