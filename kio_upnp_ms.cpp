/********************************************************************
 This file is part of the KDE project.

Copyright (C) 2010 Nikhil Marathe <nsm.nikhil@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

#include "kio_upnp_ms.h"

#include <cstdio>

#include <kdebug.h>
#include <kcomponentdata.h>
#include <kcmdlineargs.h>
#include <kaboutdata.h>
#include <KDirNotify>

#include <QCoreApplication>
#include <QtDBus>
#include <QXmlStreamReader>

#include <HAction>
#include <HActionArguments>
#include <HControlPoint>
#include <HControlPointConfiguration>
#include <HDeviceInfo>
#include <HDeviceProxy>
#include <HDiscoveryType>
#include <HEndpoint>
#include <HResourceType>
#include <HServiceId>
#include <HServiceProxy>
#include <HStateVariable>
#include <HUdn>
#include <HUpnp>

#include "deviceinfo.h"
#include "dbuscodec.h"
#include "didlparser.h"
#include "didlobjects.h"
#include "upnptypes.h"

using namespace Herqq::Upnp;

/**
 * Fill UDSEntry @c entry,
 * setting uint @c property by retrieving
 * the value from the item/container @c object's meta-data
 * where the key is @c name.
 */
#define FILL_METADATA(entry, property, object, name)\
    if( object->data().contains(name) )\
        entry.insert( property, object->data()[name] )

/**
 * Fill from resource attributes
 */
#define FILL_RESOURCE_METADATA(entry, property, object, name)\
    if( object->resource().contains(name) )\
        entry.insert( property, object->resource()[name] )

// because somebody made
// QThread::msleep protected
class Sleeper : public QThread
{
public:
    using QThread::msleep;
};

extern "C" int KDE_EXPORT kdemain( int argc, char **argv )
{
    Herqq::Upnp::SetLoggingLevel(Herqq::Upnp::Warning);
  qDBusRegisterMetaType<DeviceInfo>();

  KComponentData instance( "kio_upnp_ms" );
  QCoreApplication app( argc, argv );

  if (argc != 4)
  {
    fprintf( stderr, "Usage: kio_upnp_ms protocol domain-socket1 domain-socket2\n");
    exit( -1 );
  }
  
  UPnPMS slave( argv[2], argv[3] );
  slave.dispatchLoop();
  return 0;
}

void UPnPMS::get( const KUrl &url )
{
    kDebug() << url;
    error( KIO::ERR_CANNOT_OPEN_FOR_READING, url.prettyUrl() );
}

UPnPMS::UPnPMS( const QByteArray &pool, const QByteArray &app )
  : QObject(0)
  , SlaveBase( "upnp-ms", pool, app )
{
    HControlPointConfiguration config;
    config.setAutoDiscovery(false);
    m_controlPoint = new HControlPoint( config, this );
    Q_ASSERT( 
        connect(m_controlPoint,
                SIGNAL(rootDeviceOnline(Herqq::Upnp::HDeviceProxy *)),
                this,
                SLOT(rootDeviceOnline(Herqq::Upnp::HDeviceProxy *)))
    ); 

    if( !m_controlPoint->init() )
    {
      kDebug() << m_controlPoint->errorDescription();
      kDebug() << "Error initing control point";
    }

// TODO probably remove this, and instead react to errors, its a timeout
    QTimer::singleShot(1000, this, SIGNAL(done()));
    enterLoop();
}

void UPnPMS::rootDeviceOnline(HDeviceProxy *device)
{
  m_device = device;
  emit done();
}

void UPnPMS::enterLoop()
{
  QEventLoop loop;
  Q_ASSERT( connect( this, SIGNAL( done() ), &loop, SLOT( quit() ) ) );
  loop.exec( QEventLoop::ExcludeUserInputEvents );
  loop.disconnect();
}

/**
 * Updates device information from Cagibi and gets
 * HUPnP to find the device.
 */
// TODO update to return boolean
void UPnPMS::updateDeviceInfo( const KUrl& url )
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface iface( "org.kde.Cagibi", "/org/kde/Cagibi", "org.kde.Cagibi", bus );
    QString udn = "uuid:" + url.host();
    QDBusReply<DeviceInfo> res = iface.call("deviceDetails", udn);
    if( !res.isValid() ) {
        kDebug() << "Invalid request" << res.error().message();
      error(KIO::ERR_COULD_NOT_CONNECT, udn);
      return;
    }
    m_deviceInfo = res.value();
    if( m_deviceInfo.udn().isEmpty() ) {
        error( KIO::ERR_UNKNOWN_HOST, m_deviceInfo.udn() );
        return;
    }

    HDiscoveryType specific(m_deviceInfo.udn());
    // Stick to multicast, unicast is a UDA 1.1 feature
    // all devices don't support it
    // Thanks to Tuomo Penttinen for pointing that out
    if( !m_controlPoint->scan(specific) ) {
      kDebug() << m_controlPoint->errorDescription();
      error( KIO::ERR_UNKNOWN_HOST, m_deviceInfo.udn() );
      return;
    }
    enterLoop();
    kDebug() << "Device searching done";

    // connect to any state variables here
//    HStateVariable *systemUpdateID = contentDirectory()->stateVariableByName( "SystemUpdateID" );
//    kDebug() << "SystemUpdateID is" << systemUpdateID->value();
//    connect( systemUpdateID,
//             SIGNAL( valueChanged(const Herqq::Upnp::HStateVariableEvent&) ),
//             this,
//             SLOT( slotCDSUpdated(const Herqq::Upnp::HStateVariableEvent&) ) );
// 
//    HStateVariable *containerUpdates = contentDirectory()->stateVariableByName( "ContainerUpdateIDs" );
//    connect( containerUpdates,
//             SIGNAL( valueChanged(const Herqq::Upnp::HStateVariableEvent&) ),
//             this,
//             SLOT( slotContainerUpdates(const Herqq::Upnp::HStateVariableEvent&) ) );
}

/*
 * Returns a ContentDirectory service or NULL
 */
HServiceProxy* UPnPMS::contentDirectory() const
{
    HServiceProxy *contentDir = m_device->serviceProxyById( HServiceId("urn:schemas-upnp-org:serviceId:ContentDirectory") );
    if( contentDir == NULL ) {
        contentDir = m_device->serviceProxyById( HServiceId( "urn:upnp-org:serviceId:ContentDirectory" ) );
    }
    return contentDir;
}
  
bool UPnPMS::ensureDevice( const KUrl &url )
{
    if(  !m_deviceInfo.isValid()
      || ("uuid:" + url.host()) != m_deviceInfo.udn() ) {
        updateDeviceInfo(url);
        kDebug() << m_deviceInfo.isValid();
        // invalidate the cache when the device changes
        m_reverseCache.clear();
        m_reverseCache.insert( "", new DIDL::Container( "0", "-1", false ) );
        m_reverseCache.insert( "/", new DIDL::Container( "0", "-1", false ) );
    }

    return true;
}

void UPnPMS::stat( const KUrl &url )
{

    if( !ensureDevice( url ) ) {
        //TODO error()
        return;
    }

    DIDL::Object *obj = resolvePathToObject( url.path(KUrl::RemoveTrailingSlash) );
    if( obj != NULL ) {
        KIO::UDSEntry entry;
        entry.insert( KIO::UDSEntry::UDS_NAME, obj->title() );
        if( obj->type() == DIDL::SuperObject::Container )
            entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR );
        else
            entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG );
        statEntry( entry );
        finished();
    }
    else {
        error( KIO::ERR_DOES_NOT_EXIST, url.path() );
    }
}

void UPnPMS::listDir( const KUrl &url )
{
    kDebug() << url;
  if( url.host().isEmpty() ) {
    error(KIO::ERR_UNKNOWN_HOST, QString());
  }
  else {
    browseDevice( url );
  }
}

void UPnPMS::browseDevice( const KUrl &url )
{
    ensureDevice( url );
    if( !m_device ) {
// TODO once ensureDevice() returns proper status, use that for check
      error( KIO::ERR_DOES_NOT_EXIST, url.prettyUrl() );
      return;
    }

    QString path = url.path(KUrl::RemoveTrailingSlash);

    // TODO Use path to decide
    QString idString = resolvePathToId(path);

    if( idString.isNull() ) {
        error( KIO::ERR_DOES_NOT_EXIST, path );
        return;
    }

    HActionArguments output = browseDevice( idString,
                                            BROWSE_DIRECT_CHILDREN,
                                            "*",
                                            0,
                                            0,
                                            "" );
    if( output["Result"] == NULL ) {
        kDebug() << m_lastErrorString;
        error( KIO::ERR_SLAVE_DEFINED, m_lastErrorString );
        return;
    }
    createDirectoryListing( output["Result"]->value().toString() );
}

HActionArguments UPnPMS::browseDevice( const QString &id,
                                       const QString &browseFlag,
                                       const QString &filter,
                                       const int startIndex,
                                       const int requestedCount,
                                       const QString &sortCriteria )
{
    HActionArguments output;

    if( contentDirectory() == NULL ) {
            error( KIO::ERR_UNSUPPORTED_ACTION, "UPnPMS device " + m_device->deviceInfo().friendlyName() + " does not support browsing" );
            return output;
    }
   
    HAction *browseAct = contentDirectory()->actionByName( "Browse" );
    HActionArguments args = browseAct->inputArguments();
  
    args["ObjectID"]->setValue( id );
    args["BrowseFlag"]->setValue( browseFlag );
    args["Filter"]->setValue( filter );
    args["StartingIndex"]->setValue( startIndex );
    args["RequestedCount"]->setValue( requestedCount );
    args["SortCriteria"]->setValue( sortCriteria );
   
    Q_ASSERT(connect( browseAct, SIGNAL( invokeComplete( const Herqq::Upnp::HAsyncOp& ) ),
                      this, SIGNAL( done() ) ));
    HAsyncOp invocationOp = browseAct->beginInvoke( args );
    invocationOp.setWaitTimeout( 2000 );
    enterLoop();
   
    Q_ASSERT(browseAct->disconnect());
    bool ret = browseAct->waitForInvoke( &invocationOp, &output );

    if( !ret || 
        ( invocationOp.waitCode() != HAsyncOp::WaitSuccess ) ) {
        m_lastErrorString = browseAct->errorCodeToString( invocationOp.returnValue() );
    }
    else {
        m_lastErrorString = QString();
    }

    // TODO check for success
    return output;
}

void UPnPMS::slotParseError( const QString &errorString )
{
    error(KIO::ERR_SLAVE_DEFINED, errorString);
}

void UPnPMS::slotListFillCommon( KIO::UDSEntry &entry, DIDL::Object *obj )
{
    entry.insert( KIO::UDSEntry::UDS_NAME, obj->title() );
    long long access = 0;
    // perform all permissions checks here

    access |= S_IRUSR | S_IRGRP | S_IROTH;

    entry.insert( KIO::UDSEntry::UDS_ACCESS, access );

    if( !obj->upnpClass().isNull() ) {
        entry.insert( KIO::UPNP_CLASS, obj->upnpClass() );
    }
}

void UPnPMS::slotListContainer( DIDL::Container *c )
{
    KIO::UDSEntry entry;
    slotListFillCommon( entry, c );
    entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR );

    // TODO insert attributes into meta-data in parser
    // or childCount won't be available
    FILL_METADATA(entry, KIO::UPNP_ALBUM_CHILDCOUNT, c, "childCount");
    listEntry(entry, false);
}

void UPnPMS::slotListItem( DIDL::Item *item )
{
    KIO::UDSEntry entry;
    slotListFillCommon( entry, item );
    entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG );
    if( item->hasResource() ) {
        DIDL::Resource res = item->resource();
        entry.insert( KIO::UDSEntry::UDS_MIME_TYPE, res["mimetype"] );
        entry.insert( KIO::UDSEntry::UDS_SIZE, res["size"].toULongLong() );
        entry.insert( KIO::UDSEntry::UDS_TARGET_URL, res["uri"] );

        // TODO extra meta-data
    }
    else {
        long long access = entry.numberValue( KIO::UDSEntry::UDS_ACCESS );
        // undo slotListFillCommon
        access ^= S_IRUSR | S_IRGRP | S_IROTH;
        entry.insert( KIO::UDSEntry::UDS_ACCESS, access );
    }

    FILL_METADATA(entry, KIO::UPNP_CREATOR, item, "creator");
// if the artist exists, choose the artist
    FILL_METADATA(entry, KIO::UPNP_CREATOR, item, "artist");
    FILL_METADATA(entry, KIO::UPNP_ALBUM, item, "album");
    FILL_METADATA(entry, KIO::UPNP_GENRE, item, "genre");
    FILL_METADATA(entry, KIO::UPNP_TRACK_NUMBER, item, "originalTrackNumber");
// TODO processing
    FILL_METADATA(entry, KIO::UPNP_DATE, item, "date");

    FILL_RESOURCE_METADATA(entry, KIO::UPNP_DURATION, item, "duration");
    FILL_RESOURCE_METADATA(entry, KIO::UPNP_IMAGE_RESOLUTION, item, "resolution");
    FILL_METADATA(entry, KIO::UPNP_CHANNEL_NAME, item, "channelName");
    FILL_METADATA(entry, KIO::UPNP_CHANNEL_NUMBER, item, "channelNr");
    listEntry(entry, false);
}

void UPnPMS::slotListDirDone()
{
    kDebug() << "DONE";
    listEntry(KIO::UDSEntry(), true);
    finished();
}

void UPnPMS::createDirectoryListing( const QString &didlString )
{
    DIDL::Parser parser;
    Q_ASSERT( connect( &parser, SIGNAL(error( const QString& )), this, SLOT(slotParseError( const QString& )) ) );
    Q_ASSERT( connect( &parser, SIGNAL(done()), this, SLOT(slotListDirDone()) ) );

    Q_ASSERT( connect( &parser, SIGNAL(containerParsed(DIDL::Container *)), this, SLOT(slotListContainer(DIDL::Container *)) ) );
    Q_ASSERT( connect( &parser, SIGNAL(itemParsed(DIDL::Item *)), this, SLOT(slotListItem(DIDL::Item *)) ) );
    parser.parse(didlString);
}

QString UPnPMS::idForName( const QString &name )
{
    if( m_reverseCache.contains( name ) )
        return m_reverseCache[name]->id();
    return QString();
}

/**
 * Tries to resolve a complete path to the right
 * Object for the path. Tries to use the cache.
 * If there is cache miss, backtracks along the path
 * or queries the UPnP device.
 * If not found, returns NULL
 */
DIDL::Object* UPnPMS::resolvePathToObject( const QString &path )
{
#define SEP_POS( string, from ) string.indexOf( QDir::separator(), (from) )
#define LAST_SEP_POS( string, from ) string.lastIndexOf( QDir::separator(), (from) )

    int from = -1; // see QString::lastIndexOf()

    QString startAt;

    // path is without a trailing slash, but we still want
    // to check for the last part of the path
    // to avoid a mandatory UPnP call. So the do { } while;
    int subpathLength = path.length();
    do {
        QString segment = path.left(subpathLength);
        QString id = idForName( segment );
        if( !id.isNull() ) {
            // we already had it cached
            // this only happens on the first loop run
            if( id == idForName( path ) ) {
                return m_reverseCache[path];
            }
            else {
                // we know 'a' ID, but not the one we want.
                // we can go forward from this point,
                // so break out of the loop
                startAt = segment;
                break;
            }
        }
        else {
            // well if its null, see if any parent is non null,
            // so just continue
            // don't delete this branch from the code,
            // it helps to understand
            // and the compiler will optimize it out anyway
        }

        from = -(path.length() - subpathLength + 1);
    } while( (subpathLength = LAST_SEP_POS( path, from ) ) != -1 );


// TODO
// most CDS support Search() on basic attributes
// check it, and if allowed, use Search
// but remember to handle multiple results
    from = SEP_POS( path, startAt.length() ) ;
    do {
        QString segment = path.left( from );
        // from is now the position of the slash, skip it
        from++;
        m_resolveLookingFor = path.mid( from, SEP_POS( path, from ) - from );
        m_resolvedObject = NULL;
        HActionArguments results = browseDevice( idForName(segment),
                                                 BROWSE_DIRECT_CHILDREN,
                                                 "*",
                                                 0,
                                                 0,
                                                 "dc:title" );
        // TODO check error
        if( results["Result"] == NULL ) {
            kDebug() << "Error:" << m_lastErrorString;
            error( KIO::ERR_SLAVE_DEFINED, m_lastErrorString );
            return NULL;
        }

        DIDL::Parser parser;
        Q_ASSERT( connect( &parser, SIGNAL(itemParsed(DIDL::Item *)),
                           this, SLOT(slotResolveId(DIDL::Item *)) ) );
        Q_ASSERT( connect( &parser, SIGNAL(containerParsed(DIDL::Container *)),
                 this, SLOT(slotResolveId(DIDL::Container *)) ) );

        parser.parse( results["Result"]->value().toString() );

        // we sleep because devices ( atleast MediaTomb )
        // seem to block continous TCP connections after some time
        // this interval might need modification
        Sleeper::msleep(500);

        // TODO have some kind of slot to stop the parser as 
        // soon as we find our guy, so that the rest of the
        // document isn't parsed.
        
        // if we didn't find the ID, no point in continuing
        if( m_resolvedObject == NULL ) {
            return NULL;
        }
        else {
            m_reverseCache.insert( ( segment + QDir::separator() + m_resolvedObject->title() ), m_resolvedObject );
            from = SEP_POS( path, ( segment + QDir::separator() + m_resolvedObject->title() ).length() );
            // ignore trailing slashes
            if( from == path.length()-1 ) {
                from = -1;
            }
        }
    } while( from != -1 );

    return m_resolvedObject;

#undef SEP_POS
#undef LAST_SEP_POS
}

QString UPnPMS::resolvePathToId( const QString &path )
{
    DIDL::Object *obj = resolvePathToObject( path );
    if( obj != NULL )
        return obj->id();
    return QString();
}

void UPnPMS::slotResolveId( DIDL::Object *object )
{
    // set m_resolvedId and update cache
    if( object->title() == m_resolveLookingFor ) {
        m_resolvedObject = object;
    }
}

void UPnPMS::slotResolveId( DIDL::Item *object )
{
    slotResolveId( static_cast<DIDL::Object*>( object ) );
}

void UPnPMS::slotResolveId( DIDL::Container *object )
{
    slotResolveId( static_cast<DIDL::Object*>( object ) );
}

void UPnPMS::slotCDSUpdated( const HStateVariableEvent &event )
{
    kDebug() << "UPDATE" << event.newValue();
}

void UPnPMS::slotContainerUpdates( const Herqq::Upnp::HStateVariableEvent& event )
{
    kDebug() << "UPDATED containers" << event.newValue();
    OrgKdeKDirNotifyInterface::emitFilesChanged( QStringList()<< "b.txt");
}
