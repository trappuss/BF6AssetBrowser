#include "core/app/Harness.h"
#include "core/app/AppShell.h"
#include "core/gl/GLModelWidget.h"
#include "core/store/IGameStore.h"
#include "core/tabs/CustomizeTab.h"
#include "core/tabs/ModelsTab.h"
#include "core/tabs/TexturesTab.h"

#include <QHash>
#include <QSet>
#include <iterator>

namespace core::harness {

bool applyModel(AppShell& shell, const QString& model, const QString& clip, const QString& exportPath, const QString& attach)
{
    if (model.isEmpty()) {
        if (!exportPath.isEmpty()) qWarning("devshot: --export needs --model (or a game's scene flag) beside it");
        return false;
    }
    shell.showTab(QStringLiteral("models"));
    auto* models = dynamic_cast<ModelsTab*>(shell.tab(QStringLiteral("models")));
    if (!models) { qWarning("devshot: no Models tab"); return false; }
    if (!models->selectModel(model)) { qWarning("devshot: no model matches '%s'", qUtf8Printable(model)); return false; }
    if (!clip.isEmpty() && models->current()) {
        const QVector<Clip>& clips = models->current()->clips;
        bool isNum = false; int ci = clip.toInt(&isNum);
        if (!isNum) { ci = -1; for (int k = 0; k < clips.size(); ++k) if (clips[k].name.contains(clip, Qt::CaseInsensitive)) { ci = k; break; } }
        if (ci < 0 || ci >= clips.size()) qWarning("devshot: no clip matches '%s' (%d listed)", qUtf8Printable(clip), int(clips.size()));
        else {
            models->setClipIndex(ci);
            // Said as decoded only when it was: setClipIndex falls back to the bind pose on a failed
            // decode, and the line used to claim the clip either way (audit 2026-09-25). The failure
            // wording does not contain "devshot: clip", so a check's needle cannot match it.
            const Clip* now = models->current() && ci < models->current()->clips.size() ? &models->current()->clips[ci] : nullptr;
            if (now && !now->lazy) qInfo("devshot: clip — %s (%d of %d)", qUtf8Printable(now->name), ci, int(clips.size()));
            else qWarning("devshot: FAILED to decode clip %d (%s) — the bind pose is shown", ci, qUtf8Printable(clips[ci].name));
        }
    }
    if (!attach.isEmpty() && models->current()) {
        const QVector<Attachment>& atts = models->current()->attachments;
        QSet<int> tried;   // a piece that could not be placed is not tried again by the next term
        int placed = 0;
        for (const QString& raw : attach.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString term = raw.trimmed();
            bool isNum = false; int ai = term.toInt(&isNum);
            if (!isNum) {
                ai = -1;
                for (int k = 0; k < atts.size(); ++k)
                    if (!models->attachmentsShown().value(k) && !tried.contains(k)
                        && (atts[k].label.contains(term, Qt::CaseInsensitive) || atts[k].group.contains(term, Qt::CaseInsensitive))) { ai = k; break; }
            }
            if (ai < 0 || ai >= atts.size()) { qWarning("devshot: no attachment matches '%s' (%d listed)", qUtf8Printable(term), int(atts.size())); continue; }
            if (tried.contains(ai)) continue;
            tried.insert(ai);
            if (models->setAttachmentShown(ai, true) && ++placed)
                qInfo("devshot: attached %s — %s (%d of %d)", qUtf8Printable(atts[ai].label), qUtf8Printable(atts[ai].group), ai, int(atts.size()));
            else
                qWarning("devshot: attachment %s could not be placed (see \"attachment skipped\")", qUtf8Printable(atts[ai].label));
        }
        // A person ticking a piece keeps their view (D4 does); a shot must show the assembly.
        if (placed > 0) models->view()->frameAll(false, false);
    }
    if (!exportPath.isEmpty()) {
        QString err;
        if (!models->exportTo(exportPath, &err)) qWarning("devshot: export %s — %s", qUtf8Printable(exportPath), qUtf8Printable(err));
    }
    return true;
}

bool resolvePositionalPicks(CustomizeTab& tab, customize::Build& b, QStringList* unresolved)
{
    if (b.subject.startsWith(QLatin1Char('@'))) {
        bool ok = false;
        const int n = b.subject.mid(1).toInt(&ok);
        const QString id = ok && n >= 1 ? tab.subjectIdAt(b.category, n - 1) : QString();
        if (id.isEmpty()) {
            if (unresolved)
                *unresolved << QStringLiteral("subject=%1 (the %2 list has %3)")
                                   .arg(b.subject, b.category.isEmpty() ? QStringLiteral("current category's") : b.category)
                                   .arg(tab.subjectCountIn(b.category));
            return false;
        }
        b.subject = id;
    }
    QHash<QString, int> wanted;
    for (auto it = b.picks.begin(); it != b.picks.end();) {
        bool ok = false;
        const int n = it.value().startsWith(QLatin1Char('@')) ? it.value().mid(1).toInt(&ok) : 0;
        if (ok && n >= 1) { wanted.insert(it.key(), n); it = b.picks.erase(it); } else ++it;
    }
    if (wanted.isEmpty()) return true;
    bool all = true;
    tab.applyBuild(b, false);   // the subject, variant and named picks first: the lists are theirs
    for (const QString& slot : tab.slotIds()) {
        if (!wanted.contains(slot)) continue;
        const int n = wanted.take(slot);
        const QString id = tab.optionIdAt(slot, n - 1);
        if (id.isEmpty()) {
            all = false;
            if (unresolved) *unresolved << QStringLiteral("%1=@%2 (the list has %3)").arg(slot).arg(n).arg(tab.optionCount(slot));
            continue;
        }
        customize::Build cur = tab.currentBuild();
        cur.picks.insert(slot, id);
        tab.applyBuild(cur, false);
    }
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {   // slots this subject lacks
        all = false;
        if (unresolved) *unresolved << QStringLiteral("%1=@%2 (no such slot)").arg(it.key()).arg(it.value());
    }
    b = tab.currentBuild();
    return all;
}

bool applyCustomize(AppShell& shell, const QString& tabId, const QString& buildText, const QString& exportPath)
{
    shell.showTab(tabId);
    auto* tab = dynamic_cast<CustomizeTab*>(shell.tab(tabId));
    if (!tab) { qWarning("devshot: no Customize tab '%s'", qUtf8Printable(tabId)); return false; }
    customize::Build b = tab->currentBuild();
    if (buildText.startsWith(QLatin1String("random:"))) {
        // Reproducible coverage: the same seed picks the same pieces on the same install.
        quint32 seed = buildText.mid(7).toUInt();
        b.picks.clear();   // fully determined by the seed
        for (const customize::SlotDef& d : tab->slotDefs()) {
            const int n = tab->optionCount(d.id);
            seed = seed * 1664525u + 1013904223u;
            if (n > 0 && d.randomize) b.picks.insert(d.id, tab->optionIdAt(d.id, int(seed % uint(n + 1)) - 1));
        }
        for (auto it = b.picks.begin(); it != b.picks.end();) it = it.value().isEmpty() ? b.picks.erase(it) : std::next(it);
    } else if (!buildText.isEmpty()) {
        QStringList unknown;
        customize::Build parsed;
        if (!customize::Build::fromText(buildText, &parsed, &unknown)) { qWarning("devshot: build text did not parse: %s", qUtf8Printable(buildText)); return false; }
        if (!unknown.isEmpty()) qWarning("devshot: build text — ignored: %s", qUtf8Printable(unknown.join(QStringLiteral(", "))));
        if (!parsed.subject.isEmpty()) b.subject = parsed.subject;
        // The text is the build, as a preset is: its category and variant too, and an absent
        // variant is the subject's first — not the one left on screen, which after a subject
        // change belongs to another subject and read as "not in this install" (FOX, stage 2e-2).
        if (!parsed.category.isEmpty()) b.category = parsed.category;
        b.variant = parsed.variant;
        b.picks = parsed.picks;
        for (auto it = parsed.toggles.constBegin(); it != parsed.toggles.constEnd(); ++it) b.toggles.insert(it.key(), it.value());
    }
    QStringList unresolved;
    resolvePositionalPicks(*tab, b, &unresolved);
    for (const QString& u : unresolved) qWarning("devshot: build — %s: not resolved", qUtf8Printable(u));
    QStringList missing;
    const bool ok = tab->applyBuild(b, /*assembleNow*/true, &missing);
    qInfo("devshot: build — %s%s", qUtf8Printable(tab->currentBuild().toText().replace(QLatin1Char('\n'), QStringLiteral("; "))),
          missing.isEmpty() ? "" : qUtf8Printable(QStringLiteral(" (not in this install: %1)").arg(missing.join(QStringLiteral(", ")))));
    if (!ok) {
        // An empty reason is not a reason: nothing was assembled at all — no index yet (a first-run
        // page), or a subject this install does not have (named in the line above).
        const QString why = tab->assembly().error.isEmpty() ? QStringLiteral("nothing assembled (no index yet, or the subject is not in this install)")
                                                            : tab->assembly().error;
        qWarning("devshot: the build did not assemble — %s", qUtf8Printable(why));
        return false;
    }
    if (!exportPath.isEmpty()) {
        QString err;
        if (tab->exportBuildTo(exportPath, CustomizeTab::Full, &err).isEmpty())
            qWarning("devshot: build export %s — %s", qUtf8Printable(exportPath), qUtf8Printable(err));
    }
    return true;
}

bool applyTexture(AppShell& shell, const QString& texture)
{
    if (texture.isEmpty()) return false;
    shell.showTab(QStringLiteral("textures"));
    auto* textures = dynamic_cast<TexturesTab*>(shell.tab(QStringLiteral("textures")));
    if (!textures) { qWarning("devshot: no Textures tab"); return false; }
    if (!textures->selectTexture(texture)) { qWarning("devshot: no texture matches '%s'", qUtf8Printable(texture)); return false; }
    return true;
}

}  // namespace core::harness
