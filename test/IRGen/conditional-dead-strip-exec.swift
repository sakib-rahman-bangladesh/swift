// Tests that with -conditional-runtime-records, LLVM GlobalDCE is able to
// remove unused classes, protocols and protocol conformances.

// RUN: %empty-directory(%t)

// RUN: %target-build-swift -Xfrontend -disable-objc-interop \
// RUN:     -Xfrontend -disable-reflection-metadata -Xfrontend -disable-reflection-names \
// RUN:     -Xfrontend -conditional-runtime-records \
// RUN:     %s -emit-ir -o %t/main.ll

// RUN: %target-clang %t/main.ll -isysroot %sdk -L%swift_obj_root/lib/swift/%target-sdk-name -flto -o %t/main
// RUN: %target-run %t/main | %FileCheck %s

// RUN: %llvm-nm --defined-only %t/main | %FileCheck %s --check-prefix=NM

// REQUIRES: executable_test

// Test disabled until LLVM GlobalDCE supports conditional references.
// REQUIRES: rdar81868900

// (1) used
@inline(never) func func1_used() { print("func1_used") }

// (2) unused
@inline(never) func func2_dead() { print("func2_dead") }

// (3) completely unused
protocol CompletelyUnusedProtocol { }

// (4) implemented only by an unused class (4), thus unused
protocol TheProtocol { }

// (5) unused class
class MyClass: TheProtocol {
	func unused_method() {}
}

// (6) implemented by a used class (8), but unused, thus unused
protocol UnusedProto { }

// (7) implemented and used (8)
protocol ActuallyUsedProto {
	func bark()
}

// (8) used class, but the UnusedProto conformance is unused because the UnusedProto protocol itself is unused
class UsedClass : UnusedProto, ActuallyUsedProto {
	public func bark() { print("UsedClass.bark") }
}

print("Hello!")
func1_used()
let o = UsedClass()
o.bark()
let p: ActuallyUsedProto = UsedClass()
p.bark()

// CHECK: Hello!
// CHECK: func1_used
// CHECK: UsedClass.bark
// CHECK: UsedClass.bark

// In summary, only the following should be kept alive in the binary result:
//   func func1_used() { ... }
//   protocol ActuallyUsedProto { ... }
//   class UsedClass : ActuallyUsedProto /* UnusedProto conformance removed */ { ... }

// (1)
// NM:     $s4main10func1_usedyyF

// (2)
// NM-NOT: $s4main10func2_deadyyF

// (4)
// NM-NOT: $s4main11TheProtocolMp

// (6)
// NM-NOT: $s4main11UnusedProtoMp

// (3)
// NM-NOT: $s4main24CompletelyUnusedProtocolMp

// (5)
// NM-NOT: $s4main7MyClassC13unused_methodyyF
// NM-NOT: $s4main7MyClassCAA11TheProtocolAAMc
// NM-NOT: $s4main7MyClassCAA11TheProtocolAAWP
// NM-NOT: $s4main7MyClassCMf
// NM-NOT: $s4main7MyClassCMn

// (8)
// NM-NOT: $s4main9UsedClassCAA11UnusedProtoAAMc
// NM-NOT: $s4main9UsedClassCAA11UnusedProtoAAWP
