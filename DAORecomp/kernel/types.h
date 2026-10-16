#pragma once
// Big-endian guest structures and constants for the Xbox 360 kernel (layouts follow
// Xenia's research, see tools/xenia-canary/src/xenia/kernel).

namespace dao::kernel
{
    using u8 = uint8_t;
    using u16 = be<uint16_t>;
    using u32 = be<uint32_t>;
    using u64 = be<uint64_t>;
    using s32 = be<int32_t>;
    using s64 = be<int64_t>;

    // NTSTATUS values
    constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
    constexpr uint32_t X_STATUS_WAIT_0 = 0x00000000;
    constexpr uint32_t X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
    constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
    constexpr uint32_t X_STATUS_ALERTED = 0x00000101;
    constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
    constexpr uint32_t X_STATUS_PENDING = 0x00000103;
    constexpr uint32_t X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
    constexpr uint32_t X_STATUS_BUFFER_OVERFLOW = 0x80000005;
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
    constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
    constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
    constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
    constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
    constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
    constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
    constexpr uint32_t X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
    constexpr uint32_t X_STATUS_OBJECT_NAME_INVALID = 0xC0000033;
    constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
    constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
    constexpr uint32_t X_STATUS_INSUFFICIENT_RESOURCES = 0xC000009A;
    constexpr uint32_t X_STATUS_NOT_SUPPORTED = 0xC00000BB;
    constexpr uint32_t X_STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
    constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
    constexpr uint32_t X_STATUS_NOT_A_DIRECTORY = 0xC0000103;

    // Win32 errors (XAM returns these)
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 2;
    constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t X_ERROR_ACCESS_DENIED = 5;
    constexpr uint32_t X_ERROR_INVALID_HANDLE = 6;
    constexpr uint32_t X_ERROR_NO_MORE_FILES = 18;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER = 87;
    constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 122;
    constexpr uint32_t X_ERROR_ALREADY_EXISTS = 183;
    constexpr uint32_t X_ERROR_IO_INCOMPLETE = 996;
    constexpr uint32_t X_ERROR_IO_PENDING = 997;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
    constexpr uint32_t X_ERROR_NOT_FOUND = 1168;
    constexpr uint32_t X_ERROR_CANCELLED = 1223;
    constexpr uint32_t X_ERROR_NO_SUCH_USER = 1317;
    constexpr uint32_t X_ERROR_FUNCTION_FAILED = 1627;

    constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
    constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;

    struct X_LIST_ENTRY
    {
        u32 flink;
        u32 blink;
    };
    static_assert(sizeof(X_LIST_ENTRY) == 8);

    // Dispatcher object types (header.type)
    enum : uint8_t
    {
        kTypeNotificationEvent = 0,
        kTypeSynchronizationEvent = 1,
        kTypeMutant = 2,
        kTypeProcess = 3,
        kTypeQueue = 4,
        kTypeSemaphore = 5,
        kTypeThread = 6,
        kTypeNotificationTimer = 8,
        kTypeSynchronizationTimer = 9,
    };

    struct X_DISPATCH_HEADER
    {
        uint8_t type;
        uint8_t absolute;
        uint8_t size;
        uint8_t inserted;
        s32 signalState;
        X_LIST_ENTRY waitList;
    };
    static_assert(sizeof(X_DISPATCH_HEADER) == 0x10);

    struct X_KEVENT
    {
        X_DISPATCH_HEADER header;
    };

    struct X_KSEMAPHORE
    {
        X_DISPATCH_HEADER header;
        s32 limit;
    };
    static_assert(sizeof(X_KSEMAPHORE) == 0x14);

    struct X_KMUTANT
    {
        X_DISPATCH_HEADER header;
        X_LIST_ENTRY unkList;
        u32 owner;
        uint8_t abandoned;
        uint8_t pad[3];
    };
    static_assert(sizeof(X_KMUTANT) == 0x20);

    struct X_KTIMER
    {
        X_DISPATCH_HEADER header;
        u64 dueTime;
        X_LIST_ENTRY tableBucketEntry;
        u32 dpc;
        u32 period;
    };
    static_assert(sizeof(X_KTIMER) == 0x28);

    struct X_RTL_CRITICAL_SECTION
    {
        X_DISPATCH_HEADER header; // synchronization event used for contention
        s32 lockCount;            // -1 when free
        s32 recursionCount;
        u32 owningThread;         // KTHREAD*
    };
    static_assert(sizeof(X_RTL_CRITICAL_SECTION) == 0x1C);

    struct X_ANSI_STRING
    {
        u16 length;
        u16 maximumLength;
        u32 buffer;
    };

    struct X_UNICODE_STRING
    {
        u16 length;
        u16 maximumLength;
        u32 buffer;
    };

    struct X_OBJECT_ATTRIBUTES
    {
        u32 rootDirectory;
        u32 objectName; // X_ANSI_STRING*
        u32 attributes;
    };

    struct X_IO_STATUS_BLOCK
    {
        u32 status;
        u32 information;
    };

    struct X_TIME_STAMP_BUNDLE
    {
        u64 interruptTime;
        u64 systemTime;
        u32 tickCount;
        u32 padding;
    };

    // Processor control region, pointed to by r13.
    constexpr uint32_t kPcrSize = 0x2D8;
    constexpr uint32_t kPcrTlsPtr = 0x000;
    constexpr uint32_t kPcrIrql = 0x018;          // current_irql (a byte)
    constexpr uint8_t kDispatchLevel = 2;         // DISPATCH_LEVEL, a DPC's IRQL
    constexpr uint32_t kPcrPcrPtr = 0x030;
    constexpr uint32_t kPcrStackBase = 0x070;
    constexpr uint32_t kPcrStackEnd = 0x074;
    constexpr uint32_t kPcrCurrentThread = 0x100; // prcb_data.current_thread
    constexpr uint32_t kPcrCurrentCpu = 0x10C;    // prcb_data.current_cpu
    constexpr uint32_t kPcrDpcActive = 0x150;     // prcb_data.dpc_active
    constexpr uint32_t kPcrPrcb = 0x2A8;

    // KTHREAD (subset)
    constexpr uint32_t kKThreadSize = 0xAB0;
    constexpr uint32_t kKThreadStackBase = 0x5C;
    constexpr uint32_t kKThreadStackLimit = 0x60;
    constexpr uint32_t kKThreadStackKernel = 0x64;
    constexpr uint32_t kKThreadTlsAddress = 0x68;
    constexpr uint32_t kKThreadState = 0x6C;
    constexpr uint32_t kKThreadPriority = 0x70;
    constexpr uint32_t kKThreadProcessType = 0x73;
    constexpr uint32_t kKThreadProcess = 0x84;
    constexpr uint32_t kKThreadSuspendCount = 0xBC;
    constexpr uint32_t kKThreadCurrentCpu = 0xBF;
    constexpr uint32_t kKThreadCreateTime = 0x130;
    constexpr uint32_t kKThreadThreadId = 0x14C;
    constexpr uint32_t kKThreadStartAddress = 0x150;
    constexpr uint32_t kKThreadLastError = 0x160;
    constexpr uint32_t kKThreadCreationFlags = 0x16C;

    // Memory
    constexpr uint32_t X_MEM_COMMIT = 0x00001000;
    constexpr uint32_t X_MEM_RESERVE = 0x00002000;
    constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
    constexpr uint32_t X_MEM_RELEASE = 0x00008000;
    constexpr uint32_t X_MEM_FREE = 0x00010000;
    constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
    constexpr uint32_t X_MEM_RESET = 0x00080000;
    constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
    constexpr uint32_t X_MEM_NOZERO = 0x00800000;
    constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
    constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;

    constexpr uint32_t X_PAGE_NOACCESS = 0x01;
    constexpr uint32_t X_PAGE_READONLY = 0x02;
    constexpr uint32_t X_PAGE_READWRITE = 0x04;
    constexpr uint32_t X_PAGE_WRITECOPY = 0x08;
    constexpr uint32_t X_PAGE_EXECUTE = 0x10;
    constexpr uint32_t X_PAGE_EXECUTE_READ = 0x20;
    constexpr uint32_t X_PAGE_EXECUTE_READWRITE = 0x40;
    constexpr uint32_t X_PAGE_GUARD = 0x100;
    constexpr uint32_t X_PAGE_NOCACHE = 0x200;
    constexpr uint32_t X_PAGE_WRITECOMBINE = 0x400;
}
