#include "core/app/SehGuard.h"

#include <QString>
#include <cstdio>
#include <exception>

#if defined(_MSC_VER)
#  include <windows.h>
#  include <eh.h>       // _set_se_translator
#  include <exception>
#endif

namespace core::seh {

#if defined(_MSC_VER)

namespace {

// Thrown from the SE translator; carries the SEH code so the C++ catch can log
// it. Kept as a plain type (not derived from std::exception) so it can only be
// caught deliberately by runGuarded().
struct SehThrown {
    unsigned long code;
};

QString codeName(unsigned long code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return QStringLiteral("access violation");
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return QStringLiteral("illegal instruction");
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return QStringLiteral("integer divide by zero");
        case EXCEPTION_STACK_OVERFLOW:        return QStringLiteral("stack overflow");
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return QStringLiteral("float divide by zero");
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return QStringLiteral("array bounds exceeded");
        case EXCEPTION_IN_PAGE_ERROR:         return QStringLiteral("in-page error");
        case EXCEPTION_DATATYPE_MISALIGNMENT: return QStringLiteral("datatype misalignment");
        default: break;
    }
    return QStringLiteral("SEH 0x%1").arg(code, 8, 16, QLatin1Char('0'));
}

void translator(unsigned int code, EXCEPTION_POINTERS*)
{
    throw SehThrown{ static_cast<unsigned long>(code) };
}

} // namespace

void installSehTranslator()
{
    // Per-thread; overwriting with the same function is harmless.
    _set_se_translator(translator);
}

bool runGuarded(const char* stage, const std::function<void()>& fn,
                HardwareFault* outFault)
{
    installSehTranslator();
    try {
        fn();
        return true;
    } catch (const SehThrown& e) {
        if (outFault) {
            outFault->code = e.code;
            outFault->what = QStringLiteral("%1 during %2")
                                 .arg(codeName(e.code),
                                      QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    } catch (const std::exception& e) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("%1 during %2")
                                 .arg(QString::fromLocal8Bit(e.what()),
                                      QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    } catch (...) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("unknown exception during %1")
                                 .arg(QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    }
}

#else // non-MSVC: best-effort C++-exception guard only (no SEH on this target).

void installSehTranslator() {}

bool runGuarded(const char* stage, const std::function<void()>& fn,
                HardwareFault* outFault)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("%1 during %2")
                                 .arg(QString::fromLocal8Bit(e.what()),
                                      QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    } catch (...) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("unknown exception during %1")
                                 .arg(QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    }
}

#endif


void describeCurrentException(char* buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = '\0';
    const std::exception_ptr e = std::current_exception();
    if (!e) return;
    try {
        std::rethrow_exception(e);
    }
#if defined(_MSC_VER)
    catch (const SehThrown& s) {
        std::snprintf(buf, n, "hardware fault 0x%08lx (%s)", s.code, qUtf8Printable(codeName(s.code)));
    }
#endif
    catch (const std::exception& x) {
        std::snprintf(buf, n, "uncaught std::exception: %s", x.what());
    }
    catch (...) {
        std::snprintf(buf, n, "an uncaught exception of an unknown type");
    }
}

}  // namespace core::seh
