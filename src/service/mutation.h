#ifndef WINLVM_SERVICE_MUTATION_H
#define WINLVM_SERVICE_MUTATION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace winlvm::service {

enum class MutationError : std::uint32_t {
  Ok = 0,
  Busy = 1,
  InvalidGeometry = 2,
  Unsupported = 3,
  Failed = 4
};
enum class MutationKind : std::uint32_t { Shrink = 1, Extend = 2 };

struct Geometry {
  std::uint64_t current_bytes = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t used_bytes = 0;
  std::uint32_t sector_size = 512;
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
[[nodiscard]] MutationError execute_mutation(
    MutationKind kind,
    std::string_view lv_id,
    Geometry geometry,
    const std::function<bool(std::string&)>& executor,
    std::string& error_out);
[[nodiscard]] std::string_view mutation_error_name(MutationError error) noexcept;

}  // namespace winlvm::service
#endif
