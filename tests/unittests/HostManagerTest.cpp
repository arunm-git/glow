/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/Runtime/HostManager/HostManager.h"
#include "glow/Backends/ExecutionContext.h"

#include "gtest/gtest.h"

#include <future>
#include <thread>

using namespace glow;
using namespace glow::runtime;
using DAGNodePairTy = std::pair<std::vector<std::unique_ptr<DAGNode>>,
                                std::vector<std::unique_ptr<DAGNode>>>;

class HostManagerTest : public ::testing::TestWithParam<BackendKind> {
public:
  void SetUp() override { backendKind = GetParam(); }
  BackendKind backendKind;
};

std::unique_ptr<Module> setupModule(unsigned functionCount) {
  std::unique_ptr<Module> module = llvm::make_unique<Module>();
  for (unsigned int i = 0; i < functionCount; i++) {
    Function *F = module->createFunction("function" + std::to_string(i));
    auto *X = module->createPlaceholder(ElemKind::FloatTy, {3},
                                        "X" + std::to_string(i), false);
    auto *tanh = F->createTanh("tanh" + std::to_string(i), X);
    F->createSave("save" + std::to_string(i), tanh);
  }
  return module;
}

std::unique_ptr<HostManager> createHostManager(BackendKind kind) {
  std::vector<std::unique_ptr<DeviceConfig>> configs;
  auto config = llvm::make_unique<DeviceConfig>(kind);
  configs.push_back(std::move(config));
  std::unique_ptr<HostManager> hostManager =
      llvm::make_unique<HostManager>(std::move(configs));
  return hostManager;
}

void addAndRemoveNetwork(HostManager *manager, unsigned int functionNumber) {
  std::unique_ptr<Module> module = llvm::make_unique<Module>();
  Function *F =
      module->createFunction("function" + std::to_string(functionNumber));
  auto *X = module->createPlaceholder(
      ElemKind::FloatTy, {3}, "X" + std::to_string(functionNumber), false);
  auto *tanh = F->createTanh("Tanh" + std::to_string(functionNumber), X);
  F->createSave("save" + std::to_string(functionNumber), tanh);

  // Expect this to be an Error because multiple networks with the same name
  // have been added to HostManager
  errToBool(manager->addNetwork(std::move(module)));
  manager->removeNetwork("function" + std::to_string(functionNumber));
}

TEST_P(HostManagerTest, newHostManager) { createHostManager(backendKind); }

TEST_P(HostManagerTest, addNetwork) {
  auto module = setupModule(6);
  auto hostManager = createHostManager(backendKind);
  ASSERT_FALSE(errToBool(hostManager->addNetwork(std::move(module))));
}

TEST_P(HostManagerTest, runNetwork) {
  std::unique_ptr<Module> module = llvm::make_unique<Module>();
  std::unique_ptr<ExecutionContext> context =
      llvm::make_unique<ExecutionContext>();

  Function *F = module->createFunction("main");
  auto *X = module->createPlaceholder(ElemKind::FloatTy, {3}, "X", false);
  auto *XTensor = context->getPlaceholderBindings()->allocate(X);
  XTensor->getHandle() = {1., 2., 3.};
  auto *tanh = F->createTanh("Tanh1", X);
  auto *save = F->createSave("save", tanh);
  auto *saveTensor =
      context->getPlaceholderBindings()->allocate(save->getPlaceholder());

  auto hostManager = createHostManager(backendKind);
  ASSERT_FALSE(errToBool(hostManager->addNetwork(std::move(module))));

  std::promise<void> runNetwork;
  auto ready = runNetwork.get_future();

  llvm::Error runErr = llvm::Error::success();

  hostManager->runNetwork("main", std::move(context),
                          [&runNetwork, &saveTensor, &context, &runErr](
                              RunIdentifierTy runID, llvm::Error err,
                              std::unique_ptr<ExecutionContext> context_) {
                            auto HX = saveTensor->getHandle();
                            EXPECT_NEAR(HX.at({0}), std::tanh(1), 1E-5);
                            EXPECT_NEAR(HX.at({1}), std::tanh(2), 1E-5);
                            EXPECT_NEAR(HX.at({2}), std::tanh(3), 1E-5);
                            context = std::move(context_);
                            runErr = std::move(err);
                            runNetwork.set_value();
                          });

  ready.wait();
  EXPECT_FALSE(errToBool(std::move(runErr)));

  // reset runErr
  runErr = llvm::Error::success();

  std::promise<void> newRun;
  ready = newRun.get_future();
  hostManager->runNetwork("main", std::move(context),
                          [&newRun, &saveTensor, &runErr](
                              RunIdentifierTy runID, llvm::Error err,
                              std::unique_ptr<ExecutionContext> context_) {
                            auto HX = saveTensor->getHandle();
                            EXPECT_NEAR(HX.at({0}), std::tanh(1), 1E-5);
                            EXPECT_NEAR(HX.at({1}), std::tanh(2), 1E-5);
                            EXPECT_NEAR(HX.at({2}), std::tanh(3), 1E-5);
                            runErr = std::move(err);
                            newRun.set_value();
                          });

  ready.wait();
  EXPECT_FALSE(errToBool(std::move(runErr)));
}

/// Test that HostManager properly handles concurrent add/remove requests with
/// unique network names.
TEST_P(HostManagerTest, ConcurrentAddRemoveUnique) {
  constexpr auto numThreads = 6;
  constexpr auto numItersPerThread = 20;
  auto hostManager = createHostManager(backendKind);
  std::atomic<unsigned> counter{0};
  std::vector<std::thread> threads;
  for (auto i = 0; i < numThreads; ++i) {
    threads.emplace_back([&]() {
      for (auto j = 0; j < numItersPerThread; ++j) {
        addAndRemoveNetwork(hostManager.get(), ++counter);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }
}

/// Test that HostManager properly handles concurrent add/remove requests with a
/// duplicate network name.
TEST_P(HostManagerTest, ConcurrentAddRemoveDuplicate) {
  constexpr auto numThreads = 6;
  constexpr auto numItersPerThread = 20;
  auto hostManager = createHostManager(backendKind);
  std::vector<std::thread> threads;
  for (auto i = 0; i < numThreads; ++i) {
    threads.emplace_back([&]() {
      for (auto j = 0; j < numItersPerThread; ++j) {
        addAndRemoveNetwork(hostManager.get(), 0);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }
}

INSTANTIATE_TEST_CASE_P(Interpreter, HostManagerTest,
                        ::testing::Values(BackendKind::Interpreter));

#ifdef GLOW_WITH_CPU
INSTANTIATE_TEST_CASE_P(CPU, HostManagerTest,
                        ::testing::Values(BackendKind::CPU));
#endif // GLOW_WITH_CPU

#ifdef GLOW_WITH_OPENCL
INSTANTIATE_TEST_CASE_P(OpenCL, HostManagerTest,
                        ::testing::Values(BackendKind::OpenCL));
#endif // GLOW_WITH_OPENCL

#ifdef GLOW_WITH_HABANA
INSTANTIATE_TEST_CASE_P(Habana, HostManagerTest,
                        ::testing::Values(BackendKind::Habana));
#endif // GLOW_WITH_HABANA
