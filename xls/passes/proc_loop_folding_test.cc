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

#include "xls/passes/proc_loop_folding.h"
#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/examples/proc_fir_filter.h"
#include "xls/ir/function.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_scanner.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"
#include "xls/ir/value_helpers.h"
#include "xls/passes/dce_pass.h"
#include "xls/interpreter/proc_interpreter.h"
#include "xls/interpreter/channel_queue.h"

namespace xls {

namespace {

using status_testing::IsOkAndHolds;

class RollIntoProcPassTest : public IrTestBase {
 protected:
  RollIntoProcPassTest() = default;

  absl::StatusOr<bool> Run(Proc* proc) {
    PassResults results;
    PassOptions opts = PassOptions();
    XLS_ASSIGN_OR_RETURN(bool changed,
                         RollIntoProcPass().RunOnProc(
                             proc, opts, &results));
    // Run dce to clean things up.
    XLS_RETURN_IF_ERROR(DeadCodeEliminationPass()
                            .RunOnFunctionBase((Function*)proc, PassOptions(),
                                               &results)
                            .status());
    return changed;
  }

  absl::StatusOr<bool> Run(Proc* proc, int64_t unroll_factor) {
    PassResults results;
    PassOptions opts = PassOptions();
    XLS_ASSIGN_OR_RETURN(bool changed,
                         RollIntoProcPass(unroll_factor).RunOnProc(
                             proc, opts, &results));
    // Run dce to clean things up.
    XLS_RETURN_IF_ERROR(DeadCodeEliminationPass()
                            .RunOnFunctionBase((Function*)proc, PassOptions(),
                                               &results)
                            .status());
    return changed;
  }
};

// Pass should do nothing if no CountedFor node present.
TEST_F(RollIntoProcPassTest, NoCountedFor) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_counted_for";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  // Create channels.
  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue adder = pb.Add(pb.GetStateParam(), lit1);
  BValue out = pb.Send(ch1, pb.GetTokenParam(), adder);
  BValue after_all = pb.AfterAll({out,  pb.TupleIndex(in, 0)});
  BValue next_state = adder;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(false));
}

// Pass should do nothing if no Receive node present.
TEST_F(RollIntoProcPassTest, NoReceive) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_receive";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 2, 1, loopbody, {pb.GetStateParam()});
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({out});
  BValue next_state = pb.GetStateParam();
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(false));
}

// Pass should do nothing if no Send node present.
TEST_F(RollIntoProcPassTest, NoSend) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 2, 1, loopbody, {pb.GetStateParam()});
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0)});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(false));
}

// A simple loop. The loop has no dependence on any nodes, i.e. the invariant
// is just a literal.
TEST_F(RollIntoProcPassTest, SimpleLoop) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 2, 1, loopbody, {lit1});
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // The transformed proc should just output 2 every time. It's a CountedFor
  // which adds an invariant literal 1 to the accumulator that runs four times.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 2 elements, so this will run twice. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue on the first iteration, since the CountedFor
  // runs twice.
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());

  // Check if output is equal to 2
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(2, 32))));

  // Run again
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(2, 32))));
}

// A similar simple loop as before, but it is unrolled twice.
TEST_F(RollIntoProcPassTest, SimpleLoopUnrolled) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 4, 1, loopbody, {lit1});
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc, 2), status_testing::IsOkAndHolds(true));

  // The transformed proc should just output 2 every time. It's a CountedFor
  // which adds an invariant literal 1 to the accumulator that runs four times.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 2 elements, so this will run twice. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue on the first iteration, since the CountedFor
  // runs twice.
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());

  // Check if output is equal to 2
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(4, 32))));

  // Run again
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(4, 32))));
}

// A similar simple loop as before, but it is unrolled five times.
TEST_F(RollIntoProcPassTest, SimpleLoopUnrolledFive) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 10, 1, loopbody, {lit1});
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc, 5), status_testing::IsOkAndHolds(true));

  // The transformed proc should just output 2 every time. It's a CountedFor
  // which adds an invariant literal 1 to the accumulator that runs four times.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 2 elements, so this will run twice. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue on the first iteration, since the CountedFor
  // runs twice.
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());

  // Check if output is equal to 2
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(10, 32))));

  // Run again
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(10, 32))));
}


// A similar CountedFor loop to before except this time it just sums the
// induction variable. Now we will test if moving the induction variable to the
// proc state works as intended.

// This test also implicitly covers what happens when the CountedFor has no
// loop invariants.
TEST_F(RollIntoProcPassTest, SimpleLoopUseInductionVar) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  BValue i = fb.Param("i", proc_state_type);
  BValue loop_carry_data = fb.Param("loop_carry_data", proc_state_type);
  fb.Add(loop_carry_data, i);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 10, 1, loopbody);
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // The transformed proc should just output 0 + 1 + ... + 9 each time (=45)
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 2 elements, so this will run twice. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until the tenth iteration.
  for (int64_t i = 0; i < 2; i++) {
    for (int64_t j = 0; j < 10; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 9) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    EXPECT_THAT(send_queue.Dequeue(), IsOkAndHolds(Value(UBits(45, 32))));
  }
}

// Same test as above, except the stride is now set to 3, and we set the
// trip count on CountedFor to 10.
TEST_F(RollIntoProcPassTest, SimpleLoopUseInductionVarStride) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  BValue i = fb.Param("i", proc_state_type);
  BValue loop_carry_data = fb.Param("loop_carry_data", proc_state_type);
  fb.Add(loop_carry_data, i);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 10, 3, loopbody);
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // The transformed proc should output 0 + 3 + 6 + ... + 27 each time (=135)
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 2 elements, so this will run twice. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until the tenth iteration.
  for (int64_t i = 0; i < 2; i++) {
    for (int64_t j = 0; j < 10; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 9) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    EXPECT_THAT(send_queue.Dequeue(), IsOkAndHolds(Value(UBits(135, 32))));
  }
}

// We now use a loop invariant that is dependent on the output of the receive.
// Specifically, the invariant is the output of the receive + 1.
TEST_F(RollIntoProcPassTest, SimpleLoopInvariantDependentOnRecv) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));


  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data = fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));
  BValue literal1 = pb.Literal(UBits(1, 32));
  BValue invar = pb.Add(pb.TupleIndex(in, 1), literal1);

  BValue result =
      pb.CountedFor(accumulator, 10, 1, loopbody, {invar});
  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // We will add the receive value + 1 to the accumulator 10 times. So it should
  // be equal to 10*(Receive + 1).
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 5 elements, so this will run 5 times. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(0, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(2, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(3, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(4, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until the tenth iteration.
  for (int64_t i = 0; i < 5; i++) {
    for (int64_t j = 0; j < 10; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 9) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    int64_t correct_value = (i+1)*10;
    EXPECT_THAT(send_queue.Dequeue(), IsOkAndHolds(Value(UBits(correct_value,
                                                               32))));
  }
}

TEST_F(RollIntoProcPassTest, SimpleLoopInitialCarryValDependentOnRecv) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));

  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue accumulator = pb.TupleIndex(in, 1);

  BValue result =
      pb.CountedFor(accumulator, 10, 1, loopbody, {lit1});

  BValue out = pb.Send(ch1, pb.GetTokenParam(), result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // We will add 1 to the accumulator 10 times, so the output should be the
  // initial value of the accumulator + 10.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 5 elements, so this will run 5 times. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(0, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(2, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(3, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(4, 32))}));

  std::vector<int> results = {10, 11, 12, 13, 14};
  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until the tenth iteration.
  for (int64_t i = 0; i < 5; i++) {
    for (int64_t j = 0; j < 10; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 9) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    int64_t correct_value = results[i];
    EXPECT_THAT(send_queue.Dequeue(), IsOkAndHolds(Value(UBits(correct_value,
                                                               32))));
  }
}

TEST_F(RollIntoProcPassTest, InvariantUsedAfterLoop) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));

  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue accumulator = pb.Literal(UBits(0, 32));

  BValue result =
      pb.CountedFor(accumulator, 10, 1, loopbody, {lit1});

  BValue send_result = pb.Add(result, lit1);
  BValue out = pb.Send(ch1, pb.GetTokenParam(), send_result);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // We will add 1 to the accumulator 10 times, so the output should be 10 plus
  // one, from the use of the invariant after the loop.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 5 elements, so this will run 5 times. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(0, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(2, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(3, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(4, 32))}));

  std::vector<int> results = {11, 11, 11, 11, 11};
  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until the tenth iteration.
  for (int64_t i = 0; i < 5; i++) {
    for (int64_t j = 0; j < 10; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 9) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    int64_t correct_value = results[i];
    EXPECT_THAT(send_queue.Dequeue(), IsOkAndHolds(Value(UBits(correct_value,
                                                               32))));
  }
}

// This tests if the Receive value can be correctly passed around the loop.
// This loop counts to 10, and it doesn't have any invariants that depend on
// the Receive value. The Receive is used in an equals statement after the loop
// to check if it is equal to the loop output (10).
TEST_F(RollIntoProcPassTest, ReceiveUsedAfterLoop) {
  auto p = CreatePackage();
  Value proc_initial_state = Value(UBits(0, 32));
  std::string name = "no_send";
  ProcBuilder pb(name, proc_initial_state, absl::StrFormat("%s_token", name),
                 absl::StrFormat("%s_state", name), p.get());

  Type* proc_state_type = p->GetTypeForValue(proc_initial_state);

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch0,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_in", name),
                               ChannelOps::kReceiveOnly,
                               proc_state_type));
  BValue in = pb.Receive(ch0, pb.GetTokenParam());

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* ch1,
                           p->CreateStreamingChannel(
                               absl::StrFormat("%s_out", name),
                               ChannelOps::kSendOnly,
                               proc_state_type));

  FunctionBuilder fb(absl::StrFormat("%s_loopbody", name), p.get());
  fb.Param("i", proc_state_type);
  BValue loop_carry_data =
      fb.Param("loop_carry_data", proc_state_type);
  BValue invar_loopbody = fb.Param("invar", proc_state_type);
  fb.Add(loop_carry_data, invar_loopbody);
  XLS_ASSERT_OK_AND_ASSIGN(Function * loopbody, fb.Build());

  BValue lit1 = pb.Literal(UBits(1, 32));
  BValue accumulator = pb.Literal(ZeroOfType(proc_state_type));

  BValue result =
      pb.CountedFor(accumulator, 10, 1, loopbody, {lit1});

  BValue send_val = pb.Concat({pb.Literal(UBits(0, 31)),
                               pb.Eq(result, pb.TupleIndex(in, 1))});

  BValue out = pb.Send(ch1, pb.GetTokenParam(), send_val);
  BValue after_all = pb.AfterAll({pb.TupleIndex(in, 0), out});
  BValue next_state = result;
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(after_all, next_state));

  EXPECT_THAT(Run(proc), status_testing::IsOkAndHolds(true));

  // We will add 1 to the accumulator 10 times, so the output should be 10.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(proc, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 5 elements, so this will run 5 times. The value from the Receive is
  // not used so it doesn't matter here.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(10, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(0, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(10, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(11, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(10, 32))}));

  std::vector<int> results = {1, 0, 1, 0, 1};
  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until the tenth iteration.
  for (int64_t i = 0; i < 5; i++) {
    for (int64_t j = 0; j < 10; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 9) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    int64_t correct_value = results[i];
    EXPECT_THAT(send_queue.Dequeue(), IsOkAndHolds(Value(UBits(correct_value,
                                                               32))));
  }
}

// Perform a comprehensive test on a 4-element kernel FIR filter. Test to see
// if the transformed proc will only emit an output on the fourth iteration, and
// check if the value is correct.
TEST_F(RollIntoProcPassTest, ImportFIR) {
  // Build FIR proc.
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Value kernel_value,
                           Value::UBitsArray({1, 2, 3, 4}, 32));

  absl::string_view name = "fir_proc";
  Type* kernel_type = p->GetTypeForValue(kernel_value.element(0));

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* x_in,
                           p->CreateStreamingChannel(
                           absl::StrFormat("%s_x_in", name),
                           ChannelOps::kReceiveOnly, kernel_type));

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* filter_out,
                           p->CreateStreamingChannel(
                           absl::StrFormat("%s_out", name),
                           ChannelOps::kSendOnly, kernel_type));

  XLS_ASSERT_OK_AND_ASSIGN(Proc* f, CreateFirFilter(name,
                                                    kernel_value,
                                                    x_in,
                                                    filter_out,
                                                    p.get()));

  // Run roll_into_proc_pass (+DCE).
  EXPECT_THAT(Run(f), status_testing::IsOkAndHolds(true));

  // Check if the transformed proc still works as an FIR filter.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(f, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_x_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 4 elements.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(2, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(3, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(4, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until four iterations (the length of the
  // kernel) have completed.
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  // At the end of this iteration, the result of the FIR filtering should be
  // available on the send queue.
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());
  // It should be equal to 1. Confirm.
  std::vector<int> expected_output = {1, 4, 10, 20};
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(expected_output[0], 32))));

  // Now do this three more times and confirm if the output is correct.
  for (int i = 1; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 3) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    EXPECT_THAT(send_queue.Dequeue(),
                IsOkAndHolds(Value(UBits(expected_output[i], 32))));
  }
}

// Perform a comprehensive test on a 4-element kernel FIR filter that is
// unrolled twice.
TEST_F(RollIntoProcPassTest, ImportFIRUnroll) {
  // Build FIR proc.
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Value kernel_value,
                           Value::UBitsArray({1, 2, 3, 4}, 32));

  absl::string_view name = "fir_proc";
  Type* kernel_type = p->GetTypeForValue(kernel_value.element(0));

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* x_in,
                           p->CreateStreamingChannel(
                           absl::StrFormat("%s_x_in", name),
                           ChannelOps::kReceiveOnly, kernel_type));

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* filter_out,
                           p->CreateStreamingChannel(
                           absl::StrFormat("%s_out", name),
                           ChannelOps::kSendOnly, kernel_type));

  XLS_ASSERT_OK_AND_ASSIGN(Proc* f, CreateFirFilter(name,
                                                    kernel_value,
                                                    x_in,
                                                    filter_out,
                                                    p.get()));

  // Run roll_into_proc_pass (+DCE).
  EXPECT_THAT(Run(f, 2), status_testing::IsOkAndHolds(true));

  // Check if the transformed proc still works as an FIR filter.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(f, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_x_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 4 elements.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(2, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(3, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(4, 32))}));

  // The inner FIR loop has been rolled up into the proc state. So there should
  // be nothing on the send queue until two iterations (the length of the
  // kernel divided by number of unrolls) have completed.
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_TRUE(send_queue.empty());

  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());


  // It should be equal to 1. Confirm.
  std::vector<int> expected_output = {1, 4, 10, 20};
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(expected_output[0], 32))));

  // Now do this three more times and confirm if the output is correct.
  for (int i = 1; i < 4; i++) {
    for (int j = 0; j < 2; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 1) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    EXPECT_THAT(send_queue.Dequeue(),
                IsOkAndHolds(Value(UBits(expected_output[i], 32))));
  }
}

// Perform a full unrolling of the FIR filter, so this pass basically does
// nothing.
TEST_F(RollIntoProcPassTest, ImportFIRUnrollAll) {
  // Build FIR proc.
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Value kernel_value,
                           Value::UBitsArray({1, 2, 3, 4}, 32));

  absl::string_view name = "fir_proc";
  Type* kernel_type = p->GetTypeForValue(kernel_value.element(0));

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* x_in,
                           p->CreateStreamingChannel(
                           absl::StrFormat("%s_x_in", name),
                           ChannelOps::kReceiveOnly, kernel_type));

  XLS_ASSERT_OK_AND_ASSIGN(StreamingChannel* filter_out,
                           p->CreateStreamingChannel(
                           absl::StrFormat("%s_out", name),
                           ChannelOps::kSendOnly, kernel_type));

  XLS_ASSERT_OK_AND_ASSIGN(Proc* f, CreateFirFilter(name,
                                                    kernel_value,
                                                    x_in,
                                                    filter_out,
                                                    p.get()));

  // Run roll_into_proc_pass (+DCE).
  EXPECT_THAT(Run(f, 4), status_testing::IsOkAndHolds(true));

  // Check if the transformed proc still works as an FIR filter.
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ChannelQueueManager> queue_manager,
      ChannelQueueManager::Create(/*user_defined_queues=*/{}, p.get()));
  ProcInterpreter pi(f, queue_manager.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel* send,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_out", name)));
  XLS_ASSERT_OK_AND_ASSIGN(Channel* recv,
                           p.get()->GetChannel(
                               absl::StrFormat("%s_x_in", name)));

  ChannelQueue& send_queue = queue_manager->GetQueue(send);
  ChannelQueue& recv_queue = queue_manager->GetQueue(recv);

  ASSERT_TRUE(send_queue.empty());
  ASSERT_TRUE(recv_queue.empty());

  // Enqueue 4 elements.
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(1, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(2, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(3, 32))}));
  XLS_ASSERT_OK(recv_queue.Enqueue({Value(UBits(4, 32))}));

  // This got fully unrolled so it should have something to send out on every
  // iteration
  ASSERT_THAT(
      pi.RunIterationUntilCompleteOrBlocked(),
      IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                              .progress_made = true,
                                              .blocked_channels = {}}));
  EXPECT_TRUE(pi.IsIterationComplete());
  EXPECT_FALSE(send_queue.empty());

  // It should be equal to 1. Confirm.
  std::vector<int> expected_output = {1, 4, 10, 20};
  EXPECT_THAT(send_queue.Dequeue(),
              IsOkAndHolds(Value(UBits(expected_output[0], 32))));

  // Now do this three more times and confirm if the output is correct.
  for (int i = 1; i < 4; i++) {
    for (int j = 0; j < 1; j++) {
      ASSERT_THAT(
          pi.RunIterationUntilCompleteOrBlocked(),
          IsOkAndHolds(ProcInterpreter::RunResult{.iteration_complete = true,
                                                  .progress_made = true,
                                                  .blocked_channels = {}}));
      EXPECT_TRUE(pi.IsIterationComplete());
      if (j < 0) {
        EXPECT_TRUE(send_queue.empty());
      } else {
        EXPECT_FALSE(send_queue.empty());
      }
    }
    EXPECT_THAT(send_queue.Dequeue(),
                IsOkAndHolds(Value(UBits(expected_output[i], 32))));
  }
}


}  // namespace

}  // namespace xls
