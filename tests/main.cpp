#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_support.hpp"

int main(int argc, char** argv) {
  // Unbuffered: a hanging test must still show which test it is.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string filter;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--filter" && index + 1 < argc) {
      filter = argv[++index];
    }
  }

  std::vector<rgtest::TestCase>& cases = rgtest::registry();
  if (list_only) {
    for (const rgtest::TestCase& test : cases) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  int executed = 0;
  int failed = 0;
  std::vector<std::string> failures;
  for (const rgtest::TestCase& test : cases) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    std::printf("run  %s\n", full.c_str());
    rgtest::TestContext context(test.suite, test.name);
    try {
      test.function(context);
    } catch (const rgtest::TestAbort& abort) {
      context.check(false, std::string("aborted: ") + abort.what(), __FILE__, __LINE__);
    } catch (const std::exception& error) {
      context.check(false, std::string("unexpected exception: ") + error.what(), __FILE__,
                    __LINE__);
    }
    if (context.failures() == 0) {
      std::printf("ok   %s\n", full.c_str());
    } else {
      ++failed;
      failures.push_back(full);
      std::printf("FAIL %s (%d check failure(s))\n", full.c_str(), context.failures());
    }
  }

  std::printf("\n%d test(s) run, %d failed\n", executed, failed);
  for (const std::string& name : failures) {
    std::printf("  failed: %s\n", name.c_str());
  }
  return failed == 0 ? 0 : 1;
}
