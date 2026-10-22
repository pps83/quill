#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/core/MathUtilities.h"
#include "quill/core/ThreadContextManager.h"
#include "quill/sinks/Sink.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Counts the busy thread's lines and the ended threads' lines apart
 */
class CountingSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&,
                 std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view log_message,
                 std::string_view) override
  {
    if (log_message.rfind("busy", 0) == 0)
    {
      busy_lines.fetch_add(1);
    }
    else
    {
      ended_lines.fetch_add(1);
    }
  }

  void flush_sink() override {}

  std::atomic<size_t> busy_lines{0};
  std::atomic<size_t> ended_lines{0};
};

/**
 * Keeps a backlog the backend never finishes, so it never takes its idle path
 */
void keep_backend_busy(Logger* logger, CountingSink const* sink, std::atomic<bool> const* stop)
{
  size_t logged{0};
  while (!stop->load())
  {
    if (logged - sink->busy_lines.load() < 10000)
    {
      LOG_INFO(logger, "busy line {}", logged);
      ++logged;
    }
    else
    {
      std::this_thread::yield();
    }
  }
}

/***/
void log_and_end(Logger* logger, size_t count, size_t* capacity)
{
  for (size_t i = 0; i < count; ++i)
  {
    LOG_INFO(logger, "ended thread line {}", i);
  }
  detail::ThreadContext* thread_context = detail::FrontendThreadState::thread_context;
  *capacity = thread_context->get_spsc_queue_union().unbounded_spsc_queue.producer_capacity();
}

/***/
bool wait_for_reclaimed_contexts(std::chrono::seconds timeout)
{
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  while (detail::ThreadContextManager::instance().has_invalid_thread_context())
  {
    if (std::chrono::steady_clock::now() >= deadline)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

/***/
TEST_CASE("ended_thread_contexts_are_reclaimed_while_another_thread_keeps_the_backend_busy")
{
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  auto sink = Frontend::create_or_get_sink<CountingSink>("counting_sink");
  auto& counting_sink = static_cast<CountingSink&>(*sink);
  Logger* logger = Frontend::create_or_get_logger("logger", sink);

  std::atomic<bool> stop{false};
  std::thread busy{keep_backend_busy, logger, &counting_sink, &stop};

  // Every other thread logs enough to grow its queue past the initial capacity
  size_t constexpr thread_count{8};
  size_t constexpr grown_lines{20000};
  size_t constexpr few_lines{10};
  size_t const initial_capacity = detail::next_power_of_two(FrontendOptions::initial_queue_capacity);
  for (size_t i = 0; i < thread_count; ++i)
  {
    size_t capacity{0};
    std::thread{log_and_end, logger, (i % 2 == 0) ? grown_lines : few_lines, &capacity}.join();
    if (i % 2 == 0)
    {
      REQUIRE_GT(capacity, initial_capacity);
    }
  }

  bool const reclaimed = wait_for_reclaimed_contexts(std::chrono::seconds{10});
  stop.store(true);
  busy.join();
  REQUIRE(reclaimed);

  logger->flush_log();
  REQUIRE_EQ(counting_sink.ended_lines.load(), thread_count / 2 * (grown_lines + few_lines));

  Frontend::remove_logger(logger);
  Backend::stop();
}
