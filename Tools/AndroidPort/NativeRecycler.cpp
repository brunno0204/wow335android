#include "Engine/Source/Base/CDataRecycler.h"
#include "Engine/Source/Storm/H/storm.h"
#include <algorithm>
#include <mutex>
// Native implementation of the original recycler contract. Serialize cache
// mutation instead of relying on Windows lock-free SLIST/Interlocked behavior.
namespace {
std::recursive_mutex &cacheMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
} // namespace
CDataRecycler::CDataRecycler(uint32_t nodes, int32_t maximum) {
    (void)cacheMutex();
    m_nodesRecyclable = std::max(1, maximum);
    m_nodesPerBlock = std::max(1u, nodes);
    m_nodeBlockList = nullptr;
    m_nodeFullList = m_nodeEmptyList = nullptr;
}
CDataRecycler::~CDataRecycler() { Clear(); }
void *CDataRecycler::AllocData(uint32_t size, uint32_t *bytes, const char *file, int32_t line) {
    if (bytes)
        *bytes = size;
    return SMemAlloc(size, file, line, 0);
}
void *CDataRecycler::ReallocData(void *data, uint32_t size, uint32_t *bytes, const char *file, int32_t line) {
    if (bytes)
        *bytes = size;
    return SMemReAlloc(data, size, file, line, 0);
}
void CDataRecycler::FreeData(void *data, const char *file, int32_t line) { SMemFree(data, file, line, 0); }
void CDataRecycler::Clear() {
    std::lock_guard<std::recursive_mutex> lock(cacheMutex());
    while (m_nodeFullList) {
        auto *node = m_nodeFullList;
        m_nodeFullList = node->m_next;
        FreeData(node->m_data, nullptr, 0);
        SMemFree(node, nullptr, 0, 0);
        ++m_nodesRecyclable;
    }
}
void CDataRecycler::GetData(void *&data, uint32_t &bytes, const char *, int32_t) {
    std::lock_guard<std::recursive_mutex> lock(cacheMutex());
    data = nullptr;
    bytes = 0;
    if (m_nodeFullList) {
        auto *node = m_nodeFullList;
        m_nodeFullList = node->m_next;
        data = node->m_data;
        bytes = node->m_bytes;
        SMemFree(node, nullptr, 0, 0);
        ++m_nodesRecyclable;
    }
}
void CDataRecycler::PutData(void *data, uint32_t bytes, const char *file, int32_t line) {
    std::lock_guard<std::recursive_mutex> lock(cacheMutex());
    if (m_nodesRecyclable <= 0) {
        FreeData(data, file, line);
        return;
    }
    auto *node = static_cast<Node *>(SMemAlloc(sizeof(Node), file, line, 0));
    node->m_next = m_nodeFullList;
    node->m_data = data;
    node->m_bytes = bytes;
    m_nodeFullList = node;
    --m_nodesRecyclable;
}
