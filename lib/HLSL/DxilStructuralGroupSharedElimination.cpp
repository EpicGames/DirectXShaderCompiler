// Copyright Epic Games, Inc. All Rights Reserved.
// UE Change Begin: Structural groupshared elimination pass

#include "dxc/DXIL/DxilConstants.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilShaderFlags.h"
#include "dxc/HLSL/DxilGenerationPass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace hlsl;

namespace {

static bool CollectGroupSharedAccesses(Value *V,
                                       SmallVectorImpl<StoreInst *> &Writes) {
  bool HasRead = false;
  for (User *U : V->users()) {
    if (isa<LoadInst>(U)) {
      HasRead = true;
    } else if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      if (SI->getPointerOperand() == V && SI->isSimple())
        Writes.push_back(SI);
      else
        HasRead = true;
    } else if (Operator *Op = dyn_cast<Operator>(U)) {
      switch (Op->getOpcode()) {
      case Instruction::GetElementPtr:
      case Instruction::BitCast:
      case Instruction::AddrSpaceCast:
        HasRead |= CollectGroupSharedAccesses(U, Writes);
        break;
      default:
        HasRead = true;
        break;
      }
    } else {
      HasRead = true;
    }
  }
  return HasRead;
}

static bool EliminateWriteOnlyGroupShared(GlobalVariable *GV) {
  GV->removeDeadConstantUsers();

  SmallVector<StoreInst *, 16> Writes;
  if (CollectGroupSharedAccesses(GV, Writes))
    return false;

  SmallVector<WeakVH, 32> DeadSeeds;
  for (StoreInst *SI : Writes) {
    DeadSeeds.push_back(SI->getPointerOperand());
    DeadSeeds.push_back(SI->getValueOperand());
  }
  for (User *U : GV->users())
    if (Instruction *I = dyn_cast<Instruction>(U))
      DeadSeeds.push_back(I);

  bool Changed = false;
  for (StoreInst *SI : Writes) {
    SI->eraseFromParent();
    Changed = true;
  }

  for (WeakVH &VH : DeadSeeds) {
    if (!VH)
      continue;
    if (Instruction *I = dyn_cast<Instruction>(VH))
      Changed |= RecursivelyDeleteTriviallyDeadInstructions(I);
  }

  GV->removeDeadConstantUsers();
  if (GV->use_empty()) {
    GV->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

static bool RemoveDeadFunctionDeclarations(Module &M) {
  bool Changed = false;
  for (auto It = M.begin(), E = M.end(); It != E;) {
    Function *F = &*(It++);
    if (F->isDeclaration() && F->use_empty()) {
      F->eraseFromParent();
      Changed = true;
    }
  }
  return Changed;
}

static void RefreshShaderFlags(Module &M) {
  if (!M.HasDxilModule())
    return;
  
  DxilModule &DM = M.GetDxilModule();
  
  ShaderFlags Fresh;
  DM.CollectShaderFlagsForModule(Fresh);
  
  const uint64_t Mask = ShaderFlags::GetShaderFlagsRawForCollection();
  uint64_t Raw = DM.m_ShaderFlags.GetShaderFlagsRaw();
  Raw = (Raw & ~Mask) | (Fresh.GetShaderFlagsRaw() & Mask);
  DM.m_ShaderFlags.SetShaderFlagsRaw(Raw);
}

class StructuralGroupSharedElimination : public ModulePass {
public:
  static char ID;
  explicit StructuralGroupSharedElimination() : ModulePass(ID) {}

  StringRef getPassName() const override {
    return "DXIL structural groupshared elimination";
  }

  bool runOnModule(Module &M) override {
    bool Changed = false;

    bool Again = true;
    while (Again) {
      Again = false;
      for (auto It = M.global_begin(), E = M.global_end(); It != E;) {
        GlobalVariable *GV = &*(It++);
        if (GV->getType()->getAddressSpace() != DXIL::kTGSMAddrSpace)
          continue;
        if (EliminateWriteOnlyGroupShared(GV)) {
          Changed = true;
          Again = true;
        }
      }
    }

    if (Changed) {
      RemoveDeadFunctionDeclarations(M);
      RefreshShaderFlags(M);
    }

    return Changed;
  }
};
} // namespace

char StructuralGroupSharedElimination::ID = 0;

ModulePass *llvm::createStructuralGroupSharedEliminationPass() {
  return new StructuralGroupSharedElimination();
}

INITIALIZE_PASS(StructuralGroupSharedElimination,
                "hlsl-dxil-structural-groupshared-elimination",
                "DXIL structural groupshared elimination", false, false)

// UE Change End: Structural groupshared elimination pass
