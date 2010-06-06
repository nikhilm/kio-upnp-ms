#include "stattest.h"

#include <kmainwindow.h>
#include <kio/scheduler.h>
#include <kurl.h>
#include <kio/jobclasses.h>
#include <KApplication>
#include <KCmdLineArgs>
#include <KAboutData>
#include <KUrl>
#include <kdebug.h>
#include <kio/upnptypes.h>
 
stattest::stattest(const KUrl &url)
    : QObject(NULL)
    , m_url(url)
{
    QTimer *t = new QTimer(this);
    connect( t, SIGNAL(timeout()), this, SLOT(check()) );
    t->start( 3000 );
}

void stattest::check()
{
    kDebug() << "--------- Checking ---------";
    KIO::StatJob *job = KIO::stat(m_url);
    connect( job, SIGNAL(result(KJob *)), this, SLOT(done(KJob *)));
}

void stattest::done(KJob *job)
{
    kDebug() << "Done";
    if( job->error() ) {
        kDebug() << "ERROR!" << job->errorString();
    }
    KIO::StatJob *statJob = static_cast<KIO::StatJob*>( job );
    KIO::UDSEntry entry = statJob->statResult();
    kDebug() << "++++++++++" <<  entry.stringValue(KIO::UDSEntry::UDS_NAME) << entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE);
    // we don't really care about this
    // check debug output or monitor dbus to see if modification
    // signals were sent
}

int main (int argc, char *argv[])
{
  // KAboutData (const QByteArray &appName, const QByteArray &catalogName, const KLocalizedString &programName, const QByteArray &version,
  const QByteArray& ba=QByteArray("stattest");
  const KLocalizedString name=ki18n("myName");
  KAboutData aboutData( ba, ba, name, ba, name);
  KCmdLineArgs::init( argc, argv, &aboutData );

  KCmdLineOptions options;
  options.add("+[url]", ki18n("path"));
  KCmdLineArgs::addCmdLineOptions(options);

  KApplication khello;
 
  new stattest( KCmdLineArgs::parsedArgs()->url(0).url() );
  khello.exec();
}
