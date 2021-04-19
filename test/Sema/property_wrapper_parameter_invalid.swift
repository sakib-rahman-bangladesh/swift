// RUN: %target-typecheck-verify-swift -swift-version 5

@propertyWrapper
struct NonMutatingWrapper<T> {
  var wrappedValue: T {
    get { fatalError() }
    nonmutating set { fatalError() }
  }

  init(wrappedValue: T) { fatalError() }
}

@propertyWrapper
struct MutatingWrapper<T> {
  var wrappedValue: T {
    mutating get { fatalError() }
  }

  init(wrappedValue: T) { fatalError() }
}

// expected-error@+1 {{property wrapper applied to parameter must have a nonmutating 'wrappedValue' getter}}
func testMutatingGetter(@MutatingWrapper value: Int) {}

// expected-error@+1 {{property wrapper applied to parameter must have a nonmutating 'wrappedValue' getter}}
func testComposedMutating(@MutatingWrapper @NonMutatingWrapper value: Int) {}

// okay
func testComposedNonMutating(@NonMutatingWrapper @MutatingWrapper value: Int) {}

@propertyWrapper
struct NoProjection<T> {
  var wrappedValue: T
}

// expected-note@+1 {{property wrapper type 'NoProjection<String>' does not support initialization from a projected value}}
func takesNoProjectionWrapper(@NoProjection value: String) {}

func testNoProjection(message: String) {
  takesNoProjectionWrapper(value: message) // okay

  // expected-error@+1 {{cannot use property wrapper projection argument}}
  takesNoProjectionWrapper($value: message)

  // expected-error@+2 {{cannot use property wrapper projection parameter}}
  // expected-note@+1 {{property wrapper type 'NoProjection<Int>' does not support initialization from a projected value}}
  let _: (NoProjection<Int>) -> Int = { $value in
    return value
  }
}

struct Projection<T> {
  var value: T
}

// expected-note@+2 {{generic struct 'Wrapper' is not '@usableFromInline' or public}}
@propertyWrapper
struct Wrapper<T> { // expected-note 3 {{type declared here}}
  // expected-note@+1 {{initializer 'init(wrappedValue:)' is not '@usableFromInline' or public}}
  init(wrappedValue: T) {
    self.wrappedValue = wrappedValue
  }

  init(projectedValue: Projection<T>) {
    self.wrappedValue = projectedValue.value
  }

  var wrappedValue: T
  var projectedValue: Projection<T> { Projection(value: wrappedValue) }
}

// expected-note@+2 {{property wrapper has arguments in the wrapper attribute}}
// expected-note@+1 {{in call to function 'hasWrapperAttributeArg(value:)'}}
func hasWrapperAttributeArg<T>(@Wrapper() value: T) {}

func testWrapperAttributeArg(projection: Projection<Int>) {
  hasWrapperAttributeArg(value: projection.value)

  // expected-error@+2 {{cannot use property wrapper projection argument}}
  // expected-error@+1 {{generic parameter 'T' could not be inferred}}
  hasWrapperAttributeArg($value: projection)
}

struct S {
  // expected-error@+1 {{property wrapper attribute on parameter not yet supported on subscript}}
  subscript(@Wrapper position: Int) -> Int { 0 }
}

func testInvalidArgLabel() {
  // expected-note@+1 2 {{parameter 'argLabel' does not have an attached property wrapper}}
  func noWrappers(argLabel: Int) {}

  // expected-error@+1 {{cannot use property wrapper projection argument}}
  let ref = noWrappers($argLabel:)

  // expected-error@+1 {{cannot use property wrapper projection argument}}
  noWrappers($argLabel: 10)
}

protocol P {
  // expected-error@+1 {{parameter 'arg' declared inside a protocol cannot have a wrapper}}
  func requirement(@Wrapper arg: Int)
}

enum E {
  // expected-error@+1 {{expected parameter name followed by ':'}}
  case one(@Wrapper value: Int)
}

// expected-error@+1 {{function cannot be declared public because its parameter uses an internal API wrapper type}}
public func f1(@Wrapper value: Int) {}

// expected-error@+3 {{generic struct 'Wrapper' is internal and cannot be referenced from an '@inlinable' function}}
// expected-error@+2 {{the parameter API wrapper of a '@usableFromInline' function must be '@usableFromInline' or public}}
// expected-error@+1 {{initializer 'init(wrappedValue:)' is internal and cannot be referenced from an '@inlinable' function}}
@inlinable func f2(@Wrapper value: Int) {}

// expected-error@+1 {{the parameter API wrapper of a '@usableFromInline' function must be '@usableFromInline' or public}}
@usableFromInline func f3(@Wrapper value: Int) {}

@available(*, unavailable)
@propertyWrapper
struct UnavailableWrapper<T> { // expected-note {{'UnavailableWrapper' has been explicitly marked unavailable here}}
  var wrappedValue: T
}

// expected-error@+1 {{'UnavailableWrapper' is unavailable}}
func testUnavailableWrapper(@UnavailableWrapper value: Int) {}

@propertyWrapper
public struct PublicWrapper<T> {
  public init(wrappedValue: T) { fatalError() }
  public init(projectedValue: PublicWrapper<T>) { fatalError() }
  public var wrappedValue: T
  public var projectedValue: PublicWrapper<T> { self }
}

// expected-note@+2 2 {{generic struct 'InternalWrapper' is not '@usableFromInline' or public}}
@propertyWrapper
struct InternalWrapper<T> { // expected-note 3 {{type declared here}}
  var wrappedValue: T

  // expected-note@+1 2 {{initializer 'init(wrappedValue:)' is not '@usableFromInline' or public}}
  init(wrappedValue: T) { self.wrappedValue = wrappedValue }
}

// expected-error@+1 {{function cannot be declared public because its parameter uses an internal API wrapper type}}
public func testComposition1(@PublicWrapper @InternalWrapper value: Int) {}

// Okay because `InternalWrapper` is implementation-detail.
public func testComposition2(@InternalWrapper @PublicWrapper value: Int) {}

// expected-error@+1 {{the parameter API wrapper of a '@usableFromInline' function must be '@usableFromInline' or public}}
@usableFromInline func testComposition3(@PublicWrapper @InternalWrapper value: Int) {}

// Okay because `InternalWrapper` is implementation-detail.
@usableFromInline func testComposition4(@InternalWrapper @PublicWrapper value: Int) {}

// expected-error@+3 {{generic struct 'InternalWrapper' is internal and cannot be referenced from an '@inlinable' function}}
// expected-error@+2 {{the parameter API wrapper of a '@usableFromInline' function must be '@usableFromInline' or public}}
// expected-error@+1 {{initializer 'init(wrappedValue:)' is internal and cannot be referenced from an '@inlinable' function}}
@inlinable func testComposition5(@PublicWrapper @InternalWrapper value: Int) {}

// expected-error@+2 {{generic struct 'InternalWrapper' is internal and cannot be referenced from an '@inlinable' function}}
// expected-error@+1 {{initializer 'init(wrappedValue:)' is internal and cannot be referenced from an '@inlinable' function}}
@inlinable func testComposition6(@InternalWrapper @PublicWrapper value: Int) {}

protocol Q {
  associatedtype A
}

// expected-note@+1 {{where 'T' = 'Int'}}
func takesClosure<T: Q>(type: T.Type, _ closure: (T.A) -> Void) {}

func testMissingWrapperType() {
  // expected-error@+1 {{global function 'takesClosure(type:_:)' requires that 'Int' conform to 'Q'}}
  takesClosure(type: Int.self) { $value in
    return
  }

  struct S: Q {
    typealias A = (Int, Int)
  }

  // expected-error@+1 {{inferred projection type 'S.A' (aka '(Int, Int)') is not a property wrapper}}
  takesClosure(type: S.self) { $value in
    return
  }
}
