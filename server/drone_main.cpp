/*
Copyright (C) 2010 Srivats P.

This file is part of "Ostinato"

This is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "drone.h"

#include "../common/protocolmanager.h"
#include "settings.h"

#include <google/protobuf/stubs/common.h>

#include <QCoreApplication>
#include <QFile>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

#include <QtDebug>

extern ProtocolManager *OstProtocolManager;
extern char *version;
extern char *revision;

Drone *drone;
QSettings *appSettings;
int myport;

void cleanup(int /*signum*/)
{
    QCoreApplication::instance()->exit(-1);
}

int main(int argc, char *argv[])
{
    int exitCode = 0;
    QCoreApplication app(argc, argv);

    // TODO: command line options
    // -v (--version)
    // -h (--help)
    // -p (--portnum)
    if (argc > 1)
        myport = atoi(argv[1]);

    app.setApplicationName("Drone");
    app.setOrganizationName("academy");

    /* (Portable Mode) If we have a .ini file in the same directory as the 
       executable, we use that instead of the platform specific location
       and format for the settings */
    QString portableIni = QCoreApplication::applicationDirPath() 
            + "/drone.ini";
    if (QFile::exists(portableIni))
        appSettings = new QSettings(portableIni, QSettings::IniFormat);
    else
        appSettings = new QSettings(QSettings::IniFormat, 
                                    QSettings::UserScope,
                                    app.organizationName(), 
                                    app.applicationName().toLower());

    drone = new Drone();
    OstProtocolManager = new ProtocolManager();

    if (!drone->init())
    {
        exitCode = -1;
        goto _exit;
    }

    qDebug("Version: %s", version);
    qDebug("Revision: %s", revision);

#ifdef Q_OS_UNIX
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cleanup;
    if (sigaction(SIGTERM, &sa, NULL))
        qDebug("Failed to install SIGTERM handler. Cleanup may not happen!!!");
    if (sigaction(SIGINT, &sa, NULL))
        qDebug("Failed to install SIGINT handler. Cleanup may not happen!!!");
#endif

    exitCode = app.exec();

_exit:
    delete drone;
    delete OstProtocolManager;

    google::protobuf::ShutdownProtobufLibrary();

    return exitCode;
} 

