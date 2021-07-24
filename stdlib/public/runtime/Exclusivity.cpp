//===--- Exclusivity.cpp - Exclusivity tracking ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This implements the runtime support for dynamically tracking exclusivity.
//
//===----------------------------------------------------------------------===//

// NOTE: This should really be applied in the CMakeLists.txt.  However, we do
// not have a way to currently specify that at the target specific level yet.

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VCEXTRALEAN
#endif

#include "swift/Runtime/Exclusivity.h"
#include "../SwiftShims/Visibility.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/ThreadLocalStorage.h"
#include <inttypes.h>
#include <memory>
#include <stdio.h>

// Pick a return-address strategy
#if __GNUC__
#define get_return_address() __builtin_return_address(0)
#elif _MSC_VER
#include <intrin.h>
#define get_return_address() _ReturnAddress()
#else
#error missing implementation for get_return_address
#define get_return_address() ((void*) 0)
#endif

using namespace swift;

bool swift::_swift_disableExclusivityChecking = false;

static const char *getAccessName(ExclusivityFlags flags) {
  switch (flags) {
  case ExclusivityFlags::Read: return "read";
  case ExclusivityFlags::Modify: return "modification";
  default: return "unknown";
  }
}

// In asserts builds if the environment variable
// SWIFT_DEBUG_RUNTIME_EXCLUSIVITY_LOGGING is set, emit logging information.
#ifndef NDEBUG

static inline bool isExclusivityLoggingEnabled() {
  return runtime::environment::SWIFT_DEBUG_RUNTIME_EXCLUSIVITY_LOGGING();
}

static inline void _flockfile_stderr() {
#if defined(_WIN32)
  _lock_file(stderr);
#elif defined(__wasi__)
  // WebAssembly/WASI doesn't support file locking yet
  // https://bugs.swift.org/browse/SR-12097
#else
  flockfile(stderr);
#endif
}

static inline void _funlockfile_stderr() {
#if defined(_WIN32)
  _unlock_file(stderr);
#elif defined(__wasi__)
  // WebAssembly/WASI doesn't support file locking yet
  // https://bugs.swift.org/browse/SR-12097
#else
  funlockfile(stderr);
#endif
}

/// Used to ensure that logging printfs are deterministic.
static inline void withLoggingLock(std::function<void()> func) {
  assert(isExclusivityLoggingEnabled() &&
         "Should only be called if exclusivity logging is enabled!");

  _flockfile_stderr();
  func();
  fflush(stderr);
  _funlockfile_stderr();
}

#endif

SWIFT_ALWAYS_INLINE
static void reportExclusivityConflict(ExclusivityFlags oldAction, void *oldPC,
                                      ExclusivityFlags newFlags, void *newPC,
                                      void *pointer) {
  constexpr unsigned maxMessageLength = 100;
  constexpr unsigned maxAccessDescriptionLength = 50;
  char message[maxMessageLength];
  snprintf(message, sizeof(message),
           "Simultaneous accesses to 0x%" PRIxPTR ", but modification requires "
           "exclusive access",
           reinterpret_cast<uintptr_t>(pointer));
  fprintf(stderr, "%s.\n", message);

  char oldAccess[maxAccessDescriptionLength];
  snprintf(oldAccess, sizeof(oldAccess),
           "Previous access (a %s) started at", getAccessName(oldAction));
  fprintf(stderr, "%s ", oldAccess);
  if (oldPC) {
    dumpStackTraceEntry(0, oldPC, /*shortOutput=*/true);
    fprintf(stderr, " (0x%" PRIxPTR ").\n", reinterpret_cast<uintptr_t>(oldPC));
  } else {
    fprintf(stderr, "<unknown>.\n");
  }

  char newAccess[maxAccessDescriptionLength];
  snprintf(newAccess, sizeof(newAccess), "Current access (a %s) started at",
           getAccessName(getAccessAction(newFlags)));
  fprintf(stderr, "%s:\n", newAccess);
  // The top frame is in swift_beginAccess, don't print it.
  constexpr unsigned framesToSkip = 1;
  printCurrentBacktrace(framesToSkip);

  RuntimeErrorDetails::Thread secondaryThread = {
    .description = oldAccess,
    .numFrames = 1,
    .frames = &oldPC
  };
  RuntimeErrorDetails details = {
    .version = RuntimeErrorDetails::currentVersion,
    .errorType = "exclusivity-violation",
    .currentStackDescription = newAccess,
    .framesToSkip = framesToSkip,
    .memoryAddress = pointer,
    .numExtraThreads = 1,
    .threads = &secondaryThread
  };
  _swift_reportToDebugger(RuntimeErrorFlagFatal, message, &details);
}

namespace {

/// A single access that we're tracking.
///
/// The following inputs are accepted by the begin_access runtime entry
/// point. This table show the action performed by the current runtime to
/// convert those inputs into stored fields in the Access scratch buffer.
///
/// Pointer | Runtime     | Access | PC    | Reported| Access
/// Argument| Behavior    | Pointer| Arg   | PC      | PC
/// -------- ------------- -------- ------- --------- ----------
/// null    | [trap or missing enforcement]
/// nonnull | [nontracked]| null   | null  | caller  | [discard]
/// nonnull | [nontracked]| null   | valid | <same>  | [discard]
/// nonnull | [tracked]   | <same> | null  | caller  | caller
/// nonnull | [tracked]   | <same> | valid | <same>  | <same>
///
/// [nontracked] means that the Access scratch buffer will not be added to the
/// runtime's list of tracked accesses. However, it may be passed to a
/// subsequent call to end_unpaired_access. The null Pointer field then
/// identifies the Access record as nontracked.
///
/// The runtime owns the contents of the scratch buffer, which is allocated by
/// the compiler but otherwise opaque. The runtime may later reuse the Pointer
/// or PC fields or any spare bits for additional flags, and/or a pointer to
/// out-of-line storage.
struct Access {
  void *Pointer;
  void *PC;
  uintptr_t NextAndAction;

  enum : uintptr_t {
    ActionMask = (uintptr_t)ExclusivityFlags::ActionMask,
    NextMask = ~ActionMask
  };

  Access *getNext() const {
    return reinterpret_cast<Access*>(NextAndAction & NextMask);
  }

  void setNext(Access *next) {
    NextAndAction =
      reinterpret_cast<uintptr_t>(next) | (NextAndAction & ActionMask);
  }

  ExclusivityFlags getAccessAction() const {
    return ExclusivityFlags(NextAndAction & ActionMask);
  }

  void initialize(void *pc, void *pointer, Access *next,
                  ExclusivityFlags action) {
    Pointer = pointer;
    PC = pc;
    NextAndAction = reinterpret_cast<uintptr_t>(next) | uintptr_t(action);
  }
};

static_assert(sizeof(Access) <= sizeof(ValueBuffer) &&
              alignof(Access) <= alignof(ValueBuffer),
              "Access doesn't fit in a value buffer!");

/// A set of accesses that we're tracking.  Just a singly-linked list.
class AccessSet {
  Access *Head = nullptr;
public:
  constexpr AccessSet() {}
  constexpr AccessSet(Access *Head) : Head(Head) {}

  constexpr operator bool() const { return bool(Head); }
  constexpr Access *getHead() const { return Head; }
  void setHead(Access *newHead) { Head = newHead; }
  constexpr bool isHead(Access *access) const { return Head == access; }

  bool insert(Access *access, void *pc, void *pointer, ExclusivityFlags flags) {
#ifndef NDEBUG
    if (isExclusivityLoggingEnabled()) {
      withLoggingLock(
          [&]() { fprintf(stderr, "Inserting new access: %p\n", access); });
    }
#endif
    auto action = getAccessAction(flags);

    for (Access *cur = Head; cur != nullptr; cur = cur->getNext()) {
      // Ignore accesses to different values.
      if (cur->Pointer != pointer)
        continue;

      // If both accesses are reads, it's not a conflict.
      if (action == ExclusivityFlags::Read &&
          action == cur->getAccessAction())
        continue;

      // Otherwise, it's a conflict.
      reportExclusivityConflict(cur->getAccessAction(), cur->PC,
                                flags, pc, pointer);

      // 0 means no backtrace will be printed.
      fatalError(0, "Fatal access conflict detected.\n");
    }
    if (!isTracking(flags)) {
#ifndef NDEBUG
      if (isExclusivityLoggingEnabled()) {
        withLoggingLock([&]() { fprintf(stderr, "  Not tracking!\n"); });
      }
#endif
      return false;
    }

    // Insert to the front of the array so that remove tends to find it faster.
    access->initialize(pc, pointer, Head, action);
    Head = access;
#ifndef NDEBUG
    if (isExclusivityLoggingEnabled()) {
      withLoggingLock([&]() {
        fprintf(stderr, "  Tracking!\n");
        swift_dumpTrackedAccesses();
      });
    }
#endif
    return true;
  }

  void remove(Access *access) {
    assert(Head && "removal from empty AccessSet");
#ifndef NDEBUG
    if (isExclusivityLoggingEnabled()) {
      withLoggingLock(
          [&]() { fprintf(stderr, "Removing access: %p\n", access); });
    }
#endif
    auto cur = Head;
    // Fast path: stack discipline.
    if (cur == access) {
      Head = cur->getNext();
      return;
    }

    Access *last = cur;
    for (cur = cur->getNext(); cur != nullptr;
         last = cur, cur = cur->getNext()) {
      assert(last->getNext() == cur);
      if (cur == access) {
        last->setNext(cur->getNext());
        return;
      }
    }

    swift_unreachable("access not found in set");
  }

  /// Return the parent access of \p childAccess in the list.
  Access *findParentAccess(Access *childAccess) {
    auto cur = Head;
    Access *last = cur;
    for (cur = cur->getNext(); cur != nullptr;
         last = cur, cur = cur->getNext()) {
      assert(last->getNext() == cur);
      if (cur == childAccess) {
        return last;
      }
    }
    return nullptr;
  }

  Access *getTail() const {
    auto cur = Head;
    if (!cur)
      return nullptr;

    while (auto *next = cur->getNext()) {
      cur = next;
    }
    assert(cur != nullptr);
    return cur;
  }

#ifndef NDEBUG
  /// Only available with asserts. Intended to be used with
  /// swift_dumpTrackedAccess().
  void forEach(std::function<void (Access *)> action) {
    for (auto *iter = Head; iter != nullptr; iter = iter->getNext()) {
      action(iter);
    }
  }
#endif
};

class SwiftTLSContext {
public:
  /// The set of tracked accesses.
  AccessSet accessSet;

  // The "implicit" boolean parameter which is passed to a dynamically
  // replaceable function.
  // If true, the original function should be executed instead of the
  // replacement function.
  bool CallOriginalOfReplacedFunction = false;
};

} // end anonymous namespace

// Each of these cases should define a function with this prototype:
//   AccessSets &getAllSets();

#ifdef SWIFT_STDLIB_SINGLE_THREADED_RUNTIME

static SwiftTLSContext &getTLSContext() {
  static SwiftTLSContext TLSContext;
  return TLSContext;
}

#elif SWIFT_TLS_HAS_RESERVED_PTHREAD_SPECIFIC
// Use the reserved TSD key if possible.

static SwiftTLSContext &getTLSContext() {
  SwiftTLSContext *ctx = static_cast<SwiftTLSContext*>(
    SWIFT_THREAD_GETSPECIFIC(SWIFT_RUNTIME_TLS_KEY));
  if (ctx)
    return *ctx;
  
  static OnceToken_t setupToken;
  SWIFT_ONCE_F(setupToken, [](void *) {
    pthread_key_init_np(SWIFT_RUNTIME_TLS_KEY, [](void *pointer) {
      delete static_cast<SwiftTLSContext*>(pointer);
    });
  }, nullptr);
  
  ctx = new SwiftTLSContext();
  SWIFT_THREAD_SETSPECIFIC(SWIFT_RUNTIME_TLS_KEY, ctx);
  return *ctx;
}

#elif __has_feature(cxx_thread_local)
// Second choice is direct language support for thread-locals.

static thread_local SwiftTLSContext TLSContext;

static SwiftTLSContext &getTLSContext() {
  return TLSContext;
}

#else
// Use the platform thread-local data API.

static __swift_thread_key_t createSwiftThreadKey() {
  __swift_thread_key_t key;
  int result = SWIFT_THREAD_KEY_CREATE(&key, [](void *pointer) {
    delete static_cast<SwiftTLSContext*>(pointer);
  });

  if (result != 0) {
    fatalError(0, "couldn't create thread key for exclusivity: %s\n",
               strerror(result));
  }
  return key;
}

static SwiftTLSContext &getTLSContext() {
  static __swift_thread_key_t key = createSwiftThreadKey();

  SwiftTLSContext *ctx = static_cast<SwiftTLSContext*>(SWIFT_THREAD_GETSPECIFIC(key));
  if (!ctx) {
    ctx = new SwiftTLSContext();
    SWIFT_THREAD_SETSPECIFIC(key, ctx);
  }
  return *ctx;
}

#endif

/// Begin tracking a dynamic access.
///
/// This may cause a runtime failure if an incompatible access is
/// already underway.
void swift::swift_beginAccess(void *pointer, ValueBuffer *buffer,
                              ExclusivityFlags flags, void *pc) {
  assert(pointer && "beginning an access on a null pointer?");

  Access *access = reinterpret_cast<Access*>(buffer);

  // If exclusivity checking is disabled, record in the access buffer that we
  // didn't track anything. pc is currently undefined in this case.
  if (_swift_disableExclusivityChecking) {
    access->Pointer = nullptr;
    return;
  }

  // If the provided `pc` is null, then the runtime may override it for
  // diagnostics.
  if (!pc)
    pc = get_return_address();

  if (!getTLSContext().accessSet.insert(access, pc, pointer, flags))
    access->Pointer = nullptr;
}

/// End tracking a dynamic access.
void swift::swift_endAccess(ValueBuffer *buffer) {
  Access *access = reinterpret_cast<Access*>(buffer);
  auto pointer = access->Pointer;

  // If the pointer in the access is null, we must've declined
  // to track it because exclusivity tracking was disabled.
  if (!pointer) {
    return;
  }

  getTLSContext().accessSet.remove(access);
}

char *swift::swift_getFunctionReplacement(char **ReplFnPtr, char *CurrFn) {
  char *ReplFn = *ReplFnPtr;
  char *RawReplFn = ReplFn;

#if SWIFT_PTRAUTH
  RawReplFn = ptrauth_strip(RawReplFn, ptrauth_key_function_pointer);
#endif
  if (RawReplFn == CurrFn)
    return nullptr;

  SwiftTLSContext &ctx = getTLSContext();
  if (ctx.CallOriginalOfReplacedFunction) {
    ctx.CallOriginalOfReplacedFunction = false;
    return nullptr;
  }
  return ReplFn;
}

char *swift::swift_getOrigOfReplaceable(char **OrigFnPtr) {
  char *OrigFn = *OrigFnPtr;
  getTLSContext().CallOriginalOfReplacedFunction = true;
  return OrigFn;
}

#ifndef NDEBUG

// Dump the accesses that are currently being tracked by the runtime.
//
// This is only intended to be used in the debugger.
void swift::swift_dumpTrackedAccesses() {
  auto &accessSet = getTLSContext().accessSet;
  if (!accessSet) {
    fprintf(stderr, "        No Accesses.\n");
    return;
  }
  accessSet.forEach([](Access *a) {
    fprintf(stderr, "        Access. Pointer: %p. PC: %p. AccessAction: %s\n",
            a->Pointer, a->PC, getAccessName(a->getAccessAction()));
  });
}

#endif

//===----------------------------------------------------------------------===//
//                            Concurrency Support
//===----------------------------------------------------------------------===//

namespace {

/// High Level Algorithm Description
/// --------------------------------
///
/// With the introduction of Concurrency, we add additional requirements to our
/// exclusivity model:
///
/// * We want tasks to have a consistent exclusivity access set across
///   suspensions/resumptions. This ensures that any exclusive accesses began
///   before a Task suspended are properly flagged after the Task is resumed
///   even if the Task is resumed on a different thread.
///
/// * If a synchronous code calls a subroutine that creates a set of tasks to
///   perform work and then blocks, we want the runtime to ensure that the tasks
///   respect exclusivity accesses from the outside synchronous code.
///
/// * We on purpose define exclusive access to the memory from multiple tasks as
///   undefined behavior since that would be an additional feature that needs to
///   be specifically designed in the future.
///
/// * We assume that an access in synchronous code will never be ended in
///   asynchronous code.
///
/// * We additional require that our design leaves the exclusivity runtime
///   unaware of any work we are doing here. All it should be aware of is the
///   current thread local access set and adding/removing from that access set.
///
/// We implement these requirements by reserving two pointers in each Task. The
/// first pointer points at the head access of the linked list of accesses of
/// the Task and the second pointer points at the end of the linked list of
/// accesses of the task. We will for the discussion ahead call the first
/// pointer TaskFirstAccess and the second TaskLastAccess. This allows us to
/// modify the current TLV single linked list to include/remove the tasks’s
/// access by updating a few nodes in the linked list when the task is running
/// and serialize the task’s current access set and restoring to be head the
/// original synchronous access set head when the task is running. This
/// naturally fits a push/pop access set sort of schema where every time a task
/// starts, we push its access set onto the local TLV and then pop it off when
/// the task is suspended. This ensures that the task gets the current
/// synchronous set of accesses and other Tasks do not see the accesses of the
/// specific task providing task isolation.
///
/// The cases can be described via the following table:
///
/// +------+--------------------+--------------------+--------------------+
/// | Case | Live Task Accesses | Live Sync Accesses | Live Task Accesses |
/// |      | When Push          | When Push          | When Pop           |
/// |------+--------------------+--------------------+--------------------|
/// |    1 | F                  | F                  | F                  |
/// |    2 | F                  | F                  | T                  |
/// |    3 | F                  | T                  | F                  |
/// |    4 | F                  | T                  | T                  |
/// |    5 | T                  | F                  | F                  |
/// |    6 | T                  | F                  | T                  |
/// |    7 | T                  | T                  | F                  |
/// |    8 | T                  | T                  | T                  |
/// +------+--------------------+--------------------+--------------------+
///
/// We mark the end of each title below introducing a case with 3 T/F to enable
/// easy visual matching with the chart
///
/// Case 1: Task/Sync do not have initial accesses and no Task accesses are
/// created while running (F,F,F)
///
/// In this case, TBegin and TEnd are both initially nullptr.
///
/// When we push, we see that the current exclusivity TLV has a null head and
/// leave it so. We set TBegin and TEnd as nullptr while running.
///
/// When we pop, see that the exclusivity TLV is still nullptr, so we just leave
/// TBegin and TEnd alone still as nullptr.
///
/// This means that code that does not have any exclusive accesses do not have
/// any runtime impact.
///
/// Case 2: Task/Sync do not have initial access, but Task accesses are created
/// while running (F, F, T)
///
/// In this case, TBegin and TEnd are both initially nullptr.
///
/// When we push, we see that the current exclusivity TLV has a null head. So,
/// we leave TBegin and TEnd as nullptr while the task is running.
///
/// When we pop, we see that the exclusivity TLV has a non-null head. In that
/// case, we walk the list to find the last node and update TBegin to point at
/// the current head, TEnd to point at that last node, and then set the TLV head
/// to be nullptr.
///
/// Case 3: Task does not have initial accesses, but Sync does, and new Task
/// accesses are not created while running (F, T, F)
///
/// In this case, TBegin and TEnd are both initially nullptr.
///
/// When we push, we look at the TLV and see our initial synchronous thread was
/// tracking accesses. In this case, we leave the TLV pointing at the
/// SyncAccessHead and set TBegin to SyncAccessHead and leave TEnd as nullptr.
///
/// When we pop, we see that TBegin (which we know has the old synchronous head
/// in it) is equal to the TLV so we know that we did not create any new Task
/// accesses. Then we set TBegin to nullptr and return. NOTE:  TEnd is nullptr
/// the entire time in this scenario.
///
/// Case 4: Task does not have initial accesses, but Sync does, and new Task
/// accesses are created while running (F, T, T)
///
/// In this case, TBegin and TEnd are both initially nullptr. When we push, we
/// look at the TLV and we see our initial synchronous thread was tracking
/// accesses. In this case, we leave the TLV pointing at the SyncAccessHead and
/// set TBegin to SyncAccessHead and leave TEnd as nullptr.
///
/// When we pop, we see that the TLV and TBegin differ now. We know that this
/// means that our task introduced new accesses. So, we search down from the
/// head of the AccessSet TLV until we find TBegin . The node before TBegin is
/// our new TEnd pointer. We set TBegin to then have the value of head, TEnd to
/// be the new TEnd pointer, set TEnd’s next to be nullptr and make head the old
/// value of TBegin.
///
/// Case 5: Task has an initial access set, but Sync does not have initial
/// accesses and no Task accesses exist after running (T,F,F)
///
/// In this case, TBegin and TEnd are both initially set to non-null values.
/// When we push, we look at the current TLV head and see that the TLV head is
/// nullptr. We then set TLV head to be TBegin and set TBegin to be nullptr to
/// signal the original synchronous TLV head was nullptr.
///
/// When we pop, we see that TBegin is currently nullptr, so we know the
/// synchronous access set was empty. We also know that despite us starting with
/// a task access set, those accesses must have completed while the task was
/// running since the access set is empty when we pop.
///
/// Case 6: Task has initial accesses, sync does not have initial accesss, and
/// Task access set is modified while running (T, F, T)
///
/// In this case, TBegin and TEnd are both initially set to non-null
/// values. When we push, we look at the current TLV head and see that the TLV
/// head is nullptr. We then set TLV head to be TBegin and set TBegin to be
/// nullptr to signal the original synchronous TLV head was nullptr. We have no
/// requirement on TEnd now in this case but set it to nullptr, to track flags
/// if we want to in the future in a different runtime.
///
/// When we pop, we see that TBegin is currently nullptr, so we know the
/// synchronous access set was empty. We do not have a way to know how/if we
/// modified the Task AccessSet, so we walked the list to find the last node. We
/// then make TBegin head, TEnd the last node, and set the TLV to be nullptr
/// again.
///
/// Case 7: Task has initial accesses, Sync has initial accesses, and new Task
/// accesses are not created while running (T, T, F)
///
/// In this case, TBegin and TEnd are both initially set to non-null values.
/// When we push, we look at the current TLV head and see that the TLV head is a
/// valid pointer. We then set TLV head to be the current value of TBegin, make
/// TEnd->next the old head value and stash the old head value into TBegin. We
/// have no requirement on TEnd now in this case.
///
/// When we pop, we see that TBegin is not nullptr, so we know the synchronous
/// access set had live accesses. We do not have a way to know how/if we
/// modified the Task AccessSet, so we walked the list to find TBegin (which is
/// old sync head).  Noting that the predecessor node of old sync head’s node
/// will be the end of the task’s current access set, we set TLV to point at the
/// node we found in TBegin, set TBegin to the current TLV head, set TEnd to
/// that predecessor node of the current TLV head and set TEnd->next to be
/// nullptr.
///
/// Case 8: Task has initial accesses, Sync does, and Task accesses is modified
/// while running (T, T, T)
///
/// In this case, TBegin and TEnd are both initially set to non-null values.
///
/// When we push, we look at the current TLV head and see that the TLV head is
/// a valid pointer. We then set TLV head to be the current value of TBegin,
/// make TEnd->next the old head value and stash the old head value into
/// TBegin. We have no requirement on TEnd now in this case.
///
/// When we pop, we see that TBegin is not nullptr, so we know the synchronous
/// access set had live accesses. We do not have a way to know how/if we
/// modified the Task AccessSet, so we walked the list to find TBegin (which is
/// old sync head).  Noting that the predecessor node of old sync head’s node
/// will be the end of the task’s current access set, we set TLV to point at
/// the node we found in TBegin, set TBegin to the current TLV head, set TEnd
/// to that predecessor node of the current TLV head and set TEnd->next to be
/// nullptr.
struct SwiftTaskThreadLocalContext {
  uintptr_t state[2];

#ifndef NDEBUG
  void dump() {
    fprintf(stderr,
            "        SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): "
            "(%p, %p)\n",
            (void *)state[0], (void *)state[1]);
  }
#endif

  bool hasInitialAccessSet() const {
    // If state[0] is nullptr, we have an initial access set.
    return bool(state[0]);
  }

  Access *getTaskAccessSetHead() const {
    return reinterpret_cast<Access *>(state[0]);
  }

  Access *getTaskAccessSetTail() const {
    return reinterpret_cast<Access *>(state[1]);
  }

  void setTaskAccessSetHead(Access *newHead) { state[0] = uintptr_t(newHead); }

  void setTaskAccessSetTail(Access *newTail) { state[1] = uintptr_t(newTail); }

#ifndef NDEBUG
  const char *getTaskAddress() const {
    // Constant only used when we have an asserts compiler so that we can output
    // exactly the header location of the task for FileCheck purposes.
    //
    // WARNING: This test will fail if the Task ABI changes. When that happens,
    // update the offset!
    //
    // TODO: This probably will need 32 bit help.
#if __POINTER_WIDTH__ == 64
    unsigned taskHeadOffsetFromTaskAccessSet = 128;
#else
    unsigned taskHeadOffsetFromTaskAccessSet = 68;
#endif
    auto *self = reinterpret_cast<const char *>(this);
    return self - taskHeadOffsetFromTaskAccessSet;
  }
#endif
};

} // end anonymous namespace

// See algorithm description on SwiftTaskThreadLocalContext.
void swift::swift_task_enterThreadLocalContext(char *state) {
  auto &taskCtx = *reinterpret_cast<SwiftTaskThreadLocalContext *>(state);
  auto &tlsCtxAccessSet = getTLSContext().accessSet;

#ifndef NDEBUG
  if (isExclusivityLoggingEnabled()) {
    withLoggingLock([&]() {
      fprintf(stderr,
              "Entering Thread Local Context. Before Swizzle. Task: %p\n",
              taskCtx.getTaskAddress());
      taskCtx.dump();
      swift_dumpTrackedAccesses();
    });
  }

  auto logEndState = [&] {
    if (isExclusivityLoggingEnabled()) {
      withLoggingLock([&]() {
        fprintf(stderr,
                "Entering Thread Local Context. After Swizzle. Task: %p\n",
                taskCtx.getTaskAddress());
        taskCtx.dump();
        swift_dumpTrackedAccesses();
      });
    }
  };
#else
  // Just a no-op that should inline away.
  auto logEndState = [] {};
#endif

  // First handle all of the cases where our task does not start without an
  // initial access set.
  //
  // Handles push cases 1-4.
  if (!taskCtx.hasInitialAccessSet()) {
    // In this case, the current synchronous context is not tracking any
    // accesses. So the tlsCtx and our initial access set are all nullptr, so we
    // can just return early.
    //
    // Handles push cases 1-2.
    if (!tlsCtxAccessSet) {
      logEndState();
      return;
    }

    // Ok, our task isn't tracking any task specific accesses, but our tlsCtx
    // was tracking accesses. Leave the tlsCtx alone at this point and set our
    // state's begin access to be tlsCtx head. We leave our access set tail as
    // nullptr.
    //
    // Handles push cases 3-4.
    taskCtx.setTaskAccessSetHead(tlsCtxAccessSet.getHead());
    logEndState();
    return;
  }

  // At this point, we know that we did have an initial access set. Both access
  // set pointers are valid.
  //
  // Handles push cases 5-8.

  // Now check if our synchronous code had any accesses. If not, we set TBegin,
  // TEnd to be nullptr and set the tlsCtx to point to TBegin.
  //
  // Handles push cases 5-6.
  if (!bool(tlsCtxAccessSet)) {
    tlsCtxAccessSet = taskCtx.getTaskAccessSetHead();
    taskCtx.setTaskAccessSetHead(nullptr);
    taskCtx.setTaskAccessSetTail(nullptr);
    logEndState();
    return;
  }

  // In this final case, we found that our task had its own access set and our
  // tlsCtx did as well. So we then set the Task's head to be the new TLV head,
  // set tail->next to point at old head and stash oldhead into the task ctx.
  //
  // Handles push cases 7-8.
  auto *oldHead = tlsCtxAccessSet.getHead();
  auto *tail = taskCtx.getTaskAccessSetTail();

  tlsCtxAccessSet.setHead(taskCtx.getTaskAccessSetHead());
  tail->setNext(oldHead);
  taskCtx.setTaskAccessSetHead(oldHead);
  taskCtx.setTaskAccessSetTail(nullptr);
  logEndState();
}

// See algorithm description on SwiftTaskThreadLocalContext.
void swift::swift_task_exitThreadLocalContext(char *state) {
  auto &taskCtx = *reinterpret_cast<SwiftTaskThreadLocalContext *>(state);
  auto &tlsCtxAccessSet = getTLSContext().accessSet;

#ifndef NDEBUG
  if (isExclusivityLoggingEnabled()) {
    withLoggingLock([&]() {
      fprintf(stderr,
              "Exiting Thread Local Context. Before Swizzle. Task: %p\n",
              taskCtx.getTaskAddress());
      taskCtx.dump();
      swift_dumpTrackedAccesses();
    });
  }

  auto logEndState = [&] {
    if (isExclusivityLoggingEnabled()) {
      withLoggingLock([&]() {
        fprintf(stderr,
                "Exiting Thread Local Context. After Swizzle. Task: %p\n",
                taskCtx.getTaskAddress());
        taskCtx.dump();
        swift_dumpTrackedAccesses();
      });
    }
  };
#else
  // If we are not compiling with asserts, just use a simple identity function
  // that should be inlined away.
  //
  // TODO: Can we use defer in the runtime?
  auto logEndState = [] {};
#endif

  // First check our ctx to see if we were tracking a previous synchronous
  // head. If we don't then we know that our synchronous thread was not
  // initially tracking any accesses.
  //
  // Handles pop cases 1,2,5,6
  Access *oldHead = taskCtx.getTaskAccessSetHead();
  if (!oldHead) {
    // Then check if we are currently tracking an access set in the TLS. If we
    // aren't, then we know that either we did not start with a task specific
    // access set /or/ we did start but all of those accesses ended while the
    // task was running. In either case, when we pushed initially, we set
    // TBegin, TEnd to be nullptr already and since oldHead is already nullptr,
    // we can just exit.
    //
    // Handles pop cases 1,5
    if (!tlsCtxAccessSet) {
      assert(taskCtx.getTaskAccessSetTail() == nullptr &&
             "Make sure we set this to nullptr when we pushed");
      logEndState();
      return;
    }

    // In this case, we did find that we had live accesses. Since we know we
    // did not start with any synchronous accesses, these accesses must all be
    // from the given task. So, we first find the tail of the current TLS linked
    // list, then set the Task access set head to accessSet, the Task accessSet
    // tail to the TLS linked list tail and set tlsCtx.accessSet to nullptr.
    //
    // Handles pop cases 2,6
    auto *newHead = tlsCtxAccessSet.getHead();
    auto *newTail = tlsCtxAccessSet.getTail();
    assert(newTail && "Failed to find tail?!");
    tlsCtxAccessSet = nullptr;
    taskCtx.setTaskAccessSetHead(newHead);
    taskCtx.setTaskAccessSetTail(newTail);
    logEndState();
    return;
  }

  // Otherwise, we know that we /were/ tracking accesses from a previous
  // synchronous context. So we need to unmerge our task specific state from the
  // exclusivity access set.
  //
  // Handles pop cases 3,4,7,8.

  // First check if the current head tlsAccess is the same as our oldHead. In
  // such a case, we do not have new task accesses to update. So just set task
  // access head/tail to nullptr. The end access should be nullptr.
  //
  // Handles pop cases 3.
  if (tlsCtxAccessSet.getHead() == oldHead) {
    taskCtx.setTaskAccessSetHead(nullptr);
    taskCtx.setTaskAccessSetTail(nullptr);
    logEndState();
    return;
  }

  // Otherwise, we have task specific accesses that we need to serialize into
  // the task's state. We currently can not tell if the Task actually modified
  // the task list beyond if the task list is empty. So we have to handle case 7
  // here (unfortunately).
  //
  // NOTE: If we could tell if the Task modified its access set while running,
  // we could perhaps avoid the search for newEnd.
  //
  // Handles pop cases 4,7,8.
  auto *newHead = tlsCtxAccessSet.getHead();
  auto *newEnd = tlsCtxAccessSet.findParentAccess(oldHead);
  tlsCtxAccessSet.setHead(oldHead);
  newEnd->setNext(nullptr);
  taskCtx.setTaskAccessSetHead(newHead);
  taskCtx.setTaskAccessSetTail(newEnd);
  logEndState();
}
