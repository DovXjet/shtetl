#include "OpenModelDialog.h"

#include "ThumbnailBrowser.h"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

OpenModelDialog::OpenModelDialog(ThumbnailService* service, const QString& startDirectory, QWidget* parent) : QDialog(parent)
{
  setWindowTitle("Open Model");
  resize(QSettings().value("OpenDialog/Size", QSize(820, 560)).toSize());

  auto* layout = new QVBoxLayout(this);
  m_Browser = new ThumbnailBrowser(service, this);
  layout->addWidget(m_Browser, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
  m_OpenButton = buttons->button(QDialogButtonBox::Open);
  m_OpenButton->setEnabled(false);
  layout->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
    m_Selected = m_Browser->currentFile();
    if (!m_Selected.isEmpty()) accept();
  });
  connect(m_Browser, &ThumbnailBrowser::fileSelected, this, [this]() { m_OpenButton->setEnabled(true); });
  connect(m_Browser, &ThumbnailBrowser::directoryChanged, this, [this]() { m_OpenButton->setEnabled(false); });
  connect(m_Browser, &ThumbnailBrowser::fileActivated, this, [this](const QString& path) {
    m_Selected = path;
    accept();
  });
  connect(this, &QDialog::finished, this, [this]() { QSettings().setValue("OpenDialog/Size", size()); });

  m_Browser->setDirectory(startDirectory);
}

QString OpenModelDialog::directory() const
{
  return m_Browser->directory();
}
