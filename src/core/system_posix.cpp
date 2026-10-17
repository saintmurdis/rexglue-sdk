/**
 * @file        core/system_posix.cpp
 * @brief       POSIX implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <atomic>
#include <cstdio>

#include <rex/system.h>

namespace rex {

namespace {
std::atomic<SimpleMessageBoxHandler> g_message_box_handler{nullptr};
}  // namespace

void SetSimpleMessageBoxHandler(SimpleMessageBoxHandler handler) {
  g_message_box_handler.store(handler, std::memory_order_release);
}

// stderr always (a terminal or a log collector sees it); a window too when the
// UI layer installed a handler, since an app started from Finder has no stderr.
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case SimpleMessageBoxType::Help:
      level = "INFO";
      break;
    case SimpleMessageBoxType::Warning:
      level = "WARNING";
      break;
    case SimpleMessageBoxType::Error:
      level = "ERROR";
      break;
  }
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
  if (SimpleMessageBoxHandler handler = g_message_box_handler.load(std::memory_order_acquire)) {
    handler(type, message);
  }
}

}  // namespace rex
