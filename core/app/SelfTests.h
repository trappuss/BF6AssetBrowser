#pragma once
// Startup self-tests for invariants that would otherwise fail silently (template §4, §7). POE2's
// table shape with FOX's reporting: qWarning, NOT stderr — a Windows GUI build has no console, so
// POE2's fprintf(stderr) was discarded and a failing self-test was silent (app-shell.md §1.1).
// Each test returns "" on success or the first failing case. A failure is printed loudly and does
// not stop the app — the tool still runs, it just says so; the conformance runner reads the count.
#include <QString>
#include <QVector>
#include <functional>

namespace core::selftest {

struct Test { QString name; std::function<QString()> run; };

// The core's own tests (matcher, query editors, funnel, layout, name template, exporter, rig math).
QVector<Test> coreTests();

// Run every test, log each outcome, return the number that failed.
int run(const QVector<Test>& tests);

}  // namespace core::selftest
