// RUN: %empty-directory(%t)
// RUN: %target-clang %S/Inputs/objc_async.m -c -o %t/objc_async_objc.o
// RUN: %target-build-swift -Xfrontend -enable-experimental-concurrency -parse-as-library -module-name main -import-objc-header %S/Inputs/objc_async.h %s %t/objc_async_objc.o -o %t/objc_async
// RUN: %target-run %t/objc_async | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: objc_interop

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

func buttTest() async {
  let butt = Butt()
  let result = await butt.butt(1738)
  print("finishing \(result)")
}

func farmTest() async {
  let farm = Farm()
  let dogNumber = await farm.doggo
  print("dog number = \(dogNumber)")
  do {
    let _ = try await farm.catto
  } catch {
    print("caught exception")
  }
}

@main struct Main {
  static func main() async {
    // CHECK: starting 1738
    // CHECK-NEXT: finishing 679
    await buttTest()

    // CHECK-NEXT: getting dog
    // CHECK-NEXT: dog number = 123
    // CHECK-NEXT: obtaining cat has failed!
    // CHECK-NEXT: caught exception
    await farmTest()
  }
}


