//===--- RewriteSystem.h - Generics with term rewriting ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REWRITESYSTEM_H
#define SWIFT_REWRITESYSTEM_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TrailingObjects.h"
#include <algorithm>

#include "ProtocolGraph.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class PropertyMap;
class MutableTerm;
class RewriteContext;
class Term;

/// The smallest element in the rewrite system.
///
/// enum Symbol {
///   case name(Identifier)
///   case protocol(Protocol)
///   case type([Protocol], Identifier)
///   case genericParam(index: Int, depth: Int)
///   case layout(LayoutConstraint)
///   case superclass(CanType, substitutions: [Term])
///   case concrete(CanType, substitutions: [Term])
/// }
///
/// For the concrete type symbols (`superclass` and `concrete`),
/// the type's structural components must either be concrete, or
/// generic parameters. All generic parameters must have a depth
/// of 0; the generic parameter index corresponds to an index in
/// the `substitutions` array.
///
/// For example, the superclass requirement
/// "T : MyClass<U.X, (Int) -> V.A.B>" is denoted with a symbol
/// structured as follows:
///
/// - type: MyClass<τ_0_0, (Int) -> τ_0_1>
/// - substitutions:
///   - U.X
///   - V.A.B
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class Symbol final {
public:
  enum class Kind : uint8_t {
    //////
    ////// Special symbol kind that is both type-like and property-like:
    //////

    /// When appearing at the start of a term, denotes a nested
    /// type of a protocol 'Self' type.
    ///
    /// When appearing at the end of a term, denotes that the
    /// term's type conforms to the protocol.
    Protocol,

    //////
    ////// "Type-like" symbol kinds:
    //////

    /// An associated type [P:T] or [P&Q&...:T]. The parent term
    /// must be known to conform to P (or P, Q, ...).
    AssociatedType,

    /// A generic parameter, uniquely identified by depth and
    /// index. Can only appear at the beginning of a term, where
    /// it denotes a generic parameter of the top-level generic
    /// signature.
    GenericParam,

    /// An unbound identifier name.
    Name,

    //////
    ////// "Property-like" symbol kinds:
    //////

    /// When appearing at the end of a term, denotes that the
    /// term's type satisfies the layout constraint.
    Layout,

    /// When appearing at the end of a term, denotes that the term
    /// is a subclass of the superclass constraint.
    Superclass,

    /// When appearing at the end of a term, denotes that the term
    /// is exactly equal to the concrete type.
    ConcreteType,
  };

private:
  friend class RewriteContext;

  struct Storage;

private:
  const Storage *Ptr;

  Symbol(const Storage *ptr) : Ptr(ptr) {}

public:
  Kind getKind() const;

  /// A property records something about a type term; either a protocol
  /// conformance, a layout constraint, or a superclass or concrete type
  /// constraint.
  bool isProperty() const {
    auto kind = getKind();
    return (kind == Symbol::Kind::Protocol ||
            kind == Symbol::Kind::Layout ||
            kind == Symbol::Kind::Superclass ||
            kind == Symbol::Kind::ConcreteType);
  }

  bool isSuperclassOrConcreteType() const {
    auto kind = getKind();
    return (kind == Kind::Superclass || kind == Kind::ConcreteType);
  }

  Identifier getName() const;

  const ProtocolDecl *getProtocol() const;

  ArrayRef<const ProtocolDecl *> getProtocols() const;

  GenericTypeParamType *getGenericParam() const;

  LayoutConstraint getLayoutConstraint() const;

  CanType getSuperclass() const;

  CanType getConcreteType() const;

  ArrayRef<Term> getSubstitutions() const;

  /// Returns an opaque pointer that uniquely identifies this symbol.
  const void *getOpaquePointer() const {
    return Ptr;
  }

  static Symbol forName(Identifier name,
                        RewriteContext &ctx);

  static Symbol forProtocol(const ProtocolDecl *proto,
                            RewriteContext &ctx);

  static Symbol forAssociatedType(const ProtocolDecl *proto,
                                  Identifier name,
                                  RewriteContext &ctx);

  static Symbol forAssociatedType(ArrayRef<const ProtocolDecl *> protos,
                                  Identifier name,
                                  RewriteContext &ctx);

  static Symbol forGenericParam(GenericTypeParamType *param,
                                RewriteContext &ctx);

  static Symbol forLayout(LayoutConstraint layout,
                          RewriteContext &ctx);

  static Symbol forSuperclass(CanType type,
                              ArrayRef<Term> substitutions,
                              RewriteContext &ctx);

  static Symbol forConcreteType(CanType type,
                                ArrayRef<Term> substitutions,
                                RewriteContext &ctx);

  int compare(Symbol other, const ProtocolGraph &protos) const;

  Symbol transformConcreteSubstitutions(
      llvm::function_ref<Term(Term)> fn,
      RewriteContext &ctx) const;

  Symbol prependPrefixToConcreteSubstitutions(
      const MutableTerm &prefix,
      RewriteContext &ctx) const;

  void dump(llvm::raw_ostream &out) const;

  friend bool operator==(Symbol lhs, Symbol rhs) {
    return lhs.Ptr == rhs.Ptr;
  }

  friend bool operator!=(Symbol lhs, Symbol rhs) {
    return !(lhs == rhs);
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out, Symbol symbol) {
    symbol.dump(out);
    return out;
  }
};

/// See the implementation of MutableTerm::checkForOverlap() for a discussion.
enum class OverlapKind {
  /// Terms do not overlap.
  None,
  /// First kind of overlap (TUV vs U).
  First,
  /// Second kind of overlap (TU vs UV).
  Second
};

/// A term is a sequence of one or more symbols.
///
/// The Term type is a uniqued, permanently-allocated representation,
/// used to represent terms in the rewrite rules themselves. See also
/// MutableTerm for the other representation.
///
/// The first symbol in the term must be a protocol, generic parameter, or
/// associated type symbol.
///
/// A layout, superclass or concrete type symbol must only appear at the
/// end of a term.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class Term final {
  friend class RewriteContext;

  struct Storage;

  const Storage *Ptr;

  Term(const Storage *ptr) : Ptr(ptr) {}

public:
  size_t size() const;

  ArrayRef<Symbol>::iterator begin() const;
  ArrayRef<Symbol>::iterator end() const;

  ArrayRef<Symbol>::reverse_iterator rbegin() const;
  ArrayRef<Symbol>::reverse_iterator rend() const;

  Symbol back() const;

  Symbol operator[](size_t index) const;

  /// Returns an opaque pointer that uniquely identifies this term.
  const void *getOpaquePointer() const {
    return Ptr;
  }

  static Term get(const MutableTerm &term, RewriteContext &ctx);

  void dump(llvm::raw_ostream &out) const;

  friend bool operator==(Term lhs, Term rhs) {
    return lhs.Ptr == rhs.Ptr;
  }

  friend bool operator!=(Term lhs, Term rhs) {
    return !(lhs == rhs);
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out, Term term) {
    term.dump(out);
    return out;
  }
};

/// A term is a sequence of one or more symbols.
///
/// The MutableTerm type is a dynamically-allocated representation,
/// used to represent temporary values in simplification and completion.
/// See also Term for the other representation.
///
/// The first symbol in the term must be a protocol, generic parameter, or
/// associated type symbol.
///
/// A layout constraint symbol must only appear at the end of a term.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class MutableTerm final {
  llvm::SmallVector<Symbol, 3> Symbols;

public:
  /// Creates an empty term. At least one symbol must be added for the term
  /// to become valid.
  MutableTerm() {}

  explicit MutableTerm(decltype(Symbols)::const_iterator begin,
                       decltype(Symbols)::const_iterator end)
    : Symbols(begin, end) {}

  explicit MutableTerm(llvm::SmallVector<Symbol, 3> &&symbols)
    : Symbols(std::move(symbols)) {}

  explicit MutableTerm(ArrayRef<Symbol> symbols)
    : Symbols(symbols.begin(), symbols.end()) {}

  explicit MutableTerm(Term term)
    : Symbols(term.begin(), term.end()) {}

  void add(Symbol symbol) {
    Symbols.push_back(symbol);
  }

  void append(Term other) {
    Symbols.append(other.begin(), other.end());
  }

  void append(const MutableTerm &other) {
    Symbols.append(other.begin(), other.end());
  }

  int compare(const MutableTerm &other, const ProtocolGraph &protos) const;

  bool empty() const { return Symbols.empty(); }

  size_t size() const { return Symbols.size(); }

  ArrayRef<const ProtocolDecl *> getRootProtocols() const;

  decltype(Symbols)::const_iterator begin() const { return Symbols.begin(); }
  decltype(Symbols)::const_iterator end() const { return Symbols.end(); }

  decltype(Symbols)::iterator begin() { return Symbols.begin(); }
  decltype(Symbols)::iterator end() { return Symbols.end(); }

  decltype(Symbols)::const_reverse_iterator rbegin() const { return Symbols.rbegin(); }
  decltype(Symbols)::const_reverse_iterator rend() const { return Symbols.rend(); }

  decltype(Symbols)::reverse_iterator rbegin() { return Symbols.rbegin(); }
  decltype(Symbols)::reverse_iterator rend() { return Symbols.rend(); }

  Symbol back() const {
    return Symbols.back();
  }

  Symbol &back() {
    return Symbols.back();
  }

  Symbol operator[](size_t index) const {
    return Symbols[index];
  }

  Symbol &operator[](size_t index) {
    return Symbols[index];
  }

  decltype(Symbols)::const_iterator findSubTerm(
      const MutableTerm &other) const;

  decltype(Symbols)::iterator findSubTerm(
      const MutableTerm &other);

  /// Returns true if this term contains, or is equal to, \p other.
  bool containsSubTerm(const MutableTerm &other) const {
    return findSubTerm(other) != end();
  }

  bool rewriteSubTerm(const MutableTerm &lhs, const MutableTerm &rhs);

  OverlapKind checkForOverlap(const MutableTerm &other,
                              MutableTerm &t,
                              MutableTerm &v) const;

  void dump(llvm::raw_ostream &out) const;

  friend bool operator==(const MutableTerm &lhs, const MutableTerm &rhs) {
    if (lhs.size() != rhs.size())
      return false;

    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }

  friend bool operator!=(const MutableTerm &lhs, const MutableTerm &rhs) {
    return !(lhs == rhs);
  }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       const MutableTerm &term) {
    term.dump(out);
    return out;
  }
};

/// A rewrite rule that replaces occurrences of LHS with RHS.
///
/// LHS must be greater than RHS in the linear order over terms.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class Rule final {
  MutableTerm LHS;
  MutableTerm RHS;
  bool deleted;

public:
  Rule(const MutableTerm &lhs, const MutableTerm &rhs)
      : LHS(lhs), RHS(rhs), deleted(false) {}

  const MutableTerm &getLHS() const { return LHS; }
  const MutableTerm &getRHS() const { return RHS; }

  bool apply(MutableTerm &term) const {
    return term.rewriteSubTerm(LHS, RHS);
  }

  OverlapKind checkForOverlap(const Rule &other,
                              MutableTerm &t,
                              MutableTerm &v) const {
    return LHS.checkForOverlap(other.LHS, t, v);
  }

  bool canReduceLeftHandSide(const Rule &other) const {
    return LHS.containsSubTerm(other.LHS);
  }

  /// Returns if the rule was deleted.
  bool isDeleted() const {
    return deleted;
  }

  /// Deletes the rule, which removes it from consideration in term
  /// simplification and completion. Deleted rules are simply marked as
  /// such instead of being physically removed from the rules vector
  /// in the rewrite system, to ensure that indices remain valid across
  /// deletion.
  void markDeleted() {
    assert(!deleted);
    deleted = true;
  }

  /// Returns the length of the left hand side.
  unsigned getDepth() const {
    return LHS.size();
  }

  /// Partial order on rules orders rules by their left hand side.
  int compare(const Rule &other,
              const ProtocolGraph &protos) const {
    return LHS.compare(other.LHS, protos);
  }

  void dump(llvm::raw_ostream &out) const;

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                       const Rule &rule) {
    rule.dump(out);
    return out;
  }
};

/// A term rewrite system for working with types in a generic signature.
///
/// Out-of-line methods are documented in RewriteSystem.cpp.
class RewriteSystem final {
  /// Rewrite context for memory allocation.
  RewriteContext &Context;

  /// The rules added so far, including rules from our client, as well
  /// as rules introduced by the completion procedure.
  std::vector<Rule> Rules;

  /// The graph of all protocols transitively referenced via our set of
  /// rewrite rules, used for the linear order on symbols.
  ProtocolGraph Protos;

  /// A list of pending terms for the associated type merging completion
  /// heuristic.
  ///
  /// The pair (lhs, rhs) satisfies the following conditions:
  /// - lhs > rhs
  /// - all symbols but the last are pair-wise equal in lhs and rhs
  /// - the last symbol in both lhs and rhs is an associated type symbol
  /// - the last symbol in both lhs and rhs has the same name
  ///
  /// See RewriteSystem::processMergedAssociatedTypes() for details.
  std::vector<std::pair<MutableTerm, MutableTerm>> MergedAssociatedTypes;

  /// A list of pending pairs for checking overlap in the completion
  /// procedure.
  std::deque<std::pair<unsigned, unsigned>> Worklist;

  /// Set these to true to enable debugging output.
  unsigned DebugSimplify : 1;
  unsigned DebugAdd : 1;
  unsigned DebugMerge : 1;
  unsigned DebugCompletion : 1;

public:
  explicit RewriteSystem(RewriteContext &ctx) : Context(ctx) {
    DebugSimplify = false;
    DebugAdd = false;
    DebugMerge = false;
    DebugCompletion = false;
  }

  RewriteSystem(const RewriteSystem &) = delete;
  RewriteSystem(RewriteSystem &&) = delete;
  RewriteSystem &operator=(const RewriteSystem &) = delete;
  RewriteSystem &operator=(RewriteSystem &&) = delete;

  /// Return the rewrite context used for allocating memory.
  RewriteContext &getRewriteContext() const { return Context; }

  /// Return the object recording information about known protocols.
  const ProtocolGraph &getProtocols() const { return Protos; }

  void initialize(std::vector<std::pair<MutableTerm, MutableTerm>> &&rules,
                  ProtocolGraph &&protos);

  Symbol simplifySubstitutionsInSuperclassOrConcreteSymbol(Symbol symbol) const;

  bool addRule(MutableTerm lhs, MutableTerm rhs);

  bool simplify(MutableTerm &term) const;

  enum class CompletionResult {
    /// Confluent completion was computed successfully.
    Success,

    /// Maximum number of iterations reached.
    MaxIterations,

    /// Completion produced a rewrite rule whose left hand side has a length
    /// exceeding the limit.
    MaxDepth
  };

  std::pair<CompletionResult, unsigned>
  computeConfluentCompletion(unsigned maxIterations,
                             unsigned maxDepth);

  void simplifyRightHandSides();

  std::pair<CompletionResult, unsigned>
  buildPropertyMap(PropertyMap &map,
                   unsigned maxIterations,
                   unsigned maxDepth);

  void dump(llvm::raw_ostream &out) const;

private:
  Optional<std::pair<MutableTerm, MutableTerm>>
  computeCriticalPair(const Rule &lhs, const Rule &rhs) const;

  Symbol mergeAssociatedTypes(Symbol lhs, Symbol rhs) const;
  void processMergedAssociatedTypes();
};

} // end namespace rewriting

} // end namespace swift

#endif
