#include "stdafx.h"
#include "objects.h"
#include "thread.h"
#include "memory.h"

namespace dao::kernel
{
    static std::mutex s_dispatchMutex;
    static std::condition_variable s_dispatchCv;

    std::unique_lock<std::mutex> LockDispatcher()
    {
        return std::unique_lock(s_dispatchMutex);
    }

    void NotifyDispatcher()
    {
        s_dispatchCv.notify_all();
    }

    void WaitDispatcherChange(std::unique_lock<std::mutex>& lock)
    {
        s_dispatchCv.wait(lock);
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
        if (timeout < 0)
            return now + nanoseconds(-timeout * 100);

        // Positive = absolute system time.
        const int64_t delta = timeout - int64_t(SystemTime100ns());
        return now + nanoseconds(std::max<int64_t>(delta, 0) * 100);
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
        lock.unlock();
        NotifyDispatcher();
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
        lock.unlock();
        NotifyDispatcher();
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
        lock.unlock();
        NotifyDispatcher();
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
        lock.unlock();
        NotifyDispatcher();
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
            if (timer->apcRoutine)
            {
                if (auto thread = timer->apcThread.lock())
                {
                    const uint64_t now = SystemTime100ns();
                    static_cast<GuestThread*>(thread.get())->apcQueue.push_back(
                        { timer->apcRoutine, timer->apcContext, uint32_t(now), uint32_t(now >> 32) });
                }
            }
            dispatch.unlock();
            NotifyDispatcher();
        }
    };

    static TimerService s_timers;

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

    void AttachGuestHeader(const ObjectRef& object, uint32_t size)
    {
        const uint32_t header = KernelAlloc(std::max<uint32_t>(size, sizeof(X_DISPATCH_HEADER)));
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

        std::lock_guard lock(s_headerMutex);
        BindLocked(object, header);
        object->ownsGuestObject = true;
        object->SyncGuest();
    }

    void BindGuestHeader(const ObjectRef& object, uint32_t header)
    {
        std::lock_guard lock(s_headerMutex);
        s_headerOwned.erase(header);
        BindLocked(object, header);
    }

    ObjectRef ObjectFromGuestHeader(uint32_t header)
    {
        if (!header)
            return nullptr;

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
        s_headerOwned[header] = object;
        return object;
    }

    void ReleaseGuestHeader(uint32_t header)
    {
        std::lock_guard lock(s_headerMutex);
        s_headerOwned.erase(header);
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
        for (;;)
        {
            if (alertable && self && !self->apcQueue.empty())
            {
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

            if (infinite)
            {
                s_dispatchCv.wait(lock);
            }
            else if (s_dispatchCv.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                // One last check happens on the next loop iteration only if signaled;
                // otherwise report the timeout.
                bool any = false;
                for (const auto& object : objects)
                    any = any || object->IsSignaled(self);
                if (!any || waitAll)
                {
                    if (!waitAll || !any)
                        return X_STATUS_TIMEOUT;
                }
            }
        }
    }

    uint32_t WaitSingle(const ObjectRef& object, const int64_t* timeout, bool alertable)
    {
        return WaitMultiple(std::span(&object, 1), false, timeout, alertable);
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
            SwitchToThread();
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
            if (s_dispatchCv.wait_until(lock, deadline) == std::cv_status::timeout)
                return X_STATUS_SUCCESS;
        }
    }
}
