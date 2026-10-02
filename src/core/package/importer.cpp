// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/package/importer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <zip.h>
#include <zlib.h>

namespace thespeon {
namespace {

namespace fs = std::filesystem;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::uint32_t FileCrc32(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<char, 65536> buffer{};
  if (!input) throw std::runtime_error("Could not read " + path.string());
  uLong crc = crc32(0, Z_NULL, 0);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()),
                  static_cast<uInt>(count));
    }
  }
  if (!input.eof()) throw std::runtime_error("Could not read " + path.string());
  return static_cast<std::uint32_t>(crc);
}

class ZipArchive {
 public:
  explicit ZipArchive(const fs::path& path) {
    const auto value = path.string();
    int code = 0;
    archive_ = zip_open(value.c_str(), ZIP_RDONLY, &code);
    if (archive_ != nullptr) return;

    zip_error_t error;
    zip_error_init_with_code(&error, code);
    const std::string reason = zip_error_strerror(&error);
    zip_error_fini(&error);
    throw std::runtime_error("Cannot open " + value +
                             " as a ZIP archive: " + reason);
  }

  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;
  ~ZipArchive() { zip_close(archive_); }

  zip_t* get() const { return archive_; }

 private:
  zip_t* archive_ = nullptr;
};

class ZipEntry {
 public:
  ZipEntry(zip_t* archive, zip_uint64_t index)
      : entry_(zip_fopen_index(archive, index, 0)) {}
  ZipEntry(const ZipEntry&) = delete;
  ZipEntry& operator=(const ZipEntry&) = delete;
  ~ZipEntry() {
    if (entry_ != nullptr) zip_fclose(entry_);
  }

  zip_file_t* get() const { return entry_; }

 private:
  zip_file_t* entry_ = nullptr;
};

void ExtractEntry(zip_t* archive, zip_uint64_t index, const fs::path& target) {
  ZipEntry entry(archive, index);
  if (entry.get() == nullptr) throw std::runtime_error(zip_strerror(archive));
  std::ofstream output(target, std::ios::binary);
  if (!output)
    throw std::runtime_error("Could not write " + target.string());

  std::array<char, 65536> buffer{};
  for (;;) {
    const auto count = zip_fread(entry.get(), buffer.data(), buffer.size());
    if (count < 0) throw std::runtime_error(zip_file_strerror(entry.get()));
    if (count == 0) break;
    output.write(buffer.data(), static_cast<std::streamsize>(count));
    if (!output)
      throw std::runtime_error("Could not write " + target.string());
  }
  output.close();
  if (!output)
    throw std::runtime_error("Could not write " + target.string());
}

std::vector<std::string> SplitArchivePath(std::string name) {
  std::replace(name.begin(), name.end(), '\\', '/');
  if (name.empty() || name.front() == '/' ||
      name.find(':') != std::string::npos)
    throw std::runtime_error("Unsafe path in model pack: " + name);

  std::vector<std::string> result;
  std::stringstream input(name);
  std::string part;
  while (std::getline(input, part, '/')) {
    if (part == "..")
      throw std::runtime_error("Unsafe path in model pack: " + name);
    if (!part.empty() && part != ".") result.push_back(part);
  }
  return result;
}

}  // namespace

Importer::Importer(std::filesystem::path data_directory)
    : data_directory_(std::move(data_directory)) {}

ImportResult Importer::Import(const std::filesystem::path& pack) const {
  if (ToLower(pack.extension().string()) != ".lingotion")
    throw std::runtime_error("Model packs must use the .lingotion extension");
  if (!fs::is_regular_file(pack))
    throw std::runtime_error("Model pack does not exist: " + pack.string());

  std::error_code error;
  fs::create_directories(data_directory_ / "configs", error);
  if (error)
    throw std::runtime_error("Cannot create model directory: " +
                             error.message());
  fs::create_directories(data_directory_ / "binaries", error);
  if (error)
    throw std::runtime_error("Cannot create model directory: " +
                             error.message());

  ZipArchive archive(pack);
  ImportResult result;
  std::set<fs::path> seen;
  std::size_t descriptors = 0;
  const auto count = zip_get_num_entries(archive.get(), 0);
  for (zip_int64_t entry_number = 0; entry_number < count; ++entry_number) {
    zip_stat_t stat{};
    const auto index = static_cast<zip_uint64_t>(entry_number);
    if (zip_stat_index(archive.get(), index, 0, &stat) != 0 ||
        (stat.valid & (ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_CRC)) !=
            (ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_CRC))
      throw std::runtime_error("Cannot inspect model pack entry");

    const std::string entry_name = stat.name;
    const auto parts = SplitArchivePath(entry_name);
    if ((!entry_name.empty() && entry_name.back() == '/') || parts.size() < 2)
      continue;

    const auto& section = parts[parts.size() - 2];
    const fs::path name(parts.back());
    if (name.filename() != name)
      throw std::runtime_error("Unsafe archive filename");
    const auto extension = ToLower(name.extension().string());
    const bool config = section == "configs" && extension == ".json";
    const bool binary =
        section == "binaries" &&
        (extension == ".json" || extension == ".onnx" ||
         extension == ".metagraph");
    if (!config && !binary) continue;
    if (config) ++descriptors;

    const auto destination = data_directory_ / section / name;
    if (!seen.insert(destination).second)
      throw std::runtime_error("Duplicate archive entry for " +
                               destination.string());
    if (fs::exists(destination)) {
      if (fs::file_size(destination) != stat.size ||
          FileCrc32(destination) != stat.crc)
        throw std::runtime_error("Installed file conflicts with " +
                                 name.string());
      ++result.unchanged;
      continue;
    }

    auto temporary = destination;
    temporary += ".importing";
    fs::remove(temporary, error);
    try {
      ExtractEntry(archive.get(), index, temporary);
    } catch (const std::exception& exception) {
      fs::remove(temporary, error);
      throw std::runtime_error("Failed to extract " + entry_name + ": " +
                               exception.what());
    }
    fs::rename(temporary, destination, error);
    if (error) {
      fs::remove(temporary, error);
      throw std::runtime_error("Failed to install " + destination.string());
    }
    ++result.written;
  }

  if (descriptors == 0)
    throw std::runtime_error(
        "The pack contains no configs/*.json descriptors");
  return result;
}

ImportResult ImportPack(const std::filesystem::path& pack,
                        const std::filesystem::path& data_directory) {
  return Importer(data_directory).Import(pack);
}

}  // namespace thespeon
