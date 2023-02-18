//===- FIRRTLFieldSource.cpp - Field Source Analysis ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a basic points-to like analysis.
// This analysis tracks any aggregate generated by an operation and maps any
// value derived from indexing of that aggregate back to the source of the
// aggregate along with a path through the type from the source. In parallel,
// this tracks any value which is an alias for a writable storage element, even
// if scalar.  This is sufficient to allow any value used on the LHS of a
// connect to be traced to its source, and to track any value which is a read
// of a storage element back to the source storage element.
//
// There is a redundant walk of the IR going on since flow is walking backwards
// over operations we've already visited.  We need to refactor foldFlow so we
// can build up the flow incrementally.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/FIRRTLFieldSource.h"

using namespace circt;
using namespace firrtl;

FieldSource::FieldSource(Operation *operation) {
  FModuleOp mod = cast<FModuleOp>(operation);
  // All ports define locations
  for (auto port : mod.getBodyBlock()->getArguments())
    makeNodeForValue(port, port, {}, foldFlow(port));

  mod.walk<mlir::WalkOrder::PreOrder>([&](Operation *op) { visitOp(op); });
}

void FieldSource::visitOp(Operation *op) {
  if (auto sf = dyn_cast<SubfieldOp>(op))
    return visitSubfield(sf);
  if (auto si = dyn_cast<SubindexOp>(op))
    return visitSubindex(si);
  if (auto sa = dyn_cast<SubaccessOp>(op))
    return visitSubaccess(sa);
  if (isa<WireOp, RegOp, RegResetOp>(op))
    return makeNodeForValue(op->getResult(0), op->getResult(0), {},
                            foldFlow(op->getResult(0)));
  if (auto mem = dyn_cast<MemOp>(op))
    return visitMem(mem);
  if (auto inst = dyn_cast<InstanceOp>(op))
    return visitInst(inst);

  // Track all other definitions of aggregates.
  if (op->getNumResults()) {
    auto type = op->getResult(0).getType();
    if (dyn_cast<FIRRTLBaseType>(type) &&
        !cast<FIRRTLBaseType>(type).isGround())
      makeNodeForValue(op->getResult(0), op->getResult(0), {},
                       foldFlow(op->getResult(0)));
  }
}

void FieldSource::visitSubfield(SubfieldOp sf) {
  auto value = sf.getInput();
  const auto *node = nodeForValue(value);
  assert(node && "node should be in the map");
  auto sv = node->path;
  sv.push_back(sf.getFieldIndex());
  makeNodeForValue(sf.getResult(), node->src, sv, foldFlow(sf));
}

void FieldSource::visitSubindex(SubindexOp si) {
  auto value = si.getInput();
  const auto *node = nodeForValue(value);
  if (!node) {
    si.dump();
    value.dump();
  }
  assert(node && "node should be in the map");
  auto sv = node->path;
  sv.push_back(si.getIndex());
  makeNodeForValue(si.getResult(), node->src, sv, foldFlow(si));
}

void FieldSource::visitSubaccess(SubaccessOp sa) {
  auto value = sa.getInput();
  const auto *node = nodeForValue(value);
  assert(node && "node should be in the map");
  auto sv = node->path;
  sv.push_back(-1);
  makeNodeForValue(sa.getResult(), node->src, sv, foldFlow(sa));
}

void FieldSource::visitMem(MemOp mem) {
  for (auto r : mem.getResults())
    makeNodeForValue(r, r, {}, foldFlow(r));
}

void FieldSource::visitInst(InstanceOp inst) {
  for (auto r : inst.getResults())
    makeNodeForValue(r, r, {}, foldFlow(r));
}

const FieldSource::PathNode *FieldSource::nodeForValue(Value v) const {
  auto ii = paths.find(v);
  if (ii == paths.end())
    return nullptr;
  return &ii->second;
}

void FieldSource::makeNodeForValue(Value dst, Value src, ArrayRef<int64_t> path,
                                   Flow flow) {
  auto ii = paths.try_emplace(dst, src, path, flow);
  (void)ii;
  assert(ii.second && "Double insert into the map");
}
