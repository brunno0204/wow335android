#include "Engine/Source/Os/System.h"
#include <link.h>
#include <cstdlib>
#include <cstdint>

void OsValidateFunctionPointer(const void* pointer) {
    struct Query { uintptr_t address; bool executable; } query{reinterpret_cast<uintptr_t>(pointer), false};
    dl_iterate_phdr([](dl_phdr_info* object, size_t, void* context) {
        auto& q = *static_cast<Query*>(context);
        for (unsigned i = 0; i < object->dlpi_phnum; ++i) {
            const auto& segment = object->dlpi_phdr[i];
            if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X)) continue;
            const auto begin = object->dlpi_addr + segment.p_vaddr;
            if (q.address >= begin && q.address - begin < segment.p_memsz) {
                q.executable = true;
                return 1;
            }
        }
        return 0;
    }, &query);
    if (!query.executable) std::abort();
}
