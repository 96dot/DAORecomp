#pragma once
// A Win32 handle that's closed when it goes.

namespace dao
{
    struct HandleCloser
    {
        void operator()(HANDLE handle) const
        {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
        }
    };

    // Owns a handle (move-only), closed unless it's null or INVALID_HANDLE_VALUE.
    // (One holding INVALID_HANDLE_VALUE, as CreateFileW fails with, isn't empty:
    // turn that into null first where it's tested.)
    using Handle = std::unique_ptr<void, HandleCloser>;
}
