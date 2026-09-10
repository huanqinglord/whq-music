#include <QApplication>
#include <QFile>
#include <QIcon>

#include "main_window.h"

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    application.setStyle(QStringLiteral("Fusion"));
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/app_icon.png")));

    QFile styleFile(QStringLiteral(":/styles/dark.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        application.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }

    MainWindow mainWindow;
    mainWindow.show();

    return application.exec();
}
