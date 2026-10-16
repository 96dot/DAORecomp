#include "stdafx.h"
#include "objects.h"
#include "thread.h"
#include "memory.h"

namespace dao::kernel
{
    static std::mutex s_dispatchMutex;

    // A thread's wakeup. Notified with the dispatcher lock held: the thread only
    // leaves its wait, and so its registrations, with the lock.
    struct DispatchWaiter
    {
        std::condition_variable cv;
    };

    static thread_local DispatchWaiter t_waiter;

    static void AddWaiter(KernelObject& object)
    {
        object.waiters.push_back(&t_waiter);
    }

    static void RemoveWaiter(KernelObject& object)
    {
        auto& waiters = object.waiters;
        const auto it = std::find(waiters.begin(), waiters.end(), &t_waiter);
        *it = waiters.back();
        waiters.pop_back();
    }

    std::unique_lock<std::mutex> LockDispatcher()
    {
        return std::unique_lock(s_dispatchMutex);
    }

    void NotifyDispatcher(KernelObject& object)
    {
        for (DispatchWaiter* waiter : object.waiters)
            waiter->cv.notify_one();
    }

    void WaitDispatcherChange(std::unique_lock<std::mutex>& lock, KernelObject& object)
    {
        AddWaiter(object);
        t_waiter.cv.wait(lock);
        RemoveWaiter(object);
    }

    // ------------------------------------------------------------------- time

    // FILETIME epoch (1601) offset from the Unix epoch, in 100 ns.
    constexpr uint64_t kFileTimeUnixOffset = 116444736000000000ull;

    uint64_t SystemTime100ns()
    {
        using namespace std::chrono;
        const auto now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        return kFileTimeUnixOffset + uint64_t(now / 100);
    }

    std::chrono::steady_clock::time_point TimeoutToDeadline(int64_t timeout)
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        // Waits longer than about a century are "forever" (and nanoseconds of them would
        // overflow).
        constexpr int64_t kMaxDelta = 100ll * 365 * 24 * 3600 * 10'000'000;
        if (timeout < 0)
            return now + nanoseconds(std::min<uint64_t>(0 - uint64_t(timeout), kMaxDelta) * 100);

        // Positive = absolute system time.
        const int64_t delta = timeout - int64_t(SystemTime100ns());
        return now + nanoseconds(std::clamp<int64_t>(delta, 0, kMaxDelta) * 100);
    }

    // ---------------------------------------------------------------- objects

    static std::mutex s_headerMutex;
    static std::unordered_map<uint32_t, std::weak_ptr<KernelObject>> s_headers;
    static std::unordered_map<uint32_t, ObjectRef> s_headerOwned; // objects created from inline-initialised headers
    static uint32_t s_nextBindId = 1;
    constexpr uint32_t kHeaderSignature = 0x44414F21; // 'DAO!'

    KernelObject::~KernelObject()
    {
        if (guestObject)
        {
            {
                std::lock_guard lock(s_headerMutex);
                auto it = s_headers.find(guestObject);
                if (it != s_headers.end() && it->second.expired())
                    s_headers.erase(it);
            }
            if (ownsGuestObject)
                KernelFree(guestObject);
        }
    }

    static X_DISPATCH_HEADER* Header(uint32_t address)
    {
        return g_memory.Translate<X_DISPATCH_HEADER>(address);
    }

    void Event::SyncGuest()
    {
        if (guestObject)
            Header(guestObject)->signalState = signaled ? 1 : 0;
    }

    int32_t Event::Set()
    {
        auto lock = LockDispatcher();
        const int32_t previous = signaled;
        signaled = true;
        SyncGuest();
        if (!previous)
            NotifyDispatcher(*this);
        return previous;
    }

    int32_t Event::Reset()
    {
        auto lock = LockDispatcher();
        const int32_t previous = signaled;
        signaled = false;
        SyncGuest();
        return previous;
    }

    int32_t Event::Pulse()
    {
        // Wake current waiters without leaving the event set: approximate by
        // setting and letting one dispatcher pass consume it.
        auto lock = LockDispatcher();
        const int32_t previous = signaled;
        signaled = true;
        NotifyDispatcher(*this);
        lock.unlock();
        std::this_thread::yield();
        lock.lock();
        signaled = false;
        SyncGuest();
        return previous;
    }

    void Semaphore::SyncGuest()
    {
        if (guestObject)
            Header(guestObject)->signalState = count;
    }

    int32_t Semaphore::Release(int32_t adjustment)
    {
        auto lock = LockDispatcher();
        const int32_t previous = count;
        if (int64_t(count) + adjustment > limit)
            return -1;
        count += adjustment;
        SyncGuest();
        if (adjustment > 0)
            NotifyDispatcher(*this);
        return previous;
    }

    Mutant::Mutant(GuestThread* initialOwner) : KernelObject(ObjectType::Mutant)
    {
        if (initialOwner)
        {
            owner = initialOwner;
            recursion = 1;
        }
    }

    void Mutant::Acquire(GuestThread* waiter)
    {
        owner = waiter;
        recursion++;
        SyncGuest();
    }

    uint32_t Mutant::WaitStatusAdjust()
    {
        if (abandoned)
        {
            abandoned = false;
            return X_STATUS_ABANDONED_WAIT_0;
        }
        return 0;
    }

    void Mutant::SyncGuest()
    {
        if (guestObject)
        {
            auto* m = g_memory.Translate<X_KMUTANT>(guestObject);
            m->header.signalState = owner ? 1 - int32_t(recursion) : 1;
            m->owner = owner ? owner->kthread : 0;
            m->abandoned = abandoned;
        }
    }

    int32_t Mutant::Release(GuestThread* caller, bool abandon)
    {
        auto lock = LockDispatcher();
        if (owner != caller)
            return -1;
        const int32_t previous = int32_t(recursion);
        if (abandon)
        {
            recursion = 0;
            abandoned = true;
        }
        else
        {
            recursion--;
        }
        if (recursion == 0)
            owner = nullptr;
        SyncGuest();
        if (!owner)
            NotifyDispatcher(*this);
        return previous;
    }

    // ------------------------------------------------------------------ timers

    struct TimerService
    {
        struct Entry
        {
            std::chrono::steady_clock::time_point when;
            std::weak_ptr<KernelObject> timer;
            uint64_t generation;
            bool operator>(const Entry& o) const { return when > o.when; }
        };

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<Entry> heap;
        std::thread thread;

        void Schedule(const std::shared_ptr<Timer>& timer, std::chrono::steady_clock::time_point when, uint64_t generation)
        {
            std::lock_guard lock(mutex);
            if (!thread.joinable())
                thread = std::thread([this] { Run(); });
            heap.push_back({ when, timer, generation });
            std::push_heap(heap.begin(), heap.end(), std::greater<>());
            cv.notify_one();
        }

        void Run()
        {
            SetThreadDescription(GetCurrentThread(), L"DAO Timer Service");
            std::unique_lock lock(mutex);
            for (;;)
            {
                if (heap.empty())
                {
                    cv.wait(lock);
                    continue;
                }
                const auto when = heap.front().when;
                if (std::chrono::steady_clock::now() < when)
                {
                    cv.wait_until(lock, when);
                    continue;
                }
                std::pop_heap(heap.begin(), heap.end(), std::greater<>());
                Entry entry = heap.back();
                heap.pop_back();
                lock.unlock();
                Fire(entry);
                lock.lock();
            }
        }

        void Fire(const Entry& entry)
        {
            auto object = entry.timer.lock();
            if (!object)
                return;
            auto timer = std::static_pointer_cast<Timer>(object);

            auto dispatch = LockDispatcher();
            if (timer->generation != entry.generation)
                return;
            timer->signaled = true;
            timer->SyncGuest();
            NotifyDispatcher(*timer);
            if (timer->apcRoutine)
            {
                if (auto thread = timer->apcThread.lock())
                {
                    const uint64_t now = SystemTime100ns();
                    static_cast<GuestThread*>(thread.get())->apcQueue.push_back(
                        { 0, timer->apcRoutine, timer->apcContext, uint32_t(now), uint32_t(now >> 32), 0 });
                    NotifyDispatcher(*thread);
                }
            }
        }
    };

    // Never destroyed: its thread runs until the process ends, and destroying a joinable
    // std::thread at exit would call std::terminate.
    static TimerService& s_timers = *new TimerService;

    Timer::~Timer() = default;

    void Timer::SyncGuest()
    {
        if (guestObject)
            Header(guestObject)->signalState = signaled ? 1 : 0;
    }

    bool Timer::Set(int64_t dueTime, uint32_t periodMs, uint32_t routine, uint32_t context, GuestThread* setter)
    {
        auto self = std::static_pointer_cast<Timer>(shared_from_this());
        uint64_t gen;
        bool wasSignaled;
        {
            auto lock = LockDispatcher();
            wasSignaled = signaled;
            signaled = false;
            gen = ++generation;
            apcRoutine = routine;
            apcContext = context;
            apcThread = setter ? setter->weak_from_this() : std::weak_ptr<KernelObject>();
            SyncGuest();
        }

        auto when = TimeoutToDeadline(dueTime);
        s_timers.Schedule(self, when, gen);

        if (periodMs)
        {
            // Periodic: pre-schedule a lazily extended chain from a helper thread.
            std::thread([weak = std::weak_ptr<Timer>(self), when, periodMs, gen]() mutable {
                for (;;)
                {
                    when += std::chrono::milliseconds(periodMs);
                    std::this_thread::sleep_until(when - std::chrono::milliseconds(periodMs / 2));
                    auto timer = weak.lock();
                    if (!timer || timer->generation != gen)
                        return;
                    s_timers.Schedule(timer, when, gen);
                }
            }).detach();
        }
        return wasSignaled;
    }

    bool Timer::Cancel()
    {
        auto lock = LockDispatcher();
        const bool wasSet = !signaled;
        generation++;
        return wasSet;
    }

    // ----------------------------------------------------------------- handles

    static std::mutex s_handleMutex;
    static std::unordered_map<uint32_t, ObjectRef> s_handles;
    static uint32_t s_nextHandle = 0xF8000004;
    static std::unordered_map<std::string, std::weak_ptr<KernelObject>> s_named;

    uint32_t CreateHandle(const ObjectRef& object)
    {
        std::lock_guard lock(s_handleMutex);
        uint32_t handle;
        do
        {
            handle = s_nextHandle;
            s_nextHandle += 4;
        } while (s_handles.contains(handle));
        s_handles.emplace(handle, object);
        return handle;
    }

    ObjectRef LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return GuestThread::CurrentRef();

        std::lock_guard lock(s_handleMutex);
        auto it = s_handles.find(handle);
        return it != s_handles.end() ? it->second : nullptr;
    }

    bool CloseHandle(uint32_t handle)
    {
        ObjectRef doomed;
        std::lock_guard lock(s_handleMutex);
        auto it = s_handles.find(handle);
        if (it == s_handles.end())
            return false;
        doomed = std::move(it->second); // destroyed after the lock is released
        s_handles.erase(it);
        return true;
    }

    uint32_t DuplicateHandle(uint32_t handle)
    {
        auto object = LookupHandle(handle);
        return object ? CreateHandle(object) : 0;
    }

    ObjectRef LookupNamedObject(const std::string& name)
    {
        std::lock_guard lock(s_handleMutex);
        auto it = s_named.find(name);
        return it != s_named.end() ? it->second.lock() : nullptr;
    }

    void RegisterNamedObject(const std::string& name, const ObjectRef& object)
    {
        std::lock_guard lock(s_handleMutex);
        object->name = name;
        s_named[name] = object;
    }

    // ------------------------------------------------- guest header binding

    static void BindLocked(const ObjectRef& object, uint32_t header)
    {
        auto* h = Header(header);
        h->waitList.flink = kHeaderSignature;
        h->waitList.blink = s_nextBindId++;
        s_headers[header] = object;
        object->guestObject = header;
    }

    bool AttachGuestHeader(const ObjectRef& object, uint32_t size)
    {
        const uint32_t header = KernelAlloc(std::max<uint32_t>(size, sizeof(X_DISPATCH_HEADER)));
        if (!header)
        {
            LOGE("Out of kernel memory for an object's header");
            return false;
        }
        auto* h = Header(header);
        switch (object->type)
        {
        case ObjectType::Event:
            h->type = static_cast<Event*>(object.get())->manualReset ? kTypeNotificationEvent : kTypeSynchronizationEvent;
            break;
        case ObjectType::Semaphore:
            h->type = kTypeSemaphore;
            g_memory.Translate<X_KSEMAPHORE>(header)->limit = static_cast<Semaphore*>(object.get())->limit;
            break;
        case ObjectType::Mutant: h->type = kTypeMutant; break;
        case ObjectType::Timer:
            h->type = static_cast<Timer*>(object.get())->manualReset ? kTypeNotificationTimer : kTypeSynchronizationTimer;
            break;
        case ObjectType::Thread: h->type = kTypeThread; break;
        default: break;
        }
        h->size = uint8_t(size / 4);

        {
            std::lock_guard lock(s_headerMutex);
            // (Another thread may have given it one meanwhile: that one stays.)
            if (!object->guestObject)
            {
                BindLocked(object, header);
                object->ownsGuestObject = true;
                object->SyncGuest();
                return true;
            }
        }
        KernelFree(header);
        return true;
    }

    // Objects the header maps drop are destroyed after s_headerMutex is released
    // (~KernelObject locks it).
    static ObjectRef TakeOwnedLocked(uint32_t header)
    {
        ObjectRef owned;
        if (auto it = s_headerOwned.find(header); it != s_headerOwned.end())
        {
            owned = std::move(it->second);
            s_headerOwned.erase(it);
        }
        return owned;
    }

    void BindGuestHeader(const ObjectRef& object, uint32_t header)
    {
        ObjectRef doomed;
        std::lock_guard lock(s_headerMutex);
        doomed = TakeOwnedLocked(header);
        BindLocked(object, header);
    }

    ObjectRef ObjectFromGuestHeader(uint32_t header)
    {
        if (!header)
            return nullptr;

        ObjectRef doomed;
        std::lock_guard lock(s_headerMutex);
        auto* h = Header(header);
        auto it = s_headers.find(header);
        if (it != s_headers.end() && h->waitList.flink == kHeaderSignature)
        {
            if (auto object = it->second.lock())
                return object;
        }

        // Unknown or re-initialised header: build an object from its contents.
        ObjectRef object;
        const int32_t state = h->signalState;
        switch (h->type)
        {
        case kTypeNotificationEvent:
        case kTypeSynchronizationEvent:
            object = std::make_shared<Event>(h->type == kTypeNotificationEvent, state != 0);
            break;
        case kTypeSemaphore:
            object = std::make_shared<Semaphore>(state, int32_t(g_memory.Translate<X_KSEMAPHORE>(header)->limit));
            break;
        case kTypeMutant:
            object = std::make_shared<Mutant>(nullptr);
            break;
        case kTypeNotificationTimer:
        case kTypeSynchronizationTimer:
            object = std::make_shared<Timer>(h->type == kTypeNotificationTimer);
            break;
        default:
            LOGW("Unknown dispatcher object type {} at {:08X}", h->type, header);
            return nullptr;
        }

        BindLocked(object, header);
        doomed = std::exchange(s_headerOwned[header], object);
        return object;
    }

    void ReleaseGuestHeader(uint32_t header)
    {
        ObjectRef doomed;
        std::lock_guard lock(s_headerMutex);
        doomed = TakeOwnedLocked(header);
        s_headers.erase(header);
    }

    // ----------------------------------------------------------------- waiting

    uint32_t WaitMultiple(std::span<const ObjectRef> objects, bool waitAll, const int64_t* timeout, bool alertable)
    {
        GuestThread* self = GuestThread::Current();
        const bool infinite = timeout == nullptr;
        const bool poll = timeout && *timeout == 0;
        const auto deadline = infinite || poll ? std::chrono::steady_clock::time_point{} : TimeoutToDeadline(*timeout);

        for (const auto& object : objects)
        {
            if (!object || !object->IsWaitable())
                return X_STATUS_INVALID_HANDLE;
        }

        auto lock = LockDispatcher();
        // Registered on the objects (and for APCs) from the first block to the
        // return, with the lock held.
        struct Registration
        {
            std::span<const ObjectRef> objects;
            GuestThread* apcThread;
            bool registered = false;

            void Add()
            {
                if (registered)
                    return;
                for (const auto& object : objects)
                    AddWaiter(*object);
                if (apcThread)
                    AddWaiter(*apcThread);
                registered = true;
            }

            void Remove()
            {
                if (!registered)
                    return;
                registered = false;
                for (const auto& object : objects)
                    RemoveWaiter(*object);
                if (apcThread)
                    RemoveWaiter(*apcThread);
            }

            ~Registration() { Remove(); }
        } registration{ objects, alertable ? self : nullptr };

        for (;;)
        {

            if (alertable && self && !self->apcQueue.empty())
            {
                registration.Remove();
                lock.unlock();
                self->DeliverApcs();
                return X_STATUS_USER_APC;
            }

            if (waitAll)
            {
                bool all = true;
                for (const auto& object : objects)
                    all = all && object->IsSignaled(self);
                if (all)
                {
                    uint32_t status = X_STATUS_WAIT_0;
                    for (const auto& object : objects)
                    {
                        object->Acquire(self);
                        status |= object->WaitStatusAdjust();
                        object->SyncGuest();
                    }
                    return status;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i]->IsSignaled(self))
                    {
                        objects[i]->Acquire(self);
                        objects[i]->SyncGuest();
                        return uint32_t(X_STATUS_WAIT_0 + i) + objects[i]->WaitStatusAdjust();
                    }
                }
            }

            if (poll)
                return X_STATUS_TIMEOUT;

            registration.Add();
            if (infinite)
            {
                t_waiter.cv.wait(lock);
            }
            else if (t_waiter.cv.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                // Satisfied at the deadline: taken on the next pass.
                bool satisfied = waitAll;
                for (const auto& object : objects)
                    satisfied = waitAll ? satisfied && object->IsSignaled(self)
                                        : satisfied || object->IsSignaled(self);
                if (!satisfied)
                    return X_STATUS_TIMEOUT;
            }
        }
    }

    uint32_t WaitSingle(const ObjectRef& object, const int64_t* timeout, bool alertable)
    {
        return WaitMultiple(std::span(&object, 1), false, timeout, alertable);
    }

    void ShortSleep(uint32_t microseconds)
    {
        // On Windows, a high resolution waitable timer: NtDelayExecution, like
        // Sleep, is only as fine as the timer resolution (1 ms, set with
        // timeBeginPeriod in main.cpp). Wine
        // (Proton, so the Steam Deck) is the other way round: its timers go
        // through wineserver, in milliseconds, while a non-alertable
        // NtDelayExecution sleeps in the process to the microsecond.
        using DelayFn = LONG(NTAPI*)(BOOLEAN alertable, PLARGE_INTEGER timeout);
        static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        static const auto delay = reinterpret_cast<DelayFn>(GetProcAddress(ntdll, "NtDelayExecution"));
        static const bool wine = GetProcAddress(ntdll, "wine_get_version") != nullptr;
        struct Timer
        {
            HANDLE handle = wine ? nullptr
                                 : CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
            ~Timer()
            {
                if (handle)
                    ::CloseHandle(handle);
            }
        };
        thread_local Timer timer;
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(microseconds) * 10;
        if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer.handle, INFINITE);
        else if (delay)
            delay(FALSE, &due);
        else
            SwitchToThread();
    }

    uint32_t DelayExecution(const int64_t* timeout, bool alertable)
    {
        if (!timeout)
            return WaitMultiple({}, false, nullptr, alertable);

        if (*timeout == 0)
        {
            if (alertable)
            {
                if (auto* self = GuestThread::Current(); self && self->DeliverApcs())
                    return X_STATUS_USER_APC;
            }
            // Sleep(0) in a loop is how the game waits for other threads (the
            // update thread waiting for the render thread, every frame). Yielding keeps
            // a core busy (and on handhelds takes power from the GPU), so after
            // a run of such calls with next to nothing done in between, the
            // thread sleeps briefly instead. Calls further apart (a loop doing
            // work and yielding) keep yielding, so short waits still end without
            // a sleep. A poll that sleeps can miss a flag the other thread clears
            // only briefly: then that thread should wait for the poller
            // (gpu/held_shot.cpp), rather than the sleep be made shorter.
            // (A poll's gap is a load, a compare and the call: about 0.2-2 us; a loop
            // doing work between yields is slowed if counted.)
            constexpr auto kPollGap = std::chrono::microseconds(10);
            constexpr uint32_t kPollsBeforeSleeping = 64;
            thread_local std::chrono::steady_clock::time_point lastReturn;
            thread_local uint32_t polls = 0;
            const auto now = std::chrono::steady_clock::now();
            polls = now - lastReturn < kPollGap ? polls + 1 : 0;
            if (polls >= kPollsBeforeSleeping)
                ShortSleep(kSpinSleepMicroseconds);
            else
                SwitchToThread();
            lastReturn = std::chrono::steady_clock::now();
            return X_STATUS_SUCCESS;
        }

        const auto deadline = TimeoutToDeadline(*timeout);
        if (!alertable)
        {
            std::this_thread::sleep_until(deadline);
            return X_STATUS_SUCCESS;
        }

        GuestThread* self = GuestThread::Current();
        auto lock = LockDispatcher();
        for (;;)
        {
            if (self && !self->apcQueue.empty())
            {
                lock.unlock();
                self->DeliverApcs();
                return X_STATUS_USER_APC;
            }
            if (self)
                AddWaiter(*self);
            const auto status = t_waiter.cv.wait_until(lock, deadline);
            if (self)
                RemoveWaiter(*self);
            if (status == std::cv_status::timeout)
                return X_STATUS_SUCCESS;
        }
    }
}
