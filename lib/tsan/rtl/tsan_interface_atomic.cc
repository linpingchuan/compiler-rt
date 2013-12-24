//===-- tsan_interface_atomic.cc ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//

// ThreadSanitizer atomic operations are based on C++11/C1x standards.
// For background see C++11 standard.  A slightly older, publically
// available draft of the standard (not entirely up-to-date, but close enough
// for casual browsing) is available here:
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2011/n3242.pdf
// The following page contains more background information:
// http://www.hpl.hp.com/personal/Hans_Boehm/c++mm/

#include "sanitizer_common/sanitizer_placement_new.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "tsan_flags.h"
#include "tsan_rtl.h"

using namespace __tsan;  // NOLINT

#define SCOPED_ATOMIC(func, ...) \
    const uptr callpc = (uptr)__builtin_return_address(0); \
    uptr pc = __sanitizer::StackTrace::GetCurrentPc(); \
    mo = ConvertOrder(mo); \
    mo = flags()->force_seq_cst_atomics ? (morder)mo_seq_cst : mo; \
    ThreadState *const thr = cur_thread(); \
    AtomicStatInc(thr, sizeof(*a), mo, StatAtomic##func); \
    ScopedAtomic sa(thr, callpc, a, mo, __FUNCTION__); \
    return Atomic##func(thr, pc, __VA_ARGS__); \
/**/

// These should match declarations from public tsan_interface_atomic.h header.
typedef char     a8;
typedef short    a16;  // NOLINT
typedef int      a32;
typedef long     a64;  // NOLINT
#if defined(__SIZEOF_INT128__) \
    || (__clang_major__ * 100 + __clang_minor__ >= 302)
__extension__ typedef __int128 a128;
# define __TSAN_HAS_INT128 1
#else
# define __TSAN_HAS_INT128 0
#endif

// Part of ABI, do not change.
// http://llvm.org/viewvc/llvm-project/libcxx/trunk/include/atomic?view=markup
typedef enum {
  mo_relaxed,
  mo_consume,
  mo_acquire,
  mo_release,
  mo_acq_rel,
  mo_seq_cst
} morder;

class ScopedAtomic {
 public:
  ScopedAtomic(ThreadState *thr, uptr pc, const volatile void *a,
               morder mo, const char *func)
      : thr_(thr) {
    ProcessPendingSignals(thr);
    FuncEntry(thr_, pc);
    DPrintf("#%d: %s(%p, %d)\n", thr_->tid, func, a, mo);
  }
  ~ScopedAtomic() {
    FuncExit(thr_);
  }
 private:
  ThreadState *thr_;
};

static void AtomicStatInc(ThreadState *thr, uptr size, morder mo, StatType t) {
  StatInc(thr, StatAtomic);
  StatInc(thr, t);
  StatInc(thr, size == 1 ? StatAtomic1
             : size == 2 ? StatAtomic2
             : size == 4 ? StatAtomic4
             : size == 8 ? StatAtomic8
             :             StatAtomic16);
  StatInc(thr, mo == mo_relaxed ? StatAtomicRelaxed
             : mo == mo_consume ? StatAtomicConsume
             : mo == mo_acquire ? StatAtomicAcquire
             : mo == mo_release ? StatAtomicRelease
             : mo == mo_acq_rel ? StatAtomicAcq_Rel
             :                    StatAtomicSeq_Cst);
}

static bool IsLoadOrder(morder mo) {
  return mo == mo_relaxed || mo == mo_consume
      || mo == mo_acquire || mo == mo_seq_cst;
}

static bool IsStoreOrder(morder mo) {
  return mo == mo_relaxed || mo == mo_release || mo == mo_seq_cst;
}

static bool IsReleaseOrder(morder mo) {
  return mo == mo_release || mo == mo_acq_rel || mo == mo_seq_cst;
}

static bool IsAcquireOrder(morder mo) {
  return mo == mo_consume || mo == mo_acquire
      || mo == mo_acq_rel || mo == mo_seq_cst;
}

static bool IsAcqRelOrder(morder mo) {
  return mo == mo_acq_rel || mo == mo_seq_cst;
}

static morder ConvertOrder(morder mo) {
  if (mo > (morder)100500) {
    mo = morder(mo - 100500);
    if (mo ==  morder(1 << 0))
      mo = mo_relaxed;
    else if (mo == morder(1 << 1))
      mo = mo_consume;
    else if (mo == morder(1 << 2))
      mo = mo_acquire;
    else if (mo == morder(1 << 3))
      mo = mo_release;
    else if (mo == morder(1 << 4))
      mo = mo_acq_rel;
    else if (mo == morder(1 << 5))
      mo = mo_seq_cst;
  }
  CHECK_GE(mo, mo_relaxed);
  CHECK_LE(mo, mo_seq_cst);
  return mo;
}

template<typename T> T func_xchg(volatile T *v, T op) {
  T res = __sync_lock_test_and_set(v, op);
  // __sync_lock_test_and_set does not contain full barrier.
  __sync_synchronize();
  return res;
}

template<typename T> T func_add(volatile T *v, T op) {
  return __sync_fetch_and_add(v, op);
}

template<typename T> T func_sub(volatile T *v, T op) {
  return __sync_fetch_and_sub(v, op);
}

template<typename T> T func_and(volatile T *v, T op) {
  return __sync_fetch_and_and(v, op);
}

template<typename T> T func_or(volatile T *v, T op) {
  return __sync_fetch_and_or(v, op);
}

template<typename T> T func_xor(volatile T *v, T op) {
  return __sync_fetch_and_xor(v, op);
}

template<typename T> T func_nand(volatile T *v, T op) {
  // clang does not support __sync_fetch_and_nand.
  T cmp = *v;
  for (;;) {
    T newv = ~(cmp & op);
    T cur = __sync_val_compare_and_swap(v, cmp, newv);
    if (cmp == cur)
      return cmp;
    cmp = cur;
  }
}

template<typename T> T func_cas(volatile T *v, T cmp, T xch) {
  return __sync_val_compare_and_swap(v, cmp, xch);
}

// clang does not support 128-bit atomic ops.
// Atomic ops are executed under tsan internal mutex,
// here we assume that the atomic variables are not accessed
// from non-instrumented code.
#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
a128 func_xchg(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = op;
  return cmp;
}

a128 func_add(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = cmp + op;
  return cmp;
}

a128 func_sub(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = cmp - op;
  return cmp;
}

a128 func_and(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = cmp & op;
  return cmp;
}

a128 func_or(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = cmp | op;
  return cmp;
}

a128 func_xor(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = cmp ^ op;
  return cmp;
}

a128 func_nand(volatile a128 *v, a128 op) {
  a128 cmp = *v;
  *v = ~(cmp & op);
  return cmp;
}

a128 func_cas(volatile a128 *v, a128 cmp, a128 xch) {
  a128 cur = *v;
  if (cur == cmp)
    *v = xch;
  return cur;
}
#endif

template<typename T>
static int SizeLog() {
  if (sizeof(T) <= 1)
    return kSizeLog1;
  else if (sizeof(T) <= 2)
    return kSizeLog2;
  else if (sizeof(T) <= 4)
    return kSizeLog4;
  else
    return kSizeLog8;
  // For 16-byte atomics we also use 8-byte memory access,
  // this leads to false negatives only in very obscure cases.
}

template<typename T>
static T AtomicLoad(ThreadState *thr, uptr pc, const volatile T *a,
    morder mo) {
  CHECK(IsLoadOrder(mo));
  // This fast-path is critical for performance.
  // Assume the access is atomic.
  if (!IsAcquireOrder(mo) && sizeof(T) <= sizeof(a)) {
    MemoryReadAtomic(thr, pc, (uptr)a, SizeLog<T>());
    return *a;  // as if atomic
  }
  SyncVar *s = CTX()->synctab.GetOrCreateAndLock(thr, pc, (uptr)a, false);
  AcquireImpl(thr, pc, &s->clock);
  T v = *a;
  s->mtx.ReadUnlock();
  __sync_synchronize();
  MemoryReadAtomic(thr, pc, (uptr)a, SizeLog<T>());
  return v;
}

template<typename T>
static void AtomicStore(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  CHECK(IsStoreOrder(mo));
  MemoryWriteAtomic(thr, pc, (uptr)a, SizeLog<T>());
  // This fast-path is critical for performance.
  // Assume the access is atomic.
  // Strictly saying even relaxed store cuts off release sequence,
  // so must reset the clock.
  if (!IsReleaseOrder(mo) && sizeof(T) <= sizeof(a)) {
    *a = v;  // as if atomic
    return;
  }
  __sync_synchronize();
  SyncVar *s = CTX()->synctab.GetOrCreateAndLock(thr, pc, (uptr)a, true);
  thr->fast_state.IncrementEpoch();
  // Can't increment epoch w/o writing to the trace as well.
  TraceAddEvent(thr, thr->fast_state, EventTypeMop, 0);
  ReleaseImpl(thr, pc, &s->clock);
  *a = v;
  s->mtx.Unlock();
  // Trainling memory barrier to provide sequential consistency
  // for Dekker-like store-load synchronization.
  __sync_synchronize();
}

template<typename T, T (*F)(volatile T *v, T op)>
static T AtomicRMW(ThreadState *thr, uptr pc, volatile T *a, T v, morder mo) {
  MemoryWriteAtomic(thr, pc, (uptr)a, SizeLog<T>());
  SyncVar *s = 0;
  if (mo != mo_relaxed) {
    s = CTX()->synctab.GetOrCreateAndLock(thr, pc, (uptr)a, true);
    thr->fast_state.IncrementEpoch();
    // Can't increment epoch w/o writing to the trace as well.
    TraceAddEvent(thr, thr->fast_state, EventTypeMop, 0);
    if (IsAcqRelOrder(mo))
      AcquireReleaseImpl(thr, pc, &s->clock);
    else if (IsReleaseOrder(mo))
      ReleaseImpl(thr, pc, &s->clock);
    else if (IsAcquireOrder(mo))
      AcquireImpl(thr, pc, &s->clock);
  }
  v = F(a, v);
  if (s)
    s->mtx.Unlock();
  return v;
}

template<typename T>
static T AtomicExchange(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_xchg>(thr, pc, a, v, mo);
}

template<typename T>
static T AtomicFetchAdd(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_add>(thr, pc, a, v, mo);
}

template<typename T>
static T AtomicFetchSub(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_sub>(thr, pc, a, v, mo);
}

template<typename T>
static T AtomicFetchAnd(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_and>(thr, pc, a, v, mo);
}

template<typename T>
static T AtomicFetchOr(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_or>(thr, pc, a, v, mo);
}

template<typename T>
static T AtomicFetchXor(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_xor>(thr, pc, a, v, mo);
}

template<typename T>
static T AtomicFetchNand(ThreadState *thr, uptr pc, volatile T *a, T v,
    morder mo) {
  return AtomicRMW<T, func_nand>(thr, pc, a, v, mo);
}

template<typename T>
static bool AtomicCAS(ThreadState *thr, uptr pc,
    volatile T *a, T *c, T v, morder mo, morder fmo) {
  (void)fmo;  // Unused because llvm does not pass it yet.
  MemoryWriteAtomic(thr, pc, (uptr)a, SizeLog<T>());
  SyncVar *s = 0;
  if (mo != mo_relaxed) {
    s = CTX()->synctab.GetOrCreateAndLock(thr, pc, (uptr)a, true);
    thr->fast_state.IncrementEpoch();
    // Can't increment epoch w/o writing to the trace as well.
    TraceAddEvent(thr, thr->fast_state, EventTypeMop, 0);
    if (IsAcqRelOrder(mo))
      AcquireReleaseImpl(thr, pc, &s->clock);
    else if (IsReleaseOrder(mo))
      ReleaseImpl(thr, pc, &s->clock);
    else if (IsAcquireOrder(mo))
      AcquireImpl(thr, pc, &s->clock);
  }
  T cc = *c;
  T pr = func_cas(a, cc, v);
  if (s)
    s->mtx.Unlock();
  if (pr == cc)
    return true;
  *c = pr;
  return false;
}

template<typename T>
static T AtomicCAS(ThreadState *thr, uptr pc,
    volatile T *a, T c, T v, morder mo, morder fmo) {
  AtomicCAS(thr, pc, a, &c, v, mo, fmo);
  return c;
}

static void AtomicFence(ThreadState *thr, uptr pc, morder mo) {
  // FIXME(dvyukov): not implemented.
  __sync_synchronize();
}

extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_load(const volatile a8 *a, morder mo) {
  SCOPED_ATOMIC(Load, a, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_load(const volatile a16 *a, morder mo) {
  SCOPED_ATOMIC(Load, a, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_load(const volatile a32 *a, morder mo) {
  SCOPED_ATOMIC(Load, a, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_load(const volatile a64 *a, morder mo) {
  SCOPED_ATOMIC(Load, a, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_load(const volatile a128 *a, morder mo) {
  SCOPED_ATOMIC(Load, a, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic8_store(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(Store, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic16_store(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(Store, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic32_store(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(Store, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic64_store(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(Store, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic128_store(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(Store, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_exchange(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(Exchange, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_exchange(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(Exchange, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_exchange(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(Exchange, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_exchange(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(Exchange, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_exchange(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(Exchange, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_fetch_add(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(FetchAdd, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_fetch_add(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(FetchAdd, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_fetch_add(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(FetchAdd, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_fetch_add(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(FetchAdd, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_fetch_add(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(FetchAdd, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_fetch_sub(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(FetchSub, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_fetch_sub(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(FetchSub, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_fetch_sub(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(FetchSub, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_fetch_sub(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(FetchSub, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_fetch_sub(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(FetchSub, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_fetch_and(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(FetchAnd, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_fetch_and(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(FetchAnd, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_fetch_and(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(FetchAnd, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_fetch_and(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(FetchAnd, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_fetch_and(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(FetchAnd, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_fetch_or(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(FetchOr, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_fetch_or(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(FetchOr, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_fetch_or(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(FetchOr, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_fetch_or(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(FetchOr, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_fetch_or(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(FetchOr, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_fetch_xor(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(FetchXor, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_fetch_xor(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(FetchXor, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_fetch_xor(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(FetchXor, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_fetch_xor(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(FetchXor, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_fetch_xor(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(FetchXor, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_fetch_nand(volatile a8 *a, a8 v, morder mo) {
  SCOPED_ATOMIC(FetchNand, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_fetch_nand(volatile a16 *a, a16 v, morder mo) {
  SCOPED_ATOMIC(FetchNand, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_fetch_nand(volatile a32 *a, a32 v, morder mo) {
  SCOPED_ATOMIC(FetchNand, a, v, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_fetch_nand(volatile a64 *a, a64 v, morder mo) {
  SCOPED_ATOMIC(FetchNand, a, v, mo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_fetch_nand(volatile a128 *a, a128 v, morder mo) {
  SCOPED_ATOMIC(FetchNand, a, v, mo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic8_compare_exchange_strong(volatile a8 *a, a8 *c, a8 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic16_compare_exchange_strong(volatile a16 *a, a16 *c, a16 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic32_compare_exchange_strong(volatile a32 *a, a32 *c, a32 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic64_compare_exchange_strong(volatile a64 *a, a64 *c, a64 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic128_compare_exchange_strong(volatile a128 *a, a128 *c, a128 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic8_compare_exchange_weak(volatile a8 *a, a8 *c, a8 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic16_compare_exchange_weak(volatile a16 *a, a16 *c, a16 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic32_compare_exchange_weak(volatile a32 *a, a32 *c, a32 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic64_compare_exchange_weak(volatile a64 *a, a64 *c, a64 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
int __tsan_atomic128_compare_exchange_weak(volatile a128 *a, a128 *c, a128 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
a8 __tsan_atomic8_compare_exchange_val(volatile a8 *a, a8 c, a8 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a16 __tsan_atomic16_compare_exchange_val(volatile a16 *a, a16 c, a16 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a32 __tsan_atomic32_compare_exchange_val(volatile a32 *a, a32 c, a32 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

SANITIZER_INTERFACE_ATTRIBUTE
a64 __tsan_atomic64_compare_exchange_val(volatile a64 *a, a64 c, a64 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}

#if __TSAN_HAS_INT128
SANITIZER_INTERFACE_ATTRIBUTE
a128 __tsan_atomic128_compare_exchange_val(volatile a128 *a, a128 c, a128 v,
    morder mo, morder fmo) {
  SCOPED_ATOMIC(CAS, a, c, v, mo, fmo);
}
#endif

SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic_thread_fence(morder mo) {
  char* a = 0;
  SCOPED_ATOMIC(Fence, mo);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __tsan_atomic_signal_fence(morder mo) {
}
}  // extern "C"
