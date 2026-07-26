#include "log.h"

#include "ConfigReader.h"

#include "quill/core/PatternFormatterOptions.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <system_error>

namespace app_log {
namespace {

quill::Logger* g_logger = nullptr;
quill::Logger* g_fallback_logger = nullptr;
std::string g_last_error;
std::mutex g_logger_mutex;
constexpr size_t kLogRotationMaxFileSizeBytes = 500ull * 1024ull * 1024ull;

std::filesystem::path config_root_for(const std::filesystem::path& config_path) {
    const std::filesystem::path parent_dir = config_path.parent_path();
    if (!parent_dir.empty() && parent_dir.filename() == "config" &&
        !parent_dir.parent_path().empty()) {
        return parent_dir.parent_path();
    }

    return parent_dir;
}

quill::PatternFormatterOptions make_formatter() {
    return quill::PatternFormatterOptions(
        "%(time) [%(log_level)] [%(thread_id)] [%(short_source_location):%(caller_function)] %(message)",
        "%Y-%m-%d %H:%M:%S.%Qns",
        quill::Timezone::LocalTime,
        true,
        '\n');
}

void start_backend_if_needed() {
    if (quill::Backend::is_running()) {
        return;
    }

    quill::BackendOptions backend_options;
    backend_options.thread_name = "app_log";
    quill::Backend::start(backend_options);
}

quill::Logger* ensure_fallback_logger() noexcept {
    if (g_fallback_logger) {
        return g_fallback_logger;
    }

    try {
        start_backend_if_needed();
        auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("stderr");
        g_fallback_logger = quill::Frontend::create_or_get_logger(
            "hft_stderr", sink, make_formatter(), quill::ClockSourceType::Tsc);
        g_fallback_logger->set_log_level(quill::LogLevel::Info);
        return g_fallback_logger;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace


bool init(const std::string& file_path, quill::LogLevel min_level) {
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_logger) {
        g_logger->set_log_level(min_level);
        g_last_error.clear();
        return true;
    }

    g_last_error.clear();

    std::string file_pn = file_path + "/app.log";

    if (file_pn.empty()) {
        g_last_error = "log file path is empty";
        return false;
    }

    const std::filesystem::path log_path{file_pn};
    if (!log_path.has_filename()) {
        g_last_error = "log file path does not include a file name: " + file_pn;
        return false;
    }

    const std::filesystem::path parent_dir = log_path.parent_path();
    if (!parent_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent_dir, ec);
        if (ec) {
            g_last_error =
                "failed to create log directory '" + parent_dir.string() + "': " + ec.message();
            return false;
        }
    }

    const bool backend_was_running = quill::Backend::is_running();
    try {
        start_backend_if_needed();

        quill::RotatingFileSinkConfig sink_config;
        sink_config.set_open_mode('a');
        sink_config.set_filename_append_option(quill::FilenameAppendOption::StartDateTime);
        sink_config.set_timezone(quill::Timezone::LocalTime);
        sink_config.set_write_buffer_size(1 << 20);
        sink_config.set_rotation_max_file_size(kLogRotationMaxFileSizeBytes);

        auto sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
            file_pn, sink_config);

        g_logger = quill::Frontend::create_or_get_logger(
            "app", sink, make_formatter(), quill::ClockSourceType::Tsc);
        g_logger->set_log_level(min_level);
        quill::Frontend::preallocate();
        return true;
    } catch (const std::exception& ex) {
        if (!backend_was_running && quill::Backend::is_running()) {
            quill::Backend::stop();
        }
        g_logger = nullptr;
        g_last_error = ex.what();
        return false;
    } catch (...) {
        if (!backend_was_running && quill::Backend::is_running()) {
            quill::Backend::stop();
        }
        g_logger = nullptr;
        g_last_error = "unknown error";
        return false;
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_logger && quill::Backend::is_running()) {
        g_logger->flush_log();
    }

    if (g_fallback_logger && quill::Backend::is_running()) {
        g_fallback_logger->flush_log();
    }

    if (quill::Backend::is_running()) {
        quill::Backend::stop();
    }

    g_logger = nullptr;
    g_fallback_logger = nullptr;
}

void preallocate() {
    quill::Frontend::preallocate();
}

quill::Logger* logger() noexcept {
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_logger) {
        return g_logger;
    }

    if (quill::Logger* valid_logger = quill::Frontend::get_valid_logger()) {
        return valid_logger;
    }

    return ensure_fallback_logger();
}

const std::string& last_error() noexcept {
    return g_last_error;
}

quill::LogLevel parse_log_level(const std::string& text) {
    return quill::loglevel_from_string(text);
}

std::string_view log_level_name(quill::LogLevel level) noexcept {
    switch (level) {
    case quill::LogLevel::TraceL3:
        return "TRACE_L3";
    case quill::LogLevel::TraceL2:
        return "TRACE_L2";
    case quill::LogLevel::TraceL1:
        return "TRACE_L1";
    case quill::LogLevel::Debug:
        return "DEBUG";
    case quill::LogLevel::Info:
        return "INFO";
    case quill::LogLevel::Notice:
        return "NOTICE";
    case quill::LogLevel::Warning:
        return "WARNING";
    case quill::LogLevel::Error:
        return "ERROR";
    case quill::LogLevel::Critical:
        return "CRITICAL";
    case quill::LogLevel::Backtrace:
        return "BACKTRACE";
    case quill::LogLevel::None:
        return "NONE";
    }

    return "UNKNOWN";
}

}  // namespace app_log
