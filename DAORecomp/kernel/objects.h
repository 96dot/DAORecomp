#pragma once
// Kernel objects. Waitable objects share one dispatcher lock + condition variable,
// which keeps WaitAll / WaitAny across mixed object types simple and correct.
//
// Every waitable object also has a guest DISPATCHER_HEADER (either the game's own,
// e.g. a KEVENT embedded in a struct and passed to KeSetEvent, or one we allocate
// for handle-created objects so ObReferenceObjectByHandle can hand out a pointer).
// The header's wait-list words hold a signature + object id so we can map it back.

#include "types.h"

namespace dao::kernel
{
    struct GuestThread;

    enum class ObjectType
    {
        Event,
        Semaphore,
        Mutant,
        Timer,
        Thread,
        File,
        Enumerator,
        NotifyListener,
        Other,
    };

    struct KernelObject : std::enable_shared_from_this<KernelObject>
    {
        explicit KernelObject(ObjectType type) : type(type) {}
        virtual ~KernelObject();

        const ObjectType type;
        uint32_t guestObject = 0;    // guest address of the dispatcher header / body
        bool ownsGuestObject = false;
        std::string name;

        // Waitable interface. Called with the dispatcher lock held.
        virtual bool IsWaitable() const { return false; }
        virtual bool IsSignaled(GuestThread* waiter) const { return false; }
        virtual void Acquire(GuestThread* waiter) {}     // consume the signal on satisfied wait
        virtual uint32_t WaitStatusAdjust() { return 0; } // e.g. abandoned mutant

        // Mirror state into the guest header (signal state) for code that peeks at it.
        virtual void SyncGuest() {}
    };

    using ObjectRef = std::shared_ptr<KernelObject>;

    std::unique_lock<std::mutex> LockDispatcher();
    void NotifyDispatcher(); // wake waiters; call after changing any signal state
    void WaitDispatcherChange(std::unique_lock<std::mutex>& lock); // block until the next NotifyDispatcher

    struct Event final : KernelObject
    {
        Event(bool manualReset, bool initialState) : KernelObject(ObjectType::Event), manualReset(manualReset), signaled(initialState) {}

        bool manualReset;
        bool signaled;

        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread*) const override { return signaled; }
        void Acquire(GuestThread*) override { if (!manualReset) signaled = false; }
        void SyncGuest() override;

        int32_t Set();
        int32_t Reset();
        int32_t Pulse();
    };

    struct Semaphore final : KernelObject
    {
        Semaphore(int32_t count, int32_t limit) : KernelObject(ObjectType::Semaphore), count(count), limit(limit) {}

        int32_t count;
        int32_t limit;

        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread*) const override { return count > 0; }
        void Acquire(GuestThread*) override { count--; }
        void SyncGuest() override;

        // Returns previous count, or -1 if the limit would be exceeded.
        int32_t Release(int32_t adjustment);
    };

    struct Mutant final : KernelObject
    {
        explicit Mutant(GuestThread* initialOwner);

        GuestThread* owner = nullptr;
        uint32_t recursion = 0;
        bool abandoned = false;

        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread* waiter) const override { return owner == nullptr || owner == waiter; }
        void Acquire(GuestThread* waiter) override;
        uint32_t WaitStatusAdjust() override;
        void SyncGuest() override;

        // Returns previous recursion count, or -1 if not owned by the caller.
        int32_t Release(GuestThread* caller, bool abandon = false);
    };

    struct Timer final : KernelObject
    {
        explicit Timer(bool manualReset) : KernelObject(ObjectType::Timer), manualReset(manualReset) {}
        ~Timer() override;

        bool manualReset;
        bool signaled = false;
        uint64_t generation = 0;   // bumps on every Set/Cancel so stale firings are ignored

        // APC queued to the setting thread when the timer fires.
        uint32_t apcRoutine = 0;
        uint32_t apcContext = 0;
        std::weak_ptr<KernelObject> apcThread;

        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread*) const override { return signaled; }
        void Acquire(GuestThread*) override { if (!manualReset) signaled = false; }
        void SyncGuest() override;

        // dueTime: Xbox LARGE_INTEGER semantics (negative = relative 100 ns).
        bool Set(int64_t dueTime, uint32_t periodMs, uint32_t routine, uint32_t context, GuestThread* setter);
        bool Cancel();
    };

    // Handle table ------------------------------------------------------------

    uint32_t CreateHandle(const ObjectRef& object);
    ObjectRef LookupHandle(uint32_t handle);
    template<typename T>
    std::shared_ptr<T> LookupHandle(uint32_t handle, ObjectType type)
    {
        auto object = LookupHandle(handle);
        if (!object || object->type != type)
            return nullptr;
        return std::static_pointer_cast<T>(object);
    }
    bool CloseHandle(uint32_t handle);
    uint32_t DuplicateHandle(uint32_t handle);

    // Named objects (NtCreateEvent etc. with an ObjectAttributes name).
    ObjectRef LookupNamedObject(const std::string& name);
    void RegisterNamedObject(const std::string& name, const ObjectRef& object);

    // Guest dispatcher header <-> host object --------------------------------

    // Allocates a guest header for a handle-created object and binds it.
    void AttachGuestHeader(const ObjectRef& object, uint32_t size);
    // Binds an object to an existing guest header (the game's own KEVENT etc).
    void BindGuestHeader(const ObjectRef& object, uint32_t header);
    // Returns the object for a guest header, creating one from the header's
    // type/state if the game initialised it inline (KeInitializeEvent is a macro).
    ObjectRef ObjectFromGuestHeader(uint32_t header);
    void ReleaseGuestHeader(uint32_t header);

    // Waiting -----------------------------------------------------------------

    // timeout: nullptr = infinite, else Xbox LARGE_INTEGER semantics.
    uint32_t WaitMultiple(std::span<const ObjectRef> objects, bool waitAll, const int64_t* timeout, bool alertable);
    uint32_t WaitSingle(const ObjectRef& object, const int64_t* timeout, bool alertable);
    uint32_t DelayExecution(const int64_t* timeout, bool alertable);
    // Sleeps for about `microseconds` (Sleep can't go below a millisecond), for
    // waiting on something without keeping a core busy.
    void ShortSleep(uint32_t microseconds);

    // 100 ns system time (FILETIME epoch) and conversions.
    uint64_t SystemTime100ns();
    // Absolute deadline on the steady clock for an Xbox timeout value.
    std::chrono::steady_clock::time_point TimeoutToDeadline(int64_t timeout);
}
