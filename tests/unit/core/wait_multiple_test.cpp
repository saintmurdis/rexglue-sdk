/**
 * @file        wait_multiple_test.cpp
 * @brief       Unit tests for rex::thread::WaitAny / WaitAll on several handles
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::WaitAll;
using rex::thread::WaitAny;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

TEST_CASE("rex::thread::WaitAny - wakes on a signal from another thread", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  auto sem = Semaphore::Create(0, 4);
  WaitHandle* handles[] = {a.get(), b.get(), sem.get()};

  std::thread signaler([&] {
    std::this_thread::sleep_for(50ms);
    b->Set();
  });
  auto start = std::chrono::steady_clock::now();
  auto [result, index] = WaitAny(handles, 3, false, 5000ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  signaler.join();

  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
  REQUIRE(elapsed >= 40ms);
  REQUIRE(elapsed < 1000ms);

  // Semaphore release wakes it too.
  std::thread releaser([&] {
    std::this_thread::sleep_for(50ms);
    sem->Release(1, nullptr);
  });
  auto [result2, index2] = WaitAny(handles, 3, false, 5000ms);
  releaser.join();
  REQUIRE(result2 == WaitResult::kSuccess);
  REQUIRE(index2 == 2);
}

TEST_CASE("rex::thread::WaitAny - times out when nothing is signaled", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  auto start = std::chrono::steady_clock::now();
  auto [result, index] = WaitAny(handles, 2, false, 100ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  (void)index;

  REQUIRE(result == WaitResult::kTimeout);
  REQUIRE(elapsed >= 95ms);
  REQUIRE(elapsed < 1000ms);
}

TEST_CASE("rex::thread::WaitAny - returns at once for a handle already signaled", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(true);
  WaitHandle* handles[] = {a.get(), b.get()};

  auto [result, index] = WaitAny(handles, 2, false, 5000ms);
  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(index == 1);
}

TEST_CASE("rex::thread::WaitAll - waits for every handle", "[thread][wait]") {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  std::thread signaler([&] {
    std::this_thread::sleep_for(30ms);
    a->Set();
    std::this_thread::sleep_for(60ms);
    b->Set();
  });
  auto start = std::chrono::steady_clock::now();
  auto result = WaitAll(handles, 2, false, 5000ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  signaler.join();

  REQUIRE(result == WaitResult::kSuccess);
  REQUIRE(elapsed >= 80ms);  // not satisfied by the first event alone
  REQUIRE(elapsed < 1000ms);
}

TEST_CASE("rex::thread::WaitAll - times out while one handle stays unsignaled", "[thread][wait]") {
  auto a = Event::CreateManualResetEvent(true);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  REQUIRE(WaitAll(handles, 2, false, 50ms) == WaitResult::kTimeout);
}

TEST_CASE("rex::thread::WaitAny - ping-pong between two threads does not stall", "[thread][wait]") {
  // Each side waits on two events (its own and a never-signaled one) with no timeout and
  // signals the other. A lost wakeup would cost the full poll cap per round trip.
  constexpr int kRounds = 2000;
  auto ping = Event::CreateAutoResetEvent(false);
  auto pong = Event::CreateAutoResetEvent(false);
  auto never_a = Event::CreateAutoResetEvent(false);
  auto never_b = Event::CreateAutoResetEvent(false);

  std::atomic<int> bad{0};  // Catch assertions are not thread-safe: count here, check after join
  std::thread other([&] {
    WaitHandle* handles[] = {never_b.get(), ping.get()};
    for (int i = 0; i < kRounds; ++i) {
      auto [result, index] = WaitAny(handles, 2, false);
      if (result != WaitResult::kSuccess || index != 1) {
        ++bad;
      }
      pong->Set();
    }
  });

  WaitHandle* handles[] = {pong.get(), never_a.get()};
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kRounds; ++i) {
    ping->Set();
    auto [result, index] = WaitAny(handles, 2, false);
    REQUIRE(result == WaitResult::kSuccess);
    REQUIRE(index == 0);
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  other.join();

  REQUIRE(bad == 0);
  REQUIRE(elapsed < 5s);
}
