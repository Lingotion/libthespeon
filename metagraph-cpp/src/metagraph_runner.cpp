#include "metagraph_runner.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "meta_graph.pb.h"
#include "metagraph_profile.h"
#include "session_cache.h"

namespace metagraph {
namespace {

using HostValue =
    std::variant<std::int64_t, double, bool, std::string, Tensor>;
using HostMap = std::unordered_map<std::string, HostValue>;

std::size_t CalculateElementCount(const std::vector<std::int64_t>& shape) {
  std::size_t count = 1;
  for (const auto dim : shape) {
    if (dim < 0) throw std::runtime_error("Negative tensor dimension");
    if (dim != 0 && count > std::numeric_limits<std::size_t>::max() /
                                static_cast<std::size_t>(dim)) {
      throw std::runtime_error("Tensor is too large");
    }
    count *= static_cast<std::size_t>(dim);
  }
  return count;
}

const char* DataTypeName(DType dtype) {
  return dtype == DType::Int64 ? "int64" : "float32";
}

template <typename Map>
const typename Map::mapped_type& GetRequiredEntry(const Map& map,
                                                  const std::string& name,
                                                  std::string_view kind) {
  const auto it = map.find(name);
  if (it == map.end()) {
    throw std::runtime_error(std::string(kind) + " not found: " + name);
  }
  return it->second;
}

std::int64_t TensorScalarAsInt64(const Tensor& tensor) {
  if (tensor.size() != 1) {
    throw std::runtime_error("Expected a one-element tensor");
  }
  return tensor.dtype() == DType::Int64
             ? tensor.int64_data()[0]
             : static_cast<std::int64_t>(tensor.float32_data()[0]);
}

double TensorScalarAsDouble(const Tensor& tensor) {
  if (tensor.size() != 1) {
    throw std::runtime_error("Expected a one-element tensor");
  }
  return tensor.dtype() == DType::Int64
             ? static_cast<double>(tensor.int64_data()[0])
             : static_cast<double>(tensor.float32_data()[0]);
}

std::int64_t HostValueAsInt64(const HostValue& value) {
  return std::visit(
      [](const auto& item) -> std::int64_t {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int64_t>) return item;
        if constexpr (std::is_same_v<T, double>)
          return static_cast<std::int64_t>(item);
        if constexpr (std::is_same_v<T, bool>) return item ? 1 : 0;
        if constexpr (std::is_same_v<T, std::string>) return std::stoll(item);
        if constexpr (std::is_same_v<T, Tensor>) return TensorScalarAsInt64(item);
      },
      value);
}

double HostValueAsDouble(const HostValue& value) {
  return std::visit(
      [](const auto& item) -> double {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int64_t>)
          return static_cast<double>(item);
        if constexpr (std::is_same_v<T, double>) return item;
        if constexpr (std::is_same_v<T, bool>) return item ? 1.0 : 0.0;
        if constexpr (std::is_same_v<T, std::string>) return std::stod(item);
        if constexpr (std::is_same_v<T, Tensor>) return TensorScalarAsDouble(item);
      },
      value);
}

bool IsFloat(const HostValue& value) {
  if (std::holds_alternative<double>(value)) return true;
  const auto* tensor = std::get_if<Tensor>(&value);
  return tensor != nullptr && tensor->dtype() == DType::Float32;
}

// In order to be certain that the meta graph runs identically in C++ as in Python, we need to mirror its arithmetic
std::int64_t PythonFloorDivide(std::int64_t left, std::int64_t right) {
  if (right == 0) throw std::runtime_error("Division by zero");
  auto quotient = left / right;
  const auto remainder = left % right;
  if (remainder != 0 && ((remainder < 0) != (right < 0))) --quotient;
  return quotient;
}

/* Python modulo follows a different calculation of modulo than C++,
  In python, -1 % 10 = 9, but in C++ -1 % 10 = -1
*/
std::int64_t PythonModulo(std::int64_t left, std::int64_t right) {
  if (right == 0) throw std::runtime_error("Modulo by zero");
  auto result = left % right;
  if (result != 0 && ((result < 0) != (right < 0))) result += right;
  return result;
}

double PythonModulo(double left, double right) {
  if (right == 0.0) throw std::runtime_error("Modulo by zero");
  auto result = std::fmod(left, right);
  if (result != 0.0 && ((result < 0.0) != (right < 0.0))) result += right;
  return result;
}

double ApplyFloatBinaryOp(metaonnx::HostBinaryOp::Op op, double left,
                                   double right) {
  switch (op) {
    case metaonnx::HostBinaryOp::ADD: return left + right;
    case metaonnx::HostBinaryOp::SUB: return left - right;
    case metaonnx::HostBinaryOp::MUL: return left * right;
    case metaonnx::HostBinaryOp::DIV:
      if (right == 0.0) throw std::runtime_error("Division by zero");
      return left / right;
    case metaonnx::HostBinaryOp::MOD: return PythonModulo(left, right);
    default: throw std::runtime_error("Unknown HostBinaryOp operation");
  }
}

std::int64_t ApplyIntBinaryOp(metaonnx::HostBinaryOp::Op op,
                                   std::int64_t left, std::int64_t right) {
  switch (op) {
    case metaonnx::HostBinaryOp::ADD: return left + right;
    case metaonnx::HostBinaryOp::SUB: return left - right;
    case metaonnx::HostBinaryOp::MUL: return left * right;
    case metaonnx::HostBinaryOp::MOD: return PythonModulo(left, right);
    default: throw std::runtime_error("Operation does not produce an integer");
  }
}

HostValue ApplyBinaryOp(metaonnx::HostBinaryOp::Op op, const HostValue& left,
                      const HostValue& right) {
  const auto* left_tensor = std::get_if<Tensor>(&left);
  const auto* right_tensor = std::get_if<Tensor>(&right);
  if (left_tensor != nullptr || right_tensor != nullptr) {
    const Tensor& shape_source =
        left_tensor != nullptr && (right_tensor == nullptr || left_tensor->size() != 1)
            ? *left_tensor
            : *right_tensor;
    if (left_tensor != nullptr && right_tensor != nullptr &&
        left_tensor->shape() != right_tensor->shape() && left_tensor->size() != 1 &&
        right_tensor->size() != 1) {
      throw std::runtime_error("Tensor arithmetic requires equal shapes or a scalar tensor");
    }
    const auto shape = shape_source.shape();
    const auto count = std::max(left_tensor != nullptr ? left_tensor->size() : 1,
                                right_tensor != nullptr ? right_tensor->size() : 1);
    const bool floating = IsFloat(left) || IsFloat(right) ||
                          op == metaonnx::HostBinaryOp::DIV;
    if (floating) {
      std::vector<float> output(count);
      for (std::size_t i = 0; i < count; ++i) {
        const double l = left_tensor == nullptr
                             ? HostValueAsDouble(left)
                             : (left_tensor->dtype() == DType::Int64
                                    ? static_cast<double>(left_tensor->int64_data()[
                                          left_tensor->size() == 1 ? 0 : i])
                                    : left_tensor->float32_data()[
                                          left_tensor->size() == 1 ? 0 : i]);
        const double r = right_tensor == nullptr
                             ? HostValueAsDouble(right)
                             : (right_tensor->dtype() == DType::Int64
                                    ? static_cast<double>(right_tensor->int64_data()[
                                          right_tensor->size() == 1 ? 0 : i])
                                    : right_tensor->float32_data()[
                                          right_tensor->size() == 1 ? 0 : i]);
        output[i] = static_cast<float>(ApplyFloatBinaryOp(op, l, r));
      }
      return Tensor::Float32(shape, std::move(output));
    }
    std::vector<std::int64_t> output(count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto l = left_tensor == nullptr
                         ? HostValueAsInt64(left)
                         : left_tensor->int64_data()[left_tensor->size() == 1 ? 0 : i];
      const auto r = right_tensor == nullptr
                         ? HostValueAsInt64(right)
                         : right_tensor->int64_data()[right_tensor->size() == 1 ? 0 : i];
      output[i] = ApplyIntBinaryOp(op, l, r);
    }
    return Tensor::Int64(shape, std::move(output));
  }

  if (const auto* l = std::get_if<std::string>(&left)) {
    if (op == metaonnx::HostBinaryOp::ADD) {
      if (const auto* r = std::get_if<std::string>(&right)) return *l + *r;
    }
    throw std::runtime_error("Unsupported string arithmetic");
  }
  if (op == metaonnx::HostBinaryOp::DIV || IsFloat(left) ||
      IsFloat(right)) {
    return ApplyFloatBinaryOp(op, HostValueAsDouble(left),
                                       HostValueAsDouble(right));
  }
  return ApplyIntBinaryOp(op, HostValueAsInt64(left),
                               HostValueAsInt64(right));
}

std::string DescribeHostValue(const HostValue& value) {
  return std::visit(
      [](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(item);
        if constexpr (std::is_same_v<T, double>) return std::to_string(item);
        if constexpr (std::is_same_v<T, bool>) return item ? "True" : "False";
        if constexpr (std::is_same_v<T, std::string>) return item;
        if constexpr (std::is_same_v<T, Tensor>) {
          std::ostringstream out;
          out << DataTypeName(item.dtype()) << '[';
          for (std::size_t i = 0; i < item.shape().size(); ++i) {
            if (i != 0) out << ',';
            out << item.shape()[i];
          }
          return out.str() + "]";
        }
      },
      value);
}

class ExpressionParser {
 public:
  struct Symbol {
    std::string tensor;
    std::size_t dim = 0;
  };
  using Symbols = std::unordered_map<std::string, Symbol>;

  ExpressionParser(std::string_view text, const Symbols& symbols,
                   const HostMap& host, const TensorMap& tensors)
      : text_(text), symbols_(symbols), host_(host), tensors_(tensors) {}

  std::int64_t Parse() { return ParseAdditiveExpression(); }

 private:
  void SkipWhitespace() {
    while (position_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[position_]))) {
      ++position_;
    }
  }

  std::int64_t ParseIntLiteral() {
    std::int64_t value = 0;
    while (position_ < text_.size() &&
           std::isdigit(static_cast<unsigned char>(text_[position_]))) {
      value = value * 10 + (text_[position_++] - '0');
    }
    return value;
  }

  std::int64_t ResolveVariable() {
    const auto begin = position_;
    while (position_ < text_.size() &&
           (std::isalnum(static_cast<unsigned char>(text_[position_])) ||
            text_[position_] == '_')) {
      ++position_;
    }
    if (begin == position_) throw std::runtime_error("Expected expression value");
    const std::string name(text_.substr(begin, position_ - begin));

    if (!tensors_.empty()) {
      if (const auto symbol = symbols_.find(name); symbol != symbols_.end()) {
        const Tensor* tensor = nullptr;
        if (const auto h = host_.find(symbol->second.tensor); h != host_.end()) {
          tensor = std::get_if<Tensor>(&h->second);
        }
        if (tensor == nullptr) {
          const auto t = tensors_.find(symbol->second.tensor);
          if (t != tensors_.end()) tensor = &t->second;
        }
        if (tensor == nullptr || symbol->second.dim >= tensor->shape().size()) {
          throw std::runtime_error("Cannot resolve symbolic dimension: " + name);
        }
        return tensor->shape()[symbol->second.dim];
      }
      if (const auto h = host_.find(name); h != host_.end())
        return HostValueAsInt64(h->second);
      if (const auto t = tensors_.find(name); t != tensors_.end())
        return TensorScalarAsInt64(t->second);
    }
    throw std::runtime_error("Unknown expression variable: " + name);
  }

  std::int64_t Factor() {
    SkipWhitespace();
    if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
      const bool negative = text_[position_++] == '-';
      const auto value = Factor();
      return negative ? -value : value;
    }
    if (position_ < text_.size() && text_[position_] == '(') {
      ++position_;
      const auto value = ParseAdditiveExpression();
      SkipWhitespace();
      if (position_ < text_.size() && text_[position_] == ')') ++position_;
      return value;
    }
    if (position_ < text_.size() &&
        std::isdigit(static_cast<unsigned char>(text_[position_]))) {
      return ParseIntLiteral();
    }
    return ResolveVariable();
  }

  std::int64_t Term() {
    auto value = Factor();
    while (true) {
      SkipWhitespace();
      if (position_ >= text_.size() ||
          (text_[position_] != '*' && text_[position_] != '/' &&
           text_[position_] != '%')) {
        return value;
      }
      const char op = text_[position_++];
      const auto right = Factor();
      if (op == '*') value *= right;
      if (op == '/') value = PythonFloorDivide(value, right);
      if (op == '%') value = PythonModulo(value, right);
    }
  }

  std::int64_t ParseAdditiveExpression() {
    auto value = Term();
    while (true) {
      SkipWhitespace();
      if (position_ >= text_.size() ||
          (text_[position_] != '+' && text_[position_] != '-')) {
        return value;
      }
      const char op = text_[position_++];
      const auto right = Term();
      value = op == '+' ? value + right : value - right;
    }
  }

  std::string_view text_;
  const Symbols& symbols_;
  const HostMap& host_;
  const TensorMap& tensors_;
  std::size_t position_ = 0;
};

}  // namespace

class MetaGraphRunner::Impl {
 public:
  Impl(std::filesystem::path models_path, std::shared_ptr<SessionCache> sessions,
       std::shared_ptr<GraphCache> graphs)
      : models_path_(std::move(models_path)),
        sessions_(std::move(sessions)),
        graphs_(std::move(graphs)) {}

  const std::filesystem::path& models_path() const { return models_path_; }
  SessionCache& sessions() const { return *sessions_; }
  GraphCache& graphs() const { return *graphs_; }
  // Runs the graph and extracts a float tensor as result
  Tensor ExecuteGraphWithResult(const metaonnx::MetaGraph& graph, TensorMap tensors,
                                const CancelToken& cancel) {
    callback_handler_ = {};
    run_options_.UnsetTerminate();
    CancelScope scope(cancel, [this] { run_options_.SetTerminate(); });
    try {
      ExecuteGraph(graph, std::move(tensors), ExecutionMode::Result);
      Tensor output = Tensor::Float32({0}, {});
      if (!result_audio_.empty()) {
        const auto total = static_cast<std::int64_t>(result_audio_.size());
        output = Tensor::Float32({total}, std::move(result_audio_));
      } else if (const auto result = host_.find("result"); result != host_.end()) {
        output = GetResultTensor(result->second);
      }
      ClearExecutionState();
      return output;
    } catch (...) {
      ClearExecutionState();
      RethrowAsCancelledIfStopped(scope);
      throw;
    }
  }

  // Runs the graph with callbacks
  void ExecuteGraphWithCallbacks(const metaonnx::MetaGraph& graph,
                                 TensorMap tensors,
                                 CallbackHandler callback_handler,
                                 const CancelToken& cancel) {
    callback_handler_ = std::move(callback_handler);
    run_options_.UnsetTerminate();
    CancelScope scope(cancel, [this] { run_options_.SetTerminate(); });
    try {
      ExecuteGraph(graph, std::move(tensors), ExecutionMode::Callbacks);
      ClearExecutionState();
    } catch (...) {
      ClearExecutionState();
      RethrowAsCancelledIfStopped(scope);
      throw;
    }
  }

 private:
  enum class ExecutionMode { Result, Callbacks };

  // ORT reports a terminated run as a plain failure, so only the token tells
  // a cancel from a genuine error. Returns normally when it was not one.
  void RethrowAsCancelledIfStopped(const CancelScope& scope) {
    if (!scope.cancelled()) return;
    if (debug_) {
      try {
        throw;
      } catch (const std::exception& error) {
        std::cout << "\n=== Cancelled, ORT said: " << error.what() << " ===\n";
      } catch (...) {
      }
    }
    throw Cancelled();
  }

  static Tensor GetResultTensor(const HostValue& value) {
    const auto* tensor = std::get_if<Tensor>(&value);
    if (tensor == nullptr) throw std::runtime_error("Host result is not a tensor");
    return *tensor;
  }

  void ClearExecutionState() {
    tensors_.clear();
    host_.clear();
    result_audio_.clear();
    result_audio_.shrink_to_fit();
    nodes_.clear();
    loops_.clear();
    conditions_.clear();
    callback_handler_ = {};
  }

  void ExecuteGraph(const metaonnx::MetaGraph& graph, TensorMap tensors,
                    ExecutionMode execution_mode) {
    tensors_ = std::move(tensors);
    host_.clear();
    nodes_.clear();
    loops_.clear();
    conditions_.clear();
    ValidateAndPopulateInputs(graph);
    for (const auto& node : graph.nodes()) nodes_[node.id()] = &node;
    for (const auto& loop : graph.loops()) loops_[loop.id()] = &loop;
    for (const auto& condition : graph.conditions())
      conditions_[condition.id()] = &condition;
    for (const auto& item : graph.graph())
      ExecuteGraphItem(item, execution_mode);
  }

  HostValue ConvertScalarLiteral(const metaonnx::ScalarLiteral& literal) const {
    switch (literal.value_case()) {
      case metaonnx::ScalarLiteral::kI64: return literal.i64();
      case metaonnx::ScalarLiteral::kF32:
        return static_cast<double>(literal.f32());
      case metaonnx::ScalarLiteral::kB: return literal.b();
      case metaonnx::ScalarLiteral::kS: return literal.s();
      default: throw std::runtime_error("ScalarLiteral missing value");
    }
  }

  HostValue ResolveValueReference(const metaonnx::ValueRef& reference) const {
    switch (reference.kind_case()) {
      case metaonnx::ValueRef::kHost:
        return GetRequiredEntry(host_, reference.host().name(), "Host var");
      case metaonnx::ValueRef::kTensor: {
        const auto& tensor =
            GetRequiredEntry(tensors_, reference.tensor().name(), "Tensor");
        if (!reference.tensor().has_dim()) return tensor;
        auto dim = reference.tensor().dim();
        if (dim < 0) dim += static_cast<std::int64_t>(tensor.shape().size());
        if (dim < 0 || static_cast<std::size_t>(dim) >= tensor.shape().size()) {
          throw std::runtime_error("Tensor dimension out of range: " +
                                   reference.tensor().name());
        }
        return tensor.shape()[static_cast<std::size_t>(dim)];
      }
      case metaonnx::ValueRef::kLiteral:
        return ConvertScalarLiteral(reference.literal());
      default: throw std::runtime_error("ValueRef.kind not set");
    }
  }

  Tensor CreateTensor(const metaonnx::TensorCreate& create,
                      const ExpressionParser::Symbols& symbols = {}) const {
    std::vector<std::int64_t> shape;
    for (const auto& dim : create.dims()) {
      shape.push_back(dim.type() == metaonnx::DimEntry::DIM_STATIC
                          ? static_cast<std::int64_t>(dim.size())
                          : ExpressionParser(dim.expression(), symbols, host_, tensors_)
                                .Parse());
    }
    if (shape.empty()) shape.push_back(1);
    if (create.dtype() == "int64") {
      std::int64_t fill = 0;
      if (create.fill() == metaonnx::TensorCreate::ONES) fill = 1;
      if (create.fill() == metaonnx::TensorCreate::VALUE) {
        if (!create.has_value())
          throw std::runtime_error("TensorCreate.VALUE requires value");
        fill = HostValueAsInt64(ConvertScalarLiteral(create.value()));
      } else if (create.fill() != metaonnx::TensorCreate::ZEROS &&
                 create.fill() != metaonnx::TensorCreate::ONES) {
        throw std::runtime_error("Unknown TensorCreate fill");
      }
      return Tensor::FullInt64(std::move(shape), fill);
    }
    if (create.dtype() == "float32") {
      float fill = 0.0F;
      if (create.fill() == metaonnx::TensorCreate::ONES) fill = 1.0F;
      if (create.fill() == metaonnx::TensorCreate::VALUE) {
        if (!create.has_value())
          throw std::runtime_error("TensorCreate.VALUE requires value");
        fill = static_cast<float>(
            HostValueAsDouble(ConvertScalarLiteral(create.value())));
      } else if (create.fill() != metaonnx::TensorCreate::ZEROS &&
                 create.fill() != metaonnx::TensorCreate::ONES) {
        throw std::runtime_error("Unknown TensorCreate fill");
      }
      return Tensor::FullFloat32(std::move(shape), fill);
    }
    throw std::runtime_error("Unsupported tensor dtype: " + create.dtype());
  }

  void ValidateAndPopulateInputs(const metaonnx::MetaGraph& graph) {
    struct SeenDim {
      std::string tensor;
      std::size_t dim;
      std::int64_t size;
    };
    std::unordered_map<std::string, SeenDim> symbolic;
    std::vector<std::string> errors;
    for (const auto& input : graph.inputs()) {
      const auto found = tensors_.find(input.tensor_name());
      if (found == tensors_.end()) {
        if (!input.is_optional())
          errors.push_back("Missing required input '" + input.tensor_name() + "'");
        continue;
      }
      const auto& tensor = found->second;
      if (input.dtype() != DataTypeName(tensor.dtype())) {
        errors.push_back("Input '" + input.tensor_name() +
                         "' has wrong dtype: expected '" + input.dtype() +
                         "', got '" + DataTypeName(tensor.dtype()) + "'");
      }
      if (tensor.shape().size() != static_cast<std::size_t>(input.dims_size())) {
        errors.push_back("Input '" + input.tensor_name() +
                         "' has wrong number of dimensions");
        continue;
      }
      for (int i = 0; i < input.dims_size(); ++i) {
        const auto& dim = input.dims(i);
        const auto actual = tensor.shape()[static_cast<std::size_t>(i)];
        if (dim.type() == metaonnx::DimEntry::DIM_STATIC &&
            actual != static_cast<std::int64_t>(dim.size())) {
          errors.push_back("Input '" + input.tensor_name() +
                           "' has wrong size at dimension " + std::to_string(i));
        }
        if (dim.type() == metaonnx::DimEntry::DIM_SYMBOLIC) {
          const auto prior = symbolic.find(dim.expression());
          if (prior == symbolic.end()) {
            symbolic.emplace(dim.expression(),
                             SeenDim{input.tensor_name(), static_cast<std::size_t>(i),
                                     actual});
          } else if (prior->second.size != actual) {
            errors.push_back("Input '" + input.tensor_name() + "' dimension " +
                             std::to_string(i) + " must equal input '" +
                             prior->second.tensor + "' dimension " +
                             std::to_string(prior->second.dim));
          }
        }
      }
    }
    if (!errors.empty()) {
      std::ostringstream message;
      message << "Invalid inputs:";
      for (const auto& error : errors) message << "\n  " << error;
      throw std::runtime_error(message.str());
    }
    // A default may reference another input's symbolic dims, including another
    // default's, so fill in rounds until done or a round makes no progress.
    ExpressionParser::Symbols symbols;
    for (const auto& [name, seen] : symbolic) symbols[name] = {seen.tensor, seen.dim};
    std::vector<const metaonnx::InputBinding*> pending;
    for (const auto& input : graph.inputs()) {
      if (!tensors_.contains(input.tensor_name()) && input.is_optional() &&
          input.has_default_value()) {
        pending.push_back(&input);
      }
    }
    while (!pending.empty()) {
      std::vector<const metaonnx::InputBinding*> unresolved;
      std::vector<std::string> reasons;
      for (const auto* input : pending) {
        try {
          tensors_[input->tensor_name()] = CreateTensor(input->default_value(), symbols);
        } catch (const std::exception& e) {
          unresolved.push_back(input);
          reasons.push_back("'" + input->tensor_name() + "': " + e.what());
          continue;
        }
        for (int i = 0; i < input->dims_size(); ++i) {
          if (input->dims(i).type() != metaonnx::DimEntry::DIM_SYMBOLIC) continue;
          const auto dim = static_cast<std::size_t>(i);
          const auto& bound =
              symbols.try_emplace(input->dims(i).expression(),
                                  ExpressionParser::Symbol{input->tensor_name(), dim})
                  .first->second;
          const auto size = tensors_.at(input->tensor_name()).shape()[dim];
          const auto expected = tensors_.at(bound.tensor).shape()[bound.dim];
          if (size != expected) {
            throw std::runtime_error(
                "Default for '" + input->tensor_name() + "' dimension " + std::to_string(i) +
                " has size " + std::to_string(size) + ", but must equal input '" + bound.tensor +
                "' dimension " + std::to_string(bound.dim) + " (size " +
                std::to_string(expected) + ")");
          }
        }
      }
      if (unresolved.size() == pending.size()) {
        std::ostringstream message;
        message << "Cannot fill optional input defaults:";
        for (const auto& reason : reasons) message << "\n  " << reason;
        throw std::runtime_error(message.str());
      }
      pending = std::move(unresolved);
    }
  }

  // Grows one buffer in place so accumulation stays linear in utterance
  // length. ExecuteGraphWithResult materializes the tensor once at the end.
  void AppendAudioToResult(const Tensor& tensor) {
    result_audio_.reserve(result_audio_.size() + tensor.size());
    if (tensor.dtype() == DType::Float32) {
      result_audio_.insert(result_audio_.end(), tensor.float32_data(),
                           tensor.float32_data() + tensor.size());
    } else {
      for (std::size_t i = 0; i < tensor.size(); ++i)
        result_audio_.push_back(static_cast<float>(tensor.int64_data()[i]));
    }
  }

  CallbackMetadata ReadCallbackMetadata(
      const metaonnx::HostCallback& callback) const {
    CallbackMetadata result;
    for (const auto& entry : callback.metadata()) {
      CallbackMetadataValue value;
      switch (entry.value().value_case()) {
        case metaonnx::ScalarLiteral::kI64:
          value = entry.value().i64();
          break;
        case metaonnx::ScalarLiteral::kF32:
          value = entry.value().f32();
          break;
        case metaonnx::ScalarLiteral::kB:
          value = entry.value().b();
          break;
        case metaonnx::ScalarLiteral::kS:
          value = entry.value().s();
          break;
        default:
          throw std::runtime_error("Callback metadata missing value: " +
                                   entry.key());
      }
      result.insert_or_assign(entry.key(), std::move(value));
    }
    return result;
  }

  static void AppendTensorAsFloat(const Tensor& tensor,
                                  std::vector<float>& output) {
    output.reserve(output.size() + tensor.size());
    if (tensor.dtype() == DType::Float32) {
      output.insert(output.end(), tensor.float32_data(),
                    tensor.float32_data() + tensor.size());
      return;
    }
    for (std::size_t i = 0; i < tensor.size(); ++i)
      output.push_back(static_cast<float>(tensor.int64_data()[i]));
  }

  static void AppendTensorAsInt64(const Tensor& tensor,
                                  std::vector<std::int64_t>& output) {
    output.reserve(output.size() + tensor.size());
    if (tensor.dtype() == DType::Int64) {
      output.insert(output.end(), tensor.int64_data(),
                    tensor.int64_data() + tensor.size());
      return;
    }
    for (std::size_t i = 0; i < tensor.size(); ++i)
      output.push_back(static_cast<std::int64_t>(tensor.float32_data()[i]));
  }

  // The graph names the error in callback_name and puts its message in
  // string-literal args.
  [[noreturn]] static void ThrowGraphError(
      const metaonnx::HostCallback& callback) {
    std::string message = callback.callback_name().empty()
                              ? std::string("Graph error")
                              : callback.callback_name();
    for (const auto& arg : callback.args()) {
      if (arg.has_literal() &&
          arg.literal().value_case() == metaonnx::ScalarLiteral::kS)
        message += ": " + arg.literal().s();
    }
    throw GraphError(message);
  }

  void ExecuteStreamingCallback(const metaonnx::HostCallback& callback) {
    CallbackPacket packet;
    packet.metadata = ReadCallbackMetadata(callback);

    switch (callback.callback_type()) {
      case metaonnx::CB_AUDIO: {
        packet.type = CallbackType::Audio;
        std::vector<float> samples;
        for (const auto& arg : callback.args()) {
          const auto value = ResolveValueReference(arg);
          if (const auto* tensor = std::get_if<Tensor>(&value)) {
            AppendTensorAsFloat(*tensor, samples);
          } else {
            std::cout << "WARNING: Non-tensor argument in audio callback\n";
          }
        }
        packet.payload = std::move(samples);
        break;
      }
      case metaonnx::CB_TRIGGERSAMPLE: {
        packet.type = CallbackType::TriggerSample;
        std::vector<std::int64_t> indices;
        for (const auto& arg : callback.args()) {
          const auto value = ResolveValueReference(arg);
          if (const auto* tensor = std::get_if<Tensor>(&value)) {
            AppendTensorAsInt64(*tensor, indices);
          } else {
            std::cout << "WARNING: Non-tensor argument in trigger sample callback\n";
          }
        }
        packet.payload = std::move(indices);
        break;
      }
      case metaonnx::CB_ERROR:
        ThrowGraphError(callback);
      default:
        std::cout << "WARNING: Callback with type CB_UNSPECIFIED called\n";
        return;
    }

    if (callback_handler_) callback_handler_(std::move(packet));
  }

  void ExecuteResultCallback(const metaonnx::HostCallback& callback) {
    if (callback.callback_type() == metaonnx::CB_ERROR) {
      ThrowGraphError(callback);
    } else if (callback.callback_type() == metaonnx::CB_AUDIO) {
      if (debug_) {
        std::cout << "callback called type=" << callback.callback_type()
                  << " name=" << callback.callback_name()
                  << " args=" << callback.args_size() << '\n';
      }
      for (const auto& arg : callback.args()) {
        const auto value = ResolveValueReference(arg);
        if (const auto* tensor = std::get_if<Tensor>(&value))
          AppendAudioToResult(*tensor);
      }
    } else if (callback.callback_type() == metaonnx::CB_TRIGGERSAMPLE) {
      if (callback.args().empty())
        throw std::runtime_error("Trigger sample callback has no arguments");
      if (debug_) {
        std::cout << "ran callback triggersample\n"
                  << DescribeHostValue(ResolveValueReference(callback.args(0)))
                  << '\n';
      }
    } else {
      std::cout << "WARNING: Callback with type CB_UNSPECIFIED called\n";
      return;
    }
    if (debug_) {
      for (const auto& metadata : callback.metadata())
        std::cout << "Metadata value found: {" << metadata.key() << "}\n";
    }
  }

  void ExecuteHostAction(const metaonnx::HostAction& action,
                         ExecutionMode execution_mode) {
    switch (action.action_case()) {
      case metaonnx::HostAction::kTensorToHost: {
        const auto& item = action.tensor_to_host();
        host_[item.dest_host()] =
            GetRequiredEntry(tensors_, item.src_tensor().name(), "Tensor");
        return;
      }
      case metaonnx::HostAction::kHostToTensor: {
        const auto& item = action.host_to_tensor();
        const auto& value = GetRequiredEntry(host_, item.src_host(), "Host var");
        if (const auto* tensor = std::get_if<Tensor>(&value);
            tensor != nullptr && !tensor->shape().empty()) {
          tensors_[item.dest_tensor().name()] = tensor->DeepCopy();
        } else {
          tensors_[item.dest_tensor().name()] =
              Tensor::Int64({1}, {HostValueAsInt64(value)});
        }
        return;
      }
      case metaonnx::HostAction::kTensorCopy: {
        const auto& item = action.tensor_copy();
        tensors_[item.dest_tensor().name()] =
            GetRequiredEntry(tensors_, item.src_tensor().name(), "Tensor").DeepCopy();
        return;
      }
      case metaonnx::HostAction::kTensorRename: {
        const auto& item = action.tensor_rename();
        tensors_[item.dest_tensor()] =
            GetRequiredEntry(tensors_, item.src_tensor(), "Tensor");
        return;
      }
      case metaonnx::HostAction::kCallback: {
        const auto& callback = action.callback();
        if (execution_mode == ExecutionMode::Callbacks)
          ExecuteStreamingCallback(callback);
        else
          ExecuteResultCallback(callback);
        return;
      }
      case metaonnx::HostAction::kHostBinop: {
        const auto& item = action.host_binop();
        host_[item.dest_host()] =
            ApplyBinaryOp(item.op(), ResolveValueReference(item.left()),
                          ResolveValueReference(item.right()));
        return;
      }
      case metaonnx::HostAction::kTensorCreate:
        tensors_[action.tensor_create().dest_tensor().name()] =
            CreateTensor(action.tensor_create());
        return;
      case metaonnx::HostAction::kHostSet:
        host_[action.host_set().dest_host()] =
            ResolveValueReference(action.host_set().value());
        return;
      case metaonnx::HostAction::kHostRemove:
        host_.erase(action.host_remove().name());
        return;
      default: throw std::runtime_error("HostAction.action not set");
    }
  }

  std::shared_ptr<SessionHandle> GetOrCreateSession(
      const std::filesystem::path& path, metaonnx::DeviceType device) {
    if (device != metaonnx::DEVICE_UNSPECIFIED && device != metaonnx::DEVICE_CPU &&
        device != metaonnx::DEVICE_GPU && device != metaonnx::DEVICE_NPU) {
      throw std::runtime_error("Unknown preferred device");
    }
    if (!std::filesystem::is_regular_file(path))
      throw std::runtime_error("Model not found: " + path.string());
    const bool fresh = sessions_->TryGet(path, static_cast<int>(device)) == nullptr;
    auto handle = sessions_->GetOrCreate(path, static_cast<int>(device));
    if (fresh && (device == metaonnx::DEVICE_GPU || device == metaonnx::DEVICE_NPU)) {
      std::cout << "WARNING: " << path.filename().string() << " requests "
                << metaonnx::DeviceType_Name(device)
                << ", but this build is CPU-only - falling back to CPU\n";
    }
    if (debug_) {
      std::cout << "  session " << path.filename().string() << " ["
                << metaonnx::DeviceType_Name(device)
                << "] -> [CPUExecutionProvider]\n";
    }
    return handle;
  }

  Ort::Value CreateOrtInputTensor(const Tensor& tensor,
                                  const Ort::MemoryInfo& memory) {
    const auto& shape = tensor.shape();
    if (tensor.dtype() == DType::Int64) {
      return Ort::Value::CreateTensor<std::int64_t>(
          memory, const_cast<std::int64_t*>(tensor.int64_data()), tensor.size(),
          shape.data(), shape.size());
    }
    return Ort::Value::CreateTensor<float>(
        memory, const_cast<float*>(tensor.float32_data()), tensor.size(), shape.data(),
        shape.size());
  }

  Tensor CopyOrtOutputTensor(Ort::Value& value) {
    if (!value.IsTensor()) throw std::runtime_error("ONNX output is not a tensor");
    const auto info = value.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    const auto count = info.GetElementCount();
    if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
      const auto* data = value.GetTensorData<std::int64_t>();
      return Tensor::Int64(shape, {data, data + count});
    }
    if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      const auto* data = value.GetTensorData<float>();
      return Tensor::Float32(shape, {data, data + count});
    }
    throw std::runtime_error("Unsupported ONNX output tensor dtype");
  }

  void ExecuteNode(const metaonnx::Node& node, ExecutionMode execution_mode) {
    METAGRAPH_PROFILE_SCOPE_DYNAMIC("ExecuteNode", node.id().c_str(),
                                    node.id().size());
    if (debug_) std::cout << "\n=== Running node " << node.id() << " ===\n";
    for (const auto& action : node.pre_actions())
      ExecuteHostAction(action, execution_mode);

    std::vector<const char*> input_names;
    std::vector<Ort::Value> input_values;
    ExpressionParser::Symbols symbols;
    static const auto memory =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    input_names.reserve(node.inputs_size());
    input_values.reserve(node.inputs_size());
    for (const auto& input : node.inputs()) {
      auto found = tensors_.find(input.tensor_name());
      if (found == tensors_.end()) {
        if (!input.is_optional() || !input.has_default_value()) {
          throw std::runtime_error("Tensor not found: " + input.tensor_name());
        }
        found =
            tensors_.emplace(input.tensor_name(), CreateTensor(input.default_value())).first;
      }
      input_names.push_back(input.input_name().c_str());
      input_values.push_back(CreateOrtInputTensor(found->second, memory));
      for (int i = 0; i < input.dims_size(); ++i) {
        if (input.dims(i).type() == metaonnx::DimEntry::DIM_SYMBOLIC) {
          symbols[input.dims(i).expression()] =
              {input.tensor_name(), static_cast<std::size_t>(i)};
        }
      }
    }

    for (const auto& output : node.outputs()) {
      for (const auto& dim : output.dims()) {
        if (dim.type() != metaonnx::DimEntry::DIM_STATIC) {
          // The Python helper deliberately turns an unresolvable debug-only
          // output shape into None; actual output shapes come from ORT.
          try {
            (void)ExpressionParser(dim.expression(), symbols, host_, tensors_).Parse();
          } catch (const std::exception&) {
          }
        }
      }
    }

    const auto handle =
        GetOrCreateSession(models_path_ / (node.model_path() + ".onnx"),
                           node.preferred_device());
    const auto& output_names = handle->output_name_pointers;
    std::vector<Ort::Value> outputs;
    {
      METAGRAPH_PROFILE_SCOPE("OrtRun");
      outputs = handle->session.Run(
          run_options_, input_names.data(), input_values.data(),
          input_values.size(), output_names.data(), output_names.size());
    }
    const auto bound_count =
        std::min<std::size_t>(outputs.size(), static_cast<std::size_t>(node.outputs_size()));
    for (std::size_t i = 0; i < bound_count; ++i) {
      auto tensor = CopyOrtOutputTensor(outputs[i]);
      if (debug_ && (node.id() != "vocoder_middle" ||
                     node.outputs(static_cast<int>(i)).output_name() == "vocoder_audio")) {
        std::cout << "  " << node.id() << ".output["
                  << node.outputs(static_cast<int>(i)).output_name() << "] actual shape=[";
        for (std::size_t d = 0; d < tensor.shape().size(); ++d) {
          if (d != 0) std::cout << ',';
          std::cout << tensor.shape()[d];
        }
        std::cout << "]\n";
      }
      tensors_[node.outputs(static_cast<int>(i)).output_name()] = std::move(tensor);
    }
    for (const auto& action : node.post_actions())
      ExecuteHostAction(action, execution_mode);
  }

  bool EvaluateCondition(const metaonnx::Condition& condition) const {
    const auto left = ResolveValueReference(condition.left());
    const auto right = ResolveValueReference(condition.right());
    if (std::holds_alternative<std::string>(left) ||
        std::holds_alternative<std::string>(right)) {
      const auto* l = std::get_if<std::string>(&left);
      const auto* r = std::get_if<std::string>(&right);
      if (l == nullptr || r == nullptr)
        throw std::runtime_error("Cannot compare string and numeric values");
      switch (condition.comparison()) {
        case metaonnx::Condition::CMP_EQ: return *l == *r;
        case metaonnx::Condition::CMP_NE: return *l != *r;
        case metaonnx::Condition::CMP_LT: return *l < *r;
        case metaonnx::Condition::CMP_LE: return *l <= *r;
        case metaonnx::Condition::CMP_GT: return *l > *r;
        case metaonnx::Condition::CMP_GE: return *l >= *r;
        default: throw std::runtime_error("Unknown comparison");
      }
    }
    const auto l = HostValueAsDouble(left);
    const auto r = HostValueAsDouble(right);
    switch (condition.comparison()) {
      case metaonnx::Condition::CMP_EQ: return l == r;
      case metaonnx::Condition::CMP_NE: return l != r;
      case metaonnx::Condition::CMP_LT: return l < r;
      case metaonnx::Condition::CMP_LE: return l <= r;
      case metaonnx::Condition::CMP_GT: return l > r;
      case metaonnx::Condition::CMP_GE: return l >= r;
      default: throw std::runtime_error("Unknown comparison");
    }
  }

  void ExecuteLoop(const metaonnx::Loop& loop, ExecutionMode execution_mode) {
    if (debug_) std::cout << "\n=== Loop " << loop.id() << " ===\n";
    host_[loop.iteration_var_name()] = std::int64_t{0};
    const auto* condition =
        GetRequiredEntry(conditions_, loop.condition_id(), "Condition");
    while (HostValueAsInt64(
               GetRequiredEntry(host_, loop.iteration_var_name(), "Host var")) <
               static_cast<std::int64_t>(loop.max_iterations()) &&
           EvaluateCondition(*condition)) {
      for (const auto& item : loop.subgraph())
        ExecuteGraphItem(item, execution_mode);
      host_[loop.iteration_var_name()] =
          HostValueAsInt64(
              GetRequiredEntry(host_, loop.iteration_var_name(), "Host var")) +
          1;
    }
  }

  void ExecuteGraphItem(const metaonnx::GraphItem& item,
                        ExecutionMode execution_mode) {
    switch (item.kind_case()) {
      case metaonnx::GraphItem::kNodeId:
        ExecuteNode(*GetRequiredEntry(nodes_, item.node_id(), "Node"),
                    execution_mode);
        return;
      case metaonnx::GraphItem::kHostAction:
        ExecuteHostAction(item.host_action(), execution_mode);
        return;
      case metaonnx::GraphItem::kLoopId:
        ExecuteLoop(*GetRequiredEntry(loops_, item.loop_id(), "Loop"),
                    execution_mode);
        return;
      case metaonnx::GraphItem::kConditional: {
        const auto& branch = item.conditional();
        const auto* condition =
            GetRequiredEntry(conditions_, branch.condition_id(), "Condition");
        const auto& flow =
            EvaluateCondition(*condition) ? branch.then_flow() : branch.else_flow();
        for (const auto& child : flow)
          ExecuteGraphItem(child, execution_mode);
        return;
      }
      default: throw std::runtime_error("GraphItem has no recognized kind set");
    }
  }

  std::filesystem::path models_path_;
  std::shared_ptr<SessionCache> sessions_;
  std::shared_ptr<GraphCache> graphs_;
  std::vector<float> result_audio_;
  TensorMap tensors_;
  HostMap host_;
  std::unordered_map<std::string, const metaonnx::Node*> nodes_;
  std::unordered_map<std::string, const metaonnx::Loop*> loops_;
  std::unordered_map<std::string, const metaonnx::Condition*> conditions_;
  CallbackHandler callback_handler_;
  // One per runner: ORT keeps the terminate flag set, so cancelling mid-node
  // fails every node after it too.
  Ort::RunOptions run_options_;
  bool debug_ = [] {
    const char* value = std::getenv("METAGRAPH_DEBUG");
    return value != nullptr && std::string_view(value) == "1";
  }();
};

Tensor Tensor::Int64(std::vector<std::int64_t> shape,
                     std::vector<std::int64_t> values) {
  if (CalculateElementCount(shape) != values.size())
    throw std::runtime_error("int64 tensor shape/data size mismatch");
  Tensor result;
  result.dtype_ = DType::Int64;
  result.shape_ = std::move(shape);
  result.storage_ = std::make_shared<std::vector<std::int64_t>>(std::move(values));
  return result;
}

Tensor Tensor::Float32(std::vector<std::int64_t> shape,
                       std::vector<float> values) {
  if (CalculateElementCount(shape) != values.size())
    throw std::runtime_error("float32 tensor shape/data size mismatch");
  Tensor result;
  result.dtype_ = DType::Float32;
  result.shape_ = std::move(shape);
  result.storage_ = std::make_shared<std::vector<float>>(std::move(values));
  return result;
}

Tensor Tensor::FullInt64(std::vector<std::int64_t> shape, std::int64_t value) {
  return Int64(shape,
               std::vector<std::int64_t>(CalculateElementCount(shape), value));
}

Tensor Tensor::FullFloat32(std::vector<std::int64_t> shape, float value) {
  return Float32(shape, std::vector<float>(CalculateElementCount(shape), value));
}

std::size_t Tensor::size() const {
  if (dtype_ == DType::Int64) return std::get<IntStorage>(storage_)->size();
  return std::get<FloatStorage>(storage_)->size();
}

const std::int64_t* Tensor::int64_data() const {
  if (dtype_ != DType::Int64) throw std::runtime_error("Tensor is not int64");
  return std::get<IntStorage>(storage_)->data();
}

const float* Tensor::float32_data() const {
  if (dtype_ != DType::Float32) throw std::runtime_error("Tensor is not float32");
  return std::get<FloatStorage>(storage_)->data();
}

std::int64_t* Tensor::mutable_int64_data() {
  if (dtype_ != DType::Int64) throw std::runtime_error("Tensor is not int64");
  return std::get<IntStorage>(storage_)->data();
}

float* Tensor::mutable_float32_data() {
  if (dtype_ != DType::Float32) throw std::runtime_error("Tensor is not float32");
  return std::get<FloatStorage>(storage_)->data();
}

Tensor Tensor::DeepCopy() const {
  if (dtype_ == DType::Int64)
    return Int64(shape_, {int64_data(), int64_data() + size()});
  return Float32(shape_, {float32_data(), float32_data() + size()});
}

MetaGraphRunner::MetaGraphRunner(std::filesystem::path models_path)
    : MetaGraphRunner(std::move(models_path), std::make_shared<SessionCache>(),
                      std::make_shared<GraphCache>()) {}

MetaGraphRunner::MetaGraphRunner(std::filesystem::path models_path,
                                 std::shared_ptr<SessionCache> sessions,
                                 std::shared_ptr<GraphCache> graphs)
    : impl_(std::make_unique<Impl>(std::move(models_path), std::move(sessions),
                                   std::move(graphs))) {}

MetaGraphRunner::~MetaGraphRunner() = default;
MetaGraphRunner::MetaGraphRunner(MetaGraphRunner&&) noexcept = default;
MetaGraphRunner& MetaGraphRunner::operator=(MetaGraphRunner&&) noexcept = default;

Tensor MetaGraphRunner::ExecuteGraphWithResult(
    const std::filesystem::path& graph_path, TensorMap input_tensors,
    const CancelToken& cancel) {
  const auto graph = impl_->graphs().Load(graph_path);
  return impl_->ExecuteGraphWithResult(*graph, std::move(input_tensors), cancel);
}

void MetaGraphRunner::ExecuteGraphWithCallbacks(
    const std::filesystem::path& graph_path, TensorMap input_tensors,
    CallbackHandler callback_handler, const CancelToken& cancel) {
  const auto graph = impl_->graphs().Load(graph_path);
  impl_->ExecuteGraphWithCallbacks(
      *graph, std::move(input_tensors), std::move(callback_handler), cancel);
}

void MetaGraphRunner::PreloadGraph(const std::filesystem::path& graph_path,
                                   const PreloadProgress& on_progress,
                                   const CancelToken& cancel) {
  const auto graph = impl_->graphs().Load(graph_path);
  const auto order = PreloadOrder(*graph);
  std::size_t loaded = 0;
  for (const auto* node : order) {
    ThrowIfCancelled(cancel);
    const auto path = impl_->models_path() / (node->model_path() + ".onnx");
    if (std::filesystem::is_regular_file(path))
      impl_->sessions().GetOrCreate(path, static_cast<int>(node->preferred_device()));
    if (on_progress && !on_progress(++loaded, order.size())) throw Cancelled();
  }
}

}  // namespace metagraph
