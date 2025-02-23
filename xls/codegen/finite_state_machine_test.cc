// Copyright 2020 The XLS Authors
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

#include "xls/codegen/finite_state_machine.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/codegen/vast.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/matchers.h"
#include "xls/simulation/verilog_test_base.h"

namespace xls {
namespace verilog {
namespace {

using status_testing::StatusIs;
using ::testing::HasSubstr;

constexpr char kTestName[] = "finite_state_machine_test";
constexpr char kTestdataPath[] = "xls/codegen/testdata";

class FiniteStateMachineTest : public VerilogTestBase {};

TEST_P(FiniteStateMachineTest, TrivialFsm) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  FsmBuilder fsm("TrivialFsm", module, clk, UseSystemVerilog());
  auto foo = fsm.AddState("Foo");
  auto bar = fsm.AddState("Bar");

  foo->NextState(bar);

  XLS_ASSERT_OK(fsm.Build());
  XLS_VLOG(1) << f.Emit();
  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, TrivialFsmWithOutputs) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  FsmBuilder fsm("TrivialFsm", module, clk, UseSystemVerilog());
  auto foo = fsm.AddState("Foo");
  auto bar = fsm.AddState("Bar");

  auto baz_out = fsm.AddOutput1("baz", /*default_value=*/false);
  auto qux_out = fsm.AddRegister("qux", 7);

  foo->NextState(bar);
  foo->SetOutput(baz_out, 1);

  bar->NextState(foo);
  // qux counts how many times the state "foo" has been entered.
  bar->SetRegisterNextAsExpression(
      qux_out,
      f.Add(qux_out->logic_ref, f.PlainLiteral(1, std::nullopt), std::nullopt));

  XLS_ASSERT_OK(fsm.Build());
  XLS_VLOG(1) << f.Emit();
  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, SimpleFsm) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* rst_n =
      module->AddInput("rst_n", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* ready_in =
      module->AddInput("ready_in", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* done_out =
      module->AddOutput("done_out", f.ScalarType(std::nullopt), std::nullopt);

  // The "done" output is a wire, create a reg copy for assignment in the FSM.
  LogicRef* done =
      module->AddReg("done", f.ScalarType(std::nullopt), std::nullopt);
  module->Add<ContinuousAssignment>(std::nullopt, done_out, done);

  FsmBuilder fsm("SimpleFsm", module, clk, UseSystemVerilog(),
                 Reset{rst_n, /*async=*/false, /*active_low=*/true});
  auto idle_state = fsm.AddState("Idle");
  auto busy_state = fsm.AddState("Busy");
  auto done_state = fsm.AddState("Done");

  auto fsm_done_out =
      fsm.AddExistingOutput(done,
                            /*default_value=*/f.PlainLiteral(0, std::nullopt));

  idle_state->OnCondition(ready_in).NextState(busy_state);
  busy_state->NextState(done_state);
  done_state->SetOutput(fsm_done_out, 1);

  XLS_ASSERT_OK(fsm.Build());
  XLS_VLOG(1) << f.Emit();
  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, FsmWithNestedLogic) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* rst_n =
      module->AddInput("rst_n", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* foo =
      module->AddInput("foo", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* bar =
      module->AddInput("bar", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* qux =
      module->AddOutput("qux_out", f.ScalarType(std::nullopt), std::nullopt);

  FsmBuilder fsm("NestLogic", module, clk, UseSystemVerilog(),
                 Reset{rst_n, /*async=*/false, /*active_low=*/true});
  auto a_state = fsm.AddState("A");
  auto b_state = fsm.AddState("B");

  auto fsm_qux_out = fsm.AddOutput("qux", /*width=*/8,
                                   /*default_value=*/0);

  a_state->OnCondition(foo)
      .NextState(b_state)

      // Nested Conditional
      .OnCondition(bar)
      .SetOutput(fsm_qux_out, 42)
      .Else()
      .SetOutput(fsm_qux_out, 123);
  b_state->OnCondition(f.LogicalAnd(foo, bar, std::nullopt)).NextState(a_state);

  XLS_ASSERT_OK(fsm.Build());

  module->Add<ContinuousAssignment>(std::nullopt, qux, fsm_qux_out->logic_ref);

  XLS_VLOG(1) << f.Emit();
  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, CounterFsm) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* rst =
      module->AddInput("rst", f.ScalarType(std::nullopt), std::nullopt);
  FsmBuilder fsm("CounterFsm", module, clk, UseSystemVerilog(),
                 Reset{rst, /*async=*/true, /*active_low=*/false});
  auto foo = fsm.AddState("Foo");
  auto bar = fsm.AddState("Bar");
  auto qux = fsm.AddState("Qux");

  auto counter = fsm.AddDownCounter("counter", 6);
  foo->SetCounter(counter, 42).NextState(bar);
  bar->OnCounterIsZero(counter).NextState(qux);
  qux->NextState(foo);

  XLS_ASSERT_OK(fsm.Build());
  XLS_VLOG(1) << f.Emit();
  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, ComplexFsm) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* foo_in =
      module->AddInput("foo_in", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* bar_in =
      module->AddOutput("bar_in", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* qux_in =
      module->AddOutput("qux_in", f.ScalarType(std::nullopt), std::nullopt);

  FsmBuilder fsm("ComplexFsm", module, clk, UseSystemVerilog());
  auto hungry = fsm.AddState("Hungry");
  auto sad = fsm.AddState("Sad");
  auto happy = fsm.AddState("Happy");
  auto awake = fsm.AddState("Awake");
  auto sleepy = fsm.AddState("Sleepy");

  auto sleep = fsm.AddOutput1("sleep", 0);
  auto walk = fsm.AddOutput1("walk", 0);
  auto run = fsm.AddOutput1("run", 1);
  auto die = fsm.AddOutput1("die", 1);

  hungry->OnCondition(foo_in).NextState(happy).Else().NextState(sad);
  hungry->OnCondition(qux_in).SetOutput(walk, 0).SetOutput(die, 1);

  sad->NextState(awake);
  sad->SetOutput(walk, 0);
  sad->SetOutput(run, 1);

  awake->NextState(sleepy);

  sleepy->OnCondition(bar_in)
      .NextState(hungry)
      .ElseOnCondition(qux_in)
      .NextState(sad);

  happy->OnCondition(bar_in).SetOutput(die, 0);
  happy->OnCondition(foo_in).NextState(hungry).SetOutput(sleep, 1);

  XLS_ASSERT_OK(fsm.Build());
  XLS_VLOG(1) << f.Emit();
  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, OutputAssignments) {
  // Test various conditional and unconditional assignments of output regs in
  // different states. Verify the proper insertion of assignment of default
  // values to the outputs such that each code path has exactly one assignment
  // per output.
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* rst_n =
      module->AddInput("rst_n", f.ScalarType(std::nullopt), std::nullopt);

  LogicRef* a = module->AddInput("a", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* b = module->AddInput("b", f.ScalarType(std::nullopt), std::nullopt);

  FsmBuilder fsm("SimpleFsm", module, clk, UseSystemVerilog(),
                 Reset{rst_n, /*async=*/false, /*active_low=*/true});
  auto out_42 = fsm.AddOutput("out_42", /*width=*/8, /*default_value=*/42);
  auto out_123 = fsm.AddOutput("out_123", /*width=*/8, /*default_value=*/123);

  auto idle_state = fsm.AddState("Idle");
  idle_state->NextState(idle_state);

  {
    auto state = fsm.AddState("AssignmentToDefaultValue");
    state->SetOutput(out_42, 42);
    state->SetOutput(out_123, 123);
    state->NextState(idle_state);
  }

  {
    auto state = fsm.AddState("AssignmentToNondefaultValue");
    state->SetOutput(out_42, 33);
    state->SetOutput(out_123, 22);
    state->NextState(idle_state);
  }

  {
    auto state = fsm.AddState("ConditionalAssignToDefaultValue");
    state->OnCondition(a).SetOutput(out_42, 42);
    state->OnCondition(b).SetOutput(out_123, 123);
    state->NextState(idle_state);
  }

  {
    auto state = fsm.AddState("ConditionalAssignToNondefaultValue");
    state->OnCondition(a).SetOutput(out_42, 1);
    state->OnCondition(b).SetOutput(out_123, 2).Else().SetOutput(out_123, 4);
    state->NextState(idle_state);
  }

  {
    auto state = fsm.AddState("NestedConditionalAssignToNondefaultValue");
    state->OnCondition(a).OnCondition(b).SetOutput(out_42, 1).Else().SetOutput(
        out_123, 7);
    state->NextState(idle_state);
  }

  {
    auto state = fsm.AddState("AssignToNondefaultValueAtDifferentDepths");
    ConditionalFsmBlock& if_a = state->OnCondition(a);
    if_a.SetOutput(out_42, 1);
    if_a.Else().OnCondition(b).SetOutput(out_42, 77);
    state->NextState(idle_state);
  }

  XLS_ASSERT_OK(fsm.Build());
  XLS_VLOG(1) << f.Emit();

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 f.Emit());
}

TEST_P(FiniteStateMachineTest, MultipleAssignments) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* rst_n =
      module->AddInput("rst_n", f.ScalarType(std::nullopt), std::nullopt);

  LogicRef* a = module->AddInput("a", f.ScalarType(std::nullopt), std::nullopt);

  FsmBuilder fsm("SimpleFsm", module, clk, UseSystemVerilog(),
                 Reset{rst_n, /*async=*/false, /*active_low=*/true});
  auto out = fsm.AddOutput("out", /*width=*/8, /*default_value=*/42);

  auto state = fsm.AddState("State");
  state->SetOutput(out, 123);
  state->OnCondition(a).SetOutput(out, 44);

  XLS_VLOG(1) << f.Emit();
  EXPECT_THAT(
      fsm.Build(),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Output \"out\" may be assigned more than once")));
}

TEST_P(FiniteStateMachineTest, MultipleConditionalAssignments) {
  VerilogFile f(UseSystemVerilog());
  Module* module = f.Add(f.Make<Module>(std::nullopt, TestBaseName()));

  LogicRef* clk =
      module->AddInput("clk", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* rst_n =
      module->AddInput("rst_n", f.ScalarType(std::nullopt), std::nullopt);

  LogicRef* a = module->AddInput("a", f.ScalarType(std::nullopt), std::nullopt);
  LogicRef* b = module->AddInput("b", f.ScalarType(std::nullopt), std::nullopt);

  FsmBuilder fsm("SimpleFsm", module, clk, UseSystemVerilog(),
                 Reset{rst_n, /*async=*/false, /*active_low=*/true});
  auto out = fsm.AddOutput("out", /*width=*/8, /*default_value=*/42);

  auto state = fsm.AddState("State");
  state->OnCondition(a).SetOutput(out, 44);
  // Even setting output to same value is an error.
  state->OnCondition(b).SetOutput(out, 44);

  XLS_VLOG(1) << f.Emit();
  EXPECT_THAT(
      fsm.Build(),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Output \"out\" may be assigned more than once")));
}

INSTANTIATE_TEST_SUITE_P(FiniteStateMachineTestInstantiation,
                         FiniteStateMachineTest,
                         testing::ValuesIn(kDefaultSimulationTargets),
                         ParameterizedTestName<FiniteStateMachineTest>);

}  // namespace
}  // namespace verilog
}  // namespace xls
