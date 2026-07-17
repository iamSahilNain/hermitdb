#pragma once
// Full-dict snapshot serialization. [Claude Code] — implemented at M5.
// Serialization only: the ATOMICITY of snapshot+WAL-truncate (temp file,
// rename, directory fsync) is CP4's — the human's — via Wal::rewrite().

#include <string>

#include "core/db.h"
#include "core/expiry.h"

namespace hermit::persist {

// Writes db contents + TTL deadlines to `path`. Returns false with *err set.
bool save_snapshot(const core::Db& db, const core::ExpiryManager& expiry, const std::string& path,
                   std::string* err);

// Loads `path` into db/expiry (both assumed empty). Missing file is an error;
// the M5 boot sequence checks existence first.
bool load_snapshot(core::Db& db, core::ExpiryManager& expiry, const std::string& path,
                   std::string* err);

}  // namespace hermit::persist
