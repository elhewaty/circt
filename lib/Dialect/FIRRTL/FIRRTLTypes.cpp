//===- FIRRTLTypes.cpp - Implement the FIRRTL dialect type system ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implement the FIRRTL dialect type system.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/HW/HWTypeInterfaces.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace circt;
using namespace firrtl;

using mlir::OptionalParseResult;
using mlir::TypeStorageAllocator;

//===----------------------------------------------------------------------===//
// Type Printing
//===----------------------------------------------------------------------===//

// NOLINTBEGIN(misc-no-recursion)
/// Print a type with a custom printer implementation.
///
/// This only prints a subset of all types in the dialect. Use `printNestedType`
/// instead, which will call this function in turn, as appropriate.
static LogicalResult customTypePrinter(Type type, AsmPrinter &os) {
  if (isConst(type)) {
    os << "const.";
  }

  auto printWidthQualifier = [&](std::optional<int32_t> width) {
    if (width)
      os << '<' << *width << '>';
  };
  bool anyFailed = false;
  TypeSwitch<Type>(type)
      .Case<ClockType>([&](auto) { os << "clock"; })
      .Case<ResetType>([&](auto) { os << "reset"; })
      .Case<AsyncResetType>([&](auto) { os << "asyncreset"; })
      .Case<SIntType>([&](auto sIntType) {
        os << "sint";
        printWidthQualifier(sIntType.getWidth());
      })
      .Case<UIntType>([&](auto uIntType) {
        os << "uint";
        printWidthQualifier(uIntType.getWidth());
      })
      .Case<AnalogType>([&](auto analogType) {
        os << "analog";
        printWidthQualifier(analogType.getWidth());
      })
      .Case<BundleType, OpenBundleType>([&](auto bundleType) {
        if (firrtl::type_isa<OpenBundleType>(bundleType))
          os << "open";
        os << "bundle<";
        llvm::interleaveComma(bundleType, os, [&](auto element) {
          StringRef fieldName = element.name.getValue();
          bool isLiteralIdentifier =
              !fieldName.empty() && llvm::isDigit(fieldName.front());
          if (isLiteralIdentifier)
            os << "\"";
          os << element.name.getValue();
          if (isLiteralIdentifier)
            os << "\"";
          if (element.isFlip)
            os << " flip";
          os << ": ";
          printNestedType(element.type, os);
        });
        os << '>';
      })
      .Case<FEnumType>([&](auto fenumType) {
        os << "enum<";
        llvm::interleaveComma(fenumType, os,
                              [&](FEnumType::EnumElement element) {
                                os << element.name.getValue();
                                os << ": ";
                                printNestedType(element.type, os);
                              });
        os << '>';
      })
      .Case<FVectorType, OpenVectorType>([&](auto vectorType) {
        if (firrtl::type_isa<OpenVectorType>(vectorType))
          os << "open";
        os << "vector<";
        printNestedType(vectorType.getElementType(), os);
        os << ", " << vectorType.getNumElements() << '>';
      })
      .Case<RefType>([&](auto refType) {
        if (refType.getForceable())
          os << "rw";
        os << "probe<";
        printNestedType(refType.getType(), os);
        os << '>';
      })
      .Case<StringType>([&](auto stringType) { os << "string"; })
      .Case<BigIntType>([&](auto bigIntType) { os << "bigint"; })
      .Case<ListType>([&](auto listType) {
        os << "list<";
        printNestedType(listType.getElementType(), os);
        os << '>';
      })
      .Case<MapType>([&](auto mapType) {
        os << "map<";
        printNestedType(mapType.getKeyType(), os);
        os << ", ";
        printNestedType(mapType.getValueType(), os);
        os << '>';
      })
      .Case<TypeAliasInterface>([&](auto alias) {
        os << "alias<";
        auto arr = alias.getNames();
        if (arr.size() == 1) {
          os << arr[0].template cast<StringAttr>().getValue();
        } else {
          os << "[";
          bool isFirst = true;
          for (auto o : arr) {
            if (isFirst)
              isFirst = false;
            else
              os << ", ";
            os << o.template cast<StringAttr>().getValue();
          }

          os << "]";
        }
        os << ", ";
        printNestedType(alias.getInnerType(), os);
        os << '>';
      })
      .Default([&](auto) { anyFailed = true; });
  return failure(anyFailed);
}
// NOLINTEND(misc-no-recursion)

/// Print a type defined by this dialect.
void circt::firrtl::printNestedType(Type type, AsmPrinter &os) {
  // Try the custom type printer.
  if (succeeded(customTypePrinter(type, os)))
    return;

  // None of the above recognized the type, so we bail.
  assert(false && "type to print unknown to FIRRTL dialect");
}

//===----------------------------------------------------------------------===//
// Type Parsing
//===----------------------------------------------------------------------===//

/// Parse a type with a custom parser implementation.
///
/// This only accepts a subset of all types in the dialect. Use `parseType`
/// instead, which will call this function in turn, as appropriate.
///
/// Returns `std::nullopt` if the type `name` is not covered by the custom
/// parsers. Otherwise returns success or failure as appropriate. On success,
/// `result` is set to the resulting type.
///
/// ```plain
/// firrtl-type
///   ::= clock
///   ::= reset
///   ::= asyncreset
///   ::= sint ('<' int '>')?
///   ::= uint ('<' int '>')?
///   ::= analog ('<' int '>')?
///   ::= bundle '<' (bundle-elt (',' bundle-elt)*)? '>'
///   ::= enum '<' (enum-elt (',' enum-elt)*)? '>'
///   ::= vector '<' type ',' int '>'
///   ::= const '.' type
///   ::= 'property.' firrtl-phased-type
/// bundle-elt ::= identifier flip? ':' type
/// enum-elt ::= identifier ':' type
/// ```
static OptionalParseResult customTypeParser(AsmParser &parser, StringRef name,
                                            Type &result) {
  bool isConst = false;
  const char constPrefix[] = "const.";
  if (name.starts_with(constPrefix)) {
    isConst = true;
    name = name.drop_front(std::size(constPrefix) - 1);
  }

  auto *context = parser.getContext();
  if (name.equals("clock"))
    return result = ClockType::get(context, isConst), success();
  if (name.equals("reset"))
    return result = ResetType::get(context, isConst), success();
  if (name.equals("asyncreset"))
    return result = AsyncResetType::get(context, isConst), success();

  if (name.equals("sint") || name.equals("uint") || name.equals("analog")) {
    // Parse the width specifier if it exists.
    int32_t width = -1;
    if (!parser.parseOptionalLess()) {
      if (parser.parseInteger(width) || parser.parseGreater())
        return failure();

      if (width < 0)
        return parser.emitError(parser.getNameLoc(), "unknown width"),
               failure();
    }

    if (name.equals("sint"))
      result = SIntType::get(context, width, isConst);
    else if (name.equals("uint"))
      result = UIntType::get(context, width, isConst);
    else {
      assert(name.equals("analog"));
      result = AnalogType::get(context, width, isConst);
    }
    return success();
  }

  if (name.equals("bundle")) {
    SmallVector<BundleType::BundleElement, 4> elements;

    auto parseBundleElement = [&]() -> ParseResult {
      std::string nameStr;
      StringRef name;
      FIRRTLBaseType type;

      if (failed(parser.parseKeywordOrString(&nameStr)))
        return failure();
      name = nameStr;

      bool isFlip = succeeded(parser.parseOptionalKeyword("flip"));
      if (parser.parseColon() || parseNestedBaseType(type, parser))
        return failure();

      elements.push_back({StringAttr::get(context, name), isFlip, type});
      return success();
    };

    if (parser.parseCommaSeparatedList(mlir::AsmParser::Delimiter::LessGreater,
                                       parseBundleElement))
      return failure();

    return result = BundleType::get(context, elements, isConst), success();
  }
  if (name.equals("openbundle")) {
    SmallVector<OpenBundleType::BundleElement, 4> elements;

    auto parseBundleElement = [&]() -> ParseResult {
      std::string nameStr;
      StringRef name;
      FIRRTLType type;

      if (failed(parser.parseKeywordOrString(&nameStr)))
        return failure();
      name = nameStr;

      bool isFlip = succeeded(parser.parseOptionalKeyword("flip"));
      if (parser.parseColon() || parseNestedType(type, parser))
        return failure();

      elements.push_back({StringAttr::get(context, name), isFlip, type});
      return success();
    };

    if (parser.parseCommaSeparatedList(mlir::AsmParser::Delimiter::LessGreater,
                                       parseBundleElement))
      return failure();

    result = parser.getChecked<OpenBundleType>(context, elements, isConst);
    return failure(!result);
  }

  if (name.equals("enum")) {
    SmallVector<FEnumType::EnumElement, 4> elements;

    auto parseEnumElement = [&]() -> ParseResult {
      std::string nameStr;
      StringRef name;
      FIRRTLBaseType type;

      if (failed(parser.parseKeywordOrString(&nameStr)))
        return failure();
      name = nameStr;

      if (parser.parseColon() || parseNestedBaseType(type, parser))
        return failure();

      elements.push_back({StringAttr::get(context, name), type});
      return success();
    };

    if (parser.parseCommaSeparatedList(mlir::AsmParser::Delimiter::LessGreater,
                                       parseEnumElement))
      return failure();
    if (failed(FEnumType::verify(
            [&]() { return parser.emitError(parser.getNameLoc()); }, elements,
            isConst)))
      return failure();

    return result = FEnumType::get(context, elements, isConst), success();
  }

  if (name.equals("vector")) {
    FIRRTLBaseType elementType;
    uint64_t width = 0;

    if (parser.parseLess() || parseNestedBaseType(elementType, parser) ||
        parser.parseComma() || parser.parseInteger(width) ||
        parser.parseGreater())
      return failure();

    return result = FVectorType::get(elementType, width, isConst), success();
  }
  if (name.equals("openvector")) {
    FIRRTLType elementType;
    uint64_t width = 0;

    if (parser.parseLess() || parseNestedType(elementType, parser) ||
        parser.parseComma() || parser.parseInteger(width) ||
        parser.parseGreater())
      return failure();

    result =
        parser.getChecked<OpenVectorType>(context, elementType, width, isConst);
    return failure(!result);
  }

  // For now, support both firrtl.ref and firrtl.probe.
  if (name.equals("ref") || name.equals("probe")) {
    FIRRTLBaseType type;
    // Don't pass `isConst` to `parseNestedBaseType since `ref` can point to
    // either `const` or non-`const` types
    if (parser.parseLess() || parseNestedBaseType(type, parser) ||
        parser.parseGreater())
      return failure();

    if (failed(RefType::verify(
            [&]() { return parser.emitError(parser.getNameLoc()); }, type,
            false)))
      return failure();

    return result = RefType::get(type, false), success();
  }
  if (name.equals("rwprobe")) {
    FIRRTLBaseType type;
    if (parser.parseLess() || parseNestedBaseType(type, parser) ||
        parser.parseGreater())
      return failure();

    if (failed(RefType::verify(
            [&]() { return parser.emitError(parser.getNameLoc()); }, type,
            true)))
      return failure();

    return result = RefType::get(type, true), success();
  }
  if (name.equals("string")) {
    if (isConst) {
      parser.emitError(parser.getNameLoc(), "strings cannot be const");
      return failure();
    }
    result = StringType::get(parser.getContext());
    return success();
  }
  if (name.equals("bigint")) {
    if (isConst) {
      parser.emitError(parser.getNameLoc(), "bigints cannot be const");
      return failure();
    }
    result = BigIntType::get(parser.getContext());
    return success();
  }
  if (name.equals("list")) {
    if (isConst) {
      parser.emitError(parser.getNameLoc(), "lists cannot be const");
      return failure();
    }
    PropertyType elementType;
    if (parser.parseLess() || parseNestedPropertyType(elementType, parser) ||
        parser.parseGreater())
      return failure();
    result = parser.getChecked<ListType>(context, elementType);
    if (!result)
      return failure();
    return success();
  }
  if (name.equals("map")) {
    if (isConst) {
      parser.emitError(parser.getNameLoc(), "maps cannot be const");
      return failure();
    }
    PropertyType keyType, valueType;
    if (parser.parseLess() || parseNestedPropertyType(keyType, parser) ||
        parser.parseComma() || parseNestedPropertyType(valueType, parser) ||
        parser.parseGreater())
      return failure();
    result = parser.getChecked<MapType>(context, keyType, valueType);
    if (!result)
      return failure();
    return success();
  }
  if (name.equals("alias")) {
    FIRRTLType type;
    StringRef name;
    SmallVector<Attribute> names;
    if (parser.parseLess())
      return failure();
    if (!parser.parseOptionalLSquare()) {
      // TODO: Support nested alias.
      return parser.emitError(parser.getNameLoc())
             << "nested type alias is not supported yet";
    } else if (!parser.parseOptionalKeyword(&name)) {
      names.push_back(StringAttr::get(parser.getContext(), name));
    } else {
      return failure();
    }
    if (parser.parseComma() || parseNestedType(type, parser) ||
        parser.parseGreater())
      return parser.emitError(parser.getNameLoc()) << "error";

    return result = circt::firrtl::wrapTypeAlias(ArrayAttr::get(context, names),
                                                 type),
           success();
  }

  return {};
}

FIRRTLType circt::firrtl::wrapTypeAlias(StringAttr name, FIRRTLType type) {
  return circt::firrtl::wrapTypeAlias(ArrayAttr::get(name.getContext(), {name}),
                                      type);
}

FIRRTLType circt::firrtl::wrapTypeAlias(ArrayAttr name, FIRRTLType type) {
#define HANDLE_TYPE(NAME)                                                      \
  if (auto base = dyn_cast<NAME>(type))                                        \
    return NAME##AliasType::get(name, base);

  HANDLE_TYPE(UIntType)
  HANDLE_TYPE(SIntType)
  HANDLE_TYPE(ResetType)
  HANDLE_TYPE(AsyncResetType)
  HANDLE_TYPE(ClockType)
  HANDLE_TYPE(AnalogType)
  HANDLE_TYPE(ResetType)
  HANDLE_TYPE(BundleType)
  HANDLE_TYPE(FVectorType)
  HANDLE_TYPE(FEnumType)

  HANDLE_TYPE(RefType)
  HANDLE_TYPE(OpenBundleType)
  HANDLE_TYPE(OpenVectorType)

  HANDLE_TYPE(MapType)
  HANDLE_TYPE(ListType)

#undef HANDLE_TYPE
  return type;
}

/// Parse a type defined by this dialect.
///
/// This will first try the generated type parsers and then resort to the custom
/// parser implementation. Emits an error and returns failure if `name` does not
/// refer to a type defined in this dialect.
static ParseResult parseType(Type &result, StringRef name, AsmParser &parser) {
  // Try the custom type parser.
  OptionalParseResult parseResult = customTypeParser(parser, name, result);
  if (parseResult.has_value())
    return parseResult.value();

  // None of the above recognized the type, so we bail.
  parser.emitError(parser.getNameLoc(), "unknown FIRRTL dialect type: \"")
      << name << "\"";
  return failure();
}

/// Parse a `FIRRTLType` with a `name` that has already been parsed.
///
/// Note that only a subset of types defined in the FIRRTL dialect inherit from
/// `FIRRTLType`. Use `parseType` to parse *any* of the defined types.
static ParseResult parseFIRRTLType(FIRRTLType &result, StringRef name,
                                   AsmParser &parser) {
  Type type;
  if (failed(parseType(type, name, parser)))
    return failure();
  result = type_dyn_cast<FIRRTLType>(type);
  if (result)
    return success();
  parser.emitError(parser.getNameLoc(), "unknown FIRRTL type: \"")
      << name << "\"";
  return failure();
}

static ParseResult parseFIRRTLBaseType(FIRRTLBaseType &result, StringRef name,
                                       AsmParser &parser) {
  FIRRTLType type;
  if (failed(parseFIRRTLType(type, name, parser)))
    return failure();
  if (auto base = type_dyn_cast<FIRRTLBaseType>(type)) {
    result = base;
    return success();
  }
  parser.emitError(parser.getNameLoc(), "expected base type, found ") << type;
  return failure();
}

static ParseResult parseFIRRTLPropertyType(PropertyType &result, StringRef name,
                                           AsmParser &parser) {
  FIRRTLType type;
  if (failed(parseFIRRTLType(type, name, parser)))
    return failure();
  if (auto prop = type_dyn_cast<PropertyType>(type)) {
    result = prop;
    return success();
  }
  parser.emitError(parser.getNameLoc(), "expected property type, found ")
      << type;
  return failure();
}

// NOLINTBEGIN(misc-no-recursion)
/// Parse a `FIRRTLType`.
///
/// Note that only a subset of types defined in the FIRRTL dialect inherit from
/// `FIRRTLType`. Use `parseType` to parse *any* of the defined types.
ParseResult circt::firrtl::parseNestedType(FIRRTLType &result,
                                           AsmParser &parser) {
  StringRef name;
  if (parser.parseKeyword(&name))
    return failure();
  return parseFIRRTLType(result, name, parser);
}
// NOLINTEND(misc-no-recursion)

// NOLINTBEGIN(misc-no-recursion)
ParseResult circt::firrtl::parseNestedBaseType(FIRRTLBaseType &result,
                                               AsmParser &parser) {
  StringRef name;
  if (parser.parseKeyword(&name))
    return failure();
  return parseFIRRTLBaseType(result, name, parser);
}
// NOLINTEND(misc-no-recursion)

// NOLINTBEGIN(misc-no-recursion)
ParseResult circt::firrtl::parseNestedPropertyType(PropertyType &result,
                                                   AsmParser &parser) {
  StringRef name;
  if (parser.parseKeyword(&name))
    return failure();
  return parseFIRRTLPropertyType(result, name, parser);
}
// NOLINTEND(misc-no-recursion)

//===---------------------------------------------------------------------===//
// Dialect Type Parsing and Printing
//===----------------------------------------------------------------------===//

/// Print a type registered to this dialect.
void FIRRTLDialect::printType(Type type, DialectAsmPrinter &os) const {
  printNestedType(type, os);
}

/// Parse a type registered to this dialect.
Type FIRRTLDialect::parseType(DialectAsmParser &parser) const {
  StringRef name;
  Type result;
  if (parser.parseKeyword(&name) || ::parseType(result, name, parser))
    return Type();
  return result;
}

//===----------------------------------------------------------------------===//
// Recursive Type Properties
//===----------------------------------------------------------------------===//

enum {
  /// Bit set if the type only contains passive elements.
  IsPassiveBitMask = 0x1,
  /// Bit set if the type contains an analog type.
  ContainsAnalogBitMask = 0x2,
  /// Bit set fi the type has any uninferred bit widths.
  HasUninferredWidthBitMask = 0x4,
};

//===----------------------------------------------------------------------===//
// FIRRTLBaseType Implementation
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::FIRRTLBaseTypeStorage : mlir::TypeStorage {
  // Use `char` instead of `bool` since llvm already provides a
  // DenseMapInfo<char> specialization
  using KeyTy = char;

  FIRRTLBaseTypeStorage(bool isConst) : isConst(static_cast<char>(isConst)) {}

  bool operator==(const KeyTy &key) const { return key == isConst; }
  KeyTy getAsKey() const { return isConst; }
  static KeyTy getKey(char isConst) { return isConst; }

  static FIRRTLBaseTypeStorage *construct(TypeStorageAllocator &allocator,
                                          KeyTy key) {
    return new (allocator.allocate<FIRRTLBaseTypeStorage>())
        FIRRTLBaseTypeStorage(key);
  }

  char isConst;
};

/// Return true if this is a 'ground' type, aka a non-aggregate type.
bool FIRRTLType::isGround() {
  return FIRRTLTypeSwitch<FIRRTLType, bool>(*this)
      .Case<ClockType, ResetType, AsyncResetType, SIntType, UIntType,
            AnalogType>([](Type) { return true; })
      .Case<BundleType, FVectorType, FEnumType, OpenBundleType, OpenVectorType>(
          [](Type) { return false; })
      .Case<BaseTypeAliasType>([](BaseTypeAliasType alias) {
        return alias.getAnonymousType().isGround();
      })
      // Not ground per spec, but leaf of aggregate.
      .Case<PropertyType, RefType>([](Type) { return false; })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return false;
      });
}

bool FIRRTLType::isConst() {
  return FIRRTLTypeSwitch<FIRRTLType, bool>(*this)
      .Case<FIRRTLBaseType, OpenBundleType, OpenVectorType>(
          [](auto type) { return type.isConst(); })
      .Default(false);
}

bool FIRRTLBaseType::isConst() { return getImpl()->isConst; }

RecursiveTypeProperties FIRRTLType::getRecursiveTypeProperties() const {
  return TypeSwitch<FIRRTLType, RecursiveTypeProperties>(*this)
      .Case<ClockType, ResetType, AsyncResetType>([](FIRRTLBaseType type) {
        return RecursiveTypeProperties{true,
                                       false,
                                       false,
                                       type.isConst(),
                                       false,
                                       false,
                                       firrtl::type_isa<ResetType>(type)};
      })
      .Case<SIntType, UIntType>([](auto type) {
        return RecursiveTypeProperties{
            true, false, false, type.isConst(), false, !type.hasWidth(), false};
      })
      .Case<AnalogType>([](auto type) {
        return RecursiveTypeProperties{
            true, false, true, type.isConst(), false, !type.hasWidth(), false};
      })
      .Case<BundleType, FVectorType, FEnumType, OpenBundleType, OpenVectorType,
            RefType, BaseTypeAliasType>(
          [](auto type) { return type.getRecursiveTypeProperties(); })
      .Case<StringType, BigIntType>([](auto type) {
        return RecursiveTypeProperties{true,  false, false, false,
                                       false, false, false};
      })
      .Case<TypeAliasInterface>([](TypeAliasInterface type) {
        auto i =
            type.getInnerType().cast<FIRRTLType>().getRecursiveTypeProperties();
        i.containsTypeAlias = true;
        return i;
      })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return RecursiveTypeProperties{};
      });
}

/// Return this type with any type aliases recursively removed from itself.
FIRRTLBaseType FIRRTLBaseType::getAnonymousType() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, FIRRTLBaseType>(*this)
      .Case<ClockType, ResetType, AsyncResetType, SIntType, UIntType,
            AnalogType>([&](Type) { return *this; })
      .Case<BundleType, FVectorType, FEnumType>(
          [](auto type) { return type.getAnonymousType(); })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return FIRRTLBaseType();
      });
}

/// Return this type with any flip types recursively removed from itself.
FIRRTLBaseType FIRRTLBaseType::getPassiveType() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, FIRRTLBaseType>(*this)
      .Case<ClockType, ResetType, AsyncResetType, SIntType, UIntType,
            AnalogType, FEnumType>([&](Type) { return *this; })
      .Case<BundleType, FVectorType, FEnumType>(
          [](auto type) { return type.getPassiveType(); })
      .Default([](Type t) {
        llvm_unreachable("unknown FIRRTL type");
        return FIRRTLBaseType();
      });
}

/// Return a 'const' or non-'const' version of this type.
FIRRTLBaseType FIRRTLBaseType::getConstType(bool isConst) {
  return FIRRTLTypeSwitch<FIRRTLBaseType, FIRRTLBaseType>(*this)
      .Case<ClockType, ResetType, AsyncResetType, AnalogType, SIntType,
            UIntType, BundleType, FVectorType, FEnumType, BaseTypeAliasType>(
          [&](auto type) { return type.getConstType(isConst); })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return FIRRTLBaseType();
      });
}

/// Return this type with a 'const' modifiers dropped
FIRRTLBaseType FIRRTLBaseType::getAllConstDroppedType() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, FIRRTLBaseType>(*this)
      .Case<ClockType, ResetType, AsyncResetType, AnalogType, SIntType,
            UIntType>([&](auto type) { return type.getConstType(false); })
      .Case<BundleType, FVectorType, FEnumType, BaseTypeAliasType>(
          [&](auto type) { return type.getAllConstDroppedType(); })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return FIRRTLBaseType();
      });
}

bool IntType::classof(mlir::Type type) {
  return type_isa<UIntType, SIntType>(type);
}

bool IntType::isSigned() { return type_isa<SIntType>(*this); }
bool IntType::isUnsigned() { return type_isa<UIntType>(*this); }

/// Return this type with all ground types replaced with UInt<1>.  This is
/// used for `mem` operations.
FIRRTLBaseType FIRRTLBaseType::getMaskType() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, FIRRTLBaseType>(*this)
      .Case<ClockType, ResetType, AsyncResetType, SIntType, UIntType,
            AnalogType>([&](Type) {
        return UIntType::get(this->getContext(), 1, this->isConst());
      })
      .Case<BundleType>([&](BundleType bundleType) {
        SmallVector<BundleType::BundleElement, 4> newElements;
        newElements.reserve(bundleType.getElements().size());
        for (auto elt : bundleType)
          newElements.push_back(
              {elt.name, false /* FIXME */, elt.type.getMaskType()});
        return BundleType::get(this->getContext(), newElements,
                               bundleType.isConst());
      })
      .Case<FVectorType>([](FVectorType vectorType) {
        return FVectorType::get(vectorType.getElementType().getMaskType(),
                                vectorType.getNumElements(),
                                vectorType.isConst());
      })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return FIRRTLBaseType();
      });
}

/// Remove the widths from this type. All widths are replaced with an
/// unknown width.
FIRRTLBaseType FIRRTLBaseType::getWidthlessType() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, FIRRTLBaseType>(*this)
      .Case<ClockType, ResetType, AsyncResetType>([](auto a) { return a; })
      .Case<UIntType, SIntType, AnalogType>(
          [&](auto a) { return a.get(this->getContext(), -1, a.isConst()); })
      .Case<BundleType>([&](auto a) {
        SmallVector<BundleType::BundleElement, 4> newElements;
        newElements.reserve(a.getElements().size());
        for (auto elt : a)
          newElements.push_back(
              {elt.name, elt.isFlip, elt.type.getWidthlessType()});
        return BundleType::get(this->getContext(), newElements, a.isConst());
      })
      .Case<FVectorType>([](auto a) {
        return FVectorType::get(a.getElementType().getWidthlessType(),
                                a.getNumElements(), a.isConst());
      })
      .Case<FEnumType>([&](FEnumType a) {
        SmallVector<FEnumType::EnumElement, 4> newElements;
        newElements.reserve(a.getNumElements());
        for (auto elt : a)
          newElements.push_back({elt.name, elt.type.getWidthlessType()});
        return FEnumType::get(this->getContext(), newElements, a.isConst());
      })
      .Default([](auto) {
        llvm_unreachable("unknown FIRRTL type");
        return FIRRTLBaseType();
      });
}

/// If this is an IntType, AnalogType, or sugar type for a single bit (Clock,
/// Reset, etc) then return the bitwidth.  Return -1 if the is one of these
/// types but without a specified bitwidth.  Return -2 if this isn't a simple
/// type.
int32_t FIRRTLBaseType::getBitWidthOrSentinel() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, int32_t>(*this)
      .Case<ClockType, ResetType, AsyncResetType>([](Type) { return 1; })
      .Case<SIntType, UIntType>(
          [&](IntType intType) { return intType.getWidthOrSentinel(); })
      .Case<AnalogType>(
          [](AnalogType analogType) { return analogType.getWidthOrSentinel(); })
      .Case<BundleType, FVectorType, FEnumType>([](Type) { return -2; })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return -2;
      });
}

/// Return true if this is a type usable as a reset. This must be
/// either an abstract reset, a concrete 1-bit UInt, an
/// asynchronous reset, or an uninfered width UInt.
bool FIRRTLBaseType::isResetType() {
  return TypeSwitch<FIRRTLType, bool>(*this)
      .Case<ResetType, AsyncResetType>([](Type) { return true; })
      .Case<UIntType>(
          [](UIntType a) { return !a.hasWidth() || a.getWidth() == 1; })
      .Case<BaseTypeAliasType>(
          [](auto type) { return type.getInnerType().isResetType(); })
      .Case<TypeAliasInterface>([](auto type) {
        return type_cast<FIRRTLBaseType>(type.getInnerType());
      })
      .Default([](Type) { return false; });
}

uint64_t FIRRTLBaseType::getMaxFieldID() {
  return FIRRTLTypeSwitch<FIRRTLBaseType, uint64_t>(*this)
      .Case<AnalogType, ClockType, ResetType, AsyncResetType, SIntType,
            UIntType>([](Type) { return 0; })
      .Case<BundleType, FVectorType, FEnumType, BaseTypeAliasType>(
          [](auto type) { return type.getMaxFieldID(); })
      .Default([](Type t) {
        llvm_unreachable("unknown FIRRTL type");
        return -1;
      });
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
FIRRTLBaseType::getSubTypeByFieldID(uint64_t fieldID) {
  return FIRRTLTypeSwitch<FIRRTLBaseType,
                          std::pair<circt::hw::FieldIDTypeInterface, unsigned>>(
             *this)
      .Case<AnalogType, ClockType, ResetType, AsyncResetType, SIntType,
            UIntType>([&](FIRRTLBaseType t) {
        assert(!fieldID && "non-aggregate types must have a field id of 0");
        return std::pair(type_cast<circt::hw::FieldIDTypeInterface>(t), 0);
      })
      .Case<BundleType, FVectorType, FEnumType, BaseTypeAliasType>(
          [&](auto type) { return type.getSubTypeByFieldID(fieldID); })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return std::pair(circt::hw::FieldIDTypeInterface(), 0);
      });
}

circt::hw::FieldIDTypeInterface
FIRRTLBaseType::getFinalTypeByFieldID(uint64_t fieldID) {
  std::pair<circt::hw::FieldIDTypeInterface, uint64_t> pair(
      type_cast<circt::hw::FieldIDTypeInterface>(*this), fieldID);
  while (pair.second) {
    llvm::dbgs() << pair.first << "\n";
    pair = pair.first.getSubTypeByFieldID(pair.second);
  }
  return pair.first;
}

std::pair<uint64_t, bool> FIRRTLBaseType::rootChildFieldID(uint64_t fieldID,
                                                           uint64_t index) {
  return FIRRTLTypeSwitch<FIRRTLBaseType, std::pair<uint64_t, bool>>(*this)
      .Case<AnalogType, ClockType, ResetType, AsyncResetType, SIntType,
            UIntType>([&](Type) { return std::make_pair(0, fieldID == 0); })
      .Case<BundleType, FVectorType, FEnumType, BaseTypeAliasType>(
          [&](auto type) { return type.rootChildFieldID(fieldID, index); })
      .Default([](Type) {
        llvm_unreachable("unknown FIRRTL type");
        return std::make_pair(0, false);
      });
}

bool firrtl::isConst(Type type) {
  return TypeSwitch<Type, bool>(type)
      .Case<FIRRTLBaseType, OpenBundleType, OpenVectorType>(
          [](auto base) { return base.isConst(); })
      .Default(false);
}

bool firrtl::containsConst(Type type) {
  return TypeSwitch<Type, bool>(type)
      .Case<FIRRTLBaseType, OpenBundleType, OpenVectorType>(
          [](auto base) { return base.containsConst(); })
      .Default(false);
}

/// Helper to implement the equivalence logic for a pair of bundle elements.
/// Note that the FIRRTL spec requires bundle elements to have the same
/// orientation, but this only compares their passive types. The FIRRTL dialect
/// differs from the spec in how it uses flip types for module output ports and
/// canonicalizes flips in bundles, so only passive types can be compared here.
static bool areBundleElementsEquivalent(BundleType::BundleElement destElement,
                                        BundleType::BundleElement srcElement,
                                        bool destOuterTypeIsConst,
                                        bool srcOuterTypeIsConst,
                                        bool requiresSameWidth) {
  if (destElement.name != srcElement.name)
    return false;
  if (destElement.isFlip != srcElement.isFlip)
    return false;

  if (destElement.isFlip) {
    std::swap(destElement, srcElement);
    std::swap(destOuterTypeIsConst, srcOuterTypeIsConst);
  }

  return areTypesEquivalent(destElement.type, srcElement.type,
                            destOuterTypeIsConst, srcOuterTypeIsConst,
                            requiresSameWidth);
}

/// Returns whether the two types are equivalent.  This implements the exact
/// definition of type equivalence in the FIRRTL spec.  If the types being
/// compared have any outer flips that encode FIRRTL module directions (input or
/// output), these should be stripped before using this method.
bool firrtl::areTypesEquivalent(FIRRTLType destFType, FIRRTLType srcFType,
                                bool destOuterTypeIsConst,
                                bool srcOuterTypeIsConst,
                                bool requireSameWidths) {
  auto destType = type_dyn_cast<FIRRTLBaseType>(destFType);
  auto srcType = type_dyn_cast<FIRRTLBaseType>(srcFType);

  // For non-base types, only equivalent if identical.
  if (!destType || !srcType)
    return destFType == srcFType;

  bool srcIsConst = srcOuterTypeIsConst || srcFType.isConst();
  bool destIsConst = destOuterTypeIsConst || destFType.isConst();

  // Vector types can be connected if they have the same size and element type.
  auto destVectorType = type_dyn_cast<FVectorType>(destType);
  auto srcVectorType = type_dyn_cast<FVectorType>(srcType);
  if (destVectorType && srcVectorType)
    return destVectorType.getNumElements() == srcVectorType.getNumElements() &&
           areTypesEquivalent(destVectorType.getElementType(),
                              srcVectorType.getElementType(), destIsConst,
                              srcIsConst, requireSameWidths);

  // Bundle types can be connected if they have the same size, element names,
  // and element types.
  auto destBundleType = type_dyn_cast<BundleType>(destType);
  auto srcBundleType = type_dyn_cast<BundleType>(srcType);
  if (destBundleType && srcBundleType) {
    auto destElements = destBundleType.getElements();
    auto srcElements = srcBundleType.getElements();
    size_t numDestElements = destElements.size();
    if (numDestElements != srcElements.size())
      return false;

    for (size_t i = 0; i < numDestElements; ++i) {
      auto destElement = destElements[i];
      auto srcElement = srcElements[i];
      if (!areBundleElementsEquivalent(destElement, srcElement, destIsConst,
                                       srcIsConst, requireSameWidths))
        return false;
    }
    return true;
  }

  // Enum types can be connected if they have the same size, element names, and
  // element types.
  auto dstEnumType = type_dyn_cast<FEnumType>(destType);
  auto srcEnumType = type_dyn_cast<FEnumType>(srcType);

  if (dstEnumType && srcEnumType) {
    if (dstEnumType.getNumElements() != srcEnumType.getNumElements())
      return false;
    // Enums requires the types to match exactly.
    for (const auto &[dst, src] : llvm::zip(dstEnumType, srcEnumType)) {
      // The variant names must match.
      if (dst.name != src.name)
        return false;
      // Enumeration types can only be connected if the inner types have the
      // same width.
      if (!areTypesEquivalent(dst.type, src.type, destIsConst, srcIsConst,
                              true))
        return false;
    }
    return true;
  }

  // Ground type connections must be const compatible.
  if (destIsConst && !srcIsConst)
    return false;

  // Reset types can be driven by UInt<1>, AsyncReset, or Reset types.
  if (firrtl::type_isa<ResetType>(destType))
    return srcType.isResetType();

  // Reset types can drive UInt<1>, AsyncReset, or Reset types.
  if (firrtl::type_isa<ResetType>(srcType))
    return destType.isResetType();

  // If we can implicitly truncate or extend the bitwidth, or either width is
  // currently uninferred, then compare the widthless version of these types.
  if (!requireSameWidths || destType.getBitWidthOrSentinel() == -1)
    srcType = srcType.getWidthlessType();
  if (!requireSameWidths || srcType.getBitWidthOrSentinel() == -1)
    destType = destType.getWidthlessType();

  // Ground types can be connected if their constless types are the same
  return destType.getConstType(false) == srcType.getConstType(false);
}

/// Returns whether the two types are weakly equivalent.
bool firrtl::areTypesWeaklyEquivalent(FIRRTLType destFType, FIRRTLType srcFType,
                                      bool destFlip, bool srcFlip,
                                      bool destOuterTypeIsConst,
                                      bool srcOuterTypeIsConst) {
  auto destType = type_dyn_cast<FIRRTLBaseType>(destFType);
  auto srcType = type_dyn_cast<FIRRTLBaseType>(srcFType);

  // For non-base types, only equivalent if identical.
  if (!destType || !srcType)
    return destFType == srcFType;

  bool srcIsConst = srcOuterTypeIsConst || srcFType.isConst();
  bool destIsConst = destOuterTypeIsConst || destFType.isConst();

  // Vector types can be connected if their element types are weakly equivalent.
  // Size doesn't matter.
  auto destVectorType = type_dyn_cast<FVectorType>(destType);
  auto srcVectorType = type_dyn_cast<FVectorType>(srcType);
  if (destVectorType && srcVectorType)
    return areTypesWeaklyEquivalent(destVectorType.getElementType(),
                                    srcVectorType.getElementType(), destFlip,
                                    srcFlip, destIsConst, srcIsConst);

  // Bundle types are weakly equivalent if all common elements are weakly
  // equivalent.  Non-matching fields are ignored.  Flips are "pushed" into
  // recursive weak type equivalence checks.
  auto destBundleType = type_dyn_cast<BundleType>(destType);
  auto srcBundleType = type_dyn_cast<BundleType>(srcType);
  if (destBundleType && srcBundleType)
    return llvm::all_of(destBundleType, [&](auto destElt) -> bool {
      auto destField = destElt.name.getValue();
      auto srcElt = srcBundleType.getElement(destField);
      // If the src doesn't contain the destination's field, that's okay.
      if (!srcElt)
        return true;

      return areTypesWeaklyEquivalent(
          destElt.type, srcElt->type, destFlip ^ destElt.isFlip,
          srcFlip ^ srcElt->isFlip, destOuterTypeIsConst, srcOuterTypeIsConst);
    });

  // Ground types require leaf flippedness and const compatibility
  if (destFlip != srcFlip)
    return false;
  if (destFlip && srcIsConst && !destIsConst)
    return false;
  if (srcFlip && destIsConst && !srcIsConst)
    return false;

  // Reset types can be driven by UInt<1>, AsyncReset, or Reset types.
  if (type_isa<ResetType>(destType))
    return srcType.isResetType();

  // Reset types can drive UInt<1>, AsyncReset, or Reset types.
  if (type_isa<ResetType>(srcType))
    return destType.isResetType();

  // Ground types can be connected if their passive, widthless versions
  // are equal and are const and flip compatible
  auto widthlessDestType = destType.getWidthlessType();
  auto widthlessSrcType = srcType.getWidthlessType();
  return widthlessDestType.getConstType(false) ==
         widthlessSrcType.getConstType(false);
}

/// Returns whether the srcType can be const-casted to the destType.
bool firrtl::areTypesConstCastable(FIRRTLType destFType, FIRRTLType srcFType,
                                   bool srcOuterTypeIsConst) {
  // Identical types are always castable
  if (destFType == srcFType)
    return true;

  auto destType = type_dyn_cast<FIRRTLBaseType>(destFType);
  auto srcType = type_dyn_cast<FIRRTLBaseType>(srcFType);

  // For non-base types, only castable if identical.
  if (!destType || !srcType)
    return false;

  // Types must be passive
  if (!destType.isPassive() || !srcType.isPassive())
    return false;

  bool srcIsConst = srcType.isConst() || srcOuterTypeIsConst;

  // Cannot cast non-'const' src to 'const' dest
  if (destType.isConst() && !srcIsConst)
    return false;

  // Vector types can be casted if they have the same size and castable element
  // type.
  auto destVectorType = type_dyn_cast<FVectorType>(destType);
  auto srcVectorType = type_dyn_cast<FVectorType>(srcType);
  if (destVectorType && srcVectorType)
    return destVectorType.getNumElements() == srcVectorType.getNumElements() &&
           areTypesConstCastable(destVectorType.getElementType(),
                                 srcVectorType.getElementType(), srcIsConst);
  if (destVectorType != srcVectorType)
    return false;

  // Bundle types can be casted if they have the same size, element names,
  // and castable element types.
  auto destBundleType = type_dyn_cast<BundleType>(destType);
  auto srcBundleType = type_dyn_cast<BundleType>(srcType);
  if (destBundleType && srcBundleType) {
    auto destElements = destBundleType.getElements();
    auto srcElements = srcBundleType.getElements();
    size_t numDestElements = destElements.size();
    if (numDestElements != srcElements.size())
      return false;

    return llvm::all_of_zip(
        destElements, srcElements,
        [&](const auto &destElement, const auto &srcElement) {
          return destElement.name == srcElement.name &&
                 areTypesConstCastable(destElement.type, srcElement.type,
                                       srcIsConst);
        });
  }
  if (destBundleType != srcBundleType)
    return false;

  // Ground types can be casted if the source type is a const
  // version of the destination type
  return destType == srcType.getConstType(destType.isConst());
}

bool firrtl::areTypesRefCastable(Type dstType, Type srcType) {
  auto dstRefType = type_dyn_cast<RefType>(dstType);
  auto srcRefType = type_dyn_cast<RefType>(srcType);
  if (!dstRefType || !srcRefType)
    return false;
  if (dstRefType == srcRefType)
    return true;
  if (dstRefType.getForceable() && !srcRefType.getForceable())
    return false;

  // Okay walk the types recursively.  They must be identical "structurally"
  // with exception leaf (ground) types of destination can be uninferred
  // versions of the corresponding source type. (can lose width information or
  // become a more general reset type)
  // In addition, while not explicitly in spec its useful to allow probes
  // to have const cast away, especially for probes of literals and expressions
  // derived from them.  Check const as with const cast.
  // NOLINTBEGIN(misc-no-recursion)
  auto recurse = [&](auto &&f, FIRRTLBaseType dest, FIRRTLBaseType src,
                     bool srcOuterTypeIsConst) -> bool {
    // Fast-path for identical types.
    if (dest == src)
      return true;

    // Always passive inside probes, but for sanity assert this.
    assert(dest.isPassive() && src.isPassive());

    bool srcIsConst = src.isConst() || srcOuterTypeIsConst;

    // Cannot cast non-'const' src to 'const' dest
    if (dest.isConst() && !srcIsConst)
      return false;

    // Recurse through aggregates to get the leaves, checking
    // structural equivalence re:element count + names.

    if (auto destVectorType = type_dyn_cast<FVectorType>(dest)) {
      auto srcVectorType = type_dyn_cast<FVectorType>(src);
      return srcVectorType &&
             destVectorType.getNumElements() ==
                 srcVectorType.getNumElements() &&
             f(f, destVectorType.getElementType(),
               srcVectorType.getElementType(), srcIsConst);
    }

    if (auto destBundleType = type_dyn_cast<BundleType>(dest)) {
      auto srcBundleType = type_dyn_cast<BundleType>(src);
      if (!srcBundleType)
        return false;
      // (no need to check orientation, these are always passive)
      auto destElements = destBundleType.getElements();
      auto srcElements = srcBundleType.getElements();

      return destElements.size() == srcElements.size() &&
             llvm::all_of_zip(
                 destElements, srcElements,
                 [&](const auto &destElement, const auto &srcElement) {
                   return destElement.name == srcElement.name &&
                          f(f, destElement.type, srcElement.type, srcIsConst);
                 });
    }

    if (auto destEnumType = type_dyn_cast<FEnumType>(dest)) {
      auto srcEnumType = type_dyn_cast<FEnumType>(src);
      if (!srcEnumType)
        return false;
      auto destElements = destEnumType.getElements();
      auto srcElements = srcEnumType.getElements();

      return destElements.size() == srcElements.size() &&
             llvm::all_of_zip(
                 destElements, srcElements,
                 [&](const auto &destElement, const auto &srcElement) {
                   return destElement.name == srcElement.name &&
                          f(f, destElement.type, srcElement.type, srcIsConst);
                 });
    }

    // Reset types can be driven by UInt<1>, AsyncReset, or Reset types.
    if (type_isa<ResetType>(dest))
      return src.isResetType();
    // (but don't allow the other direction, can only become more general)

    // Compare against const src if dest is const.
    src = src.getConstType(dest.isConst());

    // Compare against widthless src if dest is widthless.
    if (dest.getBitWidthOrSentinel() == -1)
      src = src.getWidthlessType();

    return dest == src;
  };

  return recurse(recurse, dstRefType.getType(), srcRefType.getType(), false);
  // NOLINTEND(misc-no-recursion)
}

// NOLINTBEGIN(misc-no-recursion)
/// Returns true if the destination is at least as wide as an equivalent source.
bool firrtl::isTypeLarger(FIRRTLBaseType dstType, FIRRTLBaseType srcType) {
  return TypeSwitch<FIRRTLBaseType, bool>(dstType)
      .Case<BundleType>([&](auto dstBundle) {
        auto srcBundle = type_cast<BundleType>(srcType);
        for (size_t i = 0, n = dstBundle.getNumElements(); i < n; ++i) {
          auto srcElem = srcBundle.getElement(i);
          auto dstElem = dstBundle.getElement(i);
          if (dstElem.isFlip) {
            if (!isTypeLarger(srcElem.type, dstElem.type))
              return false;
          } else {
            if (!isTypeLarger(dstElem.type, srcElem.type))
              return false;
          }
        }
        return true;
      })
      .Case<FVectorType>([&](auto vector) {
        return isTypeLarger(vector.getElementType(),
                            type_cast<FVectorType>(srcType).getElementType());
      })
      .Default([&](auto dstGround) {
        int32_t destWidth = dstType.getPassiveType().getBitWidthOrSentinel();
        int32_t srcWidth = srcType.getPassiveType().getBitWidthOrSentinel();
        return destWidth <= -1 || srcWidth <= -1 || destWidth >= srcWidth;
      });
}
// NOLINTEND(misc-no-recursion)

/// Return the passive version of a firrtl type
/// top level for ODS constraint usage
Type firrtl::getPassiveType(Type anyBaseFIRRTLType) {
  return type_cast<FIRRTLBaseType>(anyBaseFIRRTLType).getPassiveType();
}

//===----------------------------------------------------------------------===//
// IntType
//===----------------------------------------------------------------------===//

/// Return a SIntType or UIntType with the specified signedness, width, and
/// constness
IntType IntType::get(MLIRContext *context, bool isSigned,
                     int32_t widthOrSentinel, bool isConst) {
  if (isSigned)
    return SIntType::get(context, widthOrSentinel, isConst);
  return UIntType::get(context, widthOrSentinel, isConst);
}

int32_t IntType::getWidthOrSentinel() {
  if (auto sintType = type_dyn_cast<SIntType>(*this))
    return sintType.getWidthOrSentinel();
  if (auto uintType = type_dyn_cast<UIntType>(*this))
    return uintType.getWidthOrSentinel();
  return -1;
}

//===----------------------------------------------------------------------===//
// WidthTypeStorage
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::WidthTypeStorage : detail::FIRRTLBaseTypeStorage {
  using KeyTy = std::pair<int32_t, char>;
  WidthTypeStorage(int32_t width, bool isConst)
      : FIRRTLBaseTypeStorage(isConst), width(width) {}

  bool operator==(const KeyTy &key) const {
    return key == std::pair{width, isConst};
  }

  static WidthTypeStorage *construct(TypeStorageAllocator &allocator,
                                     const KeyTy &key) {
    return new (allocator.allocate<WidthTypeStorage>())
        WidthTypeStorage(key.first, key.second);
  }

  KeyTy getAsKey() const { return KeyTy(width, isConst); }
  static KeyTy getKey(int32_t width, char isConst) {
    return KeyTy(width, isConst);
  }

  int32_t width;
};

IntType IntType::getConstType(bool isConst) {

  if (auto sIntType = type_dyn_cast<SIntType>(*this))
    return sIntType.getConstType(isConst);
  return type_cast<UIntType>(*this).getConstType(isConst);
}

//===----------------------------------------------------------------------===//
// SIntType
//===----------------------------------------------------------------------===//

SIntType SIntType::get(MLIRContext *context) { return get(context, -1, false); }

SIntType SIntType::get(MLIRContext *context, std::optional<int32_t> width,
                       bool isConst) {
  return get(context, width ? *width : -1, isConst);
}

LogicalResult SIntType::verify(function_ref<InFlightDiagnostic()> emitError,
                               int32_t widthOrSentinel, bool isConst) {
  if (widthOrSentinel < -1)
    return emitError() << "invalid width";
  return success();
}

int32_t SIntType::getWidthOrSentinel() const { return getImpl()->width; }

SIntType SIntType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), getWidthOrSentinel(), isConst);
}

//===----------------------------------------------------------------------===//
// UIntType
//===----------------------------------------------------------------------===//

UIntType UIntType::get(MLIRContext *context) { return get(context, -1, false); }

UIntType UIntType::get(MLIRContext *context, std::optional<int32_t> width,
                       bool isConst) {
  return get(context, width ? *width : -1, isConst);
}

UIntType UIntType::get(MLIRContext *context, int32_t width, char isConst) {
  return get(context, width, static_cast<bool>(isConst));
}

LogicalResult UIntType::verify(function_ref<InFlightDiagnostic()> emitError,
                               int32_t widthOrSentinel, bool isConst) {
  if (widthOrSentinel < -1)
    return emitError() << "invalid width";
  return success();
}

int32_t UIntType::getWidthOrSentinel() const { return getImpl()->width; }

UIntType UIntType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), getWidthOrSentinel(), isConst);
}

//===----------------------------------------------------------------------===//
// Bundle Type
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::BundleTypeStorage
    : detail::FIRRTLBaseTypeStorage {
  using KeyTy = std::pair<ArrayRef<BundleType::BundleElement>, char>;

  BundleTypeStorage(ArrayRef<BundleType::BundleElement> elements, bool isConst)
      : detail::FIRRTLBaseTypeStorage(isConst),
        elements(elements.begin(), elements.end()),
        props{true, false, false, isConst, false, false, false} {
    uint64_t fieldID = 0;
    fieldIDs.reserve(elements.size());
    for (auto &element : elements) {
      auto type = element.type;
      auto eltInfo = type.getRecursiveTypeProperties();
      props.isPassive &= eltInfo.isPassive & !element.isFlip;
      props.containsAnalog |= eltInfo.containsAnalog;
      props.containsReference |= eltInfo.containsReference;
      props.containsConst |= eltInfo.containsConst;
      props.containsTypeAlias |= eltInfo.containsTypeAlias;
      props.hasUninferredWidth |= eltInfo.hasUninferredWidth;
      props.hasUninferredReset |= eltInfo.hasUninferredReset;
      fieldID += 1;
      fieldIDs.push_back(fieldID);
      // Increment the field ID for the next field by the number of subfields.
      fieldID += type.getMaxFieldID();
    }
    maxFieldID = fieldID;
  }

  bool operator==(const KeyTy &key) const {
    return key == KeyTy(elements, isConst);
  }

  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(
        llvm::hash_combine_range(key.first.begin(), key.first.end()),
        key.second);
  }

  static BundleTypeStorage *construct(TypeStorageAllocator &allocator,
                                      KeyTy key) {
    return new (allocator.allocate<BundleTypeStorage>())
        BundleTypeStorage(key.first, static_cast<bool>(key.second));
  }

  KeyTy getAsKey() const { return KeyTy(elements, isConst); }
  static KeyTy getKey(ArrayRef<BundleType::BundleElement> elements,
                      char isConst) {
    return KeyTy(elements, isConst);
  }

  SmallVector<BundleType::BundleElement, 4> elements;
  SmallVector<uint64_t, 4> fieldIDs;
  uint64_t maxFieldID;

  /// This holds the bits for the type's recursive properties, and can hold a
  /// pointer to a passive version of the type.
  RecursiveTypeProperties props;
  BundleType passiveType;
  BundleType anonymousType;
};

BundleType BundleType::get(MLIRContext *context,
                           ArrayRef<BundleElement> elements, bool isConst) {
  return Base::get(context, elements, isConst);
}

auto BundleType::getElements() const -> ArrayRef<BundleElement> {
  return getImpl()->elements;
}

/// Return a pair with the 'isPassive' and 'containsAnalog' bits.
RecursiveTypeProperties BundleType::getRecursiveTypeProperties() const {
  return getImpl()->props;
}

/// Return this type with any flip types recursively removed from itself.
FIRRTLBaseType BundleType::getPassiveType() {
  auto *impl = getImpl();

  // If we've already determined and cached the passive type, use it.
  if (impl->passiveType)
    return impl->passiveType;

  // If this type is already passive, use it and remember for next time.
  if (impl->props.isPassive) {
    impl->passiveType = *this;
    return *this;
  }

  // Otherwise at least one element is non-passive, rebuild a passive version.
  SmallVector<BundleType::BundleElement, 16> newElements;
  newElements.reserve(impl->elements.size());
  for (auto &elt : impl->elements) {
    newElements.push_back({elt.name, false, elt.type.getPassiveType()});
  }

  auto passiveType = BundleType::get(getContext(), newElements, isConst());
  impl->passiveType = passiveType;
  return passiveType;
}

BundleType BundleType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), getElements(), isConst);
}

BundleType BundleType::getAllConstDroppedType() {
  if (!containsConst())
    return *this;

  SmallVector<BundleElement> constDroppedElements(
      llvm::map_range(getElements(), [](BundleElement element) {
        element.type = element.type.getAllConstDroppedType();
        return element;
      }));
  return get(getContext(), constDroppedElements, false);
}

std::optional<unsigned> BundleType::getElementIndex(StringAttr name) {
  for (const auto &it : llvm::enumerate(getElements())) {
    auto element = it.value();
    if (element.name == name) {
      return unsigned(it.index());
    }
  }
  return std::nullopt;
}

std::optional<unsigned> BundleType::getElementIndex(StringRef name) {
  for (const auto &it : llvm::enumerate(getElements())) {
    auto element = it.value();
    if (element.name.getValue() == name) {
      return unsigned(it.index());
    }
  }
  return std::nullopt;
}

StringAttr BundleType::getElementNameAttr(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in bundle");
  return getElements()[index].name;
}

StringRef BundleType::getElementName(size_t index) {
  return getElementNameAttr(index).getValue();
}

std::optional<BundleType::BundleElement>
BundleType::getElement(StringAttr name) {
  if (auto maybeIndex = getElementIndex(name))
    return getElements()[*maybeIndex];
  return std::nullopt;
}

std::optional<BundleType::BundleElement>
BundleType::getElement(StringRef name) {
  if (auto maybeIndex = getElementIndex(name))
    return getElements()[*maybeIndex];
  return std::nullopt;
}

/// Look up an element by index.
BundleType::BundleElement BundleType::getElement(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in bundle");
  return getElements()[index];
}

FIRRTLBaseType BundleType::getElementType(StringAttr name) {
  auto element = getElement(name);
  return element ? element->type : FIRRTLBaseType();
}

FIRRTLBaseType BundleType::getElementType(StringRef name) {
  auto element = getElement(name);
  return element ? element->type : FIRRTLBaseType();
}

FIRRTLBaseType BundleType::getElementType(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in bundle");
  return getElements()[index].type;
}

uint64_t BundleType::getFieldID(uint64_t index) {
  return getImpl()->fieldIDs[index];
}

uint64_t BundleType::getIndexForFieldID(uint64_t fieldID) {
  assert(!getElements().empty() && "Bundle must have >0 fields");
  auto fieldIDs = getImpl()->fieldIDs;
  auto *it = std::prev(llvm::upper_bound(fieldIDs, fieldID));
  return std::distance(fieldIDs.begin(), it);
}

std::pair<uint64_t, uint64_t>
BundleType::getIndexAndSubfieldID(uint64_t fieldID) {
  auto index = getIndexForFieldID(fieldID);
  auto elementFieldID = getFieldID(index);
  return {index, fieldID - elementFieldID};
}

bool FIRRTLBaseType::classof(Type type) {
    return llvm::isa<FIRRTLDialect>(type.getDialect()) &&
           !circt::firrtl::type_isa<PropertyType, RefType, OpenBundleType, OpenVectorType>(
               type);
  }

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
BundleType::getSubTypeByFieldID(uint64_t fieldID) {
  if (fieldID == 0)
    return {*this, 0};
  auto fieldIDs = getImpl()->fieldIDs;
  auto subfieldIndex = getIndexForFieldID(fieldID);
  auto subfieldType = getElementType(subfieldIndex);
  auto subfieldID = fieldID - getFieldID(subfieldIndex);
  return {type_cast<circt::hw::FieldIDTypeInterface>(subfieldType),
          subfieldID};
}

uint64_t BundleType::getMaxFieldID() { return getImpl()->maxFieldID; }

std::pair<uint64_t, bool> BundleType::rootChildFieldID(uint64_t fieldID,
                                                       uint64_t index) {
  auto childRoot = getFieldID(index);
  auto rangeEnd = index + 1 >= getNumElements() ? getMaxFieldID()
                                                : (getFieldID(index + 1) - 1);
  return std::make_pair(fieldID - childRoot,
                        fieldID >= childRoot && fieldID <= rangeEnd);
}

bool BundleType::isConst() { return getImpl()->isConst; }

BundleType::ElementType
BundleType::getElementTypePreservingConst(size_t index) {
  auto type = getElementType(index);
  return type.getConstType(type.isConst() || isConst());
}

/// Return this type with any type aliases recursively removed from itself.
FIRRTLBaseType BundleType::getAnonymousType() {
  auto *impl = getImpl();

  // If we've already determined and cached the anonymous type, use it.
  if (impl->anonymousType)
    return impl->anonymousType;

  // If this type is already anonymous, use it and remember for next time.
  if (!impl->props.containsTypeAlias) {
    impl->anonymousType = *this;
    return *this;
  }

  // Otherwise at least one element has an alias type, rebuild an anonymous
  // version.
  SmallVector<BundleType::BundleElement, 16> newElements;
  newElements.reserve(impl->elements.size());
  for (auto &elt : impl->elements)
    newElements.push_back({elt.name, elt.isFlip, elt.type.getAnonymousType()});

  auto anonymousType = BundleType::get(getContext(), newElements, isConst());
  impl->anonymousType = anonymousType;
  return anonymousType;
}

//===----------------------------------------------------------------------===//
// OpenBundle Type
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::OpenBundleTypeStorage : mlir::TypeStorage {
  using KeyTy = std::pair<ArrayRef<OpenBundleType::BundleElement>, char>;

  OpenBundleTypeStorage(ArrayRef<OpenBundleType::BundleElement> elements,
                        bool isConst)
      : elements(elements.begin(), elements.end()),
        props{true, false, false, isConst, false, false, false},
        isConst(static_cast<char>(isConst)) {
    uint64_t fieldID = 0;
    fieldIDs.reserve(elements.size());
    for (auto &element : elements) {
      auto type = element.type;
      auto eltInfo = type.getRecursiveTypeProperties();
      props.isPassive &= eltInfo.isPassive & !element.isFlip;
      props.containsAnalog |= eltInfo.containsAnalog;
      props.containsReference |= eltInfo.containsReference;
      props.containsConst |= eltInfo.containsConst;
      props.containsTypeAlias |= eltInfo.containsTypeAlias;
      props.hasUninferredWidth |= eltInfo.hasUninferredWidth;
      props.hasUninferredReset |= eltInfo.hasUninferredReset;
      fieldID += 1;
      fieldIDs.push_back(fieldID);
      // Increment the field ID for the next field by the number of subfields.
      // TODO: Maybe just have elementType be FieldIDTypeInterface ?
      fieldID += type_cast<hw::FieldIDTypeInterface>(type).getMaxFieldID();
    }
    maxFieldID = fieldID;
  }

  bool operator==(const KeyTy &key) const {
    return key == KeyTy(elements, isConst);
  }

  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(
        llvm::hash_combine_range(key.first.begin(), key.first.end()),
        key.second);
  }

  static OpenBundleTypeStorage *construct(TypeStorageAllocator &allocator,
                                          KeyTy key) {
    return new (allocator.allocate<OpenBundleTypeStorage>())
        OpenBundleTypeStorage(key.first, static_cast<bool>(key.second));
  }

  KeyTy getAsKey() const { return KeyTy(elements, isConst); }
  static KeyTy getKey(ArrayRef<OpenBundleType::BundleElement> elements,
                      char isConst) {
    return KeyTy(elements, isConst);
  }

  SmallVector<OpenBundleType::BundleElement, 4> elements;
  SmallVector<uint64_t, 4> fieldIDs;
  uint64_t maxFieldID;

  /// This holds the bits for the type's recursive properties, and can hold a
  /// pointer to a passive version of the type.
  RecursiveTypeProperties props;

  // Whether this is 'const'.
  char isConst;
};

OpenBundleType OpenBundleType::get(MLIRContext *context,
                                   ArrayRef<BundleElement> elements,
                                   bool isConst) {
  return Base::get(context, elements, isConst);
}

auto OpenBundleType::getElements() const -> ArrayRef<BundleElement> {
  return getImpl()->elements;
}

/// Return a pair with the 'isPassive' and 'containsAnalog' bits.
RecursiveTypeProperties OpenBundleType::getRecursiveTypeProperties() const {
  return getImpl()->props;
}

OpenBundleType OpenBundleType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), getElements(), isConst);
}

std::optional<unsigned> OpenBundleType::getElementIndex(StringAttr name) {
  for (const auto &it : llvm::enumerate(getElements())) {
    auto element = it.value();
    if (element.name == name) {
      return unsigned(it.index());
    }
  }
  return std::nullopt;
}

std::optional<unsigned> OpenBundleType::getElementIndex(StringRef name) {
  for (const auto &it : llvm::enumerate(getElements())) {
    auto element = it.value();
    if (element.name.getValue() == name) {
      return unsigned(it.index());
    }
  }
  return std::nullopt;
}

StringAttr OpenBundleType::getElementNameAttr(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in bundle");
  return getElements()[index].name;
}

StringRef OpenBundleType::getElementName(size_t index) {
  return getElementNameAttr(index).getValue();
}

std::optional<OpenBundleType::BundleElement>
OpenBundleType::getElement(StringAttr name) {
  if (auto maybeIndex = getElementIndex(name))
    return getElements()[*maybeIndex];
  return std::nullopt;
}

std::optional<OpenBundleType::BundleElement>
OpenBundleType::getElement(StringRef name) {
  if (auto maybeIndex = getElementIndex(name))
    return getElements()[*maybeIndex];
  return std::nullopt;
}

/// Look up an element by index.
OpenBundleType::BundleElement OpenBundleType::getElement(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in bundle");
  return getElements()[index];
}

OpenBundleType::ElementType OpenBundleType::getElementType(StringAttr name) {
  auto element = getElement(name);
  return element ? element->type : FIRRTLBaseType();
}

OpenBundleType::ElementType OpenBundleType::getElementType(StringRef name) {
  auto element = getElement(name);
  return element ? element->type : FIRRTLBaseType();
}

OpenBundleType::ElementType OpenBundleType::getElementType(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in bundle");
  return getElements()[index].type;
}

uint64_t OpenBundleType::getFieldID(uint64_t index) {
  return getImpl()->fieldIDs[index];
}

uint64_t OpenBundleType::getIndexForFieldID(uint64_t fieldID) {
  assert(!getElements().empty() && "Bundle must have >0 fields");
  auto fieldIDs = getImpl()->fieldIDs;
  auto *it = std::prev(llvm::upper_bound(fieldIDs, fieldID));
  return std::distance(fieldIDs.begin(), it);
}

std::pair<uint64_t, uint64_t>
OpenBundleType::getIndexAndSubfieldID(uint64_t fieldID) {
  auto index = getIndexForFieldID(fieldID);
  auto elementFieldID = getFieldID(index);
  return {index, fieldID - elementFieldID};
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
OpenBundleType::getSubTypeByFieldID(uint64_t fieldID) {
  if (fieldID == 0)
    return {*this, 0};
  auto fieldIDs = getImpl()->fieldIDs;
  auto subfieldIndex = getIndexForFieldID(fieldID);
  auto subfieldType = getElementType(subfieldIndex);
  auto subfieldID = fieldID - getFieldID(subfieldIndex);
  return {type_cast<circt::hw::FieldIDTypeInterface>(subfieldType),
          subfieldID};
}

uint64_t OpenBundleType::getMaxFieldID() { return getImpl()->maxFieldID; }

std::pair<uint64_t, bool> OpenBundleType::rootChildFieldID(uint64_t fieldID,
                                                           uint64_t index) {
  auto childRoot = getFieldID(index);
  auto rangeEnd = index + 1 >= getNumElements() ? getMaxFieldID()
                                                : (getFieldID(index + 1) - 1);
  return std::make_pair(fieldID - childRoot,
                        fieldID >= childRoot && fieldID <= rangeEnd);
}

circt::hw::FieldIDTypeInterface
OpenBundleType::getFinalTypeByFieldID(uint64_t fieldID) const {
  std::pair<circt::hw::FieldIDTypeInterface, uint64_t> pair(*this, fieldID);
  while (pair.second)
    pair = pair.first.getSubTypeByFieldID(pair.second);
  return pair.first;
}

bool OpenBundleType::isConst() { return getImpl()->isConst; }

OpenBundleType::ElementType
OpenBundleType::getElementTypePreservingConst(size_t index) {
  auto type = getElementType(index);
  // TODO: ConstTypeInterface / Trait ?
  return TypeSwitch<FIRRTLType, ElementType>(type)
      .Case<FIRRTLBaseType, OpenBundleType, OpenVectorType>([&](auto type) {
        return type.getConstType(type.isConst() || isConst());
      })
      .Default(type);
}

LogicalResult
OpenBundleType::verify(function_ref<InFlightDiagnostic()> emitErrorFn,
                       ArrayRef<BundleElement> elements, bool isConst) {
  for (auto &element : elements) {
    if (!type_isa<hw::FieldIDTypeInterface>(element.type))
      return emitErrorFn()
             << "bundle element " << element.name
             << " has unsupported type that does not support fieldID's: "
             << element.type;
    if (FIRRTLType(element.type).containsReference() && isConst)
      return emitErrorFn()
             << "'const' bundle cannot have references, but element "
             << element.name << " has type " << element.type;
  }

  return success();
}

//===----------------------------------------------------------------------===//
// FVectorType
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::FVectorTypeStorage
    : detail::FIRRTLBaseTypeStorage {
  using KeyTy = std::tuple<FIRRTLBaseType, size_t, char>;

  FVectorTypeStorage(FIRRTLBaseType elementType, size_t numElements,
                     bool isConst)
      : detail::FIRRTLBaseTypeStorage(isConst), elementType(elementType),
        numElements(numElements),
        props(elementType.getRecursiveTypeProperties()) {
    props.containsConst |= isConst;
  }

  bool operator==(const KeyTy &key) const {
    return key == std::make_tuple(elementType, numElements, isConst);
  }

  static FVectorTypeStorage *construct(TypeStorageAllocator &allocator,
                                       KeyTy key) {
    return new (allocator.allocate<FVectorTypeStorage>())
        FVectorTypeStorage(std::get<0>(key), std::get<1>(key),
                           static_cast<bool>(std::get<2>(key)));
  }

  KeyTy getAsKey() const { return KeyTy(elementType, numElements, isConst); }
  static KeyTy getKey(FIRRTLBaseType elementType, size_t numElements,
                      bool isConst) {
    return KeyTy(elementType, numElements, isConst);
  }

  FIRRTLBaseType elementType;
  size_t numElements;

  /// This holds the bits for the type's recursive properties, and can hold a
  /// pointer to a passive version of the type.
  RecursiveTypeProperties props;
  FIRRTLBaseType passiveType;
  FIRRTLBaseType anonymousType;
};

FVectorType FVectorType::get(FIRRTLBaseType elementType, size_t numElements,
                             bool isConst) {
  return Base::get(elementType.getContext(), elementType, numElements, isConst);
}

FIRRTLBaseType FVectorType::getElementType() const {
  return getImpl()->elementType;
}

size_t FVectorType::getNumElements() const { return getImpl()->numElements; }

/// Return the recursive properties of the type.
RecursiveTypeProperties FVectorType::getRecursiveTypeProperties() const {
  return getImpl()->props;
}

/// Return this type with any flip types recursively removed from itself.
FIRRTLBaseType FVectorType::getPassiveType() {
  auto *impl = getImpl();

  // If we've already determined and cached the passive type, use it.
  if (impl->passiveType)
    return impl->passiveType;

  // If this type is already passive, return it and remember for next time.
  if (impl->elementType.getRecursiveTypeProperties().isPassive)
    return impl->passiveType = *this;

  // Otherwise, rebuild a passive version.
  auto passiveType = FVectorType::get(getElementType().getPassiveType(),
                                      getNumElements(), isConst());
  impl->passiveType = passiveType;
  return passiveType;
}

FVectorType FVectorType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getElementType(), getNumElements(), isConst);
}

FVectorType FVectorType::getAllConstDroppedType() {
  if (!containsConst())
    return *this;
  return get(getElementType().getAllConstDroppedType(), getNumElements(),
             false);
}

/// Return this type with any type aliases recursively removed from itself.
FIRRTLBaseType FVectorType::getAnonymousType() {
  auto *impl = getImpl();

  if (impl->anonymousType)
    return impl->anonymousType;

  // If this type is already anonymous, return it and remember for next time.
  if (!impl->props.containsTypeAlias)
    return impl->anonymousType = *this;

  // Otherwise, rebuild an anonymous version.
  auto anonymousType = FVectorType::get(getElementType().getAnonymousType(),
                                        getNumElements(), isConst());
  impl->anonymousType = anonymousType;
  return anonymousType;
}

uint64_t FVectorType::getFieldID(uint64_t index) {
  return 1 + index * (getElementType().getMaxFieldID() + 1);
}

uint64_t FVectorType::getIndexForFieldID(uint64_t fieldID) {
  assert(fieldID && "fieldID must be at least 1");
  // Divide the field ID by the number of fieldID's per element.
  return (fieldID - 1) / (getElementType().getMaxFieldID() + 1);
}

std::pair<uint64_t, uint64_t>
FVectorType::getIndexAndSubfieldID(uint64_t fieldID) {
  auto index = getIndexForFieldID(fieldID);
  auto elementFieldID = getFieldID(index);
  return {index, fieldID - elementFieldID};
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
FVectorType::getSubTypeByFieldID(uint64_t fieldID) {
  if (fieldID == 0)
    return {*this, 0};
  return {type_cast<circt::hw::FieldIDTypeInterface>(getElementType()),
          getIndexAndSubfieldID(fieldID).second};
}

uint64_t FVectorType::getMaxFieldID() {
  return getNumElements() * (getElementType().getMaxFieldID() + 1);
}

std::pair<uint64_t, bool> FVectorType::rootChildFieldID(uint64_t fieldID,
                                                        uint64_t index) {
  auto childRoot = getFieldID(index);
  auto rangeEnd =
      index >= getNumElements() ? getMaxFieldID() : (getFieldID(index + 1) - 1);
  return std::make_pair(fieldID - childRoot,
                        fieldID >= childRoot && fieldID <= rangeEnd);
}

bool FVectorType::isConst() { return getImpl()->isConst; }

FVectorType::ElementType FVectorType::getElementTypePreservingConst() {
  auto type = getElementType();
  return type.getConstType(type.isConst() || isConst());
}

//===----------------------------------------------------------------------===//
// OpenVectorType
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::OpenVectorTypeStorage : mlir::TypeStorage {
  using KeyTy = std::tuple<FIRRTLType, size_t, char>;

  OpenVectorTypeStorage(FIRRTLType elementType, size_t numElements,
                        bool isConst)
      : elementType(elementType), numElements(numElements),
        isConst(static_cast<char>(isConst)) {
    props = elementType.getRecursiveTypeProperties();
    props.containsConst |= isConst;
  }

  bool operator==(const KeyTy &key) const {
    return key == std::make_tuple(elementType, numElements, isConst);
  }

  static OpenVectorTypeStorage *construct(TypeStorageAllocator &allocator,
                                          KeyTy key) {
    return new (allocator.allocate<OpenVectorTypeStorage>())
        OpenVectorTypeStorage(std::get<0>(key), std::get<1>(key),
                              static_cast<bool>(std::get<2>(key)));
  }

  KeyTy getAsKey() const { return KeyTy(elementType, numElements, isConst); }
  static KeyTy getKey(FIRRTLType elementType, size_t numElements,
                      bool isConst) {
    return KeyTy(elementType, numElements, isConst);
  }

  FIRRTLType elementType;
  size_t numElements;

  RecursiveTypeProperties props;
  char isConst;
};

OpenVectorType OpenVectorType::get(FIRRTLType elementType, size_t numElements,
                                   bool isConst) {
  return Base::get(elementType.getContext(), elementType, numElements, isConst);
}

FIRRTLType OpenVectorType::getElementType() const {
  return getImpl()->elementType;
}

size_t OpenVectorType::getNumElements() const { return getImpl()->numElements; }

/// Return the recursive properties of the type.
RecursiveTypeProperties OpenVectorType::getRecursiveTypeProperties() const {
  return getImpl()->props;
}

OpenVectorType OpenVectorType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getElementType(), getNumElements(), isConst);
}

uint64_t OpenVectorType::getFieldID(uint64_t index) {
  return 1 + index * (type_cast<hw::FieldIDTypeInterface>(getElementType())
                          .getMaxFieldID() +
                      1);
}

uint64_t OpenVectorType::getIndexForFieldID(uint64_t fieldID) {
  assert(fieldID && "fieldID must be at least 1");
  // Divide the field ID by the number of fieldID's per element.
  return (fieldID - 1) / (type_cast<hw::FieldIDTypeInterface>(getElementType())
                              .getMaxFieldID() +
                          1);
}

std::pair<uint64_t, uint64_t>
OpenVectorType::getIndexAndSubfieldID(uint64_t fieldID) {
  auto index = getIndexForFieldID(fieldID);
  auto elementFieldID = getFieldID(index);
  return {index, fieldID - elementFieldID};
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
OpenVectorType::getSubTypeByFieldID(uint64_t fieldID) {
  if (fieldID == 0)
    return {*this, 0};
  return {type_cast<circt::hw::FieldIDTypeInterface>(getElementType()),
          getIndexForFieldID(fieldID)};
}

uint64_t OpenVectorType::getMaxFieldID() {
  // If this is requirement, make ODS constraint or actual elementType.
  return getNumElements() *
         (type_cast<hw::FieldIDTypeInterface>(getElementType())
              .getMaxFieldID() +
          1);
}

std::pair<uint64_t, bool> OpenVectorType::rootChildFieldID(uint64_t fieldID,
                                                           uint64_t index) {
  auto childRoot = getFieldID(index);
  auto rangeEnd =
      index >= getNumElements() ? getMaxFieldID() : (getFieldID(index + 1) - 1);
  return std::make_pair(fieldID - childRoot,
                        fieldID >= childRoot && fieldID <= rangeEnd);
}

circt::hw::FieldIDTypeInterface
OpenVectorType::getFinalTypeByFieldID(uint64_t fieldID) const {
  std::pair<circt::hw::FieldIDTypeInterface, uint64_t> pair(*this, fieldID);
  while (pair.second)
    pair = pair.first.getSubTypeByFieldID(pair.second);
  return pair.first;
}

bool OpenVectorType::isConst() { return getImpl()->isConst; }

OpenVectorType::ElementType OpenVectorType::getElementTypePreservingConst() {
  auto type = getElementType();
  // TODO: ConstTypeInterface / Trait ?
  return TypeSwitch<FIRRTLType, ElementType>(type)
      .Case<FIRRTLBaseType, OpenBundleType, OpenVectorType>([&](auto type) {
        return type.getConstType(type.isConst() || isConst());
      })
      .Default(type);
}

LogicalResult
OpenVectorType::verify(function_ref<InFlightDiagnostic()> emitErrorFn,
                       FIRRTLType elementType, size_t numElements,
                       bool isConst) {
  if (!type_isa<hw::FieldIDTypeInterface>(elementType))
    return emitErrorFn()
           << "vector element type does not support fieldID's, type: "
           << elementType;
  if (elementType.containsReference() && isConst)
    return emitErrorFn() << "vector cannot be const with references";
  return success();
}

//===----------------------------------------------------------------------===//
// FEnum Type
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::FEnumTypeStorage : detail::FIRRTLBaseTypeStorage {
  using KeyTy = std::pair<ArrayRef<FEnumType::EnumElement>, char>;

  FEnumTypeStorage(ArrayRef<FEnumType::EnumElement> elements, bool isConst)
      : detail::FIRRTLBaseTypeStorage(isConst),
        elements(elements.begin(), elements.end()) {
    RecursiveTypeProperties props{true,  false, false, isConst,
                                  false, false, false};
    uint64_t fieldID = 0;
    fieldIDs.reserve(elements.size());
    for (auto &element : elements) {
      auto type = element.type;
      auto eltInfo = type.getRecursiveTypeProperties();
      props.isPassive &= eltInfo.isPassive;
      props.containsAnalog |= eltInfo.containsAnalog;
      props.containsConst |= eltInfo.containsConst;
      props.hasUninferredWidth |= eltInfo.hasUninferredWidth;
      props.containsTypeAlias |= eltInfo.containsTypeAlias;
      fieldID += 1;
      fieldIDs.push_back(fieldID);
      // Increment the field ID for the next field by the number of subfields.
      fieldID += type.getMaxFieldID();
    }
    maxFieldID = fieldID;
    recProps = props;
  }

  bool operator==(const KeyTy &key) const {
    return key == KeyTy(elements, isConst);
  }

  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(
        llvm::hash_combine_range(key.first.begin(), key.first.end()),
        key.second);
  }

  static FEnumTypeStorage *construct(TypeStorageAllocator &allocator,
                                     KeyTy key) {
    return new (allocator.allocate<FEnumTypeStorage>())
        FEnumTypeStorage(key.first, static_cast<bool>(key.second));
  }

  KeyTy getAsKey() const { return KeyTy(elements, isConst); }
  static KeyTy getKey(ArrayRef<FEnumType::EnumElement> elements, char isConst) {
    return KeyTy(elements, isConst);
  }

  SmallVector<FEnumType::EnumElement, 4> elements;
  SmallVector<uint64_t, 4> fieldIDs;
  uint64_t maxFieldID;

  RecursiveTypeProperties recProps;
  FIRRTLBaseType anonymousType;
};

FEnumType FEnumType::get(::mlir::MLIRContext *context,
                         ArrayRef<EnumElement> elements, bool isConst) {
  return Base::get(context, elements, isConst);
}

ArrayRef<FEnumType::EnumElement> FEnumType::getElements() const {
  return getImpl()->elements;
}

FEnumType FEnumType::getConstType(bool isConst) {
  return get(getContext(), getElements(), isConst);
}

FEnumType FEnumType::getAllConstDroppedType() {
  if (!containsConst())
    return *this;

  SmallVector<EnumElement> constDroppedElements(
      llvm::map_range(getElements(), [](EnumElement element) {
        element.type = element.type.getAllConstDroppedType();
        return element;
      }));
  return get(getContext(), constDroppedElements, false);
}

/// Return a pair with the 'isPassive' and 'containsAnalog' bits.
RecursiveTypeProperties FEnumType::getRecursiveTypeProperties() const {
  return getImpl()->recProps;
}

std::optional<unsigned> FEnumType::getElementIndex(StringAttr name) {
  for (const auto &it : llvm::enumerate(getElements())) {
    auto element = it.value();
    if (element.name == name) {
      return unsigned(it.index());
    }
  }
  return std::nullopt;
}

std::optional<unsigned> FEnumType::getElementIndex(StringRef name) {
  for (const auto &it : llvm::enumerate(getElements())) {
    auto element = it.value();
    if (element.name.getValue() == name) {
      return unsigned(it.index());
    }
  }
  return std::nullopt;
}

StringAttr FEnumType::getElementNameAttr(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in enum");
  return getElements()[index].name;
}

StringRef FEnumType::getElementName(size_t index) {
  return getElementNameAttr(index).getValue();
}

std::optional<FEnumType::EnumElement> FEnumType::getElement(StringAttr name) {
  if (auto maybeIndex = getElementIndex(name))
    return getElements()[*maybeIndex];
  return std::nullopt;
}

std::optional<FEnumType::EnumElement> FEnumType::getElement(StringRef name) {
  if (auto maybeIndex = getElementIndex(name))
    return getElements()[*maybeIndex];
  return std::nullopt;
}

/// Look up an element by index.
FEnumType::EnumElement FEnumType::getElement(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in enum");
  return getElements()[index];
}

FIRRTLBaseType FEnumType::getElementType(StringAttr name) {
  auto element = getElement(name);
  return element ? element->type : FIRRTLBaseType();
}

FIRRTLBaseType FEnumType::getElementType(StringRef name) {
  auto element = getElement(name);
  return element ? element->type : FIRRTLBaseType();
}

FIRRTLBaseType FEnumType::getElementType(size_t index) {
  assert(index < getNumElements() &&
         "index must be less than number of fields in enum");
  return getElements()[index].type;
}

FIRRTLBaseType FEnumType::getElementTypePreservingConst(size_t index) {
  auto type = getElementType(index);
  return type.getConstType(type.isConst() || isConst());
}

uint64_t FEnumType::getFieldID(uint64_t index) {
  return getImpl()->fieldIDs[index];
}

uint64_t FEnumType::getIndexForFieldID(uint64_t fieldID) {
  assert(!getElements().empty() && "Enum must have >0 fields");
  auto fieldIDs = getImpl()->fieldIDs;
  auto *it = std::prev(llvm::upper_bound(fieldIDs, fieldID));
  return std::distance(fieldIDs.begin(), it);
}

std::pair<uint64_t, uint64_t>
FEnumType::getIndexAndSubfieldID(uint64_t fieldID) {
  auto index = getIndexForFieldID(fieldID);
  auto elementFieldID = getFieldID(index);
  return {index, fieldID - elementFieldID};
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
FEnumType::getSubTypeByFieldID(uint64_t fieldID) {
  if (fieldID == 0)
    return {*this, 0};
  auto fieldIDs = getImpl()->fieldIDs;
  auto subfieldIndex = getIndexForFieldID(fieldID);
  auto subfieldType = getElementType(subfieldIndex);
  auto subfieldID = fieldID - getFieldID(subfieldIndex);
  return {type_cast<circt::hw::FieldIDTypeInterface>(subfieldType),
          subfieldID};
}

uint64_t FEnumType::getMaxFieldID() { return getImpl()->maxFieldID; }

std::pair<uint64_t, bool> FEnumType::rootChildFieldID(uint64_t fieldID,
                                                      uint64_t index) {
  auto childRoot = getFieldID(index);
  auto rangeEnd = index + 1 >= getNumElements() ? getMaxFieldID()
                                                : (getFieldID(index + 1) - 1);
  return std::make_pair(fieldID - childRoot,
                        fieldID >= childRoot && fieldID <= rangeEnd);
}

auto FEnumType::verify(function_ref<InFlightDiagnostic()> emitErrorFn,
                       ArrayRef<EnumElement> elements, bool isConst)
    -> LogicalResult {
  for (auto &elt : elements) {
    auto r = elt.type.getRecursiveTypeProperties();
    if (!r.isPassive)
      return emitErrorFn() << "enum field '" << elt.name << "' not passive";
    if (r.containsAnalog)
      return emitErrorFn() << "enum field '" << elt.name << "' contains analog";
    if (r.containsConst && !isConst)
      return emitErrorFn() << "enum with 'const' elements must be 'const'";
    // TODO: exclude reference containing
  }
  return success();
}

/// Return this type with any type aliases recursively removed from itself.
FIRRTLBaseType FEnumType::getAnonymousType() {
  auto *impl = getImpl();

  if (impl->anonymousType)
    return impl->anonymousType;

  if (!impl->recProps.containsTypeAlias)
    return impl->anonymousType = *this;

  SmallVector<FEnumType::EnumElement, 4> elements;

  for (auto element : getElements())
    elements.push_back({element.name, element.type.getAnonymousType()});
  return impl->anonymousType = FEnumType::get(getContext(), elements);
}

//===----------------------------------------------------------------------===//
// BaseTypeAliasType
//===----------------------------------------------------------------------===//

struct circt::firrtl::detail::BaseTypeAliasStorage
    : circt::firrtl::detail::FIRRTLBaseTypeStorage {
  using KeyTy = std::tuple<StringAttr, FIRRTLBaseType>;

  BaseTypeAliasStorage(StringAttr name, FIRRTLBaseType innerType)
      : detail::FIRRTLBaseTypeStorage(innerType.isConst()), name(name),
        innerType(innerType) {}

  bool operator==(const KeyTy &key) const {
    return key == KeyTy(name, innerType);
  }

  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_combine(key);
  }

  static BaseTypeAliasStorage *construct(TypeStorageAllocator &allocator,
                                         KeyTy key) {
    return new (allocator.allocate<BaseTypeAliasStorage>())
        BaseTypeAliasStorage(std::get<0>(key), std::get<1>(key));
  }

  KeyTy getAsKey() const { return KeyTy(name, innerType); }
  static KeyTy getKey(StringAttr name, FIRRTLBaseType innerType) {
    return KeyTy(name, innerType);
  }

  StringAttr name;
  FIRRTLBaseType innerType;
  FIRRTLBaseType anonymousType;
};

auto BaseTypeAliasType::get(StringAttr name, FIRRTLBaseType innerType)
    -> BaseTypeAliasType {
  return Base::get(name.getContext(), name, innerType);
}

auto BaseTypeAliasType::getName() const -> StringAttr {
  return getImpl()->name;
}

auto BaseTypeAliasType::getInnerType() const -> FIRRTLBaseType {
  return getImpl()->innerType;
}

FIRRTLBaseType BaseTypeAliasType::getAnonymousType() {
  auto *impl = getImpl();
  if (impl->anonymousType)
    return impl->anonymousType;
  return impl->anonymousType = getInnerType().getAnonymousType();
}

FIRRTLBaseType BaseTypeAliasType::getPassiveType() {
  return getModifiedType(getInnerType().getPassiveType());
}

RecursiveTypeProperties BaseTypeAliasType::getRecursiveTypeProperties() const {
  auto rtp = getInnerType().getRecursiveTypeProperties();
  rtp.containsTypeAlias = true;
  return rtp;
}

// If a given `newInnerType` is identical to innerType, return `*this`
// because we can reuse the type alias. Otherwise return `newInnerType`.
FIRRTLBaseType BaseTypeAliasType::getModifiedType(FIRRTLBaseType newInnerType) {
  if (newInnerType == getInnerType())
    return *this;
  return newInnerType;
}

// FieldIDTypeInterface implementation.
FIRRTLBaseType BaseTypeAliasType::getAllConstDroppedType() {
  return getModifiedType(getInnerType().getAllConstDroppedType());
}

FIRRTLBaseType BaseTypeAliasType::getConstType(bool isConst) {
  return getModifiedType(getInnerType().getConstType(isConst));
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
BaseTypeAliasType::getSubTypeByFieldID(uint64_t fieldID) {
  return getInnerType().getSubTypeByFieldID(fieldID);
}

uint64_t BaseTypeAliasType::getMaxFieldID() {
  // We can use anonymous type.
  return getAnonymousType().getMaxFieldID();
}

std::pair<uint64_t, bool> BaseTypeAliasType::rootChildFieldID(uint64_t fieldID,
                                                              uint64_t index) {
  // We can use anonymous type.
  return getAnonymousType().rootChildFieldID(fieldID, index);
}

//===----------------------------------------------------------------------===//
// RefType
//===----------------------------------------------------------------------===//

auto RefType::get(FIRRTLBaseType type, bool forceable) -> RefType {
  return Base::get(type.getContext(), type, forceable);
}

auto RefType::verify(function_ref<InFlightDiagnostic()> emitErrorFn,
                     FIRRTLBaseType base, bool forceable) -> LogicalResult {
  if (!base.isPassive())
    return emitErrorFn() << "reference base type must be passive";
  if (forceable && base.containsConst())
    return emitErrorFn()
           << "forceable reference base type cannot contain const";
  return success();
}

//- RefType implementations of FieldIDTypeInterface --------------------------//
// Needs to be implemented to be used in a FIRRTL aggregate.

uint64_t RefType::getMaxFieldID() const { return 0; }

circt::hw::FieldIDTypeInterface
RefType::getFinalTypeByFieldID(uint64_t fieldID) const {
  assert(fieldID == 0);
  return *this;
}

std::pair<circt::hw::FieldIDTypeInterface, uint64_t>
RefType::getSubTypeByFieldID(uint64_t fieldID) const {
  assert(fieldID == 0);
  return {*this, 0};
}

std::pair<uint64_t, bool> RefType::rootChildFieldID(uint64_t fieldID,
                                                    uint64_t index) const {
  return {0, fieldID == 0};
}

RecursiveTypeProperties RefType::getRecursiveTypeProperties() const {
  auto rtp = getType().getRecursiveTypeProperties();
  rtp.containsReference = true;
  // References are not "passive", per FIRRTL spec.
  rtp.isPassive = false;
  return rtp;
}

//===----------------------------------------------------------------------===//
// AnalogType
//===----------------------------------------------------------------------===//

AnalogType AnalogType::get(mlir::MLIRContext *context) {
  return AnalogType::get(context, -1, false);
}

AnalogType AnalogType::get(mlir::MLIRContext *context,
                           std::optional<int32_t> width, bool isConst) {
  return AnalogType::get(context, width ? *width : -1, isConst);
}

AnalogType AnalogType::get(MLIRContext *context, int32_t width, char isConst) {
  return get(context, width, static_cast<bool>(isConst));
}

LogicalResult AnalogType::verify(function_ref<InFlightDiagnostic()> emitError,
                                 int32_t widthOrSentinel, bool isConst) {
  if (widthOrSentinel < -1)
    return emitError() << "invalid width";
  return success();
}

int32_t AnalogType::getWidthOrSentinel() const { return getImpl()->width; }

AnalogType AnalogType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), getWidthOrSentinel(), isConst);
}

//===----------------------------------------------------------------------===//
// ClockType
//===----------------------------------------------------------------------===//

ClockType ClockType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), isConst);
}

//===----------------------------------------------------------------------===//
// ResetType
//===----------------------------------------------------------------------===//

ResetType ResetType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), isConst);
}

//===----------------------------------------------------------------------===//
// AsyncResetType
//===----------------------------------------------------------------------===//

AsyncResetType AsyncResetType::getConstType(bool isConst) {
  if (isConst == this->isConst())
    return *this;
  return get(getContext(), isConst);
}

//===----------------------------------------------------------------------===//
// FIRRTLDialect
//===----------------------------------------------------------------------===//

void FIRRTLDialect::registerTypes() {
  addTypes<SIntType, UIntType, ClockType, ResetType, AsyncResetType, AnalogType,
           // Derived Types
           BundleType, FVectorType, FEnumType, BaseTypeAliasType,
           // References and open aggregates
           RefType, OpenBundleType, OpenVectorType,
           // Non-Hardware types
           StringType, BigIntType, ListType, MapType,
           // Alias types
           UIntTypeAliasType, SIntTypeAliasType, ClockTypeAliasType,
           ResetTypeAliasType, AsyncResetTypeAliasType, AnalogTypeAliasType,
           BundleTypeAliasType, OpenBundleTypeAliasType, FVectorTypeAliasType,
           OpenVectorTypeAliasType, FEnumTypeAliasType, RefTypeAliasType>();
}

// Get the bit width for this type, return None  if unknown. Unlike
// getBitWidthOrSentinel(), this can recursively compute the bitwidth of
// aggregate types. For bundle and vectors, recursively get the width of each
// field element and return the total bit width of the aggregate type. This
// returns None, if any of the bundle fields is a flip type, or ground type with
// unknown bit width.
std::optional<int64_t> firrtl::getBitWidth(FIRRTLBaseType type,
                                           bool ignoreFlip) {
  std::function<std::optional<int64_t>(FIRRTLBaseType)> getWidth =
      [&](FIRRTLBaseType type) -> std::optional<int64_t> {
    return FIRRTLTypeSwitch<FIRRTLBaseType, std::optional<int64_t>>(type)
        .Case<BundleType>([&](BundleType bundle) -> std::optional<int64_t> {
          int64_t width = 0;
          for (auto &elt : bundle) {
            if (elt.isFlip && !ignoreFlip)
              return std::nullopt;
            auto w = getBitWidth(elt.type);
            if (!w.has_value())
              return std::nullopt;
            width += *w;
          }
          return width;
        })
        .Case<FEnumType>([&](FEnumType fenum) -> std::optional<int64_t> {
          int64_t width = 0;
          for (auto &elt : fenum) {
            auto w = getBitWidth(elt.type);
            if (!w.has_value())
              return std::nullopt;
            width = std::max(width, *w);
          }
          return width + llvm::Log2_32_Ceil(fenum.getNumElements());
        })
        .Case<FVectorType>([&](auto vector) -> std::optional<int64_t> {
          auto w = getBitWidth(vector.getElementType());
          if (!w.has_value())
            return std::nullopt;
          return *w * vector.getNumElements();
        })
        .Case<IntType>([&](IntType iType) { return iType.getWidth(); })
        .Case<ClockType, ResetType, AsyncResetType>([](Type) { return 1; })
        .Default([&](auto t) { return std::nullopt; });
  };
  return getWidth(type);
}

//===----------------------------------------------------------------------===//
// TableGen generated logic.
//===----------------------------------------------------------------------===//

// Provide the autogenerated implementation for types.
#define GET_TYPEDEF_CLASSES
#include "circt/Dialect/FIRRTL/FIRRTLTypes.cpp.inc"
