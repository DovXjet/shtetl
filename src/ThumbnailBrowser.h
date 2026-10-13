#pragma once

#include "ThumbnailService.h"

#include <QHash>
#include <QWidget>

class QLineEdit;
class QListView;
class QSlider;
class QStandardItem;
class QStandardItemModel;
class QToolButton;

// A directory of 3D models as a grid of thumbnails, with its subdirectories
// to navigate by (like XjetStudio's part-add dialog). Used both in the dock of
// the main window and in the Open dialog; they share one ThumbnailService and,
// through it, the cache of thumbnails on disk.
class ThumbnailBrowser : public QWidget
{
    Q_OBJECT

  public:
    explicit ThumbnailBrowser(ThumbnailService* service, QWidget* parent = nullptr);
    ~ThumbnailBrowser() override;

    QString directory() const { return m_Directory; }
    void setDirectory(const QString& path);
    // Re-reads the directory (new files show up; thumbnails of changed ones are remade).
    void refresh();

    QString currentFile() const;
    void selectFile(const QString& path);
    // The model file `delta` places after the current one (wraps around), or empty.
    QString adjacentFile(int delta) const { return adjacentFile(delta, currentFile()); }
    // The same, counted from `from`.
    QString adjacentFile(int delta, const QString& from) const;
    // Drops files from the list (those that turned out not to be loadable)
    // without changing the selection or loading anything.
    void removeFiles(const QStringList& paths);

  signals:
    // The current model changed (click or arrow keys).
    void fileSelected(const QString& path);
    // A model was double-clicked or Enter was pressed.
    void fileActivated(const QString& path);
    void directoryChanged(const QString& path);

  private:
    void onActivated(const QModelIndex& index);
    void onCurrentChanged(const QModelIndex& current);
    void onThumbnailReady(const QString& path, const QImage& image);
    void setIconSize(int size);
    QIcon placeholderIcon(const QString& text) const;
    QIcon folderIcon() const;

    ThumbnailService* m_Service;
    QString m_Directory;
    CancelFlag m_Cancel;
    QToolButton* m_UpButton = nullptr;
    QLineEdit* m_PathEdit = nullptr;
    QSlider* m_SizeSlider = nullptr;
    QListView* m_View = nullptr;
    QStandardItemModel* m_Model = nullptr;
    QHash<QString, QStandardItem*> m_Items; // by file path
};
