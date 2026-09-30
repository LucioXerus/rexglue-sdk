/**
 * @file        core/seh_posix.cpp
 * @brief       POSIX platform SEH implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>
#include <cstdlib>  // std::abort()

#if defined(__x86_64__)
#include <ucontext.h>  // ucontext_t for the SIGFPE self-heal below
#endif

#include <rex/platform/exceptions.h>

namespace rex::platform {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(uint32_t /*code*/, void* /*ep*/) {
  // Not used on POSIX - signal handlers throw directly
  return 0;
}

/// Signal handler for SIGSEGV/SIGBUS/SIGFPE/SIGILL
static void signal_handler(int sig, siginfo_t* info, void* ucontext) {
#if defined(__x86_64__)
  // Self-heal for SIMD FP faults with a corrupted exception mask.
  //
  // Recompiled guest code executes native divss/divps for PPC fdivs, which
  // trap with SIGFPE on x/0 when the MXCSR Zero-divide mask (bit 9) is clear.
  // ReXGlue's host policy is "all FP exceptions masked" (seeded by
  // FPSCRRegister::InitHost), and neither generated code nor the runtime ever
  // clears those masks -- but a stale MXCSR can still arrive via
  // swapcontext/setcontext restoring a recycled fiber ucontext (observed live
  // value 0x1FB2: ZM cleared). The game itself divides by zero routinely and
  // expects masked (inf/nan) semantics, so dying here is always wrong.
  //
  // Fix the mask IN THE SIGNAL FRAME (a plain _mm_setcsr would be discarded:
  // sigreturn restores FP state from the frame) and resume; the faulting
  // instruction re-executes with masked semantics and the game continues.
  // Only resumes when it actually changed something: an integer divide fault
  // (idiv) arrives with masks already set and falls through to die as before,
  // so this cannot turn a genuine crash into an infinite loop.
  if (sig == SIGFPE) {
    auto* uc = static_cast<ucontext_t*>(ucontext);
    if (uc != nullptr && uc->uc_mcontext.fpregs != nullptr) {
      constexpr uint32_t kExceptionMask = 0x1F80u;  // IM|DM|ZM|OM|UM|PM
      uint32_t mxcsr = uc->uc_mcontext.fpregs->mxcsr;
      if ((mxcsr & kExceptionMask) != kExceptionMask) {
        uc->uc_mcontext.fpregs->mxcsr = mxcsr | kExceptionMask;
        return;
      }
    }
  }
#endif
  // Only handle if we're in SEH-protected code
  if (!tls_seh_active) {
    // Not in SEH region - restore default handler and re-raise
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }

  // Determine exception code based on signal
  SehException::Code code;
  switch (sig) {
    case SIGSEGV:
      code = SehException::ACCESS_VIOLATION;
      break;
    case SIGBUS:
      code = SehException::IN_PAGE_ERROR;
      break;
    case SIGFPE:
      code = SehException::FLOAT_DIVIDE_BY_ZERO;
      break;
    case SIGILL:
      code = SehException::ILLEGAL_INSTRUCTION;
      break;
    default:
      code = SehException::UNKNOWN;
      break;
  }

  // Get fault address
  uintptr_t address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

  // Store in thread state for potential rethrow
  tls_seh_state.code = static_cast<uint32_t>(code);
  tls_seh_state.info[0] = 0;
  tls_seh_state.info[1] = address;

  // Use libunwind to throw from signal context
  // This works because libunwind can unwind through signal frames
  throw SehException(code, address);
}

[[noreturn]] void seh_rethrow() {
  // Map stored code back to signal and re-raise
  uint32_t code = tls_seh_state.code;
  int sig;
  switch (code) {
    case SehException::ACCESS_VIOLATION:
      sig = SIGSEGV;
      break;
    case SehException::IN_PAGE_ERROR:
      sig = SIGBUS;
      break;
    case SehException::FLOAT_DIVIDE_BY_ZERO:
    case SehException::INTEGER_DIVIDE_BY_ZERO:
      sig = SIGFPE;
      break;
    case SehException::ILLEGAL_INSTRUCTION:
      sig = SIGILL;
      break;
    default:
      abort();
  }
  // Restore default handler and re-raise
  signal(sig, SIG_DFL);
  raise(sig);
  abort();  // Should not reach here
}

void seh_initialize() {
  if (g_seh_initialized.exchange(true)) {
    return;  // Already initialized
  }

  struct sigaction sa;
  sa.sa_sigaction = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;  // SA_NODEFER allows re-entry for nested exceptions

  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform
