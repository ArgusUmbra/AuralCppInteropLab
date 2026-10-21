#ifndef AURAL_CORE_ZIP_CLASSIC_CENTRAL_DIRECTORY_INTERNAL_HPP
#define AURAL_CORE_ZIP_CLASSIC_CENTRAL_DIRECTORY_INTERNAL_HPP

#include "classic_central_directory.hpp"

namespace aural::core::zip {

// Internal same-snapshot entry point used by later ZIP inspection stages.
[[nodiscard]] CentralDirectoryInspection inspect_classic_central_directory_open(
    const FileReader& reader,
    ClassicZipLimits limits,
    bool defer_local_header_overlap_checks = false);

} // namespace aural::core::zip

#endif
