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

#include "xls/jit/proc_jit.h"

#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/subprocess.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/jit/jit_channel_queue.h"
#include "xls/jit/jit_runtime.h"
#include "xls/jit/llvm_type_converter.h"

namespace xls {
namespace {

void EnqueueData(JitChannelQueue* queue, uint32_t data) {
  queue->Send(absl::bit_cast<uint8_t*>(&data), sizeof(uint32_t));
}

uint32_t DequeueData(JitChannelQueue* queue) {
  uint32_t data;
  queue->Recv(absl::bit_cast<uint8_t*>(&data), sizeof(uint32_t));
  return data;
}

class ProcJitTest : public IrTestBase {};

// Recv/Send functions for the "CanCompileProcs" test.
void CanCompileProcs_recv(JitChannelQueue* queue_ptr, Receive* recv_ptr,
                          uint8_t* data_ptr, int64_t data_sz, void* user_data) {
  JitChannelQueue* queue = absl::bit_cast<JitChannelQueue*>(queue_ptr);
  queue->Recv(data_ptr, data_sz);
}

void CanCompileProcs_send(JitChannelQueue* queue_ptr, Send* send_ptr,
                          uint8_t* data_ptr, int64_t data_sz, void* user_data) {
  JitChannelQueue* queue = absl::bit_cast<JitChannelQueue*>(queue_ptr);
  queue->Send(data_ptr, data_sz);
}

// Simple smoke-style test that the ProcBuilderVisitor can compile Procs!
TEST_F(ProcJitTest, CanCompileProcs) {
  const std::string kIrText = R"(
package p

chan c_i(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=none, metadata="")
chan c_o(bits[32], id=1, kind=streaming, ops=send_only, flow_control=none, metadata="")

proc the_proc(my_token: token, state: (), init={()}) {
  literal.1: bits[32] = literal(value=3)
  receive.2: (token, bits[32]) = receive(my_token, channel_id=0)
  tuple_index.3: token = tuple_index(receive.2, index=0)
  tuple_index.4: bits[32] = tuple_index(receive.2, index=1)
  umul.5: bits[32] = umul(literal.1, tuple_index.4)
  send.6: token = send(tuple_index.3, umul.5, channel_id=1)
  next (send.6, state)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           ParsePackage(kIrText));
  XLS_ASSERT_OK_AND_ASSIGN(auto queue_mgr,
                           JitChannelQueueManager::Create(package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProcJit> jit,
      ProcJit::Create(FindProc("the_proc", package.get()), queue_mgr.get(),
                      CanCompileProcs_recv, CanCompileProcs_send));

  {
    EnqueueData(queue_mgr->GetQueueById(0).value(), 7);
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run({Value::Tuple({})}));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), 21);
  }

  // Let's make sure we can call it 2x!
  {
    EnqueueData(queue_mgr->GetQueueById(0).value(), 7);
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run({Value::Tuple({})}));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), 21);
  }
}

TEST_F(ProcJitTest, RecvIf) {
  const std::string kIrText = R"(
package p

chan c_i(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=none,
metadata="") chan c_o(bits[32], id=1, kind=streaming, ops=send_only,
flow_control=none, metadata="")

proc the_proc(my_token: token, state: bits[1], init={0}) {
  receive.2: (token, bits[32]) = receive(my_token, predicate=state,
  channel_id=0) tuple_index.3: token = tuple_index(receive.2, index=0)
  tuple_index.4: bits[32] = tuple_index(receive.2, index=1)
  send.5: token = send(tuple_index.3, tuple_index.4, channel_id=1)
  next (send.5, state)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           ParsePackage(kIrText));

  constexpr uint32_t kQueueData = 0xbeef;
  XLS_ASSERT_OK_AND_ASSIGN(auto queue_mgr,
                           JitChannelQueueManager::Create(package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProcJit> jit,
      ProcJit::Create(FindProc("the_proc", package.get()), queue_mgr.get(),
                      CanCompileProcs_recv, CanCompileProcs_send));

  EnqueueData(queue_mgr->GetQueueById(0).value(), kQueueData);

  {
    // First: set state to 0; see that recv_if returns 0.
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run({Value(UBits(0, 1))}));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), 0);
  }

  {
    // First: set state to 0; see that recv_if returns 0.
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run({Value(UBits(1, 1))}));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), kQueueData);
  }
}

TEST_F(ProcJitTest, ConditionalSend) {
  const std::string kIrText = R"(
package p

chan c_i(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=none,
metadata="") chan c_o(bits[32], id=1, kind=streaming, ops=send_only,
flow_control=none, metadata="")

proc the_proc(my_token: token, state: bits[1], init={0}) {
  receive.2: (token, bits[32]) = receive(my_token, channel_id=0)
  tuple_index.3: token = tuple_index(receive.2, index=0)
  tuple_index.4: bits[32] = tuple_index(receive.2, index=1)
  send.5: token = send(tuple_index.3, tuple_index.4, predicate=state,
  channel_id=1) next (send.5, state)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           ParsePackage(kIrText));

  constexpr uint32_t kQueueData = 0xbeef;
  XLS_ASSERT_OK_AND_ASSIGN(auto queue_mgr,
                           JitChannelQueueManager::Create(package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProcJit> jit,
      ProcJit::Create(FindProc("the_proc", package.get()), queue_mgr.get(),
                      CanCompileProcs_recv, CanCompileProcs_send));

  EnqueueData(queue_mgr->GetQueueById(0).value(), kQueueData);
  EnqueueData(queue_mgr->GetQueueById(0).value(), kQueueData + 1);

  {
    // First: with state 0, make sure no send occurred (i.e., our output queue
    // is empty).
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run({Value(UBits(0, 1))}));
    EXPECT_TRUE(queue_mgr->GetQueueById(1).value()->Empty());
  }

  {
    // Second: with state 1, make sure we've now got output data.
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> result,
                             jit->Run({Value(UBits(1, 1))}));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), kQueueData + 1);
  }
}

// Recv/Send functions for the "GetsUserData" test.
void GetsUserData_recv(JitChannelQueue* queue_ptr, Receive* recv_ptr,
                       uint8_t* data_ptr, int64_t data_sz, void* user_data) {
  JitChannelQueue* queue = absl::bit_cast<JitChannelQueue*>(queue_ptr);
  uint64_t* int_data = absl::bit_cast<uint64_t*>(user_data);
  *int_data = *int_data * 2;
  queue->Recv(data_ptr, data_sz);
}

void GetsUserData_send(JitChannelQueue* queue_ptr, Send* send_ptr,
                       uint8_t* data_ptr, int64_t data_sz, void* user_data) {
  JitChannelQueue* queue = absl::bit_cast<JitChannelQueue*>(queue_ptr);
  uint64_t* int_data = absl::bit_cast<uint64_t*>(user_data);
  *int_data = *int_data * 3;
  queue->Send(data_ptr, data_sz);
}

// Verifies that the "user data" pointer is properly passed into proc callbacks.
TEST_F(ProcJitTest, GetsUserData) {
  const std::string kIrText = R"(
package p

chan c_i(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=none,
metadata="") chan c_o(bits[32], id=1, kind=streaming, ops=send_only,
flow_control=none, metadata="")

proc the_proc(my_token: token, state: (), init={()}) {
  literal.1: bits[32] = literal(value=3)
  receive.2: (token, bits[32]) = receive(my_token, channel_id=0)
  tuple_index.3: token = tuple_index(receive.2, index=0)
  tuple_index.4: bits[32] = tuple_index(receive.2, index=1)
  umul.5: bits[32] = umul(literal.1, tuple_index.4)
  send.6: token = send(tuple_index.3, umul.5, channel_id=1)
  next (send.6, state)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           ParsePackage(kIrText));

  XLS_ASSERT_OK_AND_ASSIGN(auto queue_mgr,
                           JitChannelQueueManager::Create(package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProcJit> jit,
      ProcJit::Create(FindProc("the_proc", package.get()), queue_mgr.get(),
                      GetsUserData_recv, GetsUserData_send));

  EnqueueData(queue_mgr->GetQueueById(0).value(), 7);

  {
    uint64_t user_data = 7;
    XLS_ASSERT_OK_AND_ASSIGN(
        InterpreterResult<Value> result,
        jit->Run({Value::Tuple({})}, absl::bit_cast<void*>(&user_data)));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), 21);
    EXPECT_EQ(user_data, 7 * 2 * 3);
  }

  {
    // Let's make sure we can call it 2x!
    uint64_t user_data = 7;
    EnqueueData(queue_mgr->GetQueueById(0).value(), 7);
    XLS_ASSERT_OK_AND_ASSIGN(
        InterpreterResult<Value> result,
        jit->Run({Value::Tuple({})}, absl::bit_cast<void*>(&user_data)));
    EXPECT_EQ(DequeueData(queue_mgr->GetQueueById(1).value()), 21);
    EXPECT_EQ(user_data, 7 * 2 * 3);
  }
}

TEST_F(ProcJitTest, SingleValueChannel) {
  const std::string kIrText = R"(
package p

chan c_sv(bits[32], id=0, kind=single_value, ops=receive_only, metadata="")
chan c_i(bits[32], id=1, kind=streaming, ops=receive_only, flow_control=none,
metadata="") chan c_o(bits[32], id=2, kind=streaming, ops=send_only,
flow_control=none, metadata="")

proc the_proc(my_token: token, state: (), init={()}) {
  recv_sv: (token, bits[32]) = receive(my_token, channel_id=0)
  tkn0: token = tuple_index(recv_sv, index=0)
  single_value: bits[32] = tuple_index(recv_sv, index=1)

  recv_streaming: (token, bits[32]) = receive(tkn0, channel_id=1)
  tkn1: token = tuple_index(recv_streaming, index=0)
  streaming_value: bits[32] = tuple_index(recv_streaming, index=1)

  sum: bits[32] = add(single_value, streaming_value)
  tkn2: token = send(tkn1, sum, channel_id=2)
  next (tkn2, state)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           ParsePackage(kIrText));

  XLS_ASSERT_OK_AND_ASSIGN(auto queue_mgr,
                           JitChannelQueueManager::Create(package.get()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProcJit> jit,
      ProcJit::Create(FindProc("the_proc", package.get()), queue_mgr.get(),
                      CanCompileProcs_recv, CanCompileProcs_send));

  XLS_ASSERT_OK_AND_ASSIGN(JitChannelQueue * single_value_input,
                           queue_mgr->GetQueueById(0));
  XLS_ASSERT_OK_AND_ASSIGN(JitChannelQueue * streaming_input,
                           queue_mgr->GetQueueById(1));
  XLS_ASSERT_OK_AND_ASSIGN(JitChannelQueue * streaming_output,
                           queue_mgr->GetQueueById(2));

  EnqueueData(single_value_input, 7);
  EnqueueData(streaming_input, 42);
  EnqueueData(streaming_input, 123);

  auto tick = [&]() { XLS_EXPECT_OK(jit->Run({Value::Tuple({})}).status()); };

  tick();
  tick();
  EXPECT_EQ(DequeueData(streaming_output), 49);
  EXPECT_EQ(DequeueData(streaming_output), 130);

  EnqueueData(single_value_input, 10);
  EnqueueData(streaming_input, 42);
  EnqueueData(streaming_input, 123);

  tick();
  tick();
  EXPECT_EQ(DequeueData(streaming_output), 52);
  EXPECT_EQ(DequeueData(streaming_output), 133);
}

}  // namespace
}  // namespace xls
