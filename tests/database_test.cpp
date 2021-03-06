/*
 *   Copyright (C) 2020 Nippon Telegraph and Telephone Corporation.

 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at

 *   http://www.apache.org/licenses/LICENSE-2.0

 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <lineairdb/config.h>
#include <lineairdb/database.h>
#include <lineairdb/transaction.h>
#include <lineairdb/tx_status.h>

#include <atomic>
#include <chrono>
#include <experimental/filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

typedef std::function<void(LineairDB::Transaction&)> TransactionProcedure;
class DatabaseTest : public ::testing::Test {
 protected:
  LineairDB::Config config_;
  std::unique_ptr<LineairDB::Database> db_;
  virtual void SetUp() {
    std::experimental::filesystem::remove_all("lineairdb_logs");
    config_.max_thread = 4;
    db_                = std::make_unique<LineairDB::Database>(config_);
  }

  void DoTransactions(const std::vector<TransactionProcedure> txns) {
    std::atomic<size_t> terminated(0);
    for (auto& tx : txns) {
      db_->ExecuteTransaction(tx,
                              [&](const LineairDB::TxStatus) { terminated++; });
      db_->Fence();
    }

    size_t msec_elapsed_for_termination = 0;
    while (terminated != txns.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      msec_elapsed_for_termination++;
      bool too_long_time_elapsed = (db_->GetConfig().epoch_duration_ms * 1000) <
                                   msec_elapsed_for_termination;
      EXPECT_FALSE(too_long_time_elapsed);
      if (too_long_time_elapsed) break;
    }
  }

  size_t DoTransactionsOnMultiThreads(
      const std::vector<TransactionProcedure> txns) {
    std::atomic<size_t> terminated(0);
    std::atomic<size_t> committed(0);
    std::vector<std::thread> threads;
    for (auto& tx : txns) {
      threads.push_back(std::thread([&]() {
        db_->ExecuteTransaction(tx, [&](const LineairDB::TxStatus status) {
          terminated++;
          if (status == LineairDB::TxStatus::Committed) { committed++; }
        });
      }));
    }

    size_t msec_elapsed_for_termination = 0;
    while (terminated != txns.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      msec_elapsed_for_termination++;
      bool too_long_time_elapsed = (db_->GetConfig().epoch_duration_ms * 1000) <
                                   msec_elapsed_for_termination;
      EXPECT_FALSE(too_long_time_elapsed);
      if (too_long_time_elapsed) break;
    }

    for (auto& thread : threads) { thread.join(); }
    return committed;
  }
};

TEST_F(DatabaseTest, Instantiate) {}
TEST_F(DatabaseTest, InstantiateWithConfig) {
  db_.reset(nullptr);
  LineairDB::Config conf(1);
  ASSERT_NO_THROW(db_ = std::make_unique<LineairDB::Database>(conf));
}

TEST_F(DatabaseTest, ExecuteTransaction) {
  int value_of_alice = 1;
  DoTransactions(
      {[&](LineairDB::Transaction& tx) {
         tx.Write("alice", reinterpret_cast<std::byte*>(&value_of_alice),
                  sizeof(int));
       },
       [&](LineairDB::Transaction& tx) {
         auto alice = tx.Read("alice");
         ASSERT_NE(alice.first, nullptr);
         ASSERT_EQ(value_of_alice, *reinterpret_cast<const int*>(alice.first));
         ASSERT_EQ(0, tx.Read("bob").second);
       }});
}

TEST_F(DatabaseTest, ExecuteTransactionWithTemplates) {
  int value_of_alice = 1;
  DoTransactions({[&](LineairDB::Transaction& tx) {
                    tx.Write<int>("alice", value_of_alice);
                  },
                  [&](LineairDB::Transaction& tx) {
                    auto alice = tx.Read<int>("alice");
                    ASSERT_EQ(value_of_alice, alice.value());
                    ASSERT_FALSE(tx.Read<int>("bob").has_value());
                  }});
}

TEST_F(DatabaseTest, SaveAsString) {
  DoTransactions({[&](LineairDB::Transaction& tx) {
                    tx.Write<std::string_view>("alice", "value");
                  },
                  [&](LineairDB::Transaction& tx) {
                    auto alice = tx.Read<std::string_view>("alice");
                    ASSERT_EQ("value", alice.value());
                  }});
}

TEST_F(DatabaseTest, UserAbort) {
  DoTransactions({[&](LineairDB::Transaction& tx) {
                    int value_of_alice = 1;
                    tx.Write<int>("alice", value_of_alice);
                    tx.Abort();
                  },
                  [&](LineairDB::Transaction& tx) {
                    auto alice = tx.Read<int>("alice");
                    ASSERT_FALSE(alice.has_value());  // Opacity
                    tx.Abort();
                  }});
}

TEST_F(DatabaseTest, ReadYourOwnWrites) {
  int value_of_alice = 1;
  DoTransactions({[&](LineairDB::Transaction& tx) {
    tx.Write<int>("alice", value_of_alice);
    auto alice = tx.Read<int>("alice");
    ASSERT_EQ(value_of_alice, alice.value());
  }});
}

TEST_F(DatabaseTest, ThreadSafetyInsertions) {
  TransactionProcedure insertTenTimes([](LineairDB::Transaction& tx) {
    int value = 0xBEEF;
    for (size_t idx = 0; idx <= 10; idx++) {
      tx.Write<int>("alice" + std::to_string(idx), value);
    }
  });

  ASSERT_NO_THROW({
    DoTransactionsOnMultiThreads(
        {insertTenTimes, insertTenTimes, insertTenTimes, insertTenTimes});
  });
  db_->Fence();

  DoTransactions({[](LineairDB::Transaction& tx) {
    for (size_t idx = 0; idx <= 10; idx++) {
      auto alice = tx.Read<int>("alice" + std::to_string(idx));
      ASSERT_TRUE(alice.has_value());
      auto current_value = alice.value();
      ASSERT_EQ(0xBEEF, current_value);
    }
  }});
}

TEST_F(DatabaseTest, Recovery) {
  // We expect LineairDB enables recovery logging by default.
  const LineairDB::Config config = db_->GetConfig();
  ASSERT_TRUE(config.enable_logging);

  int initial_value = 1;
  DoTransactions({[&](LineairDB::Transaction& tx) {
                    tx.Write<int>("alice", initial_value);
                  },
                  [&](LineairDB::Transaction& tx) {
                    tx.Write<int>("bob", initial_value);
                  }});
  db_->Fence();

  db_.reset(nullptr);
  db_ = std::make_unique<LineairDB::Database>(config);

  DoTransactions({[&](LineairDB::Transaction& tx) {
    auto alice = tx.Read<int>("alice");
    ASSERT_TRUE(alice.has_value());
    auto current_value = alice.value();
    ASSERT_EQ(initial_value, current_value);
    auto bob = tx.Read<int>("bob");
    ASSERT_TRUE(bob.has_value());
    current_value = bob.value();
    ASSERT_EQ(initial_value, current_value);
  }});
}
