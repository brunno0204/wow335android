#include <cstdint>
#include "Engine/Source/Os/Processor.h"

// VERIFY: e905bbd2 auto
// OFFSET: 0x0086BB80
uint32_t OsGetProcessorFeatures() {
    int32_t vendor;
    return OsGetProcessorFeaturesEx(vendor);
}
