//===- EmitHGLDD.cpp - HGLDD debug info emission --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Analysis/DebugInfo.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Debug/DebugOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVOps.h"
#include "circt/Support/Namespace.h"
#include "circt/Target/DebugInfo.h"
#include "mlir/IR/Threading.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Support/IndentedOstream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"

#define DEBUG_TYPE "di"

using namespace mlir;
using namespace circt;
using namespace debug;

using llvm::MapVector;
using llvm::SmallMapVector;

using JValue = llvm::json::Value;
using JArray = llvm::json::Array;
using JObject = llvm::json::Object;
using JOStream = llvm::json::OStream;

/// Walk the given `loc` and collect file-line-column locations that we want to
/// report as source ("HGL") locations or as emitted Verilog ("HDL") locations.
///
/// This function treats locations inside a `NameLoc` called "emitted" or a
/// `FusedLoc` with the metadata attribute string "verilogLocations" as emitted
/// Verilog locations. All other locations are considered to be source
/// locations.
///
/// The `level` parameter is used to track into how many "emitted" or
/// "verilogLocations" we have already descended. For every one of those we look
/// through the level gets decreased by one. File-line-column locations are only
/// collected at level 0. We don't descend into "emitted" or "verilogLocations"
/// once we've reached level 0. This effectively makes the `level` parameter
/// decide behind how many layers of "emitted" or "verilogLocations" we want to
/// collect file-line-column locations. Setting this to 0 effectively collects
/// source locations, i.e., everything not marked as emitted. Setting this to 1
/// effectively collects emitted locations, i.e., nothing that isn't behind
/// exactly one layer of "emitted" or "verilogLocations".
static void findLocations(Location loc, unsigned level,
                          SmallVectorImpl<FileLineColLoc> &locs) {
  if (auto nameLoc = dyn_cast<NameLoc>(loc)) {
    if (nameLoc.getName() == "emitted")
      if (level-- == 0)
        return;
    findLocations(nameLoc.getChildLoc(), level, locs);
  } else if (auto fusedLoc = dyn_cast<FusedLoc>(loc)) {
    auto strAttr = dyn_cast_or_null<StringAttr>(fusedLoc.getMetadata());
    if (strAttr && strAttr.getValue() == "verilogLocations")
      if (level-- == 0)
        return;
    for (auto innerLoc : fusedLoc.getLocations())
      findLocations(innerLoc, level, locs);
  } else if (auto fileLoc = dyn_cast<FileLineColLoc>(loc)) {
    if (level == 0)
      locs.push_back(fileLoc);
  }
}

/// Find the best location to report as source location ("HGL", emitted = false)
/// or as emitted location ("HDL", emitted = true). Returns any non-FIR file it
/// finds, and only falls back to FIR files if nothing else is found.
static FileLineColLoc findBestLocation(Location loc, bool emitted) {
  SmallVector<FileLineColLoc> locs;
  findLocations(loc, emitted ? 1 : 0, locs);
  for (auto loc : locs)
    if (!loc.getFilename().getValue().endswith(".fir"))
      return loc;
  for (auto loc : locs)
    if (loc.getFilename().getValue().endswith(".fir"))
      return loc;
  return {};
}

// Allow `json::Value`s to be used as map keys for the purpose of struct
// definition uniquification. This abuses the `null` and `[null]` JSON values as
// markers, and uses a very inefficient hashing of the value's JSON string.
namespace llvm {
template <>
struct DenseMapInfo<JValue> {
  static JValue getEmptyKey() { return nullptr; }
  static JValue getTombstoneKey() { return JArray({nullptr}); }
  static unsigned getHashValue(const JValue &x) {
    SmallString<128> buffer;
    llvm::raw_svector_ostream(buffer) << x;
    return hash_value(buffer);
  }
  static bool isEqual(const JValue &a, const JValue &b) { return a == b; }
};
} // namespace llvm

//===----------------------------------------------------------------------===//
// HGLDD File Emission
//===----------------------------------------------------------------------===//

namespace {

/// An emitted type.
struct EmittedType {
  StringRef name;
  SmallVector<int64_t, 1> packedDims;
  SmallVector<int64_t, 1> unpackedDims;

  EmittedType() = default;
  EmittedType(StringRef name) : name(name) {}
  EmittedType(Type type) {
    while (type) {
      type = hw::getCanonicalType(type);
      if (auto inoutType = dyn_cast<hw::InOutType>(type)) {
        type = inoutType.getElementType();
        continue;
      }
      if (hw::isHWIntegerType(type)) {
        name = "logic";
        addPackedDim(hw::getBitWidth(type));
      }
      break;
    }
  }

  void addPackedDim(int64_t dim) { packedDims.push_back(dim); }
  void addUnpackedDim(int64_t dim) { unpackedDims.push_back(dim); }

  operator bool() const { return !name.empty(); }

  static JArray emitDims(ArrayRef<int64_t> dims, bool skipFirstLen1Dim) {
    JArray json;
    if (skipFirstLen1Dim && !dims.empty() && dims[0] == 1)
      dims = dims.drop_front();
    for (auto dim : llvm::reverse(dims)) {
      json.push_back(dim - 1);
      json.push_back(0);
    }
    return json;
  }
  JArray emitPackedDims() const { return emitDims(packedDims, true); }
  JArray emitUnpackedDims() const { return emitDims(unpackedDims, false); }
};

/// An emitted expression and its type.
struct EmittedExpr {
  JValue expr = nullptr;
  EmittedType type;
  operator bool() const { return expr != nullptr && type; }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const EmittedType &type) {
  if (!type)
    return os << "<null>";
  os << type.name;
  for (auto dim : type.packedDims)
    os << '[' << dim << ']';
  if (!type.unpackedDims.empty()) {
    os << '$';
    for (auto dim : type.unpackedDims)
      os << '[' << dim << ']';
  }
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const EmittedExpr &expr) {
  if (!expr)
    return os << "<null>";
  return os << expr.expr << " : " << expr.type;
}

/// Contextual information for a single HGLDD file to be emitted.
struct FileEmitter {
  const EmitHGLDDOptions *options = nullptr;
  SmallVector<DIModule *> modules;
  SmallString<64> outputFileName;
  StringAttr hdlFile;
  SmallMapVector<StringAttr, unsigned, 8> sourceFiles;
  Namespace objectNamespace;
  SmallMapVector<JValue, StringRef, 8> structDefs;
  SmallString<128> structNameHint;

  void emit(llvm::raw_ostream &os);
  void emit(JOStream &json);
  JValue emitLoc(FileLineColLoc loc, FileLineColLoc endLoc, bool emitted);
  void emitModule(JOStream &json, DIModule *module);
  void emitInstance(JOStream &json, DIInstance *instance);
  void emitVariable(JOStream &json, DIVariable *variable);
  EmittedExpr emitExpression(Value value);

  /// Get a numeric index for the given `sourceFile`. Populates `sourceFiles`
  /// with a unique ID assignment for each source file.
  unsigned getSourceFile(StringAttr sourceFile, bool emitted) {
    // Apply the source file prefix if this is a source file (emitted = false).
    if (!emitted && !options->sourceFilePrefix.empty() &&
        !llvm::sys::path::is_absolute(sourceFile.getValue())) {
      SmallString<64> buffer;
      buffer = options->sourceFilePrefix;
      llvm::sys::path::append(buffer, sourceFile.getValue());
      sourceFile = StringAttr::get(sourceFile.getContext(), buffer);
    }

    // Apply the output file prefix if this is an outpu file (emitted = true).
    if (emitted && !options->outputFilePrefix.empty() &&
        !llvm::sys::path::is_absolute(sourceFile.getValue())) {
      SmallString<64> buffer;
      buffer = options->outputFilePrefix;
      llvm::sys::path::append(buffer, sourceFile.getValue());
      sourceFile = StringAttr::get(sourceFile.getContext(), buffer);
    }

    auto &slot = sourceFiles[sourceFile];
    if (slot == 0)
      slot = sourceFiles.size();
    return slot;
  }

  /// Find the best location and, if one is found, emit it under the given
  /// `fieldName`.
  void findAndEmitLoc(JOStream &json, StringRef fieldName, Location loc,
                      bool emitted) {
    if (auto fileLoc = findBestLocation(loc, emitted))
      json.attribute(fieldName, emitLoc(fileLoc, {}, emitted));
  }

  /// Find the best location and, if one is found, emit it under the given
  /// `fieldName`. If none is found, guess a location by looking at nested
  /// operations.
  void findAndEmitLocOrGuess(JOStream &json, StringRef fieldName, Operation *op,
                             bool emitted) {
    if (auto fileLoc = findBestLocation(op->getLoc(), emitted)) {
      json.attribute(fieldName, emitLoc(fileLoc, {}, emitted));
      return;
    }

    // Otherwise do a majority vote on the file name to report as location. Walk
    // the operation, collect all locations, and group them by file name.
    SmallMapVector<StringAttr, std::pair<SmallVector<FileLineColLoc>, unsigned>,
                   4>
        locsByFile;
    op->walk([&](Operation *subop) {
      // Consider operations.
      if (auto fileLoc = findBestLocation(subop->getLoc(), emitted))
        locsByFile[fileLoc.getFilename()].first.push_back(fileLoc);

      // Consider block arguments.
      for (auto &region : subop->getRegions())
        for (auto &block : region)
          for (auto arg : block.getArguments())
            if (auto fileLoc = findBestLocation(arg.getLoc(), emitted))
              locsByFile[fileLoc.getFilename()].first.push_back(fileLoc);
    });

    // Give immediate block arguments a larger weight.
    for (auto &region : op->getRegions())
      for (auto &block : region)
        for (auto arg : block.getArguments())
          if (auto fileLoc = findBestLocation(arg.getLoc(), emitted))
            locsByFile[fileLoc.getFilename()].second += 10;

    if (locsByFile.empty())
      return;

    // Pick the highest-scoring file and create a location from it.
    llvm::sort(locsByFile, [](auto &a, auto &b) {
      return (a.second.first.size() + a.second.second) >
             (b.second.first.size() + a.second.second);
    });

    auto &locs = locsByFile.front().second.first;
    llvm::sort(locs, [](auto &a, auto &b) {
      if (a.getLine() < b.getLine())
        return true;
      if (a.getLine() > b.getLine())
        return false;
      if (a.getColumn() < b.getColumn())
        return true;
      return false;
    });

    json.attribute(fieldName, emitLoc(locs.front(), locs.back(), emitted));
  }

  /// Find the best locations to report for HGL and HDL and set them as fields
  /// on the `into` JSON object.
  void findAndSetLocs(JObject &into, Location loc) {
    if (auto fileLoc = findBestLocation(loc, false))
      into["hgl_loc"] = emitLoc(fileLoc, {}, false);
    if (auto fileLoc = findBestLocation(loc, true))
      into["hdl_loc"] = emitLoc(fileLoc, {}, true);
  }
};

} // namespace

void FileEmitter::emit(llvm::raw_ostream &os) {
  JOStream json(os, 2);
  emit(json);
  os << "\n";
}

void FileEmitter::emit(JOStream &json) {
  for (auto *module : modules)
    objectNamespace.newName(module->name.getValue());

  // The "HGLDD" header field needs to be the first in the JSON file (which
  // violates the JSON spec, but what can you do). But we only know after module
  // emission what the contents of the header will be.
  SmallVector<std::string, 16> rawObjects;
  for (auto *module : modules) {
    llvm::raw_string_ostream objectOS(rawObjects.emplace_back());
    JOStream objectJson(objectOS, 2);
    objectJson.arrayBegin(); // dummy for indentation
    objectJson.arrayBegin(); // dummy for indentation
    emitModule(objectJson, module);
    objectJson.arrayEnd(); // dummy for indentation
    objectJson.arrayEnd(); // dummy for indentation
  }

  std::optional<unsigned> hdlFileIndex;
  if (hdlFile)
    hdlFileIndex = getSourceFile(hdlFile, true);

  json.objectBegin();
  json.attributeObject("HGLDD", [&] {
    json.attribute("version", "1.0");
    json.attributeArray("file_info", [&] {
      for (auto [file, index] : sourceFiles)
        json.value(file.getValue());
    });
    if (hdlFileIndex)
      json.attribute("hdl_file_index", *hdlFileIndex);
  });
  json.attributeArray("objects", [&] {
    for (auto &[structDef, name] : structDefs)
      json.value(structDef);
    for (auto &rawObject : rawObjects)
      json.rawValue(StringRef(rawObject)
                        .drop_front()
                        .drop_back()
                        .trim()
                        .drop_front()
                        .drop_back()
                        .trim());
  });
  json.objectEnd();
}

JValue FileEmitter::emitLoc(FileLineColLoc loc, FileLineColLoc endLoc,
                            bool emitted) {
  JObject obj;
  obj["file"] = getSourceFile(loc.getFilename(), emitted);
  if (loc.getLine()) {
    obj["begin_line"] = loc.getLine();
    obj["end_line"] = loc.getLine();
  }
  if (loc.getColumn()) {
    obj["begin_column"] = loc.getColumn();
    obj["end_column"] = loc.getColumn();
  }
  if (endLoc) {
    if (endLoc.getLine())
      obj["end_line"] = endLoc.getLine();
    if (endLoc.getColumn())
      obj["end_column"] = endLoc.getColumn();
  }
  return obj;
}

StringAttr getVerilogModuleName(DIModule &module) {
  if (auto *op = module.op)
    if (auto attr = op->getAttrOfType<StringAttr>("verilogName"))
      return attr;
  return module.name;
}

StringAttr getVerilogInstanceName(DIInstance &inst) {
  if (auto *op = inst.op)
    if (auto attr = op->getAttrOfType<StringAttr>("hw.verilogName"))
      return attr;
  return inst.name;
}

/// Emit the debug info for a `DIModule`.
void FileEmitter::emitModule(JOStream &json, DIModule *module) {
  structNameHint = module->name.getValue();
  json.objectBegin();
  json.attribute("kind", "module");
  json.attribute("obj_name", module->name.getValue()); // HGL
  json.attribute("module_name",
                 getVerilogModuleName(*module).getValue()); // HDL
  if (module->isExtern)
    json.attribute("isExtModule", 1);
  if (auto *op = module->op) {
    findAndEmitLocOrGuess(json, "hgl_loc", op, false);
    findAndEmitLoc(json, "hdl_loc", op->getLoc(), true);
  }
  json.attributeArray("port_vars", [&] {
    for (auto *var : module->variables)
      emitVariable(json, var);
  });
  json.attributeArray("children", [&] {
    for (auto *instance : module->instances)
      emitInstance(json, instance);
  });
  json.objectEnd();
}

/// Emit the debug info for a `DIInstance`.
void FileEmitter::emitInstance(JOStream &json, DIInstance *instance) {
  json.objectBegin();
  json.attribute("name", instance->name.getValue());
  auto verilogName = getVerilogInstanceName(*instance);
  if (verilogName != instance->name)
    json.attribute("hdl_obj_name", verilogName.getValue());
  json.attribute("obj_name", instance->module->name.getValue()); // HGL
  json.attribute("module_name",
                 getVerilogModuleName(*instance->module).getValue()); // HDL
  if (auto *op = instance->op) {
    findAndEmitLoc(json, "hgl_loc", op->getLoc(), false);
    findAndEmitLoc(json, "hdl_loc", op->getLoc(), true);
  }
  json.objectEnd();
}

/// Emit the debug info for a `DIVariable`.
void FileEmitter::emitVariable(JOStream &json, DIVariable *variable) {
  json.objectBegin();
  json.attribute("var_name", variable->name.getValue());
  findAndEmitLoc(json, "hgl_loc", variable->loc, false);
  findAndEmitLoc(json, "hdl_loc", variable->loc, true);

  EmittedExpr emitted;
  if (auto value = variable->value) {
    auto structNameHintLen = structNameHint.size();
    structNameHint += '_';
    structNameHint += variable->name.getValue();
    emitted = emitExpression(value);
    structNameHint.resize(structNameHintLen);
  }

  LLVM_DEBUG(llvm::dbgs() << "- " << variable->name << ": " << emitted << "\n");
  if (emitted) {
    json.attributeBegin("value");
    json.rawValue([&](auto &os) { os << emitted.expr; });
    json.attributeEnd();
    json.attribute("type_name", emitted.type.name);
    if (auto dims = emitted.type.emitPackedDims(); !dims.empty())
      json.attribute("packed_range", std::move(dims));
    if (auto dims = emitted.type.emitUnpackedDims(); !dims.empty())
      json.attribute("unpacked_range", std::move(dims));
  }

  json.objectEnd();
}

/// Emit the DI expression necessary to materialize a value.
EmittedExpr FileEmitter::emitExpression(Value value) {
  if (auto blockArg = dyn_cast<BlockArgument>(value)) {
    auto module = dyn_cast<hw::HWModuleOp>(blockArg.getOwner()->getParentOp());
    if (!module)
      return {};
    auto name = module.getInputNameAttr(blockArg.getArgNumber());
    if (!name)
      return {};
    return {JObject({{"sig_name", name.getValue()}}), value.getType()};
  }

  auto result = cast<OpResult>(value);
  auto *op = result.getOwner();

  // If the operation has only this one result and is named in some form, use
  // that name.
  if (op->getNumResults() == 1) {
    // If a `hw.verilogName` is available, emit the value as just a reference to
    // that name.
    if (auto name = op->getAttrOfType<StringAttr>("hw.verilogName");
        name && !name.empty())
      return {JObject({{"sig_name", name.getValue()}}), result.getType()};

    // Use the "name" attribute of certain Verilog-visible ops directly.
    if (auto name = op->getAttrOfType<StringAttr>("name");
        name && !name.empty() &&
        isa<hw::WireOp, sv::WireOp, sv::RegOp, sv::LogicOp>(op))
      return {JObject({{"sig_name", name.getValue()}}), result.getType()};
  }

  // Emit references to instance ports as `<instName>.<portName>`.
  if (auto instOp = dyn_cast<hw::InstanceOp>(op)) {
    auto instName = instOp->getAttrOfType<StringAttr>("hw.verilogName");
    if (!instName)
      instName = instOp.getInstanceNameAttr();
    if (!instName)
      return {};
    auto portName = instOp
                        .getPortList()[instOp.getPortIdForOutputId(
                            result.getResultNumber())]
                        .name;
    if (!portName)
      return {};
    auto inner = JObject({{"sig_name", instName.getValue()}});
    return {JObject({
                {"var_ref", std::move(inner)},
                {"field", portName.getValue()},
            }),
            result.getType()};
  }

  // Emit constants directly.
  if (auto constOp = dyn_cast<hw::ConstantOp>(op)) {
    // Determine the bit width of the constant.
    auto type = constOp.getType();
    auto width = hw::getBitWidth(type);

    // Emit zero-width constants as a 1-bit zero value. This ensures we get a
    // proper Verilog-compatible value as a result. Expressions like
    // concatenation should instead skip zero-width values.
    if (width < 1)
      return {JObject({{"bit_vector", "0"}}),
              IntegerType::get(op->getContext(), 1)};

    // Serialize the constant as a base-2 binary string.
    SmallString<64> buffer;
    buffer.reserve(width);
    constOp.getValue().toStringUnsigned(buffer, 2);

    // Pad the string with leading zeros such that it is exactly of the required
    // width. This is needed since tools will use the string length to determine
    // the width of the constant.
    std::reverse(buffer.begin(), buffer.end());
    while (buffer.size() < (size_t)width)
      buffer += '0';
    std::reverse(buffer.begin(), buffer.end());
    assert(buffer.size() == (size_t)width);

    return {JObject({{"bit_vector", buffer}}), type};
  }

  // Emit structs as assignment patterns and generate corresponding struct
  // definitions for inclusion in the main "objects" array.
  if (auto structOp = dyn_cast<debug::StructOp>(op)) {
    // Collect field names, expressions, and types.
    auto structNameHintLen = structNameHint.size();
    std::vector<JValue> values;
    SmallVector<std::tuple<EmittedType, StringAttr, Location>> types;
    for (auto [nameAttr, field] :
         llvm::zip(structOp.getNamesAttr(), structOp.getFields())) {
      auto name = cast<StringAttr>(nameAttr);
      structNameHint += '_';
      structNameHint += name.getValue();
      if (auto value = emitExpression(field)) {
        values.push_back(value.expr);
        types.push_back({value.type, name, field.getLoc()});
      }
      structNameHint.resize(structNameHintLen);
    }

    // Emit empty structs as 0 `bit`.
    if (values.empty())
      return {JObject({{"integer_num", 0}}), EmittedType("bit")};

    // Assemble the struct type definition.
    JArray fieldDefs;
    for (auto [type, name, loc] : types) {
      JObject fieldDef;
      fieldDef["var_name"] = name.getValue();
      fieldDef["type_name"] = type.name;
      if (auto dims = type.emitPackedDims(); !dims.empty())
        fieldDef["packed_range"] = std::move(dims);
      if (auto dims = type.emitUnpackedDims(); !dims.empty())
        fieldDef["unpacked_range"] = std::move(dims);
      findAndSetLocs(fieldDef, loc);
      fieldDefs.push_back(std::move(fieldDef));
    }
    auto structName = objectNamespace.newName(structNameHint);
    JObject structDef;
    structDef["kind"] = "struct";
    structDef["obj_name"] = structName;
    structDef["port_vars"] = std::move(fieldDefs);
    findAndSetLocs(structDef, structOp.getLoc());

    StringRef structNameFinal =
        structDefs.insert({std::move(structDef), structName}).first->second;

    return {JObject({
                {"opcode", "'{"},
                {"operands", values},
            }),
            EmittedType(structNameFinal)};
  }

  // Emit arrays as assignment patterns.
  if (auto arrayOp = dyn_cast<debug::ArrayOp>(op)) {
    std::vector<JValue> values;
    EmittedType type;
    for (auto element : arrayOp.getElements()) {
      if (auto value = emitExpression(element)) {
        values.push_back(value.expr);
        if (type && type != value.type)
          return {};
        type = value.type;
      }
    }

    // Emit empty arrays as 0 `bit`.
    if (!type)
      return {JObject({{"integer_num", 0}}), EmittedType("bit")};

    type.addUnpackedDim(values.size());
    return {JObject({
                {"opcode", "'{"},
                {"operands", values},
            }),
            type};
  }

  // Look through read inout ops.
  if (auto readOp = dyn_cast<sv::ReadInOutOp>(op))
    return emitExpression(readOp.getInput());

  // Emit unary and binary combinational ops as their corresponding HGLDD
  // operation.
  StringRef unaryOpcode = TypeSwitch<Operation *, StringRef>(op)
                              .Case<comb::ParityOp>([](auto) { return "^"; })
                              .Default([](auto) { return ""; });
  if (!unaryOpcode.empty() && op->getNumOperands() == 1) {
    auto arg = emitExpression(op->getOperand(0));
    if (!arg)
      return {};
    return {JObject({
                {"opcode", unaryOpcode},
                {"operands", JArray{arg.expr}},
            }),
            result.getType()};
  }

  StringRef binaryOpcode =
      TypeSwitch<Operation *, StringRef>(op)
          .Case<comb::AddOp>([](auto) { return "+"; })
          .Case<comb::SubOp>([](auto) { return "-"; })
          .Case<comb::MulOp>([](auto) { return "*"; })
          .Case<comb::DivUOp, comb::DivSOp>([](auto) { return "/"; })
          .Case<comb::ModUOp, comb::ModSOp>([](auto) { return "%"; })
          .Case<comb::ShlOp>([](auto) { return "<<"; })
          .Case<comb::ShrUOp>([](auto) { return ">>"; })
          .Case<comb::ShrSOp>([](auto) { return ">>>"; })
          .Case<comb::ICmpOp>([](auto cmpOp) -> StringRef {
            switch (cmpOp.getPredicate()) {
            case comb::ICmpPredicate::eq:
              return "==";
            case comb::ICmpPredicate::ne:
              return "!=";
            case comb::ICmpPredicate::ceq:
              return "===";
            case comb::ICmpPredicate::cne:
              return "!==";
            case comb::ICmpPredicate::weq:
              return "==?";
            case comb::ICmpPredicate::wne:
              return "!=?";
            case comb::ICmpPredicate::ult:
            case comb::ICmpPredicate::slt:
              return "<";
            case comb::ICmpPredicate::ugt:
            case comb::ICmpPredicate::sgt:
              return ">";
            case comb::ICmpPredicate::ule:
            case comb::ICmpPredicate::sle:
              return "<=";
            case comb::ICmpPredicate::uge:
            case comb::ICmpPredicate::sge:
              return ">=";
            }
            return {};
          })
          .Default([](auto) { return ""; });
  if (!binaryOpcode.empty() && op->getNumOperands() == 2) {
    auto lhs = emitExpression(op->getOperand(0));
    auto rhs = emitExpression(op->getOperand(1));
    if (!lhs || !rhs)
      return {};
    return {JObject({
                {"opcode", binaryOpcode},
                {"operands", {lhs.expr, rhs.expr}},
            }),
            result.getType()};
  }

  // Expand variadic combinational ops into nested binary HGLDD operations.
  StringRef variadicOpcode = TypeSwitch<Operation *, StringRef>(op)
                                 .Case<comb::AndOp>([](auto) { return "&"; })
                                 .Case<comb::OrOp>([](auto) { return "|"; })
                                 .Case<comb::XorOp>([](auto) { return "^"; })
                                 .Default([](auto) { return ""; });
  if (!variadicOpcode.empty()) {
    auto operands = op->getOperands();
    auto value = emitExpression(operands[0]);
    if (!value)
      return {};
    operands = operands.drop_front();
    while (!operands.empty()) {
      auto otherValue = emitExpression(operands[0]);
      if (!otherValue)
        return {};
      operands = operands.drop_front();
      value = {JObject({
                   {"opcode", variadicOpcode},
                   {"operands", {value.expr, otherValue.expr}},
               }),
               result.getType()};
    }
    return value;
  }

  // Special handling for concatenation.
  if (auto concatOp = dyn_cast<comb::ConcatOp>(op)) {
    std::vector<JValue> args;
    for (auto operand : concatOp.getOperands()) {
      auto value = emitExpression(operand);
      if (!value)
        return {};
      args.push_back(value.expr);
    }
    return {JObject({
                {"opcode", "{}"},
                {"operands", args},
            }),
            concatOp.getType()};
  }

  // Emit `ReplicateOp` as HGLDD `R{}` op.
  if (auto replicateOp = dyn_cast<comb::ReplicateOp>(op)) {
    auto arg = emitExpression(replicateOp.getInput());
    if (!arg)
      return {};
    return {JObject({
                {"opcode", "R{}"},
                {"operands",
                 {
                     JObject({{"integer_num", replicateOp.getMultiple()}}),
                     arg.expr,
                 }},
            }),
            replicateOp.getType()};
  }

  // Emit extracts as HGLDD `[]` ops.
  if (auto extractOp = dyn_cast<comb::ExtractOp>(op)) {
    auto arg = emitExpression(extractOp.getInput());
    if (!arg)
      return {};
    auto lowBit = extractOp.getLowBit();
    auto highBit = lowBit + extractOp.getType().getIntOrFloatBitWidth() - 1;
    return {JObject({
                {"opcode", "[]"},
                {"operands",
                 {
                     arg.expr,
                     JObject({{"integer_num", highBit}}),
                     JObject({{"integer_num", lowBit}}),
                 }},
            }),
            extractOp.getType()};
  }

  // Emit `MuxOp` as HGLDD `?:` ternary op.
  if (auto muxOp = dyn_cast<comb::MuxOp>(op)) {
    auto cond = emitExpression(muxOp.getCond());
    auto lhs = emitExpression(muxOp.getTrueValue());
    auto rhs = emitExpression(muxOp.getFalseValue());
    if (!cond || !lhs || !rhs)
      return {};
    return {JObject({
                {"opcode", "?:"},
                {"operands", {cond.expr, lhs.expr, rhs.expr}},
            }),
            muxOp.getType()};
  }

  return {};
}

//===----------------------------------------------------------------------===//
// Output Splitting
//===----------------------------------------------------------------------===//

namespace {

/// Contextual information for HGLDD emission shared across multiple HGLDD
/// files. This struct is used to determine an initial split of debug info files
/// and to distribute work.
struct Emitter {
  DebugInfo di;
  SmallVector<FileEmitter, 0> files;

  Emitter(Operation *module, const EmitHGLDDOptions &options);
};

} // namespace

Emitter::Emitter(Operation *module, const EmitHGLDDOptions &options)
    : di(module) {
  // Group the DI modules according to their emitted file path. Modules that
  // don't have an emitted file path annotated are collected in a separate
  // group. That group, with a null `StringAttr` key, is emitted into a separate
  // "global.dd" file.
  MapVector<StringAttr, FileEmitter> groups;
  for (auto [moduleName, module] : di.moduleNodes) {
    StringAttr hdlFile;
    if (module->op)
      if (auto fileLoc = findBestLocation(module->op->getLoc(), true))
        hdlFile = fileLoc.getFilename();
    groups[hdlFile].modules.push_back(module);
  }

  // Determine the output file names and move the emitters into the `files`
  // member.
  files.reserve(groups.size());
  for (auto &[hdlFile, emitter] : groups) {
    emitter.options = &options;
    emitter.hdlFile = hdlFile;
    emitter.outputFileName = options.outputDirectory;
    StringRef fileName = hdlFile ? hdlFile.getValue() : "global";
    if (llvm::sys::path::is_absolute(fileName))
      emitter.outputFileName = fileName;
    else
      llvm::sys::path::append(emitter.outputFileName, fileName);
    llvm::sys::path::replace_extension(emitter.outputFileName, "dd");
    files.push_back(std::move(emitter));
  }

  // Dump some information about the files to be created.
  LLVM_DEBUG({
    llvm::dbgs() << "HGLDD files:\n";
    for (auto &emitter : files) {
      llvm::dbgs() << "- " << emitter.outputFileName << " (from "
                   << emitter.hdlFile << ")\n";
      for (auto *module : emitter.modules)
        llvm::dbgs() << "  - " << module->name << "\n";
    }
  });
}

//===----------------------------------------------------------------------===//
// Emission Entry Points
//===----------------------------------------------------------------------===//

LogicalResult debug::emitHGLDD(Operation *module, llvm::raw_ostream &os,
                               const EmitHGLDDOptions &options) {
  Emitter emitter(module, options);
  for (auto &fileEmitter : emitter.files) {
    os << "\n// ----- 8< ----- FILE \"" + fileEmitter.outputFileName +
              "\" ----- 8< -----\n\n";
    fileEmitter.emit(os);
  }
  return success();
}

LogicalResult debug::emitSplitHGLDD(Operation *module,
                                    const EmitHGLDDOptions &options) {
  Emitter emitter(module, options);

  auto emit = [&](auto &fileEmitter) {
    // Open the output file for writing.
    std::string errorMessage;
    auto output =
        mlir::openOutputFile(fileEmitter.outputFileName, &errorMessage);
    if (!output) {
      module->emitError(errorMessage);
      return failure();
    }

    // Emit the debug information and keep the file around.
    fileEmitter.emit(output->os());
    output->keep();
    return success();
  };

  return mlir::failableParallelForEach(module->getContext(), emitter.files,
                                       emit);
}
