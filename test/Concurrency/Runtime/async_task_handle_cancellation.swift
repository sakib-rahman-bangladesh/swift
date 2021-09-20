// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

@available(SwiftStdlib 5.5, *)
@main struct Main {
  static func main() async {
    let handle = detach {
      while (!Task.isCancelled) { // no need for await here, yay
        print("waiting")
      }

      print("done")
    }

    handle.cancel()

    // CHECK: done
    await handle.get()
  }
}
