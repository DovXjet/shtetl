#pragma once

#include "CompactMesh.h"

#include <QDateTime>
#include <QColor>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>

class QAction;
class QProgressBar;
class QCloseEvent;
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
    void closeEvent(QCloseEvent* event) override;
    void saveWindowSettings(); // geometry, dock state and width
    void createMenus();
    void showAboutDialog();
    void copyToClipboard();
    void openFileDialog();
    void openDirectoryDialog();
    // Loads in the background (the previous model stays in view meanwhile, and
    // a load still running is dropped); see onModelLoaded().
    void loadModel(const QString& path);
    void onModelLoaded(quint64 id, const QString& path, LoadedModel model, const QString& error);
    void cancelLoad(); // the load in progress, if any
    void updateLoadProgress();
    // The model `path` could not be shown: on to the next one in the direction
    // that was being stepped through, if any.
    void skipFailed(const QString& path, int direction);
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
    // The color of STL and 3MF models (the others bring their own materials);
    // passed to each load, and applied to the model shown when it changes.
    QColor m_ModelColor;

    // The background load: on their own threads (two at most, so that a new
    // one need not wait for the cancelled one to notice), numbered so that the result of one
    // that was replaced is ignored.
    struct Load
    {
        quint64 id = 0; // of the latest started
        bool running = false;
        QString path;
        int direction = 0; // the step (+1/-1) that led to it, to skip bad files
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<std::atomic<int>> percent; // -1: not known
    };
    QThreadPool m_LoadPool;
    Load m_Load;
    int m_StepDirection = 0; // for the next loadModel(), see goAdjacent()
    QElapsedTimer m_LoadTime;
    QTimer m_ProgressTimer;
    QProgressBar* m_Progress = nullptr;

    // Auto reload: the file and its directory are watched (editors and CadQuery
    // scripts often replace a file rather than write it), changes are debounced,
    // and unrelated changes in the directory are told apart by time and size.
    QFileSystemWatcher m_Watcher;
    QTimer m_ReloadTimer;
    bool m_AutoReload = false;
    QDateTime m_LoadedModified;
    qint64 m_LoadedSize = -1;
};
