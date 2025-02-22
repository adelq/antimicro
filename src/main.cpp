/* antimicro Gamepad to KB+M event mapper
 * Copyright (C) 2015 Travis Nickles <nickles.travis@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mainwindow.h"
#include "inputdevice.h"
#include "setjoystick.h"
#include "simplekeygrabberbutton.h"
#include "joybuttonslot.h"
#include "inputdaemon.h"
#include "common.h"
#include "commandlineutility.h"
#include "autoprofileinfo.h"
#include "localantimicroserver.h"
#include "antimicrosettings.h"
#include "applaunchhelper.h"
#include "antkeymapper.h"

#include "eventhandlerfactory.h"
#include "messagehandler.h"
#include "logger.h"

#include <QApplication>
#include <QtGlobal>
#include <QMainWindow>
#include <QMap>
#include <QMapIterator>
#include <QDir>
#include <QDebug>
#include <QTranslator>
#include <QLibraryInfo>
#include <QTextStream>
#include <QLocalSocket>
#include <QSettings>
#include <QThread>
#include <QPointer>
#include <QCommandLineParser>
#include <QStandardPaths>


#ifdef Q_OS_UNIX
    #include <signal.h>
    #include <unistd.h>

    #include <sys/types.h>
    #include <sys/stat.h>

    #ifdef WITH_X11
      #include "x11extras.h"
    #endif

#endif

#ifdef Q_OS_WIN
  #include <SDL2/SDL.h>
  #undef main
#endif

#ifdef Q_OS_WIN
    #include "winextras.h"
    #include <QStyle>
    #include <QStyleFactory>
#endif



#ifndef Q_OS_WIN
static void termSignalTermHandler(int signal)
{
    qInstallMessageHandler(MessageHandler::myMessageOutput);

    Q_UNUSED(signal)

    qApp->exit(0);
}

static void termSignalIntHandler(int signal)
{
    qInstallMessageHandler(MessageHandler::myMessageOutput);

    Q_UNUSED(signal)

    qApp->exit(0);
}

#endif

// was non static
static void deleteInputDevices(QMap<SDL_JoystickID, InputDevice*> *joysticks)
{
    qInstallMessageHandler(MessageHandler::myMessageOutput);

    QMapIterator<SDL_JoystickID, InputDevice*> iter(*joysticks);

    while (iter.hasNext())
    {
        InputDevice *joystick = iter.next().value();

        if (joystick != nullptr)
        {
            delete joystick;
            joystick = nullptr;
        }
    }

    joysticks->clear();
}


int main(int argc, char *argv[])
{
    qInstallMessageHandler(MessageHandler::myMessageOutput);

    QApplication antimicro(argc, argv);
    QCoreApplication::setApplicationName("antimicro");
    QCoreApplication::setApplicationVersion(PadderCommon::programVersion);

    qRegisterMetaType<JoyButtonSlot*>();
    qRegisterMetaType<SetJoystick*>();
    qRegisterMetaType<InputDevice*>();
    qRegisterMetaType<AutoProfileInfo*>();
    qRegisterMetaType<QThread*>();
    qRegisterMetaType<SDL_JoystickID>("SDL_JoystickID");
    qRegisterMetaType<JoyButtonSlot::JoySlotInputAction>("JoyButtonSlot::JoySlotInputAction");

#if defined(Q_OS_UNIX) && defined(WITH_X11)

    if (QApplication::platformName() == QStringLiteral("xcb"))
    {
        XInitThreads();
    }

#endif

    QFile logFile;
    QTextStream logFileStream;
    QTextStream outstream(stdout);
    QTextStream errorstream(stderr);

    // If running Win version, check if an explicit style
    // was defined on the command-line. If so, make a note
    // of it.
#ifdef Q_OS_WIN
    bool styleChangeFound = false;
    for (int i=0; (i < argc) && !styleChangeFound; i++)
    {
        char *tempchrstr = argv[i];
        QString temp = QString::fromUtf8(tempchrstr);

        if (temp == "-style")
        {
            styleChangeFound = true;
        }
    }
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate("antimicro", "Graphical program used to map keyboard buttons and mouse controls to a gamepad. Useful for playing games with no gamepad support."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({
            // A boolean option with a single name (-p)
            {"tray",
                QCoreApplication::translate("main", "Launch program in system tray only.")},
            // A boolean option with multiple names (-f, --force)
            {"no-tray",
                QCoreApplication::translate("main", "Launch program with the tray menu disabled")},
            // An option with a value
            {"hidden",
                QCoreApplication::translate("main", "Launch program without the main window displayed")},
            {"profile",
                QCoreApplication::translate("main", "Launch program with the configuration file selected as the default for selected controllers. Defaults to all controllers"),
                QCoreApplication::translate("main", "location")},
            {"profile-controller",
                QCoreApplication::translate("main", "Apply configuration file to a specific controller. Value can be a controller index, name, or GUID"),
                QCoreApplication::translate("main", "value")},
            {"unload",
                QCoreApplication::translate("main", "Unload currently enabled profile(s)"),
                QCoreApplication::translate("main", "value(s)")},
            {"startSet",
                QCoreApplication::translate("main", "Start joysticks on a specific set. Value can be a controller index, name, or GUID"),
                QCoreApplication::translate("main", "number value")},
            {{"daemon","d"},
                QCoreApplication::translate("main", "Launch program as a daemon. Use only on Linux.")},
            {"log-level",
                QCoreApplication::translate("main", "Enable logging"),
                QCoreApplication::translate("main", "log-type")},
            {"log-file",
                QCoreApplication::translate("main", "Choose a file for logs writing"),
                QCoreApplication::translate("main", "filename")},
            {"eventgen",
                QCoreApplication::translate("main", "Choose between using XTest support and uinput support for event generation. Use only if you have enabled xtest and uinput options on Linux or vmulti on Windows. Default: xtest."),
                QCoreApplication::translate("main", "event-generation-type"),
                "xtest"}, // default
            {{"list","l"},
                QCoreApplication::translate("main", "Print information about joysticks detected by SDL. Use only if you have sdl library. You can check your controller index, name or even GUID.")},
           // {"display",
           //     QCoreApplication::translate("main", "Use specified display for X11 calls")},
           // {"next",
           //     QCoreApplication::translate("main", "Advance profile loading set options")},
          //  {"map",
          //      QCoreApplication::translate("main", "Open game controller mapping window of selected controller. Value can be a controller index or GUID."),
          //      QCoreApplication::translate("main", "value")},
        });


    parser.process(antimicro);

    CommandLineUtility cmdutility;
    cmdutility.parseArguments(&parser);

    Logger appLogger(&outstream, &errorstream);

    // If a log level wasn't specified at the command-line, then use a default.
    if( cmdutility.getCurrentLogLevel() == Logger::LOG_NONE )
    {
        appLogger.setLogLevel( Logger::LOG_WARNING );
    }
    else if (cmdutility.getCurrentLogLevel() != appLogger.getCurrentLogLevel())
    {
        appLogger.setLogLevel(cmdutility.getCurrentLogLevel());
    }

    if( !cmdutility.getCurrentLogFile().isEmpty() )
    {
          appLogger.setCurrentLogFile( cmdutility.getCurrentLogFile() );
          appLogger.setCurrentErrorStream(nullptr);
    }

    Q_INIT_RESOURCE(resources);


#if defined(Q_OS_WIN) && defined(WIN_PORTABLE_PACKAGE)

    // If in portable mode, make sure the current directory is the same as the
    // config directory. This is to ensure that all relative paths resolve
    // correctly when loading on startup.

    QDir::setCurrent( PadderCommon::configPath() );
#endif

    QDir configDir(PadderCommon::configPath());

    if (!configDir.exists())
    {
        configDir.mkpath(PadderCommon::configPath());
    }

    QMap<SDL_JoystickID, InputDevice*> *joysticks = new QMap<SDL_JoystickID, InputDevice*>();
    QThread *inputEventThread = nullptr;

    // Cross-platform way of performing IPC. Currently,
    // only establish a connection and then disconnect.
    // In the future, there might be a reason to actually send
    // messages to the QLocalServer.
    QLocalSocket socket;

    if ((socket.serverName() == QString()))
    {
        socket.connectToServer(PadderCommon::localSocketKey);

        if (!socket.waitForConnected(3000))
        {
            qDebug() << "Socket's state: " << socket.state() << endl;
            qDebug() << "Server name: " << socket.serverName() << endl;
            qDebug() << "Socket descriptor: " << socket.socketDescriptor() << endl;
            qDebug() <<  "The connection hasn't been established: \nerror text -> " << socket.error() << "\nerror text 2 ->" << socket.errorString() << endl;
        }
        else
        {
            qDebug() << "Socket connected" << endl;
        }
    }
    else
    {
        socket.abort();
    }

    if (!socket.isValid())
    {
        qDebug() << "Socket is not valid" << endl;
        qDebug() << "Socket's state: " << socket.state() << endl;
        qDebug() << "Server name: " << socket.serverName() << endl;
        qDebug() << "Socket descriptor: " << socket.socketDescriptor() << endl;
    }

    if (socket.state() == QLocalSocket::ConnectedState)
    {
        // An instance of this program is already running.
        // Save app config and exit.
        AntiMicroSettings settings(PadderCommon::configFilePath(), QSettings::IniFormat);
	
        // Update log info based on config values
        if( cmdutility.getCurrentLogLevel() == Logger::LOG_NONE &&
            settings.contains("LogLevel"))
        {
            appLogger.setLogLevel( static_cast<Logger::LogLevel>(settings.value("LogLevel").toInt()) );
        }

        if( cmdutility.getCurrentLogFile().isEmpty() &&
            settings.contains("LogFile"))
        {
            appLogger.setCurrentLogFile( settings.value("LogFile").toString() );
            appLogger.setCurrentErrorStream(nullptr);
        }

        QPointer<InputDaemon> joypad_worker = new InputDaemon(joysticks, &settings, false);
        MainWindow mainWindow(joysticks, &cmdutility, &settings, false);
        mainWindow.fillButtons();
        mainWindow.alterConfigFromSettings();

        if (!cmdutility.hasError() &&
           (cmdutility.hasProfile() || cmdutility.hasProfileInOptions()))
        {
            mainWindow.saveAppConfig();
        }
        else if (!cmdutility.hasError() && cmdutility.isUnloadRequested())
        {
            mainWindow.saveAppConfig();
        }

        mainWindow.removeJoyTabs();
        QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(), &InputDaemon::quit);
        QTimer::singleShot(50, &antimicro, &QApplication::quit);

        int result = antimicro.exec();

        settings.sync();
        socket.disconnectFromServer();
        if (socket.waitForDisconnected(2000)) qDebug() << "Socket " << socket.socketDescriptor() << " disconnected!" << endl;
        deleteInputDevices(joysticks);

        delete joysticks;
        joysticks = nullptr;

        if (!joypad_worker.isNull())
        {
            delete joypad_worker;
            joypad_worker.clear();
        }

        return result;
    }

    LocalAntiMicroServer *localServer = nullptr;

#ifdef Q_OS_UNIX
    if (cmdutility.launchAsDaemon())
    {
        pid_t pid, sid;
        pid = fork(); // Fork the Parent Process

        if (pid == 0)
        {
            appLogger.LogInfo(QObject::trUtf8("Daemon launched"), true, true);

            localServer = new LocalAntiMicroServer();
            localServer->startLocalServer();
        }
        else if (pid < 0)
        {
            appLogger.LogError(QObject::trUtf8("Failed to launch daemon"), true, true);

            deleteInputDevices(joysticks);
            delete joysticks;
            joysticks = nullptr;

            exit(EXIT_FAILURE);
        }
        else if (pid > 0) // We got a good pid, Close the Parent Process
        {
            appLogger.LogInfo(QObject::trUtf8("Launching daemon"), true, true);

            deleteInputDevices(joysticks);
            delete joysticks;
            joysticks = nullptr;

            exit(EXIT_SUCCESS);
        }


    #ifdef WITH_X11

        if (QApplication::platformName() == QStringLiteral("xcb"))
        {

            if (cmdutility.getDisplayString().isEmpty())
            {
                X11Extras::getInstance()->syncDisplay();
            }
            else
            {
                X11Extras::setCustomDisplay(cmdutility.getDisplayString());
                X11Extras::getInstance()->syncDisplay();

                if (X11Extras::getInstance()->display() == nullptr)
                {
                    appLogger.LogError(QObject::trUtf8("Display string \"%1\" is not valid.")
                                       .arg(cmdutility.getDisplayString()), true, true);

                    deleteInputDevices(joysticks);
                    delete joysticks;
                    joysticks = nullptr;

                    delete localServer;
                    localServer = nullptr;

                    X11Extras::getInstance()->closeDisplay();

                    exit(EXIT_FAILURE);
                }
            }
        }

    #endif

        umask(0); //Change File Mask
        sid = setsid(); //Create a new Signature Id for our child

        if (sid < 0)
        {
            appLogger.LogError(QObject::trUtf8("Failed to set a signature id for the daemon"), true, true);

            deleteInputDevices(joysticks);
            delete joysticks;
            joysticks = nullptr;

            delete localServer;
            localServer = nullptr;

    #ifdef WITH_X11
            if (QApplication::platformName() == QStringLiteral("xcb"))
            {
                X11Extras::getInstance()->closeDisplay();
            }
    #endif

            exit(EXIT_FAILURE);
        }

        if ((chdir("/")) < 0)
        {
            appLogger.LogError(QObject::trUtf8("Failed to change working directory to /"), true, true);

            deleteInputDevices(joysticks);
            delete joysticks;
            joysticks = nullptr;

            delete localServer;
            localServer = nullptr;

    #ifdef WITH_X11

            if (QApplication::platformName() == QStringLiteral("xcb"))
            {
                X11Extras::getInstance()->closeDisplay();
            }
    #endif

            exit(EXIT_FAILURE);
        }

        //Close Standard File Descriptors
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }
    else
    {
        localServer = new LocalAntiMicroServer();
        localServer->startLocalServer();


    #ifdef WITH_X11

        if (QApplication::platformName() == QStringLiteral("xcb"))
        {
            if (!cmdutility.getDisplayString().isEmpty())
            {
                X11Extras::getInstance()->syncDisplay(cmdutility.getDisplayString());

                if (X11Extras::getInstance()->display() == nullptr)
                {
                    appLogger.LogError(QObject::trUtf8("Display string \"%1\" is not valid.")
                                       .arg(cmdutility.getDisplayString()), true, true);

                    deleteInputDevices(joysticks);
                    delete joysticks;
                    joysticks = nullptr;

                    delete localServer;
                    localServer = nullptr;

                    X11Extras::getInstance()->closeDisplay();

                    exit(EXIT_FAILURE);
                }
            }
        }

    #endif
    }

#elif defined(Q_OS_WIN)
    localServer = new LocalAntiMicroServer();
    localServer->startLocalServer();
#endif

    antimicro.setQuitOnLastWindowClosed(false);

    // If running Win version and no explicit style was
    // defined, use the style Fusion by default. I find the
    // windowsvista style a tad ugly
#ifdef Q_OS_WIN
    if (!styleChangeFound)
    {
        qApp->setStyle(QStyleFactory::create("Fusion"));
    }

    QIcon::setThemeName("/");
#elif defined(Q_OS_LINUX)

    QStringList appDirsLocations = QStandardPaths::standardLocations(QStandardPaths::DataLocation);
    QStringList themePathsTries = QStringList();

    QList<QString>::const_iterator i;

    for (i = appDirsLocations.constBegin(); i != appDirsLocations.constEnd(); ++i) {
        themePathsTries.append(QString("%1%2").arg(*i).arg("/icons"));
        qDebug() << QString("%1%2").arg(*i).arg("/icons");
    }

    QIcon::setThemeSearchPaths(themePathsTries);
    QIcon::setThemeName("hicolor");
    bool tr = QIcon::hasThemeIcon("games_config_custom"); // real
    bool tr2 = QIcon::hasThemeIcon("xxx"); // fake
    qDebug() << "Theme name: " << QIcon::themeName();
    qDebug() << "has icon theme named games_config_custom: " << tr;
    qDebug() << "if icon theme always returns true: " << tr2;


#endif

    AntiMicroSettings *settings = new AntiMicroSettings(PadderCommon::configFilePath(),
                                                        QSettings::IniFormat);
    settings->importFromCommandLine(cmdutility);

    // Update log info based on config values
    if( cmdutility.getCurrentLogLevel() == Logger::LOG_NONE &&
        settings->contains("LogLevel"))
    {
        appLogger.setLogLevel( static_cast<Logger::LogLevel>(settings->value("LogLevel").toInt()) );
    }

    if( cmdutility.getCurrentLogFile().isEmpty() &&
        settings->contains("LogFile"))
    {
        appLogger.setCurrentLogFile( settings->value("LogFile").toString() );
        appLogger.setCurrentErrorStream(nullptr);
    }

    QString targetLang = QLocale::system().name();

    if (settings->contains("Language"))
    {
        targetLang = settings->value("Language").toString();
    }

    QTranslator qtTranslator;

#if defined(Q_OS_UNIX)
    QString transPath = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
    
    if(QDir(transPath).entryInfoList(QDir::NoDotAndDotDot|QDir::AllEntries).count() == 0) 
    {
        qtTranslator.load(QString("qt_").append(targetLang), "/app/share/antimicro/translations");
    }
    else
    {
       qtTranslator.load(QString("qt_").append(targetLang), transPath); 
    }
    
#elif defined(Q_OS_WIN)
  #ifdef QT_DEBUG
    qtTranslator.load(QString("qt_").append(targetLang), QLibraryInfo::location(QLibraryInfo::TranslationsPath));
  #else
    qtTranslator.load(QString("qt_").append(targetLang),
                      QApplication::applicationDirPath().append("\\share\\qt\\translations"));
  #endif
#endif
    antimicro.installTranslator(&qtTranslator);

    QTranslator myappTranslator;

#if defined(Q_OS_UNIX)
    if(QDir("/app/share/antimicro").entryInfoList(QDir::NoDotAndDotDot|QDir::AllEntries).count() > 0) 
    {
        myappTranslator.load(QString("antimicro_").append(targetLang), "app/share/antimicro/translations"); 
    }
    else
    {
       myappTranslator.load(QString("antimicro_").append(targetLang), QApplication::applicationDirPath().append("/../share/antimicro/translations")); 
    }
    
#elif defined(Q_OS_WIN)
    myappTranslator.load(QString("antimicro_").append(targetLang), QApplication::applicationDirPath().append("\\share\\antimicro\\translations"));
#endif
    antimicro.installTranslator(&myappTranslator);

#ifndef Q_OS_WIN
    // Have program handle SIGTERM
    struct sigaction termaction;
    termaction.sa_handler = &termSignalTermHandler;
    sigemptyset(&termaction.sa_mask);
    termaction.sa_flags = 0;

    sigaction(SIGTERM, &termaction, nullptr);

    // Have program handle SIGINT
    struct sigaction termint;
    termint.sa_handler = &termSignalIntHandler;
    sigemptyset(&termint.sa_mask);
    termint.sa_flags = 0;

    sigaction(SIGINT, &termint, nullptr);

#endif

    if (cmdutility.shouldListControllers())
    {
        QPointer<InputDaemon> joypad_worker = new InputDaemon(joysticks, settings, false);
        AppLaunchHelper mainAppHelper(settings, false);
        mainAppHelper.printControllerList(joysticks);

        joypad_worker->quit();
        joypad_worker->deleteJoysticks();

        delete joysticks;
        joysticks = nullptr;

        delete localServer;
        localServer = nullptr;

    #ifdef WITH_X11

        if (QApplication::platformName() == QStringLiteral("xcb"))
        {
            X11Extras::getInstance()->closeDisplay();
        }

    #endif

        return 0;
    }
    else if (cmdutility.shouldMapController())
    {
        PadderCommon::mouseHelperObj.initDeskWid();
        QPointer<InputDaemon> joypad_worker = new InputDaemon(joysticks, settings);
        inputEventThread = new QThread;

        MainWindow *mainWindow = new MainWindow(joysticks, &cmdutility, settings);

        QObject::connect(&antimicro, &QApplication::aboutToQuit, mainWindow, &MainWindow::removeJoyTabs);
        QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(), &InputDaemon::quit);
        QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(),
                         &InputDaemon::deleteJoysticks, Qt::BlockingQueuedConnection);
        QObject::connect(&antimicro, &QApplication::aboutToQuit, &PadderCommon::mouseHelperObj,
                         &MouseHelper::deleteDeskWid, Qt::DirectConnection);
        QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(), &InputDaemon::deleteLater,
                         Qt::BlockingQueuedConnection);

        mainWindow->makeJoystickTabs();
        QTimer::singleShot(0, mainWindow, &MainWindow::controllerMapOpening);

        joypad_worker->startWorker();

        joypad_worker->moveToThread(inputEventThread);
        PadderCommon::mouseHelperObj.moveToThread(inputEventThread);
        inputEventThread->start(QThread::HighPriority);


        int app_result = antimicro.exec();

        appLogger.Log(); // Log any remaining messages if they exist.

        inputEventThread->quit();
        inputEventThread->wait();

        delete joysticks;
        joysticks = nullptr;

        delete localServer;
        localServer = nullptr;

        delete inputEventThread;
        inputEventThread = nullptr;

#ifdef WITH_X11

        if (QApplication::platformName() == QStringLiteral("xcb"))
        {
            X11Extras::getInstance()->closeDisplay();
        }

#endif

        delete mainWindow;
        mainWindow = nullptr;

        if (!joypad_worker.isNull())
        {
            delete joypad_worker;
            joypad_worker.clear();
        }

        return app_result;
    }

    bool status = true;
    QString eventGeneratorIdentifier = QString();
    AntKeyMapper *keyMapper = nullptr;
    EventHandlerFactory *factory = EventHandlerFactory::getInstance(cmdutility.getEventGenerator());

    if (!factory)
    {
        status = false;
    }
    else
    {
        eventGeneratorIdentifier = factory->handler()->getIdentifier();
        keyMapper = AntKeyMapper::getInstance(eventGeneratorIdentifier);
        status = factory->handler()->init();
        factory->handler()->printPostMessages();
    }

#if (defined(Q_OS_UNIX) && defined(WITH_UINPUT) && defined(WITH_XTEST)) || \
     defined(Q_OS_WIN)

    // Use fallback event handler.
    if (!status && cmdutility.getEventGenerator() != EventHandlerFactory::fallBackIdentifier())
    {
        QString eventDisplayName = EventHandlerFactory::handlerDisplayName(
                    EventHandlerFactory::fallBackIdentifier());
        appLogger.LogInfo(QObject::trUtf8("Attempting to use fallback option %1 for event generation.")
                                     .arg(eventDisplayName));

        if (keyMapper != nullptr)
        {
            keyMapper->deleteInstance();
            keyMapper = nullptr;
        }

        factory->deleteInstance();
        factory = EventHandlerFactory::getInstance(EventHandlerFactory::fallBackIdentifier());

        if (!factory)
        {
            status = false;
        }
        else
        {
            eventGeneratorIdentifier = factory->handler()->getIdentifier();
            keyMapper = AntKeyMapper::getInstance(eventGeneratorIdentifier);
            status = factory->handler()->init();
            factory->handler()->printPostMessages();
        }
    }
#endif

    if (!status)
    {
        appLogger.LogError(QObject::trUtf8("Failed to open event generator. Exiting."));
        appLogger.Log();

        deleteInputDevices(joysticks);
        delete joysticks;
        joysticks = nullptr;

        delete localServer;
        localServer = nullptr;

        if (keyMapper != nullptr)
        {
            keyMapper->deleteInstance();
            keyMapper = nullptr;
        }

#if defined(Q_OS_UNIX) && defined(WITH_X11)

        if (QApplication::platformName() == QStringLiteral("xcb"))
        {
            X11Extras::getInstance()->closeDisplay();
        }

#endif

        return EXIT_FAILURE;
    }
    else
    {
        appLogger.LogInfo(QObject::trUtf8("Using %1 as the event generator.")
                          .arg(factory->handler()->getName()));
    }

    PadderCommon::mouseHelperObj.initDeskWid();
    QPointer<InputDaemon> joypad_worker = new InputDaemon(joysticks, settings);
    inputEventThread = new QThread();

    MainWindow *mainWindow = new MainWindow(joysticks, &cmdutility, settings);


    mainWindow->setAppTranslator(&qtTranslator);
    mainWindow->setTranslator(&myappTranslator);

    AppLaunchHelper mainAppHelper(settings, mainWindow->getGraphicalStatus());

    QObject::connect(mainWindow, &MainWindow::joystickRefreshRequested, joypad_worker.data(), &InputDaemon::refresh);
    QObject::connect(joypad_worker.data(), &InputDaemon::joystickRefreshed, mainWindow, &MainWindow::fillButtonsID);
    QObject::connect(joypad_worker.data(), &InputDaemon::joysticksRefreshed, mainWindow, &MainWindow::fillButtonsMap);

    QObject::connect(&antimicro, &QApplication::aboutToQuit, localServer, &LocalAntiMicroServer::close);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, mainWindow, &MainWindow::saveAppConfig);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, mainWindow, &MainWindow::removeJoyTabs);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, &mainAppHelper, &AppLaunchHelper::revertMouseThread);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(), &InputDaemon::quit);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(), &InputDaemon::deleteJoysticks);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, joypad_worker.data(), &InputDaemon::deleteLater);
    QObject::connect(&antimicro, &QApplication::aboutToQuit, &PadderCommon::mouseHelperObj, &MouseHelper::deleteDeskWid,
                     Qt::DirectConnection);

#ifdef Q_OS_WIN
    QObject::connect(&antimicro, &QApplication::aboutToQuit, &mainAppHelper, &AppLaunchHelper::appQuitPointerPrecision);
#endif
    QObject::connect(localServer, &LocalAntiMicroServer::clientdisconnect, mainWindow, &MainWindow::handleInstanceDisconnect);
    QObject::connect(mainWindow, &MainWindow::mappingUpdated,
                     joypad_worker.data(), &InputDaemon::refreshMapping);
    QObject::connect(joypad_worker.data(), &InputDaemon::deviceUpdated,
                     mainWindow, &MainWindow::testMappingUpdateNow);

    QObject::connect(joypad_worker.data(), &InputDaemon::deviceRemoved,
                     mainWindow, &MainWindow::removeJoyTab);
    QObject::connect(joypad_worker.data(), &InputDaemon::deviceAdded,
                     mainWindow, &MainWindow::addJoyTab);

#ifdef Q_OS_WIN

    // Raise process priority. Helps reduce timer delays caused by
    // the running of other processes.

    bool raisedPriority = WinExtras::raiseProcessPriority();
    if (!raisedPriority)
    {
        appLogger.LogInfo(QObject::trUtf8("Could not raise process priority."));
    }
#endif

    mainAppHelper.initRunMethods();
    QTimer::singleShot(0, mainWindow, SLOT(fillButtons()));
    QTimer::singleShot(0, mainWindow, SLOT(alterConfigFromSettings()));
    QTimer::singleShot(0, mainWindow, SLOT(changeWindowStatus()));


    mainAppHelper.changeMouseThread(inputEventThread);

    joypad_worker->startWorker();

    joypad_worker->moveToThread(inputEventThread);
    PadderCommon::mouseHelperObj.moveToThread(inputEventThread);
    inputEventThread->start(QThread::HighPriority);


    int app_result = antimicro.exec();


    appLogger.Log(); // Log any remaining messages if they exist.
    appLogger.LogInfo(QObject::trUtf8("Quitting Program"), true, true);

    delete localServer;
    localServer = nullptr;

    inputEventThread->quit();
    inputEventThread->wait();

    delete inputEventThread;
    inputEventThread = nullptr;

    delete joysticks;
    joysticks = nullptr;

    AntKeyMapper::getInstance()->deleteInstance();

#if defined(Q_OS_UNIX) && defined(WITH_X11)

    if (QApplication::platformName() == QStringLiteral("xcb"))
    {
        X11Extras::getInstance()->closeDisplay();
    }

#endif

    EventHandlerFactory::getInstance()->handler()->cleanup();
    EventHandlerFactory::getInstance()->deleteInstance();

    delete mainWindow;
    mainWindow = nullptr;

    delete settings;
    settings = nullptr;

    if (!joypad_worker.isNull())
    {
        delete joypad_worker;
        joypad_worker.clear();
    }

    return app_result;
}
