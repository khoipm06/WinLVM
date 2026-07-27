// A minimal test framework.
//
// Deliberately not bare assert(): assert() compiles out under NDEBUG, so a
// release test run would pass while checking nothing. Failures here throw, so
// one bad case aborts its own test and the runner continues.
#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Failure {
  std::string message;
};

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, std::function<void()> fn);
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);
void note(const std::string& message);
int run_all();

template <typename A, typename B>
std::string describe(const char* expr_a, const char* expr_b, const A& a, const B& b) {
  std::ostringstream os;
  os << expr_a << " == " << expr_b << "\n      left:  " << a << "\n      right: " << b;
  return os.str();
}

}  // namespace testing

#define TEST(name)                                                    \
  static void name();                                                 \
  static ::testing::Registrar registrar_##name(#name, name);          \
  static void name()

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
  } while (0)

#define CHECK_MSG(cond, msg)                                          \
  do {                                                                \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, std::string("CHECK failed: " #cond " — ") + (msg)); \
  } while (0)

#define CHECK_EQ(a, b)                                                \
  do {                                                                \
    if (!((a) == (b)))                                                \
      ::testing::fail(__FILE__, __LINE__, ::testing::describe(#a, #b, (a), (b))); \
  } while (0)
