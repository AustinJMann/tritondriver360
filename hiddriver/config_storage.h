#pragma once

#include "triton_config.h"

namespace ConfigStorage {

// Uses native kernel paths for USB volumes before the native Hdd1 partition,
// then creates the generated default on the first writable device if none
// exists. Existing invalid files are never overwritten.
bool LoadOrCreate(TritonConfig::Config* config);

} // namespace ConfigStorage
