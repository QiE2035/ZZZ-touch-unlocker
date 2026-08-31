#pragma once

namespace zzz::log {

enum class Level : int {
    Info = 0,
    Warn = 1,
    Error = 2,
};

void Init(const wchar_t* root_dir);
void Write(Level level, const char* fmt, ...);

}  // namespace zzz::log

#define LOG_INFO(...)  zzz::log::Write(zzz::log::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  zzz::log::Write(zzz::log::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) zzz::log::Write(zzz::log::Level::Error, __VA_ARGS__)
