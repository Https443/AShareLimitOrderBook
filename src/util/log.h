#pragma once

#include <stdio.h>
#include <cstdio>
#include <string>
#include <string_view>

#ifndef EOF
#define APP_LOG_DEFINED_EOF_FOR_QUILL 1
#define EOF (-1)
#endif

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/HelperMacros.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/core/LogLevel.h"

#ifdef APP_LOG_DEFINED_EOF_FOR_QUILL
#undef EOF
#undef APP_LOG_DEFINED_EOF_FOR_QUILL
#endif

#include <ql/time/date.hpp>

QUILL_LOGGABLE_DEFERRED_FORMAT(QuantLib::Date)

namespace app_log {

bool init(const std::string& file_path, quill::LogLevel min_level);
void shutdown();
void preallocate();
quill::Logger* logger() noexcept;
const std::string& last_error() noexcept;
quill::LogLevel parse_log_level(const std::string& text);
std::string_view log_level_name(quill::LogLevel level) noexcept;

}  // namespace app_log
