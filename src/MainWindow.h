#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QTimer>

class QAction;
class QDockWidget;
class QMenu;
class ThumbnailBrowser;
class ThumbnailService;
class Viewport3D;

class MainWindow : public QMainWindow
{
    Q_OBJECT

  public:
    MainWindow();
    ~MainWindow() override;

    // A model file is shown (and its directory browsed); a directory is browsed.
    void openPath(const QString& path);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void createMenus();
    void showAboutDialog();
    void copyToClipboard();
    void openFileDialog();
    void openDirectoryDialog();
    bool loadModel(const QString& path); // false: not loaded, the previous model stays
    void showDirectory(const QString& path);
    void goAdjacent(int delta);
    void reload();
    void watchCurrentFile();
    void autoReload();
    void addRecent(const QString& path);
    void updateRecentMenu();

    // Declared first, so destroyed last: the thumbnail thread stops (see the
    // destructor) before the viewport takes the Vulkan device with it.
    Viewport3D* m_Viewport = nullptr;
    ThumbnailService* m_Service = nullptr;
    ThumbnailBrowser* m_Browser = nullptr;
    QDockWidget* m_BrowserDock = nullptr;
    QMenu* m_RecentMenu = nullptr;
    QAction* m_CopyAction = nullptr;
    QString m_CurrentFile;

    // Auto reload: the file and its directory are watched (editors and CadQuery
    // scripts often replace a file rather than write it), changes are debounced,
    // and unrelated changes in the directory are told apart by time and size.
    QFileSystemWatcher m_Watcher;
    QTimer m_ReloadTimer;
    bool m_AutoReload = false;
    QDateTime m_LoadedModified;
    qint64 m_LoadedSize = -1;
};
