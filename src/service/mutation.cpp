#include "mutation.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace winlvm::service {
namespace {
std::mutex lock_mutex;
std::unordered_map<std::string, std::weak_ptr<void>> locks;
}  // namespace

MutationLock::MutationLock(MutationLock&& other) noexcept : state_(std::move(other.state_)) {}
MutationLock& MutationLock::operator=(MutationLock&& other) noexcept {
  state_ = std::move(other.state_);
  return *this;
}
MutationLock::~MutationLock() = default;

MutationLock MutationLock::acquire(std::string_view lv_id, MutationError& error) {
  if (lv_id.empty()) {
    error = MutationError::InvalidGeometry;
    return {};
  }
  std::scoped_lock guard(lock_mutex);
  const std::string key(lv_id);
  if (const auto it = locks.find(key); it != locks.end() && !it->second.expired()) {
    error = MutationError::Busy;
    return {};
  }
  auto token = std::shared_ptr<void>(new std::string(key), [](void* value) {
    auto* owned_key = static_cast<std::string*>(value);
    {
      std::scoped_lock erase_guard(lock_mutex);
      if (const auto it = locks.find(*owned_key); it != locks.end() && it->second.expired()) {
        locks.erase(it);
      }
    }
    delete owned_key;
  });
  locks[key] = token;
  error = MutationError::Ok;
  return MutationLock(std::move(token));
}

MutationError validate_geometry(MutationKind kind, Geometry geometry) {
  if (geometry.sector_size == 0 || geometry.current_bytes == 0 || geometry.requested_bytes == 0 ||
      geometry.current_bytes % geometry.sector_size != 0 ||
      geometry.requested_bytes % geometry.sector_size != 0 ||
      geometry.used_bytes > geometry.current_bytes) {
    return MutationError::InvalidGeometry;
  }
  if (kind == MutationKind::Shrink &&
      (geometry.requested_bytes >= geometry.current_bytes ||
       geometry.requested_bytes < geometry.used_bytes)) {
    return MutationError::InvalidGeometry;
  }
  if (kind == MutationKind::Extend && geometry.requested_bytes <= geometry.current_bytes) {
    return MutationError::InvalidGeometry;
  }
  return MutationError::Ok;
}

MutationError mutate(MutationKind kind, std::string_view lv_id, Geometry geometry,
                     bool backend_available) {
  const auto geometry_error = validate_geometry(kind, geometry);
  if (geometry_error != MutationError::Ok) return geometry_error;
  if (!backend_available) return MutationError::Unsupported;
  MutationError lock_error = MutationError::Ok;
  const auto lock = MutationLock::acquire(lv_id, lock_error);
  if (!lock.owns_lock()) return lock_error;
  return MutationError::Unsupported;
}

std::string_view mutation_error_name(MutationError error) {
  switch (error) {
    case MutationError::Ok:
      return "ok";
    case MutationError::Busy:
      return "busy";
    case MutationError::InvalidGeometry:
      return "invalid-geometry";
    case MutationError::Unsupported:
      return "unsupported";
  }
  return "unknown-error";
}

}  // namespace winlvm::service
