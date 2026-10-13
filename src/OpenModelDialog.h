#pragma once

#include <QDialog>

class ThumbnailBrowser;
class ThumbnailService;
class QPushButton;

// Open a model by its thumbnail, like XjetStudio's part-add dialog.
class OpenModelDialog : public QDialog
{
    Q_OBJECT

  public:
    OpenModelDialog(ThumbnailService* service, const QString& startDirectory, QWidget* parent = nullptr);

    QString selectedFile() const { return m_Selected; }
    QString directory() const;

  private:
    ThumbnailBrowser* m_Browser = nullptr;
    QPushButton* m_OpenButton = nullptr;
    QString m_Selected;
};
