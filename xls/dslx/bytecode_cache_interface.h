// Copyright 2022 The XLS Authors
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
#ifndef XLS_DSLX_BYTECODE_CACHE_INTERFACE_H_
#define XLS_DSLX_BYTECODE_CACHE_INTERFACE_H_

#include "absl/status/statusor.h"
#include "xls/dslx/ast.h"
#include "xls/dslx/bytecode.h"
#include "xls/dslx/symbolic_bindings.h"
#include "xls/dslx/type_info.h"

namespace xls::dslx {

// Defines the interface a type must provide in order to serve as a bytecode
// cache. In practice, this type exists to avoid attaching too many concrete
// dependencies onto ImportData, which is the primary cache owner.
class BytecodeCacheInterface {
 public:
  virtual ~BytecodeCacheInterface() = default;
  // Returns the BytecodeFunction for the given function, whose types and
  // constants are held inside the given TypeInfo - different instances of a
  // parametric function will have different TypeInfos associated with them.
  virtual absl::StatusOr<BytecodeFunction*> GetOrCreateBytecodeFunction(
      const Function* f, const TypeInfo* type_info,
      const absl::optional<SymbolicBindings>& caller_bindings) = 0;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_BYTECODE_CACHE_INTERFACE_H_
