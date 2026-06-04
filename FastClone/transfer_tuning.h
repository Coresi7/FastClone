#pragma once

#include "cli.h"

#include <cstddef>
#include <cstdint>

namespace fc {

struct TunedTransferOptions {
    uint32_t streamLimit = 16;
    uint32_t chunkSize = 256 * 1024;
};

TunedTransferOptions ResolveTransferOptions(const CliOptions& options);

uint32_t EffectiveChunkSizeForStreams(uint32_t configuredChunkSize, uint32_t streamLimit);
size_t DownloadFlushThresholdForStreams(uint32_t streamLimit, uint32_t effectiveChunkSize);

}  // namespace fc
