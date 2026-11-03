#pragma once
namespace wow::android {
// Writes a short report (signal, last breadcrumb, native backtrace) to path when the
// process dies from a fatal signal or std::terminate; the app shows it on the next start.
void installCrashReport(const char *path);
// Per-thread alternate signal stack (stack overflows); call on threads that run game code.
void crashReportThread();
// Last known activity (copied, at most 255 bytes); cheap enough to call per step.
void crashBreadcrumb(const char *text);
void crashBreadcrumb2(const char *prefix, const char *text);
// Recent diagnostic lines (Lua errors etc.), last few kept and included in the report.
void crashNote(const char *text);
} // namespace wow::android
