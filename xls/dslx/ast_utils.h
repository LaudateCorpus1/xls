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
#ifndef XLS_DSLX_AST_UTILS_H_
#define XLS_DSLX_AST_UTILS_H_

#include "absl/status/statusor.h"
#include "xls/dslx/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_info.h"

namespace xls::dslx {

// Finds the Proc identified by the given node (either NameRef or ColonRef),
// using the associated ImportData for import Module lookup.
absl::StatusOr<Proc*> ResolveProc(Expr* callee, TypeInfo* type_info);

// Returns the basis of the given ColonRef; either a Module for a constant
// reference or the EnumDef whose attribute is specified.
absl::StatusOr<absl::variant<Module*, EnumDef*>> ResolveColonRefSubject(
    ImportData* import_data, const TypeInfo* type_info, ColonRef* colon_ref);

}  // namespace xls::dslx

#endif  // XLS_DSLX_AST_UTILS_H_
