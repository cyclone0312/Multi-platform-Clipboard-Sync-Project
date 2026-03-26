#include <QApplication>

#include "app/AppController.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    AppController controller;
    if (!controller.initialize())
    {
        return 1;
    }

    return app.exec();
}
