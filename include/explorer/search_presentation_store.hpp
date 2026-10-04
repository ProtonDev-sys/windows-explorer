#pragma once
#include "explorer/search.hpp"

namespace explorer {
// This is per-app saved-view metadata, separate from search history and the
// native query file. Normal hosts resolve this path; headless hosts never do.
std::filesystem::path searchPresentationDirectory();
// Explicit directory arguments permit isolated owned fixtures. Production
// callers must not perform these reads/writes in headless mode. Records bind
// to the query's canonical path, volume/FileID, size and modification/change
// timestamps, so external edits or replacements cannot inherit stale layouts.
HRESULT saveSearchPresentationCompanion(const std::filesystem::path& query,
                                       const std::filesystem::path& directory,
                                       const SearchViewPresentation& actual);
// S_FALSE means absent/stale. Only an exact valid match updates mode/iconSize;
// public columns/group/sort are retained from the query's imported metadata.
HRESULT loadSearchPresentationCompanion(const std::filesystem::path& query,
                                       const std::filesystem::path& directory,
                                       SearchViewPresentation* result);
} // namespace explorer
