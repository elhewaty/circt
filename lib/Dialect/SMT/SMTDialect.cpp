//===- SMTDialect.cpp - SMT dialect implementation ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/SMT/SMTDialect.h"
#include "circt/Dialect/SMT/SMTAttributes.h"
#include "circt/Dialect/SMT/SMTOps.h"
#include "circt/Dialect/SMT/SMTTypes.h"

using namespace circt;
using namespace smt;

void SMTDialect::initialize() {
  registerAttributes();
  registerTypes();
  addOperations<
#define GET_OP_LIST
#include "circt/Dialect/SMT/SMT.cpp.inc"
      >();
}

Operation *SMTDialect::materializeConstant(OpBuilder &builder, Attribute value,
                                           Type type, Location loc) {
  // BitVectorType constants can materialize into smt.bv.constant
  if (auto bvType = type.dyn_cast<BitVectorType>()) {
    if (auto attrValue = value.dyn_cast<BitVectorAttr>()) {
      bool typesMatch = bvType == attrValue.getType();
      assert(typesMatch && "attribute and desired result types have to match");
      return builder.create<BVConstantOp>(loc, attrValue);
    }
  }

  return nullptr;
}

#include "circt/Dialect/SMT/SMTDialect.cpp.inc"
#include "circt/Dialect/SMT/SMTEnums.cpp.inc"
