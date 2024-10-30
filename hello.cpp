#include <liburing.h>
#include <coroutine>
#include <print>

int main() {
  std::coroutine_handle<> _;
  std::print("Hello world.\n");
  return 0;
}
