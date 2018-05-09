//===--- SILGenDestructor.cpp - SILGen for destructors --------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGenFunction.h"
#include "RValue.h"
#include "Scope.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace Lowering;

void SILGenFunction::emitDestroyingDestructor(DestructorDecl *dd) {
  MagicFunctionName = DeclName(SGM.M.getASTContext().getIdentifier("deinit"));

  RegularLocation Loc(dd);
  if (dd->isImplicit())
    Loc.markAutoGenerated();

  auto cd = cast<ClassDecl>(dd->getDeclContext());
  SILValue selfValue = emitSelfDecl(dd->getImplicitSelfDecl());

  // Create a basic block to jump to for the implicit destruction behavior
  // of releasing the elements and calling the superclass destructor.
  // We won't actually emit the block until we finish with the destructor body.
  prepareEpilog(Type(), false, CleanupLocation::get(Loc));

  emitProfilerIncrement(dd->getBody());
  // Emit the destructor body.
  emitStmt(dd->getBody());

  Optional<SILValue> maybeReturnValue;
  SILLocation returnLoc(Loc);
  std::tie(maybeReturnValue, returnLoc) = emitEpilogBB(Loc);

  if (!maybeReturnValue)
    return;

  auto cleanupLoc = CleanupLocation::get(Loc);

  // If we have a superclass, invoke its destructor.
  SILValue resultSelfValue;
  SILType objectPtrTy = SILType::getNativeObjectType(F.getASTContext());
  SILType classTy = selfValue->getType();
  if (cd->hasSuperclass()) {
    Type superclassTy = dd->mapTypeIntoContext(cd->getSuperclass());
    ClassDecl *superclass = superclassTy->getClassOrBoundGenericClass();
    auto superclassDtorDecl = superclass->getDestructor();
    SILDeclRef dtorConstant =
      SILDeclRef(superclassDtorDecl, SILDeclRef::Kind::Destroyer);
    SILType baseSILTy = getLoweredLoadableType(superclassTy);
    SILValue baseSelf = B.createUpcast(cleanupLoc, selfValue, baseSILTy);
    ManagedValue dtorValue;
    SILType dtorTy;
    auto subMap
      = superclassTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                                superclass);
    std::tie(dtorValue, dtorTy)
      = emitSiblingMethodRef(cleanupLoc, baseSelf, dtorConstant, subMap);

    SmallVector<Substitution, 4> subs;
    if (auto *genericSig = superclass->getGenericSignature())
      genericSig->getSubstitutions(subMap, subs);
    resultSelfValue = B.createApply(cleanupLoc, dtorValue.forward(*this),
                                    dtorTy, objectPtrTy, subs, baseSelf);
  } else {
    resultSelfValue = selfValue;
  }

  {
    Scope S(Cleanups, cleanupLoc);
    ManagedValue borrowedValue =
        ManagedValue::forUnmanaged(resultSelfValue).borrow(*this, cleanupLoc);

    if (classTy != borrowedValue.getType()) {
      borrowedValue =
          B.createUncheckedRefCast(cleanupLoc, borrowedValue, classTy);
    }

    // Release our members.
    emitClassMemberDestruction(borrowedValue, cd, cleanupLoc);
  }

  if (resultSelfValue->getType() != objectPtrTy) {
    resultSelfValue =
        B.createUncheckedRefCast(cleanupLoc, resultSelfValue, objectPtrTy);
  }
  if (resultSelfValue.getOwnershipKind() != ValueOwnershipKind::Owned) {
    assert(resultSelfValue.getOwnershipKind() ==
           ValueOwnershipKind::Guaranteed);
    resultSelfValue = B.createUncheckedOwnershipConversion(
        cleanupLoc, resultSelfValue, ValueOwnershipKind::Owned);
  }
  B.createReturn(returnLoc, resultSelfValue);
}

void SILGenFunction::emitDeallocatingDestructor(DestructorDecl *dd) {
  MagicFunctionName = DeclName(SGM.M.getASTContext().getIdentifier("deinit"));

  // The deallocating destructor is always auto-generated.
  RegularLocation loc(dd);
  loc.markAutoGenerated();

  // Emit the prolog.
  SILValue initialSelfValue = emitSelfDecl(dd->getImplicitSelfDecl());

  // Form a reference to the destroying destructor.
  SILDeclRef dtorConstant(dd, SILDeclRef::Kind::Destroyer);
  auto classTy = initialSelfValue->getType();
  auto classDecl = classTy.getASTType()->getAnyNominal();
  ManagedValue dtorValue;
  SILType dtorTy;
  auto subMap = classTy.getASTType()
    ->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                classDecl);
  std::tie(dtorValue, dtorTy)
    = emitSiblingMethodRef(loc, initialSelfValue, dtorConstant, subMap);

  SmallVector<Substitution, 4> subs;
  if (auto *genericSig = classDecl->getGenericSignature())
    genericSig->getSubstitutions(subMap, subs);

  // Call the destroying destructor.
  SILValue selfForDealloc;
  {
    FullExpr CleanupScope(Cleanups, CleanupLocation::get(loc));
    ManagedValue borrowedSelf = emitManagedBeginBorrow(loc, initialSelfValue);
    SILType objectPtrTy = SILType::getNativeObjectType(F.getASTContext());
    selfForDealloc = B.createApply(loc, dtorValue.forward(*this),
                                   dtorTy, objectPtrTy, subs,
                                   borrowedSelf.getUnmanagedValue());
  }

  // Balance out the +1 from the self argument using end_lifetime.
  //
  // The issue here is that:
  //
  // 1. Self is passed into deallocating deinits at +1.
  // 2. Destroying deinits take in self as a +0 value that is then returned at
  // +1.
  //
  // This means that the lifetime of self can not be modeled statically in a
  // deallocating deinit without analyzing the body of the destroying deinit
  // (something that violates semantic sil). Thus we add an artificial destroy of
  // self before the actual destroy of self so that the verifier can understand
  // that self is being properly balanced.
  B.createEndLifetime(loc, initialSelfValue);

  // Deallocate the object.
  selfForDealloc = B.createUncheckedRefCast(loc, selfForDealloc, classTy);
  B.createDeallocRef(loc, selfForDealloc, false);

  emitProfilerIncrement(dd->getBody());

  // Return.
  B.createReturn(loc, emitEmptyTuple(loc));
}

void SILGenFunction::emitIVarDestroyer(SILDeclRef ivarDestroyer) {
  auto cd = cast<ClassDecl>(ivarDestroyer.getDecl());
  RegularLocation loc(cd);
  loc.markAutoGenerated();

  ManagedValue selfValue = ManagedValue::forUnmanaged(
      emitSelfDecl(cd->getDestructor()->getImplicitSelfDecl()));

  auto cleanupLoc = CleanupLocation::get(loc);
  prepareEpilog(TupleType::getEmpty(getASTContext()), false, cleanupLoc);
  {
    Scope S(*this, cleanupLoc);
    emitClassMemberDestruction(selfValue, cd, cleanupLoc);
  }

  B.createReturn(loc, emitEmptyTuple(loc));
  emitEpilog(loc);
}

void SILGenFunction::emitClassMemberDestruction(ManagedValue selfValue,
                                                ClassDecl *cd,
                                                CleanupLocation cleanupLoc) {
  selfValue = selfValue.borrow(*this, cleanupLoc);
  for (VarDecl *vd : cd->getStoredProperties()) {
    const TypeLowering &ti = getTypeLowering(vd->getType());
    if (!ti.isTrivial()) {
      SILValue addr =
          B.createRefElementAddr(cleanupLoc, selfValue.getValue(), vd,
                                 ti.getLoweredType().getAddressType());
      B.createDestroyAddr(cleanupLoc, addr);
    }
  }
}


void SILGenFunction::emitObjCDestructor(SILDeclRef dtor) {
  auto dd = cast<DestructorDecl>(dtor.getDecl());
  auto cd = cast<ClassDecl>(dd->getDeclContext());
  MagicFunctionName = DeclName(SGM.M.getASTContext().getIdentifier("deinit"));

  RegularLocation loc(dd);
  if (dd->isImplicit())
    loc.markAutoGenerated();

  SILValue selfValue = emitSelfDecl(dd->getImplicitSelfDecl());

  // Create a basic block to jump to for the implicit destruction behavior
  // of releasing the elements and calling the superclass destructor.
  // We won't actually emit the block until we finish with the destructor body.
  prepareEpilog(Type(), false, CleanupLocation::get(loc));

  emitProfilerIncrement(dd->getBody());
  // Emit the destructor body.
  emitStmt(dd->getBody());

  Optional<SILValue> maybeReturnValue;
  SILLocation returnLoc(loc);
  std::tie(maybeReturnValue, returnLoc) = emitEpilogBB(loc);

  if (!maybeReturnValue)
    return;

  auto cleanupLoc = CleanupLocation::get(loc);

  // Note: the ivar destroyer is responsible for destroying the
  // instance variables before the object is actually deallocated.

  // Form a reference to the superclass -dealloc.
  Type superclassTy = dd->mapTypeIntoContext(cd->getSuperclass());
  assert(superclassTy && "Emitting Objective-C -dealloc without superclass?");
  ClassDecl *superclass = superclassTy->getClassOrBoundGenericClass();
  auto superclassDtorDecl = superclass->getDestructor();
  auto superclassDtor = SILDeclRef(superclassDtorDecl,
                                   SILDeclRef::Kind::Deallocator)
    .asForeign();
  auto superclassDtorType = SGM.Types.getConstantType(superclassDtor);
  SILValue superclassDtorValue = B.createObjCSuperMethod(
                                   cleanupLoc, selfValue, superclassDtor,
                                   superclassDtorType);

  // Call the superclass's -dealloc.
  SILType superclassSILTy = getLoweredLoadableType(superclassTy);
  SILValue superSelf = B.createUpcast(cleanupLoc, selfValue, superclassSILTy);
  assert(superSelf.getOwnershipKind() == ValueOwnershipKind::Owned);

  auto subMap
    = superclassTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                              superclass);

  auto substDtorType = superclassDtorType.substGenericArgs(SGM.M, subMap);
  CanSILFunctionType substFnType = substDtorType.castTo<SILFunctionType>();
  SILFunctionConventions dtorConv(substFnType, SGM.M);
  assert(substFnType->getSelfParameter().getConvention() ==
             ParameterConvention::Direct_Unowned &&
         "Objective C deinitializing destructor takes self as unowned");

  SmallVector<Substitution, 4> subs;
    if (auto *genericSig = superclass->getGenericSignature())
      genericSig->getSubstitutions(subMap, subs);

  B.createApply(cleanupLoc, superclassDtorValue, substDtorType,
                dtorConv.getSILResultType(), subs, superSelf);

  // We know that the givne value came in at +1, but we pass the relevant value
  // as unowned to the destructor. Create a fake balance for the verifier to be
  // happy.
  B.createEndLifetime(cleanupLoc, superSelf);

  // Return.
  B.createReturn(returnLoc, emitEmptyTuple(cleanupLoc));
}
