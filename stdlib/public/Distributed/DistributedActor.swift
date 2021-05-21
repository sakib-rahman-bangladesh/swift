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

/// Common protocol to which all distributed actors conform implicitly.
///
/// It is not possible to conform to this protocol manually explicitly.
/// Only a 'distributed actor' declaration or protocol with 'DistributedActor'
/// requirement may conform to this protocol.
///
/// The 'DistributedActor' protocol provides the core functionality of any
/// distributed actor, which involves transforming actor
/// which involves enqueuing new partial tasks to be executed at some
/// point.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public protocol DistributedActor: Actor, Codable {

    /// Creates new (local) distributed actor instance, bound to the passed transport.
    ///
    /// Upon initialization, the `actorAddress` field is populated by the transport,
    /// with an address assigned to this actor.
    ///
    /// - Parameter transport: the transport this distributed actor instance will
    ///   associated with.
    init(transport: ActorTransport)

    /// Resolves the passed in `address` against the `transport`,
    /// returning either a local or remote actor reference.
    ///
    /// The transport will be asked to `resolve` the address and return either
    /// a local instance or determine that a proxy instance should be created
    /// for this address. A proxy actor will forward all invocations through
    /// the transport, allowing it to take over the remote messaging with the
    /// remote actor instance.
    ///
    /// - Parameter address: the address to resolve, and produce an instance or proxy for.
    /// - Parameter transport: transport which should be used to resolve the `address`.
    init(resolve address: ActorAddress, using transport: ActorTransport) throws

    /// The `ActorTransport` associated with this actor.
    /// It is immutable and equal to the transport passed in the local/resolve
    /// initializer.
    ///
    /// Conformance to this requirement is synthesized automatically for any
    /// `distributed actor` declaration.
    nonisolated var actorTransport: ActorTransport { get }

    /// Logical address which this distributed actor represents.
    ///
    /// An address is always uniquely pointing at a specific actor instance.
    ///
    /// Conformance to this requirement is synthesized automatically for any
    /// `distributed actor` declaration.
    nonisolated var actorAddress: ActorAddress { get }
}

// ==== Codable conformance ----------------------------------------------------

extension CodingUserInfoKey {
    @available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
    static let actorTransportKey = CodingUserInfoKey(rawValue: "$dist_act_trans")!
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension DistributedActor {
    nonisolated public init(from decoder: Decoder) throws {
//        guard let transport = decoder.userInfo[.actorTransportKey] as? ActorTransport else {
//            throw DistributedActorCodingError(message:
//            "ActorTransport not available under the decoder.userInfo")
//        }
//
//        var container = try decoder.singleValueContainer()
//        let address = try container.decode(ActorAddress.self)
//         self = try Self(resolve: address, using: transport) // FIXME: This is going to be solved by the init() work!!!!
        fatalError("\(#function) is not implemented yet for distributed actors'")
    }

    nonisolated public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(self.actorAddress)
    }
}
/******************************************************************************/
/***************************** Actor Transport ********************************/
/******************************************************************************/

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public protocol ActorTransport: Sendable {
    /// Resolve a local or remote actor address to a real actor instance, or throw if unable to.
    /// The returned value is either a local actor or proxy to a remote actor.
    func resolve<Act>(address: ActorAddress, as actorType: Act.Type)
    throws -> ActorResolved<Act> where Act: DistributedActor

    /// Create an `ActorAddress` for the passed actor type.
    ///
    /// This function is invoked by an distributed actor during its initialization,
    /// and the returned address value is stored along with it for the time of its
    /// lifetime.
    ///
    /// The address MUST uniquely identify the actor, and allow resolving it.
    /// E.g. if an actor is created under address `addr1` then immediately invoking
    /// `transport.resolve(address: addr1, as: Greeter.self)` MUST return a reference
    /// to the same actor.
    func assignAddress<Act>(
        _ actorType: Act.Type
    ) -> ActorAddress
        where Act: DistributedActor

    func actorReady<Act>(
        _ actor: Act
    ) where Act: DistributedActor

    /// Called during actor deinit/destroy.
    func resignAddress(
        _ address: ActorAddress
    )

}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public enum ActorResolved<Act: DistributedActor> {
    case resolved(Act)
    case makeProxy
}

/******************************************************************************/
/***************************** Actor Address **********************************/
/******************************************************************************/

/// Uniquely identifies a distributed actor, and enables sending messages even to remote actors.
///
/// ## Identity
/// The address is the source of truth with regards to referring to a _specific_ actor in the system.
/// This is in contrast to an `ActorPath` which can be thought of as paths in a filesystem, however without any uniqueness
/// or identity guarantees about the files those paths point to.
///
/// ## Lifecycle
/// Note, that an ActorAddress is a pure value, and as such does not "participate" in an actors lifecycle;
/// Thus, it may represent an address of an actor that has already terminated, so attempts to locate (resolve)
/// an `ActorRef` for this address may result with a reference to dead letters (meaning, that the actor this address
/// had pointed to does not exist, and most likely is dead / terminated).
///
/// ## Serialization
///
/// An address can be serialized using `Codable` or other serialization mechanisms.
/// When shared over the network or with other processes it must include the origin's
/// system address (e.g. the network address of the host, or process identifier).
///
/// When using `Codable` serialization this is done automatically, by looking up
/// the address of the `ActorTransport` the actor is associated with if was a local
/// instance, or simply carrying the full address if it already was a remote reference.
///
/// ## Format
/// The address consists of the following parts:
///
/// ```
/// |              node                  | path               | incarnation |
///  (  protocol | name? | host | port  ) ( [segments] name )? (  uint32   )
/// ```
///
/// For example: `sact://human-readable-name@127.0.0.1:7337/user/wallet/id-121242`.
/// Note that the `ActorIncarnation` is not printed by default in the String representation of a path, yet may be inspected on demand.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public struct ActorAddress: Codable, Sendable, Equatable, Hashable {
    /// Uniquely specifies the actor transport and the protocol used by it.
    ///
    /// E.g. "xpc", "specific-clustering-protocol" etc.
    public var `protocol`: String

    public var host: String?
    public var port: Int?
    public var nodeID: UInt64?
    public var path: String?

    /// Unique Identifier of this actor.
    public var uid: UInt64 // TODO: should we remove this

    // FIXME: remove this or implement for real; this is just a hack implementation for now
    public init(parse: String) {
        self.protocol = "sact"
        self.host = "xxx"
        self.port = 7337
        self.nodeID = 11
        self.path = "example"
        self.uid = 123123
    }
}

// TODO: naive impl, bring in a real one
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension ActorAddress: CustomStringConvertible {
    public var description: String {
        var result = `protocol`
        result += "://"
        if let host = host {
            result += host
        }
        if let port = port {
            result += ":\(port)"
        }
        // TODO: decide if we'd want to print the nodeID too here.
        if let path = path {
            result += "/\(path)"
        }
        if uid > 0 {
            result += "#\(uid)"
        }
        return result
    }
}

/******************************************************************************/
/******************************** Misc ****************************************/
/******************************************************************************/

/// Error protocol to which errors thrown by any `ActorTransport` should conform.
@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public protocol ActorTransportError: Error {}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
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

/// Called to initialize the distributed-remote actor 'proxy' instance in an actor.
/// The implementation will call this within the actor's initializer.
@_silgen_name("swift_distributedActor_remote_initialize")
func _distributedActorRemoteInitialize(_ actor: AnyObject)

/// Called to destroy the default actor instance in an actor.
/// The implementation will call this within the actor's deinit.
///
/// This will call `actorTransport.resignAddress(self.actorAddress)`.
@_silgen_name("swift_distributedActor_destroy")
func _distributedActorDestroy(_ actor: AnyObject)
