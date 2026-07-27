#include "testing.h"

#include <cstdio>
#include <iostream>

namespace testing {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* name, std::function<void()> fn) {
  registry().push_back(TestCase{name, std::move(fn)});
}

void fail(const char* file, int line, const std::string& message) {
  std::ostringstream os;
  os << file << ":" << line << ": " << message;
  throw Failure{os.str()};
}

void note(const std::string& message) {
  std::cout << "      " << message << "\n";
}

int run_all() {
  int passed = 0;
  std::vector<std::string> failures;

  for (const auto& tc : registry()) {
    std::cout << "[ RUN  ] " << tc.name << "\n";
    try {
      tc.fn();
      std::cout << "[  OK  ] " << tc.name << "\n";
      ++passed;
    } catch (const Failure& f) {
      std::cout << "[ FAIL ] " << tc.name << "\n         " << f.message << "\n";
      failures.push_back(tc.name);
    } catch (const std::exception& e) {
      std::cout << "[ FAIL ] " << tc.name << " (exception: " << e.what() << ")\n";
      failures.push_back(tc.name);
    }
  }

  std::cout << "\n"
            << passed << " passed, " << failures.size() << " failed, "
            << registry().size() << " total\n";
  for (const auto& name : failures) std::cout << "  FAILED: " << name << "\n";
  return failures.empty() ? 0 : 1;
}

}  // namespace testing

int main() { return testing::run_all(); }
