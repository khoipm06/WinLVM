#include "winlvm/hash.h"

#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#endif

namespace winlvm {

namespace {

std::string bytes_to_hex(const std::uint8_t* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

Status ntstatus_to_status(long status, const char* action) {
  if (status >= 0) {
    return Status::success();
  }
  std::ostringstream out;
  out << action << " failed with NTSTATUS 0x" << std::hex << static_cast<unsigned long>(status);
  return Status::failure(out.str());
}

}  // namespace

Sha256Hasher::Sha256Hasher() {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status < 0) {
    return;
  }
  algorithm_ = algorithm;

  DWORD object_length = 0;
  DWORD returned = 0;
  status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
                             sizeof(object_length), &returned, 0);
  if (status < 0) {
    destroy();
    return;
  }

  object_.assign(object_length, 0);
  BCRYPT_HASH_HANDLE hash = nullptr;
  status = BCryptCreateHash(algorithm, &hash, object_.data(), object_length, nullptr, 0, 0);
  if (status < 0) {
    destroy();
    return;
  }
  hash_ = hash;
#endif
}

Sha256Hasher::Sha256Hasher(Sha256Hasher&& other) noexcept {
  *this = std::move(other);
}

Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&& other) noexcept {
  if (this != &other) {
    destroy();
    algorithm_ = other.algorithm_;
    hash_ = other.hash_;
    object_ = std::move(other.object_);
    finished_ = other.finished_;
    other.algorithm_ = nullptr;
    other.hash_ = nullptr;
    other.finished_ = true;
  }
  return *this;
}

Sha256Hasher::~Sha256Hasher() {
  destroy();
}

void Sha256Hasher::destroy() {
#ifdef _WIN32
  if (hash_) {
    BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    hash_ = nullptr;
  }
  if (algorithm_) {
    BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
    algorithm_ = nullptr;
  }
#endif
}

Status Sha256Hasher::update(std::span<const char> bytes) {
#ifdef _WIN32
  if (!algorithm_ || !hash_) {
    return Status::failure("SHA-256 provider is not initialized");
  }
  if (finished_) {
    return Status::failure("SHA-256 hash already finalized");
  }
  const NTSTATUS status = BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                                         reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                         static_cast<ULONG>(bytes.size()), 0);
  return ntstatus_to_status(status, "BCryptHashData");
#else
  (void)bytes;
  return Status::failure("SHA-256 hashing is only implemented on Windows");
#endif
}

Result<std::string> Sha256Hasher::finish_hex() {
#ifdef _WIN32
  if (!algorithm_ || !hash_) {
    return Result<std::string>::failure("SHA-256 provider is not initialized");
  }
  if (finished_) {
    return Result<std::string>::failure("SHA-256 hash already finalized");
  }
  std::array<std::uint8_t, 32> digest{};
  const NTSTATUS status = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest.data(),
                                           static_cast<ULONG>(digest.size()), 0);
  Status finish_status = ntstatus_to_status(status, "BCryptFinishHash");
  if (!finish_status.ok) {
    return Result<std::string>::failure(finish_status.message);
  }
  finished_ = true;
  return Result<std::string>::success(bytes_to_hex(digest.data(), digest.size()));
#else
  return Result<std::string>::failure("SHA-256 hashing is only implemented on Windows");
#endif
}

}  // namespace winlvm
