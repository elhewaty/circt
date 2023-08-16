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
#include "mlir/IR/SymbolTable.h"
#include "mlir/Support/LogicalResult.h"

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
  enum class Kind { Attr, Object, List };
  EvaluatorValue(Kind kind) : kind(kind) {}
  Kind getKind() const { return kind; }

private:
  const Kind kind;
};

/// Values which can be directly representable by MLIR attributes.
struct AttributeValue : EvaluatorValue {
  AttributeValue(Attribute attr) : EvaluatorValue(Kind::Attr), attr(attr) {}
  Attribute getAttr() const { return attr; }
  template <typename AttrTy>
  AttrTy getAs() const {
    return dyn_cast<AttrTy>(attr);
  }
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Attr;
  }

private:
  Attribute attr;
};

/// A List which contains variadic length of elements with the same type.
struct ListValue : EvaluatorValue {
  ListValue(om::ListType type, SmallVector<EvaluatorValuePtr> elements)
      : EvaluatorValue(Kind::List), type(type), elements(std::move(elements)) {}

  const auto &getElements() const { return elements; }

  /// Return the type of the value, which is a ListType.
  om::ListType getType() const { return type; }

  /// Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::List;
  }

private:
  om::ListType type;
  SmallVector<EvaluatorValuePtr> elements;
};

/// A composite Object, which has a type and fields.
struct ObjectValue : EvaluatorValue {
  ObjectValue(om::ClassOp cls, ObjectFields fields)
      : EvaluatorValue(Kind::Object), cls(cls), fields(std::move(fields)) {}
  om::ClassOp getClassOp() const { return cls; }
  const auto &getFields() const { return fields; }

  /// Return the type of the value, which is a ClassType.
  om::ClassType getType() const {
    auto clsConst = const_cast<ClassOp &>(cls);
    return ClassType::get(clsConst.getContext(),
                          FlatSymbolRefAttr::get(clsConst.getNameAttr()));
  }

  /// Implement LLVM RTTI.
  static bool classof(const EvaluatorValue *e) {
    return e->getKind() == Kind::Object;
  }

  /// Get a field of the Object by name.
  FailureOr<EvaluatorValuePtr> getField(StringAttr field);

  /// Get all the field names of the Object.
  ArrayAttr getFieldNames();

private:
  om::ClassOp cls;
  llvm::SmallDenseMap<StringAttr, EvaluatorValuePtr> fields;
};

} // namespace evaluator

using Object = evaluator::ObjectValue;
using EvaluatorValuePtr = evaluator::EvaluatorValuePtr;

SmallVector<EvaluatorValuePtr>
getEvaluatorValuesFromAttributes(ArrayRef<Attribute> attributes);

/// An Evaluator, which is constructed with an IR module and can instantiate
/// Objects. Further refinement is expected.
struct Evaluator {
  /// Construct an Evaluator with an IR module.
  Evaluator(ModuleOp mod);

  /// Instantiate an Object with its class name and actual parameters.
  FailureOr<std::shared_ptr<Object>>
  instantiate(StringAttr className, ArrayRef<EvaluatorValuePtr> actualParams);

  /// Get the Module this Evaluator is built from.
  mlir::ModuleOp getModule();

private:
  using Params = std::unique_ptr<SmallVector<EvaluatorValuePtr>>;
  /// Evaluate a Value in a Class body according to the small expression grammar
  /// described in the rationale document. The actual parameters are the values
  /// supplied at the current instantiation of the Class being evaluated.
  FailureOr<EvaluatorValuePtr> evaluateValue(Value value, Params &actualParams);

  /// Evaluator dispatch functions for the small expression grammar.
  FailureOr<EvaluatorValuePtr> evaluateParameter(BlockArgument formalParam,
                                                 Params &actualParams);

  FailureOr<EvaluatorValuePtr> evaluateConstant(ConstantOp op,
                                                Params &actualParams);
  FailureOr<EvaluatorValuePtr> evaluateObjectInstance(ObjectOp op,
                                                      Params &actualParams);
  FailureOr<EvaluatorValuePtr> evaluateObjectField(ObjectFieldOp op,
                                                   Params &actualParams);
  FailureOr<EvaluatorValuePtr> evaluateListCreate(ListCreateOp op,
                                                  Params &actualParams);

  /// The symbol table for the IR module the Evaluator was constructed with.
  /// Used to look up class definitions.
  SymbolTable symbolTable;

  /// Object storage. Currently used for memoizing calls to
  /// evaluateObjectInstance. Further refinement is expected.
  using Key = std::pair<Value, SmallVector<EvaluatorValuePtr>*>;
  DenseMap<Key, std::shared_ptr<evaluator::EvaluatorValue>> values;

  SmallVector<Params> history;

  DenseMap<Value, std::shared_ptr<evaluator::EvaluatorValue>> objects;
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
