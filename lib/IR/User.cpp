//===-- User.cpp - Implement the User class -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/User.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Operator.h"
// UE Change Begin: Improved cache locality of users
#include "llvm/Support/ThreadLocal.h"
// UE Change End: Improved cache locality of users

namespace llvm {
class BasicBlock;

//===----------------------------------------------------------------------===//
//                                 User Class
//===----------------------------------------------------------------------===//

void User::anchor() {}

void User::replaceUsesOfWith(Value *From, Value *To) {
  if (From == To) return;   // Duh what?

  assert((!isa<Constant>(this) || isa<GlobalValue>(this)) &&
         "Cannot call User::replaceUsesOfWith on a constant!");

  for (unsigned i = 0, E = getNumOperands(); i != E; ++i)
    if (getOperand(i) == From) {  // Is This operand is pointing to oldval?
      // The side effects of this setOperand call include linking to
      // "To", adding "this" to the uses list of To, and
      // most importantly, removing "this" from the use list of "From".
      setOperand(i, To); // Fix it now...
    }
}

//===----------------------------------------------------------------------===//
//                         User allocHungoffUses Implementation
//===----------------------------------------------------------------------===//

void User::allocHungoffUses(unsigned N, bool IsPhi) {
  assert(HasHungOffUses && "alloc must have hung off uses");

  static_assert(alignof(Use) >= alignof(BasicBlock *),
                "Alignment is insufficient for 'hung-off-uses' pieces");

  // Allocate the array of Uses, followed by a pointer (with bottom bit set) to
  // the User.
  size_t size = N * sizeof(Use);
  if (IsPhi)
    size += N * sizeof(BasicBlock *);
  Use *Begin = static_cast<Use*>(::operator new(size));
  Use *End = Begin + N;
  setOperandList(Begin);
  for (; Begin != End; Begin++)
    new (Begin) Use(this);
}

void User::growHungoffUses(unsigned NewNumUses, bool IsPhi) {
  assert(HasHungOffUses && "realloc must have hung off uses");

  unsigned OldNumUses = getNumOperands();

  // We don't support shrinking the number of uses.  We wouldn't have enough
  // space to copy the old uses in to the new space.
  assert(NewNumUses > OldNumUses && "realloc must grow num uses");

  Use *OldOps = getOperandList();
  allocHungoffUses(NewNumUses, IsPhi);
  Use *NewOps = getOperandList();

  // Now copy from the old operands list to the new one.
  std::copy(OldOps, OldOps + OldNumUses, NewOps);

  // If this is a Phi, then we need to copy the BB pointers too.
  if (IsPhi) {
    auto *OldPtr = reinterpret_cast<char *>(OldOps + OldNumUses);
    auto *NewPtr = reinterpret_cast<char *>(NewOps + NewNumUses);
    std::copy(OldPtr, OldPtr + (OldNumUses * sizeof(BasicBlock *)), NewPtr);
  }
  Use::zap(OldOps, OldOps + OldNumUses, true);
}

//===----------------------------------------------------------------------===//
//                         User operator new Implementations
//===----------------------------------------------------------------------===//

// UE Change Begin: Improved cache locality of users
#if DXC_USE_USER_BLOCK_ALLOCATOR
class UserBlockAllocator {
public:
  static constexpr size_t  BlockSize                = 1 << 16;
  static constexpr size_t  LargeAllocationThreshold = 1 << 12;
  static constexpr uint8_t InvalidBucket            = 1u << (User::AllocationBits - 1);

  ~UserBlockAllocator() {
    for (Block& Block : Blocks) {
      delete[] Block.Blob;
    }
  }

  void *Allocate(size_t Size) {
    if (Size > LargeAllocationThreshold) {
      void* Data = ::operator new(Size);
      SetPrivateData(Data, InvalidBucket);
      return Data;
    }

    // Check for a free allocation first, the bucket will be the nearest accommodating power of two.
    if (void* FreeUser; TryPopFree(GetReuseBucketIndex(Size), &FreeUser)) {
      return FreeUser;
    }

    Block &Block = GetFirstBlockFor(Size);
    void  *Data = Block.Blob + Block.Offset;

    // Store the size in previously unused memory
    const uint32_t BucketIndex = GetFreeBucketIndex(Size);
    assert(BucketIndex <= (1u << User::AllocationBits) && "Allocation size exceeds number of allotted bits");
    SetPrivateData(Data, BucketIndex);

    // Assume value alignment to platform
    constexpr size_t AlignmentSub1 = alignof(void *) - 1;
    Block.Offset = (Block.Offset + Size + AlignmentSub1) & ~(AlignmentSub1);

    return Data;
  }

  void Free(void *Ptr) {
    uint32_t BucketIndex = GetPrivateData(Ptr);
    if (BucketIndex == InvalidBucket) {
      ::operator delete(Ptr);
      return;
    }
    
    Bucket& Bucket = GetBucketFor(BucketIndex);
    Bucket.FreeUsers.push_back(Ptr);
  }

private:
  struct Block {
    uint8_t *Blob   = nullptr;
    uint64_t Offset = 0;
  };

  struct Bucket {
    std::vector<void*> FreeUsers;
  };

  Block &GetFirstBlockFor(size_t Size) {
    // Sequential user allocations are likely in the same bb, so don't try to bucket things
    
    if (!Blocks.empty()) {
      size_t Remaining = BlockSize - Blocks.back().Offset;

      if (Remaining >= Size) {
        return Blocks.back();
      }
    }

    Block &block = Blocks.emplace_back();
    block.Blob = new uint8_t[BlockSize];
    return block;
  }

  uint32_t GetFreeBucketIndex(size_t Size) {
    unsigned long Index;

    // Note that for free's we use the floored power of two, we never want to promote the size to something greater.
    // The effective wasted space is the difference to the previous (or equal) power of two
    uint8_t Result = _BitScanReverse(&Index, static_cast<uint32_t>(PowerOf2Floor(Size)));
    assert(Result);
    return Index;
  }

  uint32_t GetReuseBucketIndex(size_t Size) {
    unsigned long Index;

    // When allocating, we need the next power of two, or current, that can accomodate the allocation
    uint8_t Result = _BitScanReverse(&Index, static_cast<uint32_t>(NextPowerOf2(Size)));
    assert(Result);
    return Index;
  }
  
  Bucket& GetBucketFor(uint32_t BucketIndex) {
    if (Buckets.size() <= BucketIndex) {
      Buckets.resize(BucketIndex + 1);
    }

    return Buckets[BucketIndex];
  }

  bool TryPopFree(uint32_t BucketIndex, void **Out) {
    if (BucketIndex >= Buckets.size()) {
      return false;
    }

    Bucket& Bucket = Buckets[BucketIndex];
    if (Bucket.FreeUsers.empty()) {
      return false;
	}

    *Out = Bucket.FreeUsers.back();
    Bucket.FreeUsers.pop_back();
    
    return true;
  }

  uint32_t GetPrivateData(void *Ptr) {
    return static_cast<User *>(Ptr)->PrivateAllocatorData;
  }

  void SetPrivateData(void *Ptr, uint32_t Value) {
    static_cast<User *>(Ptr)->PrivateAllocatorData = Value;
  }

  std::vector<Block>  Blocks;
  std::vector<Bucket> Buckets;
};

static sys::ThreadLocal<UserBlockAllocator> g_ThreadUserAllocatorTls;

UserThreadAlloc::UserThreadAlloc() {
  assert(!g_ThreadUserAllocatorTls.get() && "User allocator already assigned");
  g_ThreadUserAllocatorTls.set(new UserBlockAllocator());
}

UserThreadAlloc::~UserThreadAlloc() {
  assert(g_ThreadUserAllocatorTls.get() && "User allocator double free");
  delete g_ThreadUserAllocatorTls.get();
  g_ThreadUserAllocatorTls.set(nullptr);
}

static void* UserAlloc(size_t Size) {
  if (UserBlockAllocator* Alloc = g_ThreadUserAllocatorTls.get()) {
    return Alloc->Allocate(Size);
  } else {
    return new uint8_t[Size];
  }
}

static void UserFree(void* Ptr) {
  if (UserBlockAllocator* Alloc = g_ThreadUserAllocatorTls.get()) {
    Alloc->Free(Ptr);
  } else {
    delete[] static_cast<uint8_t*>(Ptr);
  }
}
#endif // DXC_USE_USER_BLOCK_ALLOCATOR
// UE Change End: Improved cache locality of users

void *User::operator new(size_t Size, unsigned Us) {
  assert(Us < (1u << NumUserOperandsBits) && "Too many operands");
  // UE Change Begin: Improved cache locality of users
#if DXC_USE_USER_BLOCK_ALLOCATOR
  void *Storage = UserAlloc(Size + sizeof(Use) * Us);
#else // DXC_USE_USER_BLOCK_ALLOCATOR
  void *Storage = ::operator new(Size + sizeof(Use) * Us);
#endif // DXC_USE_USER_BLOCK_ALLOCATOR
  // UE Change End: Improved cache locality of users
  Use *Start = static_cast<Use*>(Storage);
  Use *End = Start + Us;
  User *Obj = reinterpret_cast<User*>(End);
  Obj->NumUserOperands = Us;
  Obj->HasHungOffUses = false;
  for (; Start != End; Start++)
    new (Start) Use(Obj);
  return Obj;
}

void *User::operator new(size_t Size) {
  // Allocate space for a single Use*
  // UE Change Begin: Improved cache locality of users
#if DXC_USE_USER_BLOCK_ALLOCATOR
  void *Storage = UserAlloc(Size + sizeof(Use *));
#else // DXC_USE_USER_BLOCK_ALLOCATOR
  void *Storage = ::operator new(Size + sizeof(Use *));
#endif // DXC_USE_USER_BLOCK_ALLOCATOR
  // UE Change End: Improved cache locality of users
  Use **HungOffOperandList = static_cast<Use **>(Storage);
  User *Obj = reinterpret_cast<User *>(HungOffOperandList + 1);
  Obj->NumUserOperands = 0;
  Obj->HasHungOffUses = true;
  *HungOffOperandList = nullptr;
  return Obj;
}

//===----------------------------------------------------------------------===//
//                         User operator delete Implementation
//===----------------------------------------------------------------------===//

void User::operator delete(void *Usr) {
  // Hung off uses use a single Use* before the User, while other subclasses
  // use a Use[] allocated prior to the user.
  User *Obj = static_cast<User *>(Usr);
  if (Obj->HasHungOffUses) {
    Use **HungOffOperandList = static_cast<Use **>(Usr) - 1;
    // drop the hung off uses.
    Use::zap(*HungOffOperandList, *HungOffOperandList + Obj->NumUserOperands,
             /* Delete */ true);
  // UE Change Begin: Improved cache locality of users
#if DXC_USE_USER_BLOCK_ALLOCATOR
    UserFree(HungOffOperandList);
#else // DXC_USE_USER_BLOCK_ALLOCATOR
    ::operator delete(HungOffOperandList);
#endif // DXC_USE_USER_BLOCK_ALLOCATOR
  // UE Change End: Improved cache locality of users
  } else {
    Use *Storage = static_cast<Use *>(Usr) - Obj->NumUserOperands;
    Use::zap(Storage, Storage + Obj->NumUserOperands,
             /* Delete */ false);
  // UE Change Begin: Improved cache locality of users
#if DXC_USE_USER_BLOCK_ALLOCATOR
    UserFree(Storage);
#else // DXC_USE_USER_BLOCK_ALLOCATOR
    ::operator delete(Storage);
#endif // DXC_USE_USER_BLOCK_ALLOCATOR
  // UE Change End: Improved cache locality of users
  }
}

// HLSL Change Starts
void User::operator delete(void *Usr, unsigned NumUserOperands) {
  // Fun fact: during construction Obj->NumUserOperands is overwritten
  Use *Storage = static_cast<Use *>(Usr) - NumUserOperands;
  Use::zap(Storage, Storage + NumUserOperands, /* Delete */ false);
  // UE Change Begin: Improved cache locality of users
#if DXC_USE_USER_BLOCK_ALLOCATOR
  UserFree(Storage);
#else // DXC_USE_USER_BLOCK_ALLOCATOR
  ::operator delete(Storage);
#endif // DXC_USE_USER_BLOCK_ALLOCATOR
  // UE Change End: Improved cache locality of users
}
// HLSL Change Ends

//===----------------------------------------------------------------------===//
//                             Operator Class
//===----------------------------------------------------------------------===//

Operator::~Operator() {
  llvm_unreachable("should never destroy an Operator");
}

} // End llvm namespace
