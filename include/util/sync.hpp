// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license
// Based on Bitcoin Core's sync.h

#ifndef COINBASECHAIN_UTIL_SYNC_HPP
#define COINBASECHAIN_UTIL_SYNC_HPP

#include "util/threadsafety.hpp"
#include "util/macros.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

//////////////////////////////////////////////
//                                          //
// THE SIMPLE DEFINITION (NO DEBUG LOCKING) //
//                                          //
//////////////////////////////////////////////

/*
RecursiveMutex mutex;
    std::recursive_mutex mutex;

LOCK(mutex);
    std::unique_lock<std::recursive_mutex> criticalblock(mutex);

LOCK2(mutex1, mutex2);
    std::unique_lock<std::recursive_mutex> criticalblock1(mutex1);
    std::unique_lock<std::recursive_mutex> criticalblock2(mutex2);

TRY_LOCK(mutex, name);
    std::unique_lock<std::recursive_mutex> name(mutex, std::try_to_lock_t);
*/

///////////////////////////////
//                           //
// THE ACTUAL IMPLEMENTATION //
//                           //
///////////////////////////////

// For now, simplified version without DEBUG_LOCKORDER
// Can be enhanced later with lock order checking if needed

template <typename MutexType>
inline void AssertLockHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) EXCLUSIVE_LOCKS_REQUIRED(cs) {}

template <typename MutexType>
inline void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs) {}

/**
 * Template mixin that adds -Wthread-safety locking annotations and lock order
 * checking to a subset of the mutex API.
 */
template <typename PARENT>
class LOCKABLE AnnotatedMixin : public PARENT
{
public:
    void lock() EXCLUSIVE_LOCK_FUNCTION()
    {
        PARENT::lock();
    }

    void unlock() UNLOCK_FUNCTION()
    {
        PARENT::unlock();
    }

    bool try_lock() EXCLUSIVE_TRYLOCK_FUNCTION(true)
    {
        return PARENT::try_lock();
    }

    using unique_lock = std::unique_lock<PARENT>;

#ifdef __clang__
    //! For negative capabilities in the Clang Thread Safety Analysis.
    //! A negative requirement uses the EXCLUSIVE_LOCKS_REQUIRED attribute, in conjunction
    //! with the ! operator, to indicate that a mutex should not be held.
    const AnnotatedMixin& operator!() const { return *this; }
#endif // __clang__
};

/**
 * Wrapped mutex: supports recursive locking, but no waiting
 * TODO: We should move away from using the recursive lock by default.
 */
using RecursiveMutex = AnnotatedMixin<std::recursive_mutex>;

/** Wrapped mutex: supports waiting but not recursive locking */
using Mutex = AnnotatedMixin<std::mutex>;

/** Different type to mark Mutex at global scope
 *
 * Thread safety analysis can't handle negative assertions about mutexes
 * with global scope well, so mark them with a separate type.
 */
class GlobalMutex : public Mutex { };

#define AssertLockHeld(cs) AssertLockHeldInternal(#cs, __FILE__, __LINE__, &cs)

inline void AssertLockNotHeldInline(const char* name, const char* file, int line, Mutex* cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, RecursiveMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, GlobalMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
#define AssertLockNotHeld(cs) AssertLockNotHeldInline(#cs, __FILE__, __LINE__, &cs)

/** Wrapper around std::unique_lock style lock for MutexType. */
template <typename MutexType>
class SCOPED_LOCKABLE UniqueLock : public MutexType::unique_lock
{
private:
    using Base = typename MutexType::unique_lock;

public:
    UniqueLock(MutexType& mutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(mutexIn) : Base(mutexIn, std::defer_lock)
    {
        if (fTry) {
            Base::try_lock();
        } else {
            Base::lock();
        }
    }

    UniqueLock(MutexType* pmutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(pmutexIn)
    {
        if (!pmutexIn) return;

        *static_cast<Base*>(this) = Base(*pmutexIn, std::defer_lock);
        if (fTry) {
            Base::try_lock();
        } else {
            Base::lock();
        }
    }

    ~UniqueLock() UNLOCK_FUNCTION()
    {
    }

    operator bool()
    {
        return Base::owns_lock();
    }
};

// When locking a Mutex, require negative capability to ensure the lock
// is not already held
inline Mutex& MaybeCheckNotHeld(Mutex& cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) LOCK_RETURNED(cs) { return cs; }
inline Mutex* MaybeCheckNotHeld(Mutex* cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) LOCK_RETURNED(cs) { return cs; }

// When locking a GlobalMutex or RecursiveMutex, just check it is not
// locked in the surrounding scope.
template <typename MutexType>
inline MutexType& MaybeCheckNotHeld(MutexType& m) LOCKS_EXCLUDED(m) LOCK_RETURNED(m) { return m; }
template <typename MutexType>
inline MutexType* MaybeCheckNotHeld(MutexType* m) LOCKS_EXCLUDED(m) LOCK_RETURNED(m) { return m; }

#define LOCK(cs) UniqueLock UNIQUE_NAME(criticalblock)(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__)
#define LOCK2(cs1, cs2)                                               \
    UniqueLock criticalblock1(MaybeCheckNotHeld(cs1), #cs1, __FILE__, __LINE__); \
    UniqueLock criticalblock2(MaybeCheckNotHeld(cs2), #cs2, __FILE__, __LINE__)
#define TRY_LOCK(cs, name) UniqueLock name(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__, true)
#define WAIT_LOCK(cs, name) UniqueLock name(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__)

//! Run code while locking a mutex.
//!
//! Examples:
//!
//!   WITH_LOCK(cs, shared_val = shared_val + 1);
//!
//!   int val = WITH_LOCK(cs, return shared_val);
#define WITH_LOCK(cs, code) (MaybeCheckNotHeld(cs), [&]() -> decltype(auto) { LOCK(cs); code; }())

#endif // COINBASECHAIN_UTIL_SYNC_HPP
