#pragma once
// When the tool dies, it says where. FOX's app/CrashHandler, with the product name injected.
//
// The process writes data/<product>-crash.txt before it goes: build, fault code + address, the
// last twenty log lines (the tool's own account of what it was doing) and the call stack. Windows:
// an unhandled-exception filter plus the invalid-parameter and pure-call handlers — the three ways
// an MSVC build ends without a C++ exception. Linux (the container, where the checks run): the
// fatal signals with backtrace(). Both write the SAME file.
//
// The handler runs in a broken process, so it does the least it can: fixed buffers filled BEFORE
// the crash, no Qt containers, plain write()/WriteFile rather than QFile.
#include <QString>

namespace core::crash {

void install(const QString& productName);   // right after log::install()
QString filePath();
// The newest note install() moved aside (<product>-crash-<yyyyMMdd-HHmmss>.txt; the ten newest
// are kept), or "" when there is none.
QString previousNotePath();
void noteLogLine(const QString& line);      // AppLog feeds every line
[[noreturn]] void faultForShot(const QString& how);   // "segv" | "abort" | "throw"
// Where THIS process writes its note, for the harness's --crash-test: a deliberate crash must not
// become one of the user's notes (install() archives real ones). Call after install().
void setNotePath(const QString& path);

}  // namespace core::crash
