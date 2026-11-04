#pragma once
// The generic Textures tab (template §13): every texture the store lists, decoded in-tool, with
// channel isolation, scroll-zoom/pan, a pixel inspector, and the same search + funnel machinery as
// the Models tab — through the ONE list model, so "a|b" works here too (D4's tab bypassed the
// matcher; tabs-store.md §9.1). POE2's structure with D4's funnel; the thumbnails decode on the
// thread pool (a texture decode is pure).
#include "core/app/BrowserTab.h"

#include <QVector>

class QAbstractItemView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QTableView;
class QTimer;
class QToolButton;

namespace core {

class AppPlugin;
class AssetListModel;
class FunnelFilter;
class ThumbnailCache;
namespace gl { class GLTextureWidget; }

class TexturesTab : public BrowserTab {
    Q_OBJECT
public:
    TexturesTab(AppPlugin& plugin, const QStringList& kinds, QWidget* parent = nullptr);
    ~TexturesTab() override;

    void onIndexReady() override;
    void onSecondaryReady() override;
    void reset() override;
    void persistView() override;
    bool    hasExportSelection() const override;
    void    exportSelection() override;         // PNG(s)
    void    exportSelectionToLast() override;
    QString exportNoun() const override;
    void    populateExportMenu(QMenu* menu) override;

    AssetListModel* listModel() const { return m_model; }
    QVector<AssetId> selectedIds() const;
    AssetId currentId() const { return m_currentId; }
    void showTexture(const AssetId& id);
    // Harness (--texture): filter by this search text now — no debounce — and load the first match
    // synchronously; false when nothing matched.
    bool selectTexture(const QString& searchText);

Q_SIGNALS:
    void textureLoaded(const core::AssetId& id);
    void rowMenuAboutToShow(QMenu* menu, const QVector<core::AssetId>& ids);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void applyFilter();
    void loadCurrentRow();
    void loadTexture(const AssetId& id);
    void savePng(const QVector<AssetId>& ids, bool toLast);
    void saveOriginal(const AssetId& id);
    // Raw, unmodified originals (+ any sidecar dependencies) for several selected rows at once, into a
    // chosen folder — the multi-select counterpart of saveOriginal (works for any kind: textures,
    // sounds, the Files tab, …).
    void extractOriginals(const QVector<AssetId>& ids);
    void showRowContextMenu(const QPoint& viewPos, QAbstractItemView* view);
    void setGridView(bool on);
    void setIconPx(int px);
    QString stemOf(const AssetId& id) const;

    AppPlugin& m_plugin;
    QStringList m_kinds;
    AssetListModel* m_model = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_kindFacet = nullptr;
    FunnelFilter* m_funnel = nullptr;
    QTableView* m_list = nullptr;
    QListView* m_grid = nullptr;
    QToolButton* m_gridBtn = nullptr;
    QLabel* m_count = nullptr;
    gl::GLTextureWidget* m_view = nullptr;
    QComboBox* m_channel = nullptr;
    QCheckBox* m_checker = nullptr;
    QLabel* m_info = nullptr;
    QTimer* m_debounce = nullptr;
    QTimer* m_loadDebounce = nullptr;
    ThumbnailCache* m_thumbs = nullptr;
    int m_iconPx = 128;
    AssetId m_currentId;
    QImage m_currentImage;
};

}  // namespace core
