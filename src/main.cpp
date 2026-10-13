#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  QApplication::setOrganizationName("XJet");
  QApplication::setApplicationName("shtetl");

  MainWindow window;
  window.show();

  // shtetl [model-file | directory]
  if (argc > 1) window.openPath(QString::fromLocal8Bit(argv[1]));

  return app.exec();
}
