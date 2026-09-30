/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>

#if defined(__x86_64__)
// <x86intrin.h> for _mm_getcsr/_mm_setcsr (diagnostic hardening, see below).
#include <x86intrin.h>

namespace rex::thread::detail {
// Host FP exception policy: all SIMD FP exceptions masked. Guest code assumes
// this (the recompiler emits native divss/divps for PPC fdivs, which trap on
// x/0 unless the Zero-divide mask is set). ReXGlue seeds this policy at thread
// start (FPSCRRegister::InitHost) but ucontext save/restore in swapcontext can
// reinstate a stale MXCSR from a recycled fiber context, so re-assert it after
// every fiber switch. Cost when already correct: one stmxcsr (no serializing
// ldmxcsr). See NFSMW-Recompiled Linux bring-up notes (SIGFPE/MXCSR 0x1FB2).
inline void ReassertHostFpMasks() noexcept {
  constexpr uint32_t kExceptionMask = 0x1F80u;  // IM|DM|ZM|OM|UM|PM
  const uint32_t csr = _mm_getcsr();
  if ((csr & kExceptionMask) != kExceptionMask) [[unlikely]] {
    _mm_setcsr(csr | kExceptionMask);
  }
}
}  // namespace rex::thread::detail
#endif  // defined(__x86_64__)

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
#if defined(__x86_64__)
  // Don't seed the new ucontext with a stale MXCSR.
  detail::ReassertHostFpMasks();
#endif
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

#if defined(__x86_64__)
  // Don't seed the new ucontext with a stale MXCSR.
  detail::ReassertHostFpMasks();
#endif
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
#if defined(__x86_64__)
  // swapcontext restores MXCSR from the target ucontext; a stale/recycled
  // context can unmask FP exceptions (observed: ZM cleared -> SIGFPE on the
  // next x/0.0 divss in recompiled guest code). Re-assert host policy.
  detail::ReassertHostFpMasks();
#endif
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
