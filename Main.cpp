#ifdef _WIN32
#include <windows.h>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QLoggingCategory>
#include <QInputMethod>
#include <QDebug>
#include "MainWindow.h"

int main(int argc, char *argv[]) {
#ifdef _WIN32
    FreeConsole();  // Hide console safely on Windows

    /*
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    */ // to show console for debugging
    
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);

    /*
    qDebug() << "SDL2 version:" << SDL_GetRevision();
    qDebug() << "Num Joysticks:" << SDL_NumJoysticks();

    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            qDebug() << "Controller" << i << "is" << SDL_GameControllerNameForIndex(i);
        } else {
            qDebug() << "Joystick" << i << "is not a recognized controller";
        }
    }
    */  // For sdl2 debugging
    


    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));  // ✅ Enable Virtual Keyboard
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QString notebookFile;
    if (argc >= 2) {
        notebookFile = QString::fromLocal8Bit(argv[1]);
        qDebug() << "Notebook file received:" << notebookFile;
    }

    MainWindow w;
    if (!notebookFile.isEmpty()) {
        w.importNotebookFromFile(notebookFile);
    }
    w.show();
    return app.exec();
}
