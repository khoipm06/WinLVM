#ifndef WINLVM_SERVICE_MUTATION_H
#define WINLVM_SERVICE_MUTATION_H

#include <cstdint>
#include <memory>
#include <string_view>

namespace winlvm::service {

enum class MutationError : std::uint32_t {
  Ok = 0,
  Busy = 1,
  InvalidGeometry = 2,
  Unsupported = 3
};
enum class MutationKind : std::uint32_t { Shrink = 1, Extend = 2 };

struct Geometry {
  std::uint64_t current_bytes;
  std::uint64_t requested_bytes;
  std::uint64_t used_bytes;
  std::uint32_t sector_size;
};

class MutationLock {
 public:
  MutationLock() = default;
  MutationLock(const MutationLock&) = delete;
  MutationLock& operator=(const MutationLock&) = delete;
  MutationLock(MutationLock&&) noexcept;
  MutationLock& operator=(MutationLock&&) noexcept;
  ~MutationLock();
  [[nodiscard]] static MutationLock acquire(std::string_view lv_id, MutationError& error);
  [[nodiscard]] bool owns_lock() const noexcept { return state_ != nullptr; }

 private:
  explicit MutationLock(std::shared_ptr<void> state) : state_(std::move(state)) {}
  std::shared_ptr<void> state_;
};

[[nodiscard]] MutationError validate_geometry(MutationKind kind, Geometry geometry);
[[nodiscard]] MutationError mutate(MutationKind kind, std::string_view lv_id, Geometry geometry,
                                   bool backend_available);
[[nodiscard]] std::string_view mutation_error_name(MutationError error);

}  // namespace winlvm::service
#endif
