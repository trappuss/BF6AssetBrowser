// See CrashHandler.h. FOX's implementation, verbatim but for the product name; every design
// note below is FOX's and each was learned from a real crash.
#include "core/app/CrashHandler.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "core/app/AppPaths.h"
#include "core/app/SehGuard.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace core::crash {
namespace {

// ── The pieces the handler is allowed to touch ──────────────────────────────
// All fixed-size and filled in BEFORE the crash: a handler that formats a
// QString or opens a QFile is a handler that can fault a second time, and the
// second fault is the one that leaves no note at all.
constexpr int kTailLines = 20;
constexpr int kLineMax = 400;
char g_tail[kTailLines][kLineMax];
std::atomic<unsigned> g_tailNext{0};
char g_path[1024];
char g_build[128];
char g_product[96] = "AssetBrowser";
QString g_productName = QStringLiteral("AssetBrowser");
std::atomic<bool> g_written{false};

void writeAll(int fdOrHandle, const char* text, size_t len);

#ifdef Q_OS_WIN
HANDLE g_file = INVALID_HANDLE_VALUE;
void openNote()
{
    g_file = CreateFileA(g_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}
void writeAll(int, const char* text, size_t len)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD done = 0;
    WriteFile(g_file, text, DWORD(len), &done, nullptr);
}
void closeNote()
{
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}
#else
int g_fd = -1;
void openNote()
{
    g_fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
void writeAll(int, const char* text, size_t len)
{
    if (g_fd < 0) return;
    ssize_t off = 0;
    while (size_t(off) < len) {
        const ssize_t n = ::write(g_fd, text + off, len - size_t(off));
        if (n <= 0) break;
        off += n;
    }
}
void closeNote()
{
    if (g_fd >= 0) { ::close(g_fd); g_fd = -1; }
}
#endif

void say(const char* text) { writeAll(0, text, std::strlen(text)); }

void sayLine(const char* text)
{
    say(text);
    say("\n");
}

// The header every note starts with, and the log tail under it. Written by
// whichever handler fired, once — a fault inside the handler must not start
// the file again from the top.
void beginNote(const char* what, const void* address)
{
    bool expected = false;
    if (!g_written.compare_exchange_strong(expected, true)) return;
    openNote();
    say(g_product);
    say(" crash note\n");
    say("build   : ");
    sayLine(g_build);
    say("fault   : ");
    say(what);
    if (address) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " at %p", address);
        say(buf);
    }
    say("\n\nThe last lines the tool logged, oldest first:\n");
    const unsigned next = g_tailNext.load();
    for (unsigned i = 0; i < kTailLines; ++i) {
        const unsigned at = (next + i) % kTailLines;
        if (g_tail[at][0] == '\0') continue;
        say("  ");
        sayLine(g_tail[at]);
    }
    say("\nCall stack, innermost first:\n");
}

#ifdef Q_OS_WIN
void writeStack(CONTEXT* ctx)
{
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    void* frames[62];
    USHORT n = 0;
    if (ctx) {
        // Walking from the exception CONTEXT is what gets the frames BELOW the
        // filter — RtlCaptureStackBackTrace from in here would show the
        // handler's own stack, which is never the interesting one.
        STACKFRAME64 sf{};
        CONTEXT local = *ctx;
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        sf.AddrPC.Offset = local.Rip;      sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = local.Rbp;   sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = local.Rsp;   sf.AddrStack.Mode = AddrModeFlat;
        while (n < 62
               && StackWalk64(machine, proc, GetCurrentThread(), &sf, &local,
                              nullptr, SymFunctionTableAccess64,
                              SymGetModuleBase64, nullptr)
               && sf.AddrPC.Offset)
            frames[n++] = reinterpret_cast<void*>(sf.AddrPC.Offset);
    }
    if (n == 0) n = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    char line[1024];
    for (USHORT i = 0; i < n; ++i) {
        const DWORD64 addr = DWORD64(frames[i]);
        char symbuf[sizeof(SYMBOL_INFO) + 512];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 li{};
        li.SizeOfStruct = sizeof(li);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &li))
            std::snprintf(line, sizeof(line), "  %2u  %s +0x%llx  (%s:%lu)\n", i,
                          name, static_cast<unsigned long long>(disp),
                          li.FileName, li.LineNumber);
        else
            std::snprintf(line, sizeof(line), "  %2u  %s +0x%llx  [0x%llx]\n", i,
                          name, static_cast<unsigned long long>(disp),
                          static_cast<unsigned long long>(addr));
        say(line);
    }
}

LONG WINAPI onException(EXCEPTION_POINTERS* info)
{
    char what[64];
    std::snprintf(what, sizeof(what), "0x%08lx",
                  info && info->ExceptionRecord
                      ? info->ExceptionRecord->ExceptionCode
                      : 0ul);
    beginNote(what, info && info->ExceptionRecord
                        ? info->ExceptionRecord->ExceptionAddress
                        : nullptr);
    writeStack(info ? info->ContextRecord : nullptr);
    closeNote();
    // EXIT 3, like every other handler here — NOT
    // EXCEPTION_EXECUTE_HANDLER. Returning that unwinds to the CRT, which
    // ends the process with the EXCEPTION CODE: a caller sees 0xC0000005
    // (-1073741819) and cannot tell a handled fault, which left a note, from
    // a raw one that left nothing. The note was written either way — the
    // regression bat's `:rc` step is what caught this, and the note it was
    // looking for was sitting right there beside the wrong exit code.
    ::_exit(3);
    return EXCEPTION_EXECUTE_HANDLER;   // unreachable
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned,
                        uintptr_t)
{
    beginNote("invalid parameter (CRT)", nullptr);
    writeStack(nullptr);
    closeNote();
    ::_exit(3);
}

void onPureCall()
{
    beginNote("pure virtual call", nullptr);
    writeStack(nullptr);
    closeNote();
    ::_exit(3);
}

// abort() — qFatal, an assert, and the default terminate of EVERY thread (the MSVC runtime keeps a
// terminate handler per thread, so a worker's uncaught exception never reached onTerminate) — raises
// SIGABRT before anything else, so this is where those notes are written (audit 2026-09-25: none were).
void onAbort(int)
{
    beginNote("abort (SIGABRT: qFatal, abort(), or an exception uncaught on a worker thread)", nullptr);
    writeStack(nullptr);
    closeNote();
    ::_exit(3);
}
#else
void writeStack()
{
    // backtrace_symbols_fd, not backtrace_symbols: the latter mallocs, and a fault INSIDE malloc
    // (its lock held) would deadlock the handler instead of writing the note (audit 2026-09-25).
    void* frames[62];
    const int n = ::backtrace(frames, 62);
    if (g_fd >= 0) ::backtrace_symbols_fd(frames, n, g_fd);
}

void onSignal(int sig)
{
    const char* what = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGABRT ? "SIGABRT"
                       : sig == SIGBUS  ? "SIGBUS"
                       : sig == SIGFPE  ? "SIGFPE"
                       : sig == SIGILL  ? "SIGILL"
                                        : "signal";
    beginNote(what, nullptr);
    writeStack();
    closeNote();
    ::_exit(3);
}
#endif

void onTerminate()
{
    // Say WHAT escaped: a hardware fault the translator turned into a C++ exception keeps its code.
    char what[320] = "std::terminate (an exception nobody caught)";
    char desc[256];
    seh::describeCurrentException(desc, sizeof(desc));
    if (desc[0]) std::snprintf(what, sizeof(what), "std::terminate — %s", desc);
    beginNote(what, nullptr);
#ifdef Q_OS_WIN
    writeStack(nullptr);
#else
    writeStack();
#endif
    closeNote();
    ::_exit(3);
}

}  // namespace

QString filePath()
{
    return paths::file(g_productName + QStringLiteral("-crash.txt"));
}

namespace {
constexpr int kKeptNotes = 10;
QStringList archivedNotes()   // newest first
{
    QDir d(QFileInfo(filePath()).absolutePath());
    return d.entryList({g_productName + QStringLiteral("-crash-2*.txt")}, QDir::Files, QDir::Name | QDir::Reversed);
}
}  // namespace

QString previousNotePath()
{
    const QStringList n = archivedNotes();
    return n.isEmpty() ? QString() : QDir(QFileInfo(filePath()).absolutePath()).filePath(n.first());
}

void noteLogLine(const QString& line)
{
    const QByteArray raw = line.toUtf8();
    const unsigned at = g_tailNext.fetch_add(1) % kTailLines;
    const int n = qMin(raw.size(), qsizetype(kLineMax - 1));
    std::memcpy(g_tail[at], raw.constData(), size_t(n));
    g_tail[at][n] = '\0';
}

void install(const QString& productName)
{
    g_productName = productName;
    {
        const QByteArray pn = productName.toUtf8();
        const int k = qMin(pn.size(), qsizetype(sizeof(g_product) - 1));
        std::memcpy(g_product, pn.constData(), size_t(k));
        g_product[k] = '\0';
    }
    const QByteArray p = QDir::toNativeSeparators(filePath()).toLocal8Bit();
    const int n = qMin(p.size(), qsizetype(sizeof(g_path) - 1));
    std::memcpy(g_path, p.constData(), size_t(n));
    g_path[n] = '\0';
    std::snprintf(g_build, sizeof(g_build), "%s %s", __DATE__, __TIME__);

    // A note from LAST time. The file is only ever written by a crash, so its
    // presence at startup means the previous session ended badly. It is said
    // ONCE: the note is moved to <product>-crash-<when it was written>.txt and
    // the warning names that path. Left in place it was re-announced on every
    // start for days after the crash was fixed (run 12). The newest ten are
    // kept: a single "-previous" file (the first cut) let the next crash of a
    // test run replace the user's own note — every harness start moves one
    // (audit 2026-09-25). Copy diagnostics names the newest.
    if (QFile::exists(filePath())) {
        const QDir dir(QFileInfo(filePath()).absolutePath());
        const QString stamp = QFileInfo(filePath()).lastModified().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        QString prev = dir.filePath(QStringLiteral("%1-crash-%2.txt").arg(g_productName, stamp));
        for (int n = 2; QFile::exists(prev); ++n)
            prev = dir.filePath(QStringLiteral("%1-crash-%2-%3.txt").arg(g_productName, stamp).arg(n));
        const bool moved = QFile::rename(filePath(), prev);
        const QString where = moved ? prev : filePath();
        const QStringList old = archivedNotes();
        for (int i = kKeptNotes; i < old.size(); ++i) QFile::remove(dir.filePath(old[i]));
        qWarning("crash: the previous session left a note, now at %s — send it if "
                 "the tool disappeared on you",
                 qUtf8Printable(QDir::toNativeSeparators(where)));
    }

    std::set_terminate(onTerminate);
#ifdef Q_OS_WIN
    ::signal(SIGABRT, onAbort);
    SetUnhandledExceptionFilter(onException);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
#else
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        ::signal(sig, onSignal);
#endif
}

void setNotePath(const QString& path)
{
    const QByteArray p = QDir::toNativeSeparators(path).toLocal8Bit();
    const int n = qMin(p.size(), qsizetype(sizeof(g_path) - 1));
    std::memcpy(g_path, p.constData(), size_t(n));
    g_path[n] = '\0';
}

void faultForShot(const QString& how)
{
    if (how == QLatin1String("abort")) std::abort();
    if (how == QLatin1String("throw")) throw std::bad_alloc();
    // "segv" and anything else: a write through a pointer the compiler cannot
    // prove is null, so no optimiser turns this into a trap of its own.
    volatile int* p = reinterpret_cast<volatile int*>(std::atoi("0") ? 8 : 0);
    *p = 42;
    std::abort();   // unreachable; keeps the [[noreturn]] promise honest
}

}  // namespace core::crash
