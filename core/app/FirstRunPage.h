#pragma once
// What the window shows when no game folder is configured (FOX app/FirstRunPage: "a fresh install
// opened five empty tabs and one status-bar line that said to look in the File menu"; headless, the
// harness hung waiting for an index that never started). The core default is a card with the
// product name, the plugin's own explanatory text, a Set folder… button that calls
// AppPlugin::configureFolder (the game's own picker and validation) and Start. A plugin with a
// richer page (FOX's multi-folder list) returns it from firstRunPage() instead.
#include <QWidget>

class QLabel;
class QPushButton;

namespace core {

class AppPlugin;

class FirstRunPage : public QWidget {
    Q_OBJECT
public:
    FirstRunPage(AppPlugin& plugin, QWidget* parent = nullptr);
    void refresh();            // re-read isConfigured()

Q_SIGNALS:
    void startRequested();     // configured and Start pressed

private:
    AppPlugin& m_plugin;
    QLabel* m_hint = nullptr;
    QPushButton* m_start = nullptr;
};

}  // namespace core
