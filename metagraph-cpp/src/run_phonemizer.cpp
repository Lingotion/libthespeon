#include "metagraph_runner.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

metagraph::TensorMap PhonemizerInputs() {
  const std::vector<std::int64_t> source = {
      1, 40, 38, 43, 2, 0, 0,
      1, 25, 18, 29, 32, 26, 2,
  };

  metagraph::TensorMap inputs;
  inputs["src"] = metagraph::Tensor::Int64({2, 7}, source);
  inputs["tgt.in"] = metagraph::Tensor::FullInt64({2, 1}, 1);
  inputs["mask_tensor.in"] = metagraph::Tensor::FullInt64({2, 1}, 1);
  inputs["finished_indices.in"] = metagraph::Tensor::FullInt64({2}, 0);
  return inputs;
}

void PrintTensor(const metagraph::Tensor& tensor) {
  std::cout << "shape=[";
  for (std::size_t i = 0; i < tensor.shape().size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << tensor.shape()[i];
  }
  std::cout << "] dtype="
            << (tensor.dtype() == metagraph::DType::Int64 ? "int64" : "float32")
            << "\n[";
  for (std::size_t i = 0; i < tensor.size(); ++i) {
    if (i != 0) std::cout << ", ";
    if (tensor.dtype() == metagraph::DType::Int64)
      std::cout << tensor.int64_data()[i];
    else
      std::cout << tensor.float32_data()[i];
  }
  std::cout << "]\n";
}

struct Arguments {
  std::filesystem::path graph;
  std::filesystem::path models;
};

Arguments ParseArguments(int argc, char** argv) {
  Arguments arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if ((option == "-g" || option == "--graph_file_path") && i + 1 < argc) {
      arguments.graph = argv[++i];
    } else if ((option == "-m" || option == "--model_files_path") &&
               i + 1 < argc) {
      arguments.models = argv[++i];
    } else if (option == "-h" || option == "--help") {
      std::cout << "Usage: run_phonemizer -g GRAPH -m MODEL_DIRECTORY\n";
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + option);
    }
  }
  if (arguments.graph.empty() || arguments.models.empty())
    throw std::runtime_error("Both -g GRAPH and -m MODEL_DIRECTORY are required");
  return arguments;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    metagraph::MetaGraphRunner runner(arguments.models);
    const auto result =
        runner.ExecuteGraphWithResult(arguments.graph, PhonemizerInputs());
    PrintTensor(result);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
  }
  return 1;
}
