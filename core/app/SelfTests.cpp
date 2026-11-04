#include "core/app/SelfTests.h"
#include "core/export/ExportLayout.h"
#include "core/export/GlbExporter.h"
#include "core/export/NameTemplate.h"
#include "core/gl/Pose.h"
#include "core/math/RigMath.h"
#include "core/ui/FunnelFilter.h"
#include "core/customize/Build.h"
#include "core/ui/QueryTerm.h"
#include "core/ui/SearchQuery.h"

#include <QtGlobal>

namespace core::selftest {

QVector<Test> coreTests()
{
    return {
        {QStringLiteral("core::rm"),         [] { return QString::fromLatin1(rm::selfTest()); }},
        {QStringLiteral("core::gl::pose"),   [] { return QString::fromLatin1(gl::selfTest()); }},
        {QStringLiteral("QueryTerm"),        [] { return search::selfTest(); }},
        {QStringLiteral("SearchQuery"),      [] { return searchQuerySelfTest(); }},
        {QStringLiteral("FunnelFilter"),     [] { return FunnelFilter::selfTest(); }},
        {QStringLiteral("Customize build"),  [] { return customize::buildSelfTest(); }},
        {QStringLiteral("ExportLayout"),     [] { return layout::selfTest(); }},
        {QStringLiteral("NameTemplate"),     [] { return name::selfTest(); }},
        {QStringLiteral("GlbExporter"),      [] { return glb::selfTest(); }},
    };
}

int run(const QVector<Test>& tests)
{
    int failed = 0;
    for (const Test& t : tests) {
        const QString r = t.run ? t.run() : QString();
        if (r.isEmpty()) {
            qInfo("self-test: %s PASS", qUtf8Printable(t.name));
        } else {
            ++failed;
            qWarning("SELF-TEST FAILED: %s — %s", qUtf8Printable(t.name), qUtf8Printable(r));
        }
    }
    qInfo("self-test: %d test(s), %d failed", int(tests.size()), failed);
    return failed;
}

}  // namespace core::selftest
