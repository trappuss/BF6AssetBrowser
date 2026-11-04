#pragma once
#include <QString>
#include <cstdint>

// ── Export file-name templates ──────────────────────────────────────────────────────────────────
//
// D4's `util/NameTemplate.h` and POE2's are BYTE-IDENTICAL (md5 8ca7ffd9…), which is the whole
// argument for this file. Two things changed on the way in:
//
//  1. The QSettings readers (texture()/model()/frame(), each opening QSettings per call) are gone.
//     Core never owns a settings key — the app reads its key and passes the template string. That
//     is not a style preference: D4's ExportLayout had to add a `tpl` parameter to modelFolder()
//     precisely because the convenience overload was reopening QSettings once per exported item.
//
//  2. `{{SNO}}` is a D4 word. The placeholder is `{{Id}}` here, with `{{SNO}}` kept as an alias so
//     every template a D4 or POE2 user has already saved keeps working. Removing it would silently
//     rename their files.
//
// Placeholders: {{FileName}} · {{Id}} (alias {{SNO}}) · {{Collection}} · {{FrameIdx}} · {{FrameName}}
namespace core::name {

struct Fields {
    QString fileName;               // the asset's own name, without extension
    qint64  id = 0;                 // [D4 SNO] [POE2 MurmurHash64 of the path] [FOX StrCode64]
    QString collection;             // [D4 none] [FOX archive] [POE2 bundle] — usable, never required
    int     frameIndex = -1;        // atlas/texframe exports only; < 0 renders as empty
    QString frameName;
};

// Substitute, then make the result safe to use as a file stem. Never returns empty: a template
// that resolves to nothing falls back to the asset's own name, and an asset with no name falls
// back to its id.
QString apply(const QString& tpl, const Fields& f);

// The file-system safety pass on its own, for a stem that did not come from a template.
//
// Strips the characters Windows forbids in a name, collapses the runs of whitespace an empty
// placeholder leaves behind, drops trailing dots and spaces, and prefixes the reserved DEVICE names
// (CON, PRN, AUX, NUL, COM1-9, LPT1-9) with '_'. D4 applied the device rule to FOLDERS only, which
// left `CON.glb` unwritable with nothing in the log to say why — the rule belongs on both.
QString sanitizeStem(QString stem);

QString selfTest();   // "" on success

}  // namespace core::name
