#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/core/ThreadContextManager.h"
#include "quill/sinks/Sink.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Keeps every message with the id of the thread that logged it
 */
class RecordingSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t, std::string_view thread_id, std::string_view,
                 std::string const&, std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view message,
                 std::string_view) override
  {
    std::lock_guard<std::mutex> const lock{mutex};
    lines.emplace_back(std::string{thread_id}, std::string{message});
  }

  void flush_sink() override {}

  std::mutex mutex;
  std::vector<std::pair<std::string, std::string>> lines;
};

static Logger* test_logger{nullptr};
static std::atomic<int> hook_calls{0};

/***/
void first_log_hook()
{
  hook_calls.fetch_add(1);
  LOG_INFO(test_logger, "hook");
}

/***/
void log_twice()
{
  LOG_INFO(test_logger, "first");
  LOG_INFO(test_logger, "second");
}

/***/
TEST_CASE("first_log_hook_runs_once_per_thread_ahead_of_its_first_message")
{
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  auto sink = Frontend::create_or_get_sink<RecordingSink>("recording_sink");
  auto& recording_sink = static_cast<RecordingSink&>(*sink);
  test_logger = Frontend::create_or_get_logger("logger", std::move(sink));
  detail::FrontendThreadState::on_first_log.store(first_log_hook);

  std::thread{log_twice}.join();
  std::thread{log_twice}.join();
  REQUIRE(Backend::flush_all(std::chrono::seconds{10}));
  detail::FrontendThreadState::on_first_log.store(nullptr);

  REQUIRE_EQ(hook_calls.load(), 2);
  std::vector<std::pair<std::string, std::string>> lines;
  {
    std::lock_guard<std::mutex> const lock{recording_sink.mutex};
    lines = recording_sink.lines;
  }
  REQUIRE_EQ(lines.size(), size_t{6});
  for (size_t thread = 0; thread < 2; ++thread)
  {
    std::string const& id = lines[thread * 3].first;
    REQUIRE_EQ(lines[thread * 3].second, "hook");
    REQUIRE_EQ(lines[thread * 3 + 1], std::make_pair(id, std::string{"first"}));
    REQUIRE_EQ(lines[thread * 3 + 2], std::make_pair(id, std::string{"second"}));
  }

  Frontend::remove_logger(test_logger);
  Backend::stop();
}
