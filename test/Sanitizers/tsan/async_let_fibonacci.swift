// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency %import-libdispatch -parse-as-library -sanitize=thread)

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch
// REQUIRES: tsan_runtime
// UNSUPPORTED: use_os_stdlib

// REQUIRES: radar76446550

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
func fib(_ n: Int) -> Int {
  var first = 0
  var second = 1
  for _ in 0..<n {
    let temp = first
    first = second
    second = temp + first
  }
  return first
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
func asyncFib(_ n: Int) async -> Int {
  if n == 0 || n == 1 {
    return n
  }

  async let first = await asyncFib(n-2)
  async let second = await asyncFib(n-1)

  // Sleep a random amount of time waiting on the result producing a result.
  await Task.sleep(UInt64.random(in: 0..<100) * 1000)

  let result = await first + second

  // Sleep a random amount of time before producing a result.
  await Task.sleep(UInt64.random(in: 0..<100) * 1000)

  return result
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
func runFibonacci(_ n: Int) async {
  let result = await asyncFib(n)

  print()
  print("Async fib = \(result), sequential fib = \(fib(n))")
  assert(result == fib(n))
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
@main struct Main {
  static func main() async {
    await runFibonacci(10)
  }
}
