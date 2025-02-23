// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Library that backs the `opt_main` tool's primary functionality.

#ifndef XLS_TOOLS_OPT_H_
#define XLS_TOOLS_OPT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

// TODO(meheff): 2021-10-04 Remove this header.
#include "xls/passes/passes.h"

namespace xls::tools {

// Various optimizer options (that generally funnel from the `opt_main` tool to
// this consolidated library).
struct OptOptions {
  int64_t opt_level = xls::kMaxOptLevel;
  absl::string_view entry;
  std::string ir_dump_path = "";
  absl::optional<std::string> ir_path = absl::nullopt;
  absl::optional<std::vector<std::string>> run_only_passes = absl::nullopt;
  std::vector<std::string> skip_passes;
  std::optional<int64_t> convert_array_index_to_select = std::nullopt;
  bool inline_procs;
};

// Helper used in the opt_main tool, optimizes the given IR for a particular
// entry point function at the given opt level and returns the resulting
// optimized IR.
absl::StatusOr<std::string> OptimizeIrForEntry(absl::string_view ir,
                                               const OptOptions& options);

}  // namespace xls::tools

#endif  // XLS_TOOLS_OPT_H_
