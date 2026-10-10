#pragma once
// The title module: XEX loading, header fields, kernel variable exports.

namespace dao::kernel
{
    struct ModuleInfo
    {
        uint32_t ldrEntry = 0;      // guest X_LDR_DATA_TABLE_ENTRY (the module "handle")
        uint32_t xexHeader = 0;     // guest copy of the XEX header
        uint32_t xexHeaderSize = 0;
        uint32_t entryPoint = 0;
        uint32_t stackSize = 0x40000;
        uint32_t systemFlags = 0;
        uint32_t titleId = 0;
        uint32_t mediaId = 0;
        uint32_t version = 0;
    };

    extern ModuleInfo g_module;

    // Decrypts/decompresses the XEX into guest memory, fills kernel variable
    // imports and sets up TLS. Returns false on failure.
    bool LoadXex(const std::filesystem::path& path);

    // RtlImageXexHeaderField semantics on a guest XEX header.
    uint32_t XexHeaderField(uint32_t header, uint32_t key);

    // Periodically refreshes KeTimeStampBundle.
    void StartTimeStampThread();
}
