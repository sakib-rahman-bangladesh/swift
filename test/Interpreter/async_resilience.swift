// RUN: %empty-directory(%t)

// RUN: %target-build-swift-dylib(%t/%target-library-name(resilient_async)) -enable-library-evolution %S/Inputs/resilient_async.swift -emit-module -emit-module-path %t/resilient_async.swiftmodule -module-name resilient_async
// RUN: %target-codesign %t/%target-library-name(resilient_async)

// RUN: %target-build-swift -parse-as-library %s -lresilient_async -I %t -L %t -o %t/main %target-rpath(%t)
// RUN: %target-codesign %t/main

// Introduce a defaulted protocol method.
// RUN: %target-build-swift-dylib(%t/%target-library-name(resilient_async)) -enable-library-evolution %S/Inputs/resilient_async2.swift -emit-module -emit-module-path %t/resilient_async.swiftmodule -module-name resilient_async
// RUN: %target-codesign %t/%target-library-name(resilient_async)

// RUN: %target-run %t/main %t/%target-library-name(resilient_async)

// REQUIRES: executable_test
// REQUIRES: concurrency


import resilient_async

class Impl : Problem {}

@main struct Main {
  static func main() async {
      let i = Impl()
      // This used to crash.
      let r = await callGenericWitness(i)
      assert(r == 1)
  }
}
