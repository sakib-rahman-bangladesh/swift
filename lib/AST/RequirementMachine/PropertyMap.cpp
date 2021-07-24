//===--- PropertyMap.cpp - Collects properties of type parameters ---------===//
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
//
// In the rewrite system, a conformance requirement 'T : P' is represented as
// rewrite rule of the form:
//
//    T.[P] => T
//
// Similarly, layout, superclass, and concrete-type requirements are represented
// by a rewrite rule of the form:
//
//    T.[p] => T
//
// Where [p] is a "property symbol": [layout: L], [superclass: Foo],
// [concrete: Bar].
//
// Given an arbitrary type T and a property [p], we can check if T satisfies the
// property by checking if the two terms T.[p] and T reduce to the same term T'.
// That is, if our rewrite rules allow us to eliminate the [p] suffix, we know
// the type satisfies [p].
//
// However, the question then becomes, given an arbitrary type T, how do we find
// *all* properties [p] satisfied by T?
//
// The trick is that we can take advantage of confluence here.
//
// If T.[p] => T', and T => T'', then it must follow that T''.[p] => T'.
// Furthermore, since T'' is fully reduced, T'' == T'. So T'' == UV for some
// terms U and V, and there exist be a rewrite rule V.[p] => V' in the system.
//
// Therefore, in order to find all [p] satisfied by T, we start by fully reducing
// T, then we look for rules of the form V.[p] => V' where V is fully reduced,
// and a suffix of T.
//
// This is the idea behind the property map. We collect all rules of the form
// V.[p] => V into a multi-map keyed by V. Then given an arbitrary type T,
// we can reduce it and look up successive suffixes to find all properties [p]
// satisfied by T.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/Types.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>

#include "PropertyMap.h"

using namespace swift;
using namespace rewriting;

/// This papers over a behavioral difference between
/// GenericSignature::getRequiredProtocols() and ArchetypeType::getConformsTo();
/// the latter drops any protocols to which the superclass requirement
/// conforms to concretely.
llvm::TinyPtrVector<const ProtocolDecl *>
PropertyBag::getConformsToExcludingSuperclassConformances() const {
  llvm::TinyPtrVector<const ProtocolDecl *> result;

  if (SuperclassConformances.empty()) {
    result = ConformsTo;
    return result;
  }

  // The conformances in SuperclassConformances should appear in the same order
  // as the protocols in ConformsTo.
  auto conformanceIter = SuperclassConformances.begin();

  for (const auto *proto : ConformsTo) {
    if (conformanceIter == SuperclassConformances.end()) {
      result.push_back(proto);
      continue;
    }

    if (proto == (*conformanceIter)->getProtocol()) {
      ++conformanceIter;
      continue;
    }

    result.push_back(proto);
  }

  assert(conformanceIter == SuperclassConformances.end());
  return result;
}

void PropertyBag::dump(llvm::raw_ostream &out) const {
  out << Key << " => {";

  if (!ConformsTo.empty()) {
    out << " conforms_to: [";
    bool first = true;
    for (const auto *proto : ConformsTo) {
      if (first)
        first = false;
      else
        out << " ";
      out << proto->getName();
    }
    out << "]";
  }

  if (Layout) {
    out << " layout: " << Layout;
  }

  if (Superclass) {
    out << " superclass: " << *Superclass;
  }

  if (ConcreteType) {
    out << " concrete_type: " << *ConcreteType;
  }

  out << " }";
}

/// Concrete type terms are written in terms of generic parameter types that
/// have a depth of 0, and an index into an array of substitution terms.
///
/// See RewriteSystemBuilder::getConcreteSubstitutionSchema().
static unsigned getGenericParamIndex(Type type) {
  auto *paramTy = type->castTo<GenericTypeParamType>();
  assert(paramTy->getDepth() == 0);
  return paramTy->getIndex();
}

/// Reverses the transformation performed by
/// RewriteSystemBuilder::getConcreteSubstitutionSchema().
static Type getTypeFromSubstitutionSchema(Type schema,
                                          ArrayRef<Term> substitutions,
                              TypeArrayView<GenericTypeParamType> genericParams,
                                          const ProtocolGraph &protos,
                                          RewriteContext &ctx) {
  assert(!schema->isTypeParameter() && "Must have a concrete type here");

  if (!schema->hasTypeParameter())
    return schema;

  return schema.transformRec([&](Type t) -> Optional<Type> {
    if (t->is<GenericTypeParamType>()) {
      auto index = getGenericParamIndex(t);

      return ctx.getTypeForTerm(substitutions[index],
                                genericParams, protos);
    }

    assert(!t->isTypeParameter());
    return None;
  });
}

/// Get the superclass bound for the term represented by this property bag.
///
/// Asserts if this property bag does not have a superclass bound.
Type PropertyBag::getSuperclassBound(
    TypeArrayView<GenericTypeParamType> genericParams,
    const ProtocolGraph &protos,
    RewriteContext &ctx) const {
  return getTypeFromSubstitutionSchema(Superclass->getSuperclass(),
                                       Superclass->getSubstitutions(),
                                       genericParams,
                                       protos,
                                       ctx);
}

/// Get the concrete type of the term represented by this property bag.
///
/// Asserts if this property bag is not concrete.
Type PropertyBag::getConcreteType(
    TypeArrayView<GenericTypeParamType> genericParams,
    const ProtocolGraph &protos,
    RewriteContext &ctx) const {
  return getTypeFromSubstitutionSchema(ConcreteType->getConcreteType(),
                                       ConcreteType->getSubstitutions(),
                                       genericParams,
                                       protos,
                                       ctx);
}

/// Computes the term corresponding to a member type access on a substitution.
///
/// The type witness is a type parameter of the form τ_0_n.X.Y.Z,
/// where 'n' is an index into the substitution array.
///
/// If the nth entry in the array is S, this will produce S.X.Y.Z.
///
/// There is a special behavior if the substitution is a term consisting of a
/// single protocol symbol [P]. If the innermost associated type in
/// \p typeWitness is [Q:Foo], the result will be [P:Foo], not [P].[Q:Foo] or
/// [Q:Foo].
static MutableTerm getRelativeTermForType(CanType typeWitness,
                                          ArrayRef<Term> substitutions,
                                          RewriteContext &ctx) {
  MutableTerm result;

  // Get the substitution S corresponding to τ_0_n.
  unsigned index = getGenericParamIndex(typeWitness->getRootGenericParam());
  result = MutableTerm(substitutions[index]);

  // If the substitution is a term consisting of a single protocol symbol
  // [P], save P for later.
  const ProtocolDecl *proto = nullptr;
  if (result.size() == 1 &&
      result[0].getKind() == Symbol::Kind::Protocol) {
    proto = result[0].getProtocol();
  }

  // Collect zero or more member type names in reverse order.
  SmallVector<Symbol, 3> symbols;
  while (auto memberType = dyn_cast<DependentMemberType>(typeWitness)) {
    typeWitness = memberType.getBase();

    auto *assocType = memberType->getAssocType();
    assert(assocType != nullptr &&
           "Conformance checking should not produce unresolved member types");

    // If the substitution is a term consisting of a single protocol symbol [P],
    // produce [P:Foo] instead of [P].[Q:Foo] or [Q:Foo].
    const auto *thisProto = assocType->getProtocol();
    if (proto && isa<GenericTypeParamType>(typeWitness)) {
      thisProto = proto;

      assert(result.size() == 1);
      assert(result[0].getKind() == Symbol::Kind::Protocol);
      assert(result[0].getProtocol() == proto);
      result = MutableTerm();
    }

    symbols.push_back(Symbol::forAssociatedType(thisProto,
                                                assocType->getName(), ctx));
  }

  // Add the member type names.
  std::reverse(symbols.begin(), symbols.end());
  for (auto symbol : symbols)
    result.add(symbol);

  return result;
}

/// This method takes a concrete type that was derived from a concrete type
/// produced by RewriteSystemBuilder::getConcreteSubstitutionSchema(),
/// either by extracting a structural sub-component or performing a (Swift AST)
/// substitution using subst(). It returns a new concrete substitution schema
/// and a new list of substitution terms.
///
/// For example, suppose we start with the concrete type
///
///   Dictionary<τ_0_0, Array<τ_0_1>> with substitutions {X.Y, Z}
///
/// We can extract out the structural sub-component Array<τ_0_1>. If we wish
/// to build a new concrete substitution schema, we call this method with
/// Array<τ_0_1> and the original substitutions {X.Y, Z}. This will produce
/// the new schema Array<τ_0_0> with substitutions {Z}.
///
/// As another example, consider we start with the schema Bar<τ_0_0> with
/// original substitutions {X.Y}, and perform a Swift AST subst() to get
/// Foo<τ_0_0.A.B>. We can then call this method with Foo<τ_0_0.A.B> and
/// the original substitutions {X.Y} to produce the new schema Foo<τ_0_0>
/// with substitutions {X.Y.A.B}.
static CanType
remapConcreteSubstitutionSchema(CanType concreteType,
                                ArrayRef<Term> substitutions,
                                RewriteContext &ctx,
                                SmallVectorImpl<Term> &result) {
  assert(!concreteType->isTypeParameter() && "Must have a concrete type here");

  if (!concreteType->hasTypeParameter())
    return concreteType;

  return CanType(concreteType.transformRec(
    [&](Type t) -> Optional<Type> {
      if (!t->isTypeParameter())
        return None;

      auto term = getRelativeTermForType(CanType(t), substitutions, ctx);

      unsigned newIndex = result.size();
      result.push_back(Term::get(term, ctx));

      return CanGenericTypeParamType::get(/*depth=*/0, newIndex,
                                          ctx.getASTContext());
    }));
}

/// When a type parameter has two concrete types, we have to unify the
/// type constructor arguments.
///
/// For example, suppose that we have two concrete same-type requirements:
///
///   T == Foo<X.Y, Z, String>
///   T == Foo<Int, A.B, W>
///
/// These lower to the following two rules:
///
///   T.[concrete: Foo<τ_0_0, τ_0_1, String> with {X.Y, Z}]
///   T.[concrete: Foo<Int, τ_0_0, τ_0_1> with {A.B, W}]
///
/// The two concrete type symbols will be added to the property bag of 'T',
/// and we will eventually end up in this method, where we will generate three
/// induced rules:
///
///   X.Y.[concrete: Int] => X.Y
///   A.B => Z
///   W.[concrete: String] => W
///
/// Returns the left hand side on success (it could also return the right hand
/// side; since we unified the type constructor arguments, it doesn't matter).
///
/// Returns true if a conflict was detected.
static bool unifyConcreteTypes(
    Symbol lhs, Symbol rhs, RewriteContext &ctx,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
    bool debug) {
  auto lhsType = lhs.getConcreteType();
  auto rhsType = rhs.getConcreteType();

  if (debug) {
    llvm::dbgs() << "% Unifying " << lhs << " with " << rhs << "\n";
  }

  class Matcher : public TypeMatcher<Matcher> {
    ArrayRef<Term> lhsSubstitutions;
    ArrayRef<Term> rhsSubstitutions;
    RewriteContext &ctx;
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules;
    bool debug;

  public:
    Matcher(ArrayRef<Term> lhsSubstitutions,
            ArrayRef<Term> rhsSubstitutions,
            RewriteContext &ctx,
            SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
            bool debug)
        : lhsSubstitutions(lhsSubstitutions),
          rhsSubstitutions(rhsSubstitutions),
          ctx(ctx), inducedRules(inducedRules), debug(debug) {}

    bool alwaysMismatchGenericParams() const { return true; }

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      if (isa<GenericTypeParamType>(firstType) &&
          isa<GenericTypeParamType>(secondType)) {
        // Both sides are type parameters; add a same-type requirement.
        unsigned lhsIndex = getGenericParamIndex(firstType);
        unsigned rhsIndex = getGenericParamIndex(secondType);
        if (lhsSubstitutions[lhsIndex] != rhsSubstitutions[rhsIndex]) {
          MutableTerm lhsTerm(lhsSubstitutions[lhsIndex]);
          MutableTerm rhsTerm(rhsSubstitutions[rhsIndex]);
          if (debug) {
            llvm::dbgs() << "%% Induced rule " << lhsTerm
                         << " == " << rhsTerm << "\n";
          }
          inducedRules.emplace_back(lhsTerm, rhsTerm);
        }
        return true;
      }

      if (isa<GenericTypeParamType>(firstType) &&
          !isa<GenericTypeParamType>(secondType)) {
        // A type parameter is equated with a concrete type; add a concrete
        // type requirement.
        unsigned lhsIndex = getGenericParamIndex(firstType);
        MutableTerm subjectTerm(lhsSubstitutions[lhsIndex]);

        SmallVector<Term, 3> result;
        auto concreteType = remapConcreteSubstitutionSchema(CanType(secondType),
                                                            rhsSubstitutions,
                                                            ctx, result);

        MutableTerm constraintTerm(subjectTerm);
        constraintTerm.add(Symbol::forConcreteType(concreteType, result, ctx));

        if (debug) {
          llvm::dbgs() << "%% Induced rule " << subjectTerm
                       << " == " << constraintTerm << "\n";
        }
        inducedRules.emplace_back(subjectTerm, constraintTerm);
        return true;
      }

      if (!isa<GenericTypeParamType>(firstType) &&
          isa<GenericTypeParamType>(secondType)) {
        // A concrete type is equated with a type parameter; add a concrete
        // type requirement.
        unsigned rhsIndex = getGenericParamIndex(secondType);
        MutableTerm subjectTerm(rhsSubstitutions[rhsIndex]);

        SmallVector<Term, 3> result;
        auto concreteType = remapConcreteSubstitutionSchema(CanType(firstType),
                                                            lhsSubstitutions,
                                                            ctx, result);

        MutableTerm constraintTerm(subjectTerm);
        constraintTerm.add(Symbol::forConcreteType(concreteType, result, ctx));

        if (debug) {
          llvm::dbgs() << "%% Induced rule " << subjectTerm
                       << " == " << constraintTerm << "\n";
        }
        inducedRules.emplace_back(subjectTerm, constraintTerm);
        return true;
      }

      // Any other kind of type mismatch involves different concrete types on
      // both sides, which can only happen on invalid input.
      return false;
    }
  };

  Matcher matcher(lhs.getSubstitutions(),
                  rhs.getSubstitutions(),
                  ctx, inducedRules, debug);
  if (!matcher.match(lhsType, rhsType)) {
    // FIXME: Diagnose the conflict
    if (debug) {
      llvm::dbgs() << "%% Concrete type conflict\n";
    }
    return true;
  }

  return false;
}

void PropertyBag::addProperty(
    Symbol property, RewriteContext &ctx,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules,
    bool debug) {

  switch (property.getKind()) {
  case Symbol::Kind::Protocol:
    ConformsTo.push_back(property.getProtocol());
    return;

  case Symbol::Kind::Layout:
    if (!Layout)
      Layout = property.getLayoutConstraint();
    else
      Layout = Layout.merge(property.getLayoutConstraint());

    return;

  case Symbol::Kind::Superclass: {
    auto superclass = property.getSuperclass();

    // A superclass requirement implies a layout requirement.
    auto layout =
      LayoutConstraint::getLayoutConstraint(
        superclass->getClassOrBoundGenericClass()->isObjC()
          ? LayoutConstraintKind::Class
          : LayoutConstraintKind::NativeClass,
        ctx.getASTContext());
    addProperty(Symbol::forLayout(layout, ctx), ctx, inducedRules, debug);

    // FIXME: This needs to find the most derived subclass and also call
    // unifyConcreteTypes()
    Superclass = property;
    return;
  }

  case Symbol::Kind::ConcreteType: {
    if (ConcreteType) {
      (void) unifyConcreteTypes(*ConcreteType, property,
                                ctx, inducedRules, debug);
    } else {
      ConcreteType = property;
    }

    return;
  }

  case Symbol::Kind::Name:
  case Symbol::Kind::GenericParam:
  case Symbol::Kind::AssociatedType:
    break;
  }

  llvm_unreachable("Bad symbol kind");
}

void PropertyBag::copyPropertiesFrom(const PropertyBag *next,
                                     RewriteContext &ctx) {
  // If this is the property bag of T and 'next' is the
  // property bag of V, then T := UV for some non-empty U.
  int prefixLength = Key.size() - next->Key.size();
  assert(prefixLength > 0);
  assert(std::equal(Key.begin() + prefixLength, Key.end(),
                    next->Key.begin()));

  // Conformances and the layout constraint, if any, can be copied over
  // unmodified.
  ConformsTo = next->ConformsTo;
  Layout = next->Layout;

  // If the property bag of V has superclass or concrete type
  // substitutions {X1, ..., Xn}, then the property bag of
  // T := UV should have substitutions {UX1, ..., UXn}.
  MutableTerm prefix(Key.begin(), Key.begin() + prefixLength);

  if (next->Superclass) {
    Superclass = next->Superclass->prependPrefixToConcreteSubstitutions(
        prefix, ctx);
  }

  if (next->ConcreteType) {
    ConcreteType = next->ConcreteType->prependPrefixToConcreteSubstitutions(
        prefix, ctx);
  }
}

/// Look for an property bag corresponding to the given key, returning nullptr
/// if one has not been recorded.
PropertyBag *
PropertyMap::getPropertiesIfPresent(const MutableTerm &key) const {
  assert(!key.empty());
 
  for (const auto &props : Map) {
    int compare = props->getKey().compare(key, Protos);
    if (compare == 0)
      return props.get();
    if (compare > 0)
      return nullptr;
  }

  return nullptr;
}

/// Look for an property bag corresponding to a suffix of the given key.
///
/// Returns nullptr if no information is known about this key.
PropertyBag *
PropertyMap::lookUpProperties(const MutableTerm &key) const {
  if (auto *props = getPropertiesIfPresent(key))
    return props;

  auto begin = key.begin() + 1;
  auto end = key.end();

  while (begin != end) {
    MutableTerm suffix(begin, end);

    if (auto *suffixClass = getPropertiesIfPresent(suffix))
      return suffixClass;

    ++begin;
  }

  return nullptr;
}

/// Look for an property bag corresponding to the given key, creating a new
/// property bag if necessary.
///
/// This must be called in monotonically non-decreasing key order.
PropertyBag *
PropertyMap::getOrCreateProperties(const MutableTerm &key) {
  assert(!key.empty());

  if (!Map.empty()) {
    const auto &lastEquivClass = Map.back();
    int compare = lastEquivClass->getKey().compare(key, Protos);
    if (compare == 0)
      return lastEquivClass.get();

    assert(compare < 0 && "Must record property bags in sorted order");
  }

  auto *props = new PropertyBag(key);

  // Look for the longest suffix of the key that has an property bag,
  // recording it as the next property bag if we find one.
  //
  // For example, if our rewrite system contains the following three rules:
  //
  //   A.[P] => A
  //   B.A.[Q] => B.A
  //   C.A.[R] => C.A
  //
  // Then we have three property bags:
  //
  //   A => { [P] }
  //   B.A => { [Q] }
  //   C.A => { [R] }
  //
  // The next property bag of both 'B.A' and 'C.A' is 'A'; conceptually,
  // the set of properties satisfied by 'B.A' is a superset of the properties
  // satisfied by 'A'; analogously for 'C.A'.
  //
  // Since 'A' has no proper suffix with additional properties, the next
  // property bag of 'A' is nullptr.
  if (auto *next = lookUpProperties(key))
    props->copyPropertiesFrom(next, Context);

  Map.emplace_back(props);

  return props;
}

void PropertyMap::clear() {
  Map.clear();
  ConcreteTypeInDomainMap.clear();
}

/// Record a protocol conformance, layout or superclass constraint on the given
/// key. Must be called in monotonically non-decreasing key order.
void PropertyMap::addProperty(
    const MutableTerm &key, Symbol property,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) {
  assert(property.isProperty());
  auto *props = getOrCreateProperties(key);
  props->addProperty(property, Context,
                     inducedRules, DebugConcreteUnification);
}

/// For each fully-concrete type, find the shortest term having that concrete type.
/// This is later used by computeConstraintTermForTypeWitness().
void PropertyMap::computeConcreteTypeInDomainMap() {
  for (const auto &props : Map) {
    if (!props->isConcreteType())
      continue;

    auto concreteType = props->ConcreteType->getConcreteType();
    if (concreteType->hasTypeParameter())
      continue;

    assert(props->ConcreteType->getSubstitutions().empty());

    auto domain = props->Key.getRootProtocols();
    auto concreteTypeKey = std::make_pair(concreteType, domain);

    auto found = ConcreteTypeInDomainMap.find(concreteTypeKey);
    if (found != ConcreteTypeInDomainMap.end()) {
      const auto &otherTerm = found->second;
      assert(props->Key.compare(otherTerm, Protos) > 0 &&
             "Out-of-order keys?");
      continue;
    }

    auto inserted = ConcreteTypeInDomainMap.insert(
        std::make_pair(concreteTypeKey, props->Key));
    assert(inserted.second);
    (void) inserted;
  }
}

void PropertyMap::concretizeNestedTypesFromConcreteParents(
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) const {
  for (const auto &props : Map) {
    if (props->getConformsTo().empty())
      continue;

    if (DebugConcretizeNestedTypes) {
      if (props->isConcreteType() ||
          props->hasSuperclassBound()) {
        llvm::dbgs() << "^ Concretizing nested types of ";
        props->dump(llvm::dbgs());
        llvm::dbgs() << "\n";
      }
    }

    if (props->isConcreteType()) {
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "- via concrete type requirement\n";
      }

      concretizeNestedTypesFromConcreteParent(
          props->getKey(),
          RequirementKind::SameType,
          props->ConcreteType->getConcreteType(),
          props->ConcreteType->getSubstitutions(),
          props->getConformsTo(),
          props->ConcreteConformances,
          inducedRules);
    }

    if (props->hasSuperclassBound()) {
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "- via superclass requirement\n";
      }

      concretizeNestedTypesFromConcreteParent(
          props->getKey(),
          RequirementKind::Superclass,
          props->Superclass->getSuperclass(),
          props->Superclass->getSubstitutions(),
          props->getConformsTo(),
          props->SuperclassConformances,
          inducedRules);
    }
  }
}

/// Suppose a same-type requirement merges two property bags,
/// one of which has a conformance requirement to P and the other
/// one has a concrete type or superclass requirement.
///
/// If the concrete type or superclass conforms to P and P has an
/// associated type A, then we need to infer an equivalence between
/// T.[P:A] and whatever the type witness for 'A' is in the
/// concrete conformance.
///
/// For example, suppose we have a the following definitions,
///
///    protocol Q { associatedtype V }
///    protocol P { associatedtype A; associatedtype C }
///    struct Foo<A, B : Q> : P {
///      typealias C = B.V
///    }
///
/// together with the following property bag:
///
///    T => { conforms_to: [ P ], concrete: Foo<Int, τ_0_0> with <U> }
///
/// The type witness for A in the conformance Foo<Int, τ_0_0> : P is
/// the concrete type 'Int', which induces the following rule:
///
///    T.[P:A].[concrete: Int] => T.[P:A]
///
/// Whereas the type witness for B in the same conformance is the
/// abstract type 'τ_0_0.V', which via the substitutions <U> corresponds
/// to the term 'U.V', and therefore induces the following rule:
///
///    T.[P:B] => U.V
///
void PropertyMap::concretizeNestedTypesFromConcreteParent(
    const MutableTerm &key, RequirementKind requirementKind,
    CanType concreteType, ArrayRef<Term> substitutions,
    ArrayRef<const ProtocolDecl *> conformsTo,
    llvm::TinyPtrVector<ProtocolConformance *> &conformances,
    SmallVectorImpl<std::pair<MutableTerm, MutableTerm>> &inducedRules) const {
  assert(requirementKind == RequirementKind::SameType ||
         requirementKind == RequirementKind::Superclass);

  for (auto *proto : conformsTo) {
    // FIXME: Either remove the ModuleDecl entirely from conformance lookup,
    // or pass the correct one down in here.
    auto *module = proto->getParentModule();

    auto conformance = module->lookupConformance(concreteType,
                                                 const_cast<ProtocolDecl *>(proto));
    if (conformance.isInvalid()) {
      // FIXME: Diagnose conflict
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ " << concreteType << " does not conform to "
                     << proto->getName() << "\n";
      }

      continue;
    }

    // FIXME: Maybe this can happen if the concrete type is an
    // opaque result type?
    assert(!conformance.isAbstract());

    auto *concrete = conformance.getConcrete();

    // Record the conformance for use by
    // PropertyBag::getConformsToExcludingSuperclassConformances().
    conformances.push_back(concrete);

    auto assocTypes = Protos.getProtocolInfo(proto).AssociatedTypes;
    if (assocTypes.empty())
      continue;

    for (auto *assocType : assocTypes) {
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ " << "Looking up type witness for "
                     << proto->getName() << ":" << assocType->getName()
                     << " on " << concreteType << "\n";
      }

      auto t = concrete->getTypeWitness(assocType);
      if (!t) {
        if (DebugConcretizeNestedTypes) {
          llvm::dbgs() << "^^ " << "Type witness for " << assocType->getName()
                       << " of " << concreteType << " could not be inferred\n";
        }

        t = ErrorType::get(concreteType);
      }

      auto typeWitness = t->getCanonicalType();

      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ " << "Type witness for " << assocType->getName()
                     << " of " << concreteType << " is " << typeWitness << "\n";
      }

      MutableTerm subjectType = key;
      subjectType.add(Symbol::forAssociatedType(proto, assocType->getName(),
                                                Context));

      MutableTerm constraintType;

      if (concreteType == typeWitness &&
          requirementKind == RequirementKind::SameType) {
        // FIXME: ConcreteTypeInDomainMap should support substitutions so
        // that we can remove this.

        if (DebugConcretizeNestedTypes) {
          llvm::dbgs() << "^^ Type witness is the same as the concrete type\n";
        }

        // Add a rule T.[P:A] => T.
        constraintType = key;
      } else {
        constraintType = computeConstraintTermForTypeWitness(
            key, concreteType, typeWitness, subjectType,
            substitutions);
      }

      inducedRules.emplace_back(subjectType, constraintType);
      if (DebugConcretizeNestedTypes) {
        llvm::dbgs() << "^^ Induced rule " << constraintType
                     << " => " << subjectType << "\n";
      }
    }
  }
}

/// Given the key of an property bag known to have \p concreteType,
/// together with a \p typeWitness from a conformance on that concrete
/// type, return the right hand side of a rewrite rule to relate
/// \p subjectType with a term representing the type witness.
///
/// Suppose the key is T and the subject type is T.[P:A].
///
/// If the type witness is an abstract type U, this produces a rewrite
/// rule
///
///     T.[P:A] => U
///
/// If the type witness is a concrete type Foo, this produces a rewrite
/// rule
///
///     T.[P:A].[concrete: Foo] => T.[P:A]
///
/// However, this also tries to tie off recursion first using a heuristic.
///
/// If the type witness is fully concrete and we've already seen some
/// term V in the same domain with the same concrete type, we produce a
/// rewrite rule:
///
///        T.[P:A] => V
MutableTerm PropertyMap::computeConstraintTermForTypeWitness(
    const MutableTerm &key, CanType concreteType, CanType typeWitness,
    const MutableTerm &subjectType, ArrayRef<Term> substitutions) const {
  if (!typeWitness->hasTypeParameter()) {
    // Check if we have a shorter representative we can use.
    auto domain = key.getRootProtocols();
    auto concreteTypeKey = std::make_pair(typeWitness, domain);

    auto found = ConcreteTypeInDomainMap.find(concreteTypeKey);
    if (found != ConcreteTypeInDomainMap.end()) {
      if (found->second != subjectType) {
        if (DebugConcretizeNestedTypes) {
          llvm::dbgs() << "^^ Type witness can re-use property bag of "
                       << found->second << "\n";
        }
        return found->second;
      }
    }
  }

  if (typeWitness->isTypeParameter()) {
    // The type witness is a type parameter of the form τ_0_n.X.Y...Z,
    // where 'n' is an index into the substitution array.
    //
    // Add a rule T => S.X.Y...Z, where S is the nth substitution term.
    return getRelativeTermForType(typeWitness, substitutions, Context);
  }

  // The type witness is a concrete type.
  MutableTerm constraintType = subjectType;

  SmallVector<Term, 3> result;
  auto typeWitnessSchema =
      remapConcreteSubstitutionSchema(typeWitness, substitutions,
                                      Context, result);

  // Add a rule T.[P:A].[concrete: Foo.A] => T.[P:A].
  constraintType.add(
      Symbol::forConcreteType(
          typeWitnessSchema, result, Context));

  return constraintType;
}

void PropertyMap::dump(llvm::raw_ostream &out) const {
  out << "Property map: {\n";
  for (const auto &props : Map) {
    out << "  ";
    props->dump(out);
    out << "\n";
  }
  out << "}\n";
}

/// Build the property map from all rules of the form T.[p] => T, where
/// [p] is a property symbol.
///
/// Returns a pair consisting of a status and number of iterations executed.
///
/// The status is CompletionResult::MaxIterations if we exceed \p maxIterations
/// iterations.
///
/// The status is CompletionResult::MaxDepth if we produce a rewrite rule whose
/// left hand side has a length exceeding \p maxDepth.
///
/// Otherwise, the status is CompletionResult::Success.
std::pair<RewriteSystem::CompletionResult, unsigned>
RewriteSystem::buildPropertyMap(PropertyMap &map,
                                unsigned maxIterations,
                                unsigned maxDepth) {
  map.clear();

  std::vector<std::pair<MutableTerm, Symbol>> properties;

  for (const auto &rule : Rules) {
    if (rule.isDeleted())
      continue;

    const auto &lhs = rule.getLHS();

    // Collect all rules of the form T.[p] => T where T is canonical.
    auto property = lhs.back();
    if (!property.isProperty())
      continue;

    MutableTerm key(lhs.begin(), lhs.end() - 1);
    if (key != rule.getRHS())
      continue;

#ifndef NDEBUG
    assert(!simplify(key) &&
           "Right hand side of a property rule should already be reduced");
#endif

    properties.emplace_back(key, property);
  }

  // PropertyMap::addRule() requires that shorter rules are added
  // before longer rules, so that it can perform lookups on suffixes and call
  // PropertyBag::copyPropertiesFrom().
  std::sort(properties.begin(), properties.end(),
            [&](const std::pair<MutableTerm, Symbol> &lhs,
                const std::pair<MutableTerm, Symbol> &rhs) -> bool {
              return lhs.first.compare(rhs.first, Protos) < 0;
            });

  // Merging multiple superclass or concrete type rules can induce new rules
  // to unify concrete type constructor arguments.
  SmallVector<std::pair<MutableTerm, MutableTerm>, 3> inducedRules;

  for (auto pair : properties) {
    map.addProperty(pair.first, pair.second, inducedRules);
  }

  // We collect terms with fully concrete types so that we can re-use them
  // to tie off recursion in the next step.
  map.computeConcreteTypeInDomainMap();

  // Now, we merge concrete type rules with conformance rules, by adding
  // relations between associated type members of type parameters with
  // the concrete type witnesses in the concrete type's conformance.
  map.concretizeNestedTypesFromConcreteParents(inducedRules);

  // Some of the induced rules might be trivial; only count the induced rules
  // where the left hand side is not already equivalent to the right hand side.
  unsigned addedNewRules = 0;
  for (auto pair : inducedRules) {
    if (addRule(pair.first, pair.second)) {
      ++addedNewRules;

      const auto &newRule = Rules.back();
      if (newRule.getLHS().size() > maxDepth)
        return std::make_pair(CompletionResult::MaxDepth, addedNewRules);
    }
  }

  if (Rules.size() > maxIterations)
    return std::make_pair(CompletionResult::MaxIterations, addedNewRules);

  return std::make_pair(CompletionResult::Success, addedNewRules);
}