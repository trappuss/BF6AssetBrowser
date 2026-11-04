#pragma once
// The harness steps every game with the core's Models and Textures tabs shares — what FOX's DevShot
// did for --model / --clip / --export and what any new game's --model needs on day one. A plugin
// keeps the flags it parsed (AppPlugin::parseArgs) and calls these from applyArgs, so the
// behaviour and the log lines ("devshot: …", "models: loaded …", "textures: loaded …") are one
// thing family-wide rather than a copy per game.
#include "core/customize/Build.h"

#include <QString>
#include <QStringList>

namespace core {
class AppShell;
class CustomizeTab;
namespace harness {

// Show the Models tab, filter it by `model` (search text, no debounce), load the first match
// synchronously; then play `clip` (an index, or the first clip whose name contains the text —
// a lazy clip is decoded), tick `attach` — comma-separated terms, each an index or text: the first
// still-unticked attachment whose label or group contains it ("axe,axe" = two axes) — and export to
// `exportPath` (with the ticked pieces when the export includes attachments). Empty arguments are
// skipped. False when the tab is missing or nothing matched (each logged).
bool applyModel(AppShell& shell, const QString& model, const QString& clip = {}, const QString& exportPath = {},
                const QString& attach = {});

// Show the Customize tab `tabId`, apply `buildText` (core/customize/Build.h grammar; ';' may separate
// lines) and assemble synchronously; log "devshot: build — …" with what was skipped; then export the
// whole build to `exportPath` when given. False when the tab is missing or the build did not assemble.
// "random:<seed>" as the build text fills every slot at random from that seed (reproducible).
// "slot.<id>=@<n>" picks the n-th option (1-based) of that slot's list as the tab shows it for the
// build — a check that needs a part fitted without naming one this install may not carry.
// "subject=@<n>" likewise opens the n-th subject of the build's category (FOX's vehicles, 2e-4).
bool applyCustomize(AppShell& shell, const QString& tabId, const QString& buildText, const QString& exportPath = {});

// The "@<n>" subject and picks of `b` resolved against `tab`: the subject first (its category's
// list), then the picks in the tab's slot order, each list built for the picks before it; `b` is
// applied on the way and holds the result. Unresolvable picks are dropped and named in `unresolved`;
// an unresolvable subject is named and left as written (so applying `b` fails, saying so), and no
// pick is resolved against a subject the build did not ask for.
bool resolvePositionalPicks(CustomizeTab& tab, customize::Build& b, QStringList* unresolved = nullptr);

// Show the Textures tab, filter it by `texture`, load the first match. False when nothing matched.
bool applyTexture(AppShell& shell, const QString& texture);

}  // namespace harness
}  // namespace core
