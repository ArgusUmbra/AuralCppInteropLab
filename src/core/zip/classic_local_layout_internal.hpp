#ifndef AURAL_CORE_ZIP_CLASSIC_LOCAL_LAYOUT_INTERNAL_HPP
#define AURAL_CORE_ZIP_CLASSIC_LOCAL_LAYOUT_INTERNAL_HPP

#include "classic_local_layout.hpp"

namespace aural::core::zip {

// Internal same-handle entry point used by payload-integrity stages.
[[nodiscard]] LocalLayoutInspection inspect_classic_local_layout_open(
    const FileReader& reader,
    ClassicZipLimits limits = {});

} // namespace aural::core::zip

#endif
