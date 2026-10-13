#include "Logging.h"
#include "MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv)
{
  shtetl::log::init();
  spdlog::info("shtetl {} (commit {})", SHTETL_VERSION, SHTETL_COMMIT_ID);

  QApplication app(argc, argv);
  QApplication::setOrganizationName("XJet");
  QApplication::setApplicationName("shtetl");
  QApplication::setWindowIcon(QIcon(":/icons/shtetl.png"));

  MainWindow window;
  window.show();

  // shtetl [model-file | directory]
  if (argc > 1)
  {
    spdlog::info("Command line: {}", argv[1]);
    window.openPath(QString::fromLocal8Bit(argv[1]));
  }

  const int rc = app.exec();
  spdlog::info("Application exiting with code {}", rc);
  return rc;
}
