//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
import _Concurrency

// ==== Any Actor -------------------------------------------------------------

/// Shared "base" protocol for both (local) `Actor` and (potentially remote)
/// `DistributedActor`.
///
/// FIXME(distributed): We'd need Actor to also conform to this, but don't want to add that conformance in _Concurrency yet.
@_marker
@available(SwiftStdlib 5.5, *)
public protocol AnyActor: Sendable, AnyObject {}

// ==== Distributed Actor -----------------------------------------------------

/// Common protocol to which all distributed actors conform implicitly.
///
/// It is not possible to conform to this protocol manually explicitly.
/// Only a 'distributed actor' declaration or protocol with 'DistributedActor'
/// requirement may conform to this protocol.
///
/// The 'DistributedActor' protocol provides the core functionality of any
/// distributed actor.
@available(SwiftStdlib 5.5, *)
public protocol DistributedActor:
    AnyActor, Identifiable, Hashable, Codable {
    /// Resolves the passed in `identity` against the `transport`, returning
    /// either a local or remote actor reference.
    ///
    /// The transport will be asked to `resolve` the identity and return either
    /// a local instance or request a proxy to be created for this identity.
    ///
    /// A remote distributed actor reference will forward all invocations through
    /// the transport, allowing it to take over the remote messaging with the
    /// remote actor instance.
    ///
    /// - Parameter identity: identity uniquely identifying a, potentially remote, actor in the system
    /// - Parameter transport: `transport` which should be used to resolve the `identity`, and be associated with the returned actor
// FIXME: Partially blocked on SE-309, because then we can store ActorIdentity directly
//        We want to move to accepting a generic or existential identity here
//    static func resolve<Identity>(_ identity: Identity, using transport: ActorTransport)
//      throws -> Self where Identity: ActorIdentity
    static func resolve(_ identity: AnyActorIdentity, using transport: ActorTransport)
      throws -> Self

    /// The `ActorTransport` associated with this actor.
    /// It is immutable and equal to the transport passed in the local/resolve
    /// initializer.
    ///
    /// Conformance to this requirement is synthesized automatically for any
    /// `distributed actor` declaration.
    nonisolated var actorTransport: ActorTransport { get } // TODO: rename to `transport`?

    /// Logical identity of this distributed actor.
    ///
    /// Many distributed actor references may be pointing at, logically, the same actor.
    /// For example, calling `resolve(address:using:)` multiple times, is not guaranteed
    /// to return the same exact resolved actor instance, however all the references would
    /// represent logically references to the same distributed actor, e.g. on a different node.
    ///
    /// An address is always uniquely pointing at a specific actor instance.
    ///
    /// Conformance to this requirement is synthesized automatically for any
    /// `distributed actor` declaration.
    nonisolated var id: AnyActorIdentity { get }
}

// ==== Hashable conformance ---------------------------------------------------

@available(SwiftStdlib 5.5, *)
extension DistributedActor {
  nonisolated public func hash(into hasher: inout Hasher) {
    self.id.hash(into: &hasher)
  }

  nonisolated public static func == (lhs: Self, rhs: Self) -> Bool {
    lhs.id == rhs.id
  }
}

// ==== Codable conformance ----------------------------------------------------

extension CodingUserInfoKey {
  @available(SwiftStdlib 5.5, *)
  public static let actorTransportKey = CodingUserInfoKey(rawValue: "$dist_act_transport")!
}

@available(SwiftStdlib 5.5, *)
extension DistributedActor {
  nonisolated public init(from decoder: Decoder) throws {
    guard let transport = decoder.userInfo[.actorTransportKey] as? ActorTransport else {
      throw DistributedActorCodingError(message:
        "Missing ActorTransport (for key .actorTransportKey) " +
        "in Decoder.userInfo, while decoding \(Self.self).")
    }

    let id: AnyActorIdentity = try transport.decodeIdentity(from: decoder)
    self = try Self.resolve(id, using: transport)
  }

  nonisolated public func encode(to encoder: Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.id)
  }
}

/******************************************************************************/
/***************************** Actor Identity *********************************/
/******************************************************************************/

/// Uniquely identifies a distributed actor, and enables sending messages and identifying remote actors.
@available(SwiftStdlib 5.5, *)
public protocol ActorIdentity: Sendable, Hashable, Codable {}

@available(SwiftStdlib 5.5, *)
public struct AnyActorIdentity: ActorIdentity, @unchecked Sendable, CustomStringConvertible {
  public let underlying: Any
  @usableFromInline let _hashInto: (inout Hasher) -> ()
  @usableFromInline let _equalTo: (Any) -> Bool
  @usableFromInline let _encodeTo: (Encoder) throws -> ()
  @usableFromInline let _description: () -> String

  public init<ID>(_ identity: ID) where ID: ActorIdentity {
    self.underlying = identity
    _hashInto = { hasher in identity
        .hash(into: &hasher)
    }
    _equalTo = { other in
      guard let otherAnyIdentity = other as? AnyActorIdentity else {
        return false
      }
      guard let rhs = otherAnyIdentity.underlying as? ID else {
        return false
      }
      return identity == rhs
    }
    _encodeTo = { encoder in
      try identity.encode(to: encoder)
    }
    _description = { () in
      "\(identity)"
    }
  }

  public init(from decoder: Decoder) throws {
    let userInfoTransport = decoder.userInfo[.actorTransportKey]
    guard let transport = userInfoTransport as? ActorTransport else {
      throw DistributedActorCodingError(message:
          "ActorTransport not available under the decoder.userInfo")
    }

    self = try transport.decodeIdentity(from: decoder)
  }

  public func encode(to encoder: Encoder) throws {
    try _encodeTo(encoder)
  }

  public var description: String {
    "\(Self.self)(\(self._description()))"
  }

  public func hash(into hasher: inout Hasher) {
    _hashInto(&hasher)
  }

  public static func == (lhs: AnyActorIdentity, rhs: AnyActorIdentity) -> Bool {
    lhs._equalTo(rhs)
  }
}

/******************************************************************************/
/******************************** Misc ****************************************/
/******************************************************************************/

/// Error protocol to which errors thrown by any `ActorTransport` should conform.
@available(SwiftStdlib 5.5, *)
public protocol ActorTransportError: Error {
}

@available(SwiftStdlib 5.5, *)
public struct DistributedActorCodingError: ActorTransportError {
  public let message: String

  public init(message: String) {
    self.message = message
  }

  public static func missingTransportUserInfo<Act>(_ actorType: Act.Type) -> Self
      where Act: DistributedActor {
    .init(message: "Missing ActorTransport userInfo while decoding")
  }
}

/******************************************************************************/
/************************* Runtime Functions **********************************/
/******************************************************************************/

// ==== isRemote / isLocal -----------------------------------------------------

@_silgen_name("swift_distributed_actor_is_remote")
func __isRemoteActor(_ actor: AnyObject) -> Bool

func __isLocalActor(_ actor: AnyObject) -> Bool {
    return !__isRemoteActor(actor)
}

// ==== Proxy Actor lifecycle --------------------------------------------------

@_silgen_name("swift_distributedActor_remote_initialize")
func _distributedActorRemoteInitialize(_ actorType: Builtin.RawPointer) -> Any

/// Called to destroy the default actor instance in an actor.
/// The implementation will call this within the actor's deinit.
///
/// This will call `actorTransport.resignIdentity(self.id)`.
@_silgen_name("swift_distributedActor_destroy")
func _distributedActorDestroy(_ actor: AnyObject)
