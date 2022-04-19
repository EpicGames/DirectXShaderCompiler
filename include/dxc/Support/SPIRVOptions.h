//===------- SPIRVOptions.h -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file outlines the command-line options used by SPIR-V CodeGen.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SPIRV_OPTIONS_H
#define LLVM_SPIRV_OPTIONS_H

#ifdef ENABLE_SPIRV_CODEGEN

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/ArgList.h"

namespace clang {
namespace spirv {

enum class SpirvLayoutRule {
  Void,
  GLSLStd140,
  GLSLStd430,
  RelaxedGLSLStd140, // std140 with relaxed vector layout
  RelaxedGLSLStd430, // std430 with relaxed vector layout
  FxcCTBuffer,       // fxc.exe layout rule for cbuffer/tbuffer
  FxcSBuffer,        // fxc.exe layout rule for structured buffers
  Scalar,            // VK_EXT_scalar_block_layout
  Max,               // This is an invalid layout rule
};

struct SpirvCodeGenOptions {
  /// Disable legalization and optimization and emit raw SPIR-V
  bool codeGenHighLevel;
  bool debugInfoFile;
  bool debugInfoLine;
  bool debugInfoSource;
  bool debugInfoTool;
  bool debugInfoRich;
  /// Use NonSemantic.Vulkan.DebugInfo.100 debug info instead of
  /// OpenCL.DebugInfo.100
  bool debugInfoVulkan;
  bool defaultRowMajor;
  bool disableValidation;
  bool enable16BitTypes;
  bool finiteMathOnly;
  bool enableReflect;
  // UE Change Begin: Add 'fused-multiply-add' pass to emulate invariant
  // qualifier for older versions of Metal.
  bool enableFMAPass;
  // UE Change End: Add 'fused-multiply-add' pass to emulate invariant
  // UE Change Begin: Allow to disable scalar block layout
  bool disableScalarBlockLayout;
  // UE Change End: Allow to disable scalar block layout
  // qualifier for older versions of Metal.
  bool invertY; // Additive inverse
  bool invertW; // Multiplicative inverse
  bool noWarnEmulatedFeatures;
  bool noWarnIgnoredFeatures;
  bool preserveBindings;
  bool preserveInterface;
  bool useDxLayout;
  bool useGlLayout;
  bool useLegacyBufferMatrixOrder;
  bool useScalarLayout;
  // UE Change Begin: Use custom layout rules for UE5.
  bool ue5Layout;
  // UE Change End: Use custom layout rules for UE5.
  // UE Change Begin: Force subpass OpTypeImage depth flag to be set to 0
  bool forceSubpassImageDepthFalse;
  // UE Change End: Force subpass OpTypeImage depth flag to be set to 0
  // UE Change Begin: Allow preserving unused inputs in shaders, used for OpenGL to match input/outputs
  bool preserveStorageInput;
  // UE Change End: Allow preserving unused inputs in shaders, used for OpenGL to match input/outputs
  bool flattenResourceArrays;
  bool reduceLoadSize;
  bool autoShiftBindings;
  bool supportNonzeroBaseInstance;
  bool fixFuncCallArguments;
  bool allowRWStructuredBufferArrays;
  /// Maximum length in words for the OpString literal containing the shader
  /// source for DebugSource and DebugSourceContinued. If the source code length
  /// is larger than this number, we will use DebugSourceContinued instructions
  /// for follow-up source code after the first DebugSource instruction. Note
  /// that this number must be less than or equal to 0xFFFDu because of the
  /// limitation of a single SPIR-V instruction size (0xFFFF) - 2 operand words
  /// for OpString. Currently a smaller value is only used to test
  /// DebugSourceContinued generation.
  uint32_t debugSourceLen;
  SpirvLayoutRule cBufferLayoutRule;
  SpirvLayoutRule sBufferLayoutRule;
  SpirvLayoutRule tBufferLayoutRule;
  SpirvLayoutRule ampPayloadLayoutRule;
  llvm::StringRef stageIoOrder;
  llvm::StringRef targetEnv;
  llvm::SmallVector<int32_t, 4> bShift;
  llvm::SmallVector<int32_t, 4> sShift;
  llvm::SmallVector<int32_t, 4> tShift;
  llvm::SmallVector<int32_t, 4> uShift;
  llvm::SmallVector<llvm::StringRef, 4> allowedExtensions;
  llvm::SmallVector<llvm::StringRef, 4> optConfig;
  std::vector<std::string> bindRegister;
  std::vector<std::string> bindGlobals;
  std::string entrypointName;

  bool signaturePacking; ///< Whether signature packing is enabled or not

  bool printAll; // Dump SPIR-V module before each pass and after the last one.

  // String representation of all command line options and input file.
  std::string clOptions;
  std::string inputFile;
};

} // namespace spirv
} // namespace clang

#endif // ENABLE_SPIRV_CODEGEN
#endif // LLVM_SPIRV_OPTIONS_H
