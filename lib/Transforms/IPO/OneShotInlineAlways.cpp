// UE Change Begin: Reimplemented inliner scheduler with a one-shot approach

#include "llvm/Transforms/IPO.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <queue>

using namespace llvm;

#define DEBUG_TYPE "inline"

/**
 * Shader inlining can make a lot of general assumptions that end up reducing
 * the inlining overhead, particularly on instruction cloning and re-linking.
 *
 * Say you have three definitions (A, B being always_inline):
 *   A:    <set>
 *   B:    A()
 *   main: B()
 *
 * SCG-pass iteration may produce:
 *   A:    <set>
 *   B:    A()   | -> <set>
 *   main: B()   | -> A()   | -> <set>
 *
 * With a sufficiently complex module, repeated instruction cloning of
 * large instruction blocks is problematic. Instead, we clone from the roots:
 *   main: B() -> A() -> <set>
 *
 * The total number of inlined calls is roughly the same, however, the number
 * of moved instructions has been reduced dramatically. For example, a
 * non-trivial compute rasterization shader:
 *   InlineAlways        : 1'199'293 instruction moves
 *   OneShotInlineAlways :   222'575 instruction moves
 *
 * Roots are determined from the first caller, within the global function set,
 * that is either not always_inline, or a function with non-trivial
 * references (e.g., md).
 */

namespace {

class OneShotAlwaysInliner : public ModulePass {
  bool InsertLifetime;

public:
  static char ID;
  
  OneShotAlwaysInliner(bool InsertLifetime)
    : ModulePass(ID)
    , InsertLifetime(InsertLifetime) {
  }

  OneShotAlwaysInliner() : OneShotAlwaysInliner(false) {
    /** poof */
  }

  bool runOnModule(Module &M) override {
    bool ModuleMutated = false;
    
    AliasAnalysis& AA  = getAnalysis<AliasAnalysis>();
    AssumptionCacheTracker& ACT = getAnalysis<AssumptionCacheTracker>();
    CallGraph& CG  = getAnalysis<CallGraphWrapperPass>().getCallGraph();

    // Find all the root sites to traverse down from
    RootContainer RootSites;
    FindRootSites(M, RootSites);

    // Collect all call sites of the roots
    WorkContainer WorkList;
    for (Function* Site : RootSites) {
      for (BasicBlock& BB : Site->getBasicBlockList()) {
        for (Instruction &I : BB) {
          AppendWork(&I, WorkList);
        }
      }
    } 
    
    InlineFunctionInfo  IFI(&CG, &AA, &ACT);
    DenseSet<Function*> InlinedSites;

    // Work loop, inline until we're out of call sites
    while (!WorkList.empty()) {
      CallSite CS = WorkList.front();
      WorkList.pop();

      // Actually inline the site
      bool Result = InlineFunction(CS, IFI, InsertLifetime);
      ModuleMutated |= Result;

      if (!Result) {
        continue;
      }
      
      // Keep track of all unique inlined sites
      InlinedSites.insert(CS.getCalledFunction());

      // Traverse down the call chain
      for (Value* call : IFI.InlinedCalls) {
        AppendWork(cast<Instruction>(call), WorkList);
      }

      IFI.reset();
    }

    // Prune dead functions
    for (Function* F : InlinedSites) {
      F->removeDeadConstantUsers();

      // No pending users?
      if (!F->isDefTriviallyDead()) {
        continue;
      }
      
      // Cleanup the call graph
      CallGraphNode *CGN = CG.getOrInsertFunction(F);
      CGN->removeAllCalledFunctions();
      CG.getExternalCallingNode()->removeAnyCallEdgeTo(CGN);

      // Actually remove it
      delete CG.removeFunctionFromModule(CGN);
    }
    
    return ModuleMutated;
  }
  
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<CallGraphWrapperPass>();
    AU.addRequired<AliasAnalysis>();
  }

private:
  using RootContainer = std::vector<Function*>;
  using WorkContainer = std::queue<CallSite>;

  void FindRootSites(Module &M, RootContainer& RootSites) {
    std::queue<Function*> WorkList;
    DenseSet<Function*>   VisitedRoots;

    // Collect all always_inline functions
    for (Function& F : M.getFunctionList()) {
      if (F.hasFnAttribute(Attribute::AlwaysInline)) {
        WorkList.push(&F);
        VisitedRoots.insert(&F);
      }
    }

    while (!WorkList.empty()) {
      Function* F = WorkList.front();
      WorkList.pop();

      // If this isn't always inlined, consider it a root site
      if (!F->hasFnAttribute(Attribute::AlwaysInline)) {
        RootSites.push_back(F);
        continue;
      }

      // If this is trivially dead, ignore it and let the pruning clean it up
      F->removeDeadConstantUsers();
      if (F->isDefTriviallyDead()) {
        continue;
      }

      bool hasNonTrivialUser = false;

      // Otherwise, propagate up
      for (User* User : F->users()) {
        if (!isa<CallInst>(User)) {
          hasNonTrivialUser = true;
          continue;
        }
        
        CallInst* CI = cast<CallInst>(User);

        // Push if not already visited
        Function* Caller = CI->getParent()->getParent();
        if (!VisitedRoots.count(Caller)) {
          WorkList.push(Caller);
          VisitedRoots.insert(Caller);
        }
      }

      // If there's a non-trivial user, like md reference and whatnot,
      // we consider this a root site. Otherwise, it'll get cleaned up
      // during pruning
      if (hasNonTrivialUser) {
        RootSites.push_back(F);
      }
    }

#ifndef NDEBUG
    for (Function* F : RootSites) {
      DEBUG(dbgs() << "Root Site: " << F->getName() << "\n");
    }
#endif // NDEBUG
  }
  
  void AppendWork(Instruction *I, WorkContainer& WorkList) {
    CallSite CS(cast<Value>(I));

    // Ignore intrinsics
    if (!CS || isa<IntrinsicInst>(I))
      return;

    // Really should be a call at this point
    CallInst *CI = cast<CallInst>(I);
    if (!CI) {
      return;
    }

    // Ignore declaration only functions, irrelevant
    Function* Callee = CI->getCalledFunction();
    if (Callee && Callee->isDeclaration()) {
      return;
    }
    
    WorkList.push(CS);
  }
};

}

char OneShotAlwaysInliner::ID = 0;

INITIALIZE_PASS_BEGIN(OneShotAlwaysInliner, "one-shot-always-inline",
                      "Inliner for one-shot always_inline functions",
                      false, false)
  INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
  INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(InlineCostAnalysis)
INITIALIZE_PASS_END(OneShotAlwaysInliner, "one-shot-always-inline",
                    "Inliner for always_inline functions with one-shot ",
                    false, false)

Pass *llvm::createOneShotAlwaysInlinerPass(bool InsertLifetime) {
  return new OneShotAlwaysInliner(InsertLifetime);
}

// UE Change End: Reimplemented inliner scheduler with a one-shot approach
