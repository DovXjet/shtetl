// Application-wide logging (spdlog). Every log line lands in
// $HOME/.shtetl/logs/, one fresh timestamped file per run, so a failed model
// load (or anything else) can be diagnosed after the fact.
#pragma once

#include <spdlog/spdlog.h>

namespace shtetl::log
{

// Creates $HOME/.shtetl/logs (if needed), opens a new timestamped log file
// there, and installs it as spdlog's default logger. Call once, early in
// main(), before any other logging call.
//
// Also reroutes Qt messages (qDebug/qWarning/...) and VSG's logger into
// spdlog; nothing is written to the console by spdlog itself.
void init();

// Redirects fd 1/2 into the log so anything written straight to
// stdout/stderr (e.g. by drivers or C libraries) is logged instead. No-op on
// Windows.
void captureStdStreams();

} // namespace shtetl::log
