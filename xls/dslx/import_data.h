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

#ifndef XLS_DSLX_IMPORT_DATA_H_
#define XLS_DSLX_IMPORT_DATA_H_

#include <filesystem>
#include <memory>

#include "xls/dslx/ast.h"
#include "xls/dslx/bytecode_cache_interface.h"
#include "xls/dslx/default_dslx_stdlib_path.h"
#include "xls/dslx/interp_bindings.h"
#include "xls/dslx/type_info.h"

namespace xls::dslx {

// An entry that goes into the ImportData.
struct ModuleInfo {
  std::unique_ptr<Module> module;
  TypeInfo* type_info;
};

// Immutable "tuple" of tokens that name an absolute import location.
//
// e.g. ("std",) or ("xls", "examples", "foo")
//
// Hashable (usable in a flat hash map).
class ImportTokens {
 public:
  static absl::StatusOr<ImportTokens> FromString(absl::string_view module_name);

  explicit ImportTokens(std::vector<std::string> pieces)
      : pieces_(std::move(pieces)) {}

  template <typename H>
  friend H AbslHashValue(H h, const ImportTokens& self) {
    for (const std::string& s : self.pieces_) {
      h = H::combine(std::move(h), s);
    }
    return h;
  }

  bool operator==(const ImportTokens& other) const {
    if (pieces_.size() != other.pieces_.size()) {
      return false;
    }
    for (int64_t i = 0; i < pieces_.size(); ++i) {
      if (pieces_[i] != other.pieces_[i]) {
        return false;
      }
    }
    return true;
  }

  // Returns the "dotted" version of the components that would appear in an
  // import statement; e.g. "xls.examples.foo"
  std::string ToString() const { return absl::StrJoin(pieces_, "."); }

  // The underlying components.
  const std::vector<std::string>& pieces() const { return pieces_; }

 private:
  std::vector<std::string> pieces_;
};

// Wrapper around a {subject: module_info} mapping that modules can be imported
// into.
// Use the routines in create_import_data.h to instantiate an object.
class ImportData {
 public:
  // All instantiations of ImportData should pass a stdlib_path as below.
  ImportData() = delete;

  bool Contains(const ImportTokens& target) const {
    return cache_.find(target) != cache_.end();
  }

  // Note: returned pointer is not stable across mutations.
  absl::StatusOr<const ModuleInfo*> Get(const ImportTokens& subject) const;

  // Note: returned pointer is not stable across mutations.
  absl::StatusOr<const ModuleInfo*> Put(const ImportTokens& subject,
                                        ModuleInfo module_info);

  TypeInfoOwner& type_info_owner() { return type_info_owner_; }

  // Helper that gets the "root" type information for the module of the given
  // node. (Note that type information lives in a tree configuration where
  // parametric specializations live under the root, see TypeInfo.)
  absl::StatusOr<TypeInfo*> GetRootTypeInfoForNode(AstNode* node);
  absl::StatusOr<TypeInfo*> GetRootTypeInfo(Module* module);

  // The "top level bindings" for a given module are the values that get
  // resolved at module scope on import. Keeping these on the ImportData avoids
  // recomputing them.
  InterpBindings& GetOrCreateTopLevelBindings(Module* module);

  // Notes the top level bindings object for the given module.
  //
  // Precondition: bindings should not already be set for the given module, or
  // this will check-fail.
  void SetTopLevelBindings(Module* module, std::unique_ptr<InterpBindings> tlb);

  // Notes which node at the top level of the given module is currently
  // work-in-progress. "node" may be set as nullptr when done with the entire
  // module.
  void SetTypecheckWorkInProgress(Module* module, AstNode* node) {
    typecheck_wip_[module] = node;
  }

  // Retrieves which node was noted as currently work-in-progress, getter for
  // SetTypecheckWorkInProgress() above.
  AstNode* GetTypecheckWorkInProgress(Module* module) {
    return typecheck_wip_[module];
  }

  // Helpers for marking/querying whether the top-level scope for a given module
  // was completed being evaluated. That is, once the top-level bindings for a
  // module have been evaluated successfully once by the interpreter (without
  // hitting a work-in-progress indicator) those completed bindings can be
  // re-used after that without any need for re-evaluation.
  bool IsTopLevelBindingsDone(Module* module) const {
    return top_level_bindings_done_.contains(module);
  }
  void MarkTopLevelBindingsDone(Module* module) {
    top_level_bindings_done_.insert(module);
  }

  const std::string& stdlib_path() const { return stdlib_path_; }
  absl::Span<const std::filesystem::path> additional_search_paths() {
    return additional_search_paths_;
  }

  void SetBytecodeCache(std::unique_ptr<BytecodeCacheInterface> bytecode_cache);
  BytecodeCacheInterface* bytecode_cache();

 private:
  friend ImportData CreateImportData(std::string,
                                     absl::Span<const std::filesystem::path>);
  friend std::unique_ptr<ImportData> CreateImportDataPtr(
      std::string, absl::Span<const std::filesystem::path>);
  friend ImportData CreateImportDataForTest();

  ImportData(std::string stdlib_path,
             absl::Span<const std::filesystem::path> additional_search_paths)
      : stdlib_path_(std::move(stdlib_path)),
        additional_search_paths_(additional_search_paths) {}

  absl::flat_hash_map<ImportTokens, ModuleInfo> cache_;
  absl::flat_hash_map<Module*, std::unique_ptr<InterpBindings>>
      top_level_bindings_;
  absl::flat_hash_set<Module*> top_level_bindings_done_;
  absl::flat_hash_map<Module*, AstNode*> typecheck_wip_;
  TypeInfoOwner type_info_owner_;
  std::string stdlib_path_;
  absl::Span<const std::filesystem::path> additional_search_paths_;
  std::unique_ptr<BytecodeCacheInterface> bytecode_cache_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_IMPORT_DATA_H_
