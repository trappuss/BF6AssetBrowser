# abcore — everything that is not a game.
qt_add_library(abcore STATIC
    math/RigMath.h            math/RigMath.cpp
    types/Geometry.h
    types/Skeleton.h          types/Skeleton.cpp
    types/Clip.h              types/Clip.cpp
    types/Material.h
    types/Asset.h
    types/ExportOptions.h     types/ExportOptions.cpp
    store/IGameStore.h
    store/BackgroundIndex.h   store/BackgroundIndex.cpp
    export/GlbExporter.h      export/GlbExporter.cpp
    export/BlenderExport.h    export/BlenderExport.cpp
    export/NameTemplate.h     export/NameTemplate.cpp
    export/ExportLayout.h     export/ExportLayout.cpp
    export/GifEncoder.h       export/GifEncoder.cpp
    gl/ViewportExtension.h
    gl/Shaders.h              gl/Shaders.cpp
    gl/Pose.h                 gl/Pose.cpp
    gl/GLModelWidget.h        gl/GLModelWidget.cpp
    gl/GLTextureWidget.h      gl/GLTextureWidget.cpp
    gl/ThumbnailRenderer.h    gl/ThumbnailRenderer.cpp
    app/AppPlugin.h
    app/AppPaths.h
    app/AppLog.h              app/AppLog.cpp
    app/CrashHandler.h        app/CrashHandler.cpp
    app/SehGuard.h            app/SehGuard.cpp
    app/LogConsole.h          app/LogConsole.cpp
    app/Hotkeys.h             app/Hotkeys.cpp
    app/ExportNotifier.h      app/ExportNotifier.cpp
    app/StatusLine.h
    app/CoreSettings.h
    app/Density.h
    app/FontScale.h
    app/StartupProfile.h
    app/CacheMaint.h
    app/BrowserTab.h
    app/SettingsDialog.h      app/SettingsDialog.cpp
    app/FirstRunPage.h        app/FirstRunPage.cpp
    app/SelfTests.h           app/SelfTests.cpp
    app/AppShell.h            app/AppShell.cpp
    app/Application.h         app/Application.cpp
    app/Harness.h             app/Harness.cpp
    ui/Glyphs.h
    ui/PanelBox.h
    ui/PanelPersist.h
    ui/NPanel.h               ui/NPanel.cpp
    ui/FilterChips.h          ui/FilterChips.cpp
    ui/FunnelFilter.h         ui/FunnelFilter.cpp
    ui/QueryTerm.h
    ui/SearchQuery.h
    ui/SearchBox.h
    ui/MenuText.h
    ui/MenuContext.h
    ui/MenuDump.h             ui/MenuDump.cpp
    ui/HoverInfo.h
    ui/HoverPreview.h         ui/HoverPreview.cpp
    ui/TipBar.h               ui/TipBar.cpp
    ui/TextReportDialog.h
    ui/CsvCopy.h              ui/CsvCopy.cpp
    ui/SearchableCombo.h
    ui/ThumbnailCache.h       ui/ThumbnailCache.cpp
    ui/AssetListModel.h       ui/AssetListModel.cpp
    ui/CardDelegate.h         ui/CardDelegate.cpp
    ui/ExportCapture.h        ui/ExportCapture.cpp
    ui/ExportDialogs.h        ui/ExportDialogs.cpp
    tabs/ModelLoad.h          tabs/ModelLoad.cpp
    tabs/ModelsTab.h          tabs/ModelsTab.cpp
    tabs/TexturesTab.h        tabs/TexturesTab.cpp
    tabs/BulkExtractor.h      tabs/BulkExtractor.cpp
    tabs/BulkTab.h            tabs/BulkTab.cpp
    tabs/CustomizeTab.h       tabs/CustomizeTab.cpp
    customize/Build.h         customize/Build.cpp
    customize/CustomizeSource.h
    customize/Assemble.h      customize/Assemble.cpp
)
# Headers are included as "core/…" from everywhere, so the include root is the repo root. This is
# what stops the seven same-named headers the three projects each have (QueryTerm.h, PanelPersist.h,
# ExportLayout.h, NameTemplate.h, AppPaths.h, SehGuard.h, LogConsole.h) from shadowing each other.
target_include_directories(abcore PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(abcore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
if(MSVC)
    target_compile_options(abcore PRIVATE /EHa /MP /bigobj)
    target_compile_definitions(abcore PRIVATE NOMINMAX)
endif()
if(WIN32)
    # CrashHandler walks the stack with dbghelp (StackWalk64 / Sym*); PUBLIC so every exe that
    # links the static abcore gets it (FOX's CMake linked it the same way).
    target_link_libraries(abcore PUBLIC dbghelp)
endif()
