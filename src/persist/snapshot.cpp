#include "persist/snapshot.h"

// M0 scaffold stub. Real implementation lands in M5 alongside CP4 (SPEC §4).

namespace hermit::persist {

bool save_snapshot(const core::Db&, const core::ExpiryManager&, const std::string&,
                   std::string* err) {
  if (err) *err = "snapshot not implemented until M5";
  return false;
}

bool load_snapshot(core::Db&, core::ExpiryManager&, const std::string&, std::string* err) {
  if (err) *err = "snapshot not implemented until M5";
  return false;
}

}  // namespace hermit::persist
