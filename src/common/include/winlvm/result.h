#pragma once

#include <string>
#include <utility>

namespace winlvm {

struct Status {
  bool ok = true;
  std::string message;

  static Status success() { return {}; }
  static Status failure(std::string text) { return {false, std::move(text)}; }
};

template <typename T>
struct Result {
  T value{};
  Status status{};

  explicit operator bool() const { return status.ok; }

  static Result<T> success(T v) {
    Result<T> result;
    result.value = std::move(v);
    result.status = Status::success();
    return result;
  }

  static Result<T> failure(std::string text) {
    Result<T> result;
    result.status = Status::failure(std::move(text));
    return result;
  }
};

}  // namespace winlvm
