//===- Evaluator.h - Object Model dialect evaluator -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Object Model dialect declaration.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_OM_EVALUATOR_EVALUATOR_H
#define CIRCT_DIALECT_OM_EVALUATOR_EVALUATOR_H

#include "circt/Dialect/OM/OMOps.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <queue>

namespace circt {
namespace om {

namespace evaluator {
struct EvaluatorValue;

/// A value of an object in memory. It is either a composite Object, or a
/// primitive Attribute. Further refinement is expected.
using EvaluatorValuePtr = std::shared_ptr<EvaluatorValue>;

/// The fields of a composite Object, currently represented as a map. Further
/// refinement is expected.
using ObjectFields = SmallDenseMap<StringAttr, EvaluatorValuePtr>;

/// Base class for evaluator runtime values.
/// Enables the shared_from_this functionality so Object pointers can be passed
/// through the CAPI and unwrapped back into C++ smart pointers with the
/// appropriate reference count.
struct EvaluatorValue : std::enable_shared_from_this<EvaluatorValue> {
  // Implement LLVM RTTI.
  enum class Kind { Attr, Object, List, Tuple, Map, Reference };
  EvaluatorValue(MLIRContext *ctx, Kind kind) : kind(kind), ctx(ctx) {}
  Kind getKind() const { return kind; }
  MLIRContext *getContext() const { return ctx; }

  // Return true the value is fully evaluated.
  bool isFullyEvaluated() const { return fullyEvaluated; }
  void markFullyEvaluated() { fullyEvaluated = true; }

  // Return a MLIR type which the value represents.
  Type getType() const;

  // Finalize the evaluator value. Strip intermidiate reference values.
  LogicalResult finalize();

private:
  const Kind kind;
  MLIRContext *ctx;
  bool fullyEvaluated = false;
  bool finalized = false;
};

struct ReferenceValue : EvaluatorValue {
  ReferenceValue(MLIRContext *ctx, EvaluatorValuePtr value)
      : EvaluatorValue(ctx, Kind::Reference), value(value) {}

  ReferenceValue(Type type)
      : EvaluatorValue(type.getContext(), Kind::Reference), value(nullptr),
        type(type) {}

  // Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Reference;
  }

  Type getValueType() const { return type; }
  EvaluatorValuePtr getValue() const { return value; }
  void setValue(EvaluatorValuePtr newValue) {
    value = newValue;
    markFullyEvaluated();
  }

  LogicalResult finalizeImpl() {
    auto result = getStripValue();
    if (failed(result))
      return result;
    value = std::move(result.value());
    return success();
  }

  FailureOr<EvaluatorValuePtr> getStripValue() const {
    llvm::SmallPtrSet<ReferenceValue *, 4> visited;
    auto currentValue = value;
    while (auto *v = dyn_cast<ReferenceValue>(currentValue.get())) {
      // Detect a cycle.
      if (!visited.insert(v).second)
        return failure();
      currentValue = v->getValue();
    }
    return success(currentValue);
  }

private:
  EvaluatorValuePtr value;
  Type type;
};

/// Values which can be directly representable by MLIR attributes.
struct AttributeValue : EvaluatorValue {
  AttributeValue(Attribute attr)
      : EvaluatorValue(attr.getContext(), Kind::Attr), attr(attr) {
    markFullyEvaluated();
  }

  Attribute getAttr() const { return attr; }
  template <typename AttrTy>
  AttrTy getAs() const {
    return dyn_cast<AttrTy>(attr);
  }
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Attr;
  }

  LogicalResult finalizeImpl() { return success(); }

  Type getType() const { return attr.cast<TypedAttr>().getType(); }

private:
  Attribute attr = {};
};

static inline LogicalResult finalizeEvaluatorValue(EvaluatorValuePtr &value) {
  if (failed(value->finalize()))
    return failure();
  if (auto ref = llvm::dyn_cast<ReferenceValue>(value.get())) {
    auto v = ref->getStripValue();
    if (failed(v))
      return v;
    value = v.value();
  }
  return success();
}

/// A List which contains variadic length of elements with the same type.
struct ListValue : EvaluatorValue {
  ListValue(om::ListType type, SmallVector<EvaluatorValuePtr> elements)
      : EvaluatorValue(type.getContext(), Kind::List), type(type),
        elements(std::move(elements)) {
    markFullyEvaluated();
  }

  void setElements(SmallVector<EvaluatorValuePtr> newElements) {
    elements = std::move(newElements);
    markFullyEvaluated();
  }

  LogicalResult finalizeImpl() {
    assert(isFullyEvaluated());
    for (auto &value : elements) {
      if (failed(finalizeEvaluatorValue(value)))
        return failure();
    }
    return success();
  }

  // Partially evaluated value.
  ListValue(om::ListType type)
      : EvaluatorValue(type.getContext(), Kind::List), type(type) {}

  const auto &getElements() const { return elements; }

  /// Return the type of the value, which is a ListType.
  om::ListType getListType() const { return type; }

  /// Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::List;
  }

private:
  om::ListType type;
  SmallVector<EvaluatorValuePtr> elements;
};

/// A Map value.
struct MapValue : EvaluatorValue {
  MapValue(om::MapType type, DenseMap<Attribute, EvaluatorValuePtr> elements)
      : EvaluatorValue(type.getContext(), Kind::Map), type(type),
        elements(std::move(elements)) {
    markFullyEvaluated();
  }

  void setElements(DenseMap<Attribute, EvaluatorValuePtr> newElements) {
    elements = std::move(newElements);
    markFullyEvaluated();
  }

  LogicalResult finalizeImpl() {
    assert(isFullyEvaluated());
    for (auto &&[e, value] : elements)
      if (failed(finalizeEvaluatorValue(value)))
        return failure();
    return success();
  }

  // Partially evaluated value.
  MapValue(om::MapType type)
      : EvaluatorValue(type.getContext(), Kind::Map), type(type) {}
  const auto &getElements() const { return elements; }

  /// Return the type of the value, which is a MapType.
  om::MapType getMapType() const { return type; }

  /// Return an array of keys in the ascending order.
  ArrayAttr getKeys();

  /// Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Map;
  }

private:
  om::MapType type;
  DenseMap<Attribute, EvaluatorValuePtr> elements;
};

/// A composite Object, which has a type and fields.
struct ObjectValue : EvaluatorValue {
  ObjectValue(om::ClassOp cls, ObjectFields fields)
      : EvaluatorValue(cls.getContext(), Kind::Object), cls(cls),
        fields(std::move(fields)) {
    markFullyEvaluated();
  }

  // Partially evaluated value.
  ObjectValue(om::ClassOp cls)
      : EvaluatorValue(cls.getContext(), Kind::Object), cls(cls) {}

  om::ClassOp getClassOp() const { return cls; }
  const auto &getFields() const { return fields; }

  void setFields(llvm::SmallDenseMap<StringAttr, EvaluatorValuePtr> newFields) {
    fields = std::move(newFields);
    markFullyEvaluated();
  }

  /// Return the type of the value, which is a ClassType.
  om::ClassType getObjectType() const {
    auto clsConst = const_cast<ClassOp &>(cls);
    return ClassType::get(clsConst.getContext(),
                          FlatSymbolRefAttr::get(clsConst.getNameAttr()));
  }

  Type getType() const { return getObjectType(); }

  /// Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Object;
  }

  /// Get a field of the Object by name.
  FailureOr<EvaluatorValuePtr> getField(StringAttr field);
  FailureOr<EvaluatorValuePtr> getField(StringRef field) {
    return getField(StringAttr::get(getContext(), field));
  }

  /// Get all the field names of the Object.
  ArrayAttr getFieldNames();

  LogicalResult finalizeImpl() {
    for (auto &&[e, value] : fields)
      if (failed(finalizeEvaluatorValue(value)))
        return failure();

    return success();
  }

private:
  om::ClassOp cls;
  llvm::SmallDenseMap<StringAttr, EvaluatorValuePtr> fields;
};

/// Tuple values.
struct TupleValue : EvaluatorValue {
  using TupleElements = llvm::SmallVector<EvaluatorValuePtr>;
  TupleValue(TupleType type, TupleElements tupleElements)
      : EvaluatorValue(type.getContext(), Kind::Tuple), type(type),
        elements(std::move(tupleElements)) {
    markFullyEvaluated();
  }

  // Partially evaluated value.
  TupleValue(TupleType type)
      : EvaluatorValue(type.getContext(), Kind::Tuple), type(type) {}

  void setElements(TupleElements newElements) {
    elements = std::move(newElements);
    markFullyEvaluated();
  }

  LogicalResult finalizeImpl() {
    for (auto &&value : elements)
      if (failed(finalizeEvaluatorValue(value)))
        return failure();

    return success();
  }
  /// Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Tuple;
  }

  /// Return the type of the value, which is a TupleType.
  TupleType getTupleType() const { return type; }

  const TupleElements &getElements() const { return elements; }

private:
  TupleType type;
  TupleElements elements;
};

} // namespace evaluator

using Object = evaluator::ObjectValue;
using EvaluatorValuePtr = evaluator::EvaluatorValuePtr;

SmallVector<EvaluatorValuePtr>
getEvaluatorValuesFromAttributes(MLIRContext *context,
                                 ArrayRef<Attribute> attributes);

/// An Evaluator, which is constructed with an IR module and can instantiate
/// Objects. Further refinement is expected.
struct Evaluator {
  /// Construct an Evaluator with an IR module.
  Evaluator(ModuleOp mod);

  /// Instantiate an Object with its class name and actual parameters.
  FailureOr<evaluator::EvaluatorValuePtr>
  instantiate(StringAttr className, ArrayRef<EvaluatorValuePtr> actualParams);

  /// Get the Module this Evaluator is built from.
  mlir::ModuleOp getModule();

  FailureOr<evaluator::EvaluatorValuePtr> getPartiallyEvaluatedValue(Type type);

  using ActualParameters =
      SmallVectorImpl<std::shared_ptr<evaluator::EvaluatorValue>> *;

  using Key = std::pair<Value, ActualParameters>;

private:
  bool isFullyEvaluated(Value value, ActualParameters key) {
    return isFullyEvaluated({value, key});
  }

  bool isFullyEvaluated(Key key) {
    auto val = objects.lookup(key);
    return val && val->isFullyEvaluated();
  }

  EvaluatorValuePtr lookupEvaluatorValue(Key key) {
    return objects.lookup(key);
  }

  FailureOr<EvaluatorValuePtr> getOrCreateValue(Value value,
                                                ActualParameters actualParams);
  FailureOr<EvaluatorValuePtr>
  allocateObjectInstance(StringAttr clasName, ActualParameters actualParams);

  /// Evaluate a Value in a Class body according to the small expression grammar
  /// described in the rationale document. The actual parameters are the values
  /// supplied at the current instantiation of the Class being evaluated.
  FailureOr<EvaluatorValuePtr> evaluateValue(Value value,
                                             ActualParameters actualParams);

  /// Evaluator dispatch functions for the small expression grammar.
  FailureOr<EvaluatorValuePtr> evaluateParameter(BlockArgument formalParam,
                                                 ActualParameters actualParams);

  FailureOr<EvaluatorValuePtr> evaluateConstant(ConstantOp op,
                                                ActualParameters actualParams);
  /// Instantiate an Object with its class name and actual parameters.
  FailureOr<EvaluatorValuePtr>
  evaluateObjectInstance(StringAttr className, ActualParameters actualParams,
                         Key caller = {});
  FailureOr<EvaluatorValuePtr>
  evaluateObjectInstance(ObjectOp op, ActualParameters actualParams);
  FailureOr<EvaluatorValuePtr>
  evaluateObjectField(ObjectFieldOp op, ActualParameters actualParams);
  FailureOr<EvaluatorValuePtr>
  evaluateListCreate(ListCreateOp op, ActualParameters actualParams);
  FailureOr<EvaluatorValuePtr>
  evaluateTupleCreate(TupleCreateOp op, ActualParameters actualParams);
  FailureOr<EvaluatorValuePtr> evaluateTupleGet(TupleGetOp op,
                                                ActualParameters actualParams);
  FailureOr<evaluator::EvaluatorValuePtr>
  evaluateMapCreate(MapCreateOp op, ActualParameters actualParams);

  /// The symbol table for the IR module the Evaluator was constructed with.
  /// Used to look up class definitions.
  SymbolTable symbolTable;

  SmallVector<
      std::unique_ptr<SmallVector<std::shared_ptr<evaluator::EvaluatorValue>>>>
      actualParametersBuffers;

  // A worklist that tracks values which needs to be fully evaluated.
  std::queue<Key> worklist;

  /// Object storage. Currently used for memoizing calls to
  /// evaluateObjectInstance. Further refinement is expected.
  DenseMap<Key, std::shared_ptr<evaluator::EvaluatorValue>> objects;
};

/// Helper to enable printing objects in Diagnostics.
static inline mlir::Diagnostic &
operator<<(mlir::Diagnostic &diag,
           const evaluator::EvaluatorValue &evaluatorValue) {
  if (auto *attr = llvm::dyn_cast<evaluator::AttributeValue>(&evaluatorValue))
    diag << attr->getAttr();
  else if (auto *object =
               llvm::dyn_cast<evaluator::ObjectValue>(&evaluatorValue))
    diag << "Object(" << object->getType() << ")";
  else if (auto *list = llvm::dyn_cast<evaluator::ListValue>(&evaluatorValue))
    diag << "List(" << list->getType() << ")";
  else if (auto *map = llvm::dyn_cast<evaluator::MapValue>(&evaluatorValue))
    diag << "Map(" << map->getType() << ")";
  else
    assert(false && "unhandled evaluator value");
  return diag;
}

/// Helper to enable printing objects in Diagnostics.
static inline mlir::Diagnostic &
operator<<(mlir::Diagnostic &diag, const EvaluatorValuePtr &evaluatorValue) {
  return diag << *evaluatorValue.get();
}

} // namespace om
} // namespace circt

#endif // CIRCT_DIALECT_OM_EVALUATOR_EVALUATOR_H
