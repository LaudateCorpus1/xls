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
#include "xls/dslx/ast_utils.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/ast.h"
#include "xls/dslx/import_data.h"

namespace xls::dslx {
namespace {

TEST(ProcConfigIrConverterTest, ResolveProcNameRef) {
  Module module("test_module");
  NameDef* name_def = module.Make<NameDef>(Span::Fake(), "proc_name", nullptr);
  NameDef* config_name_def =
      module.Make<NameDef>(Span::Fake(), "config_name", nullptr);
  NameDef* next_name_def =
      module.Make<NameDef>(Span::Fake(), "next_name", nullptr);
  Function* config = nullptr;
  Function* next = nullptr;
  std::vector<Param*> members;
  std::vector<ParametricBinding*> bindings;
  Proc* original_proc =
      module.Make<Proc>(Span::Fake(), name_def, config_name_def, next_name_def,
                        bindings, members, config, next, /*is_public=*/true);
  module.AddTop(original_proc);
  name_def->set_definer(original_proc);

  TypeInfoOwner type_info_owner;
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfo * type_info, type_info_owner.New(&module));

  NameRef* name_ref = module.Make<NameRef>(Span::Fake(), "proc_name", name_def);
  XLS_ASSERT_OK_AND_ASSIGN(Proc * p, ResolveProc(name_ref, type_info));
  EXPECT_EQ(p, original_proc);
}

TEST(ProcConfigIrConverterTest, ResolveProcColonRef) {
  std::vector<std::string> import_tokens{"robs", "dslx", "import_module"};
  ImportTokens subject(import_tokens);
  ModuleInfo module_info;
  module_info.module = std::make_unique<Module>("import_module");
  module_info.type_info = nullptr;
  Module* import_module = module_info.module.get();

  NameDef* name_def =
      import_module->Make<NameDef>(Span::Fake(), "proc_name", nullptr);
  NameDef* config_name_def =
      import_module->Make<NameDef>(Span::Fake(), "config_name", nullptr);
  NameDef* next_name_def =
      import_module->Make<NameDef>(Span::Fake(), "next_name", nullptr);
  Function* config = nullptr;
  Function* next = nullptr;
  std::vector<Param*> members;
  std::vector<ParametricBinding*> bindings;
  Proc* original_proc = import_module->Make<Proc>(
      Span::Fake(), name_def, config_name_def, next_name_def, bindings, members,
      config, next, /*is_public=*/true);
  import_module->AddTop(original_proc);
  name_def->set_definer(original_proc);

  Module module("test_module");
  NameDef* module_def =
      module.Make<NameDef>(Span::Fake(), "import_module", nullptr);
  Import* import = module.Make<Import>(Span::Fake(), import_tokens, module_def,
                                       absl::nullopt);
  module_def->set_definer(import);
  NameRef* module_ref =
      module.Make<NameRef>(Span::Fake(), "import_module", module_def);
  ColonRef* colon_ref =
      module.Make<ColonRef>(Span::Fake(), module_ref, "proc_name");

  TypeInfoOwner type_info_owner;
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfo * type_info, type_info_owner.New(&module));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfo * imported_type_info,
                           type_info_owner.New(import_module));
  type_info->AddImport(import, import_module, imported_type_info);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * p, ResolveProc(colon_ref, type_info));
  EXPECT_EQ(p, original_proc);
}

}  // namespace
}  // namespace xls::dslx
