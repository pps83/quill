#include "doctest/doctest.h"

#include "misc/TestUtilities.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/sinks/FileSink.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace quill;

static constexpr size_t number_of_messages = 1000;

/***/
void log_messages(Logger* logger, std::string const& thread_name)
{
  for (size_t i = 0; i < number_of_messages; ++i)
  {
    LOG_INFO(logger, "{} message {}", thread_name, i);
  }
}

/**
 * Flushes from a thread that never logged, as a crash handler would
 */
void flush_without_logging(bool* flushed, bool* used_a_queue, std::chrono::nanoseconds* waited)
{
  auto const start = std::chrono::steady_clock::now();
  *flushed = Backend::flush_all(std::chrono::seconds{5});
  *waited = std::chrono::steady_clock::now() - start;
  *used_a_queue = detail::FrontendThreadState::thread_context != nullptr;
}

/***/
TEST_CASE("flush_all_writes_every_thread_queue")
{
  static constexpr char const* filename = "flush_all.log";

  // A long sleep: only flush_all's wake-up can make the backend write before it ends
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  bo.sleep_duration = std::chrono::milliseconds{400};
  Backend::start(bo);

  FileSinkConfig file_sink_config;
  file_sink_config.set_open_mode('w');
  auto file_sink =
    Frontend::create_or_get_sink<FileSink>(filename, file_sink_config, FileEventNotifier{});
  Logger* logger = Frontend::create_or_get_logger("logger", std::move(file_sink));

  std::thread other{log_messages, logger, std::string{"other"}};
  other.join();
  log_messages(logger, "main");

  bool flushed{false};
  bool used_a_queue{true};
  std::chrono::nanoseconds waited{};
  std::thread flusher{flush_without_logging, &flushed, &used_a_queue, &waited};
  flusher.join();

  REQUIRE(flushed);
  REQUIRE_FALSE(used_a_queue);
  REQUIRE(waited < std::chrono::milliseconds{300});

  // Read before stopping: the flush alone must have written every line
  std::vector<std::string> const file_contents = quill::testing::file_contents(filename);
  REQUIRE_EQ(file_contents.size(), 2 * number_of_messages);
  REQUIRE(quill::testing::file_contains(file_contents, "other message 999"));
  REQUIRE(quill::testing::file_contains(file_contents, "main message 999"));

  Frontend::remove_logger(logger);
  Backend::stop();
  REQUIRE_FALSE(Backend::flush_all(std::chrono::seconds{1}));

  testing::remove_file(filename);
}
