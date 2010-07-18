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

#include "controlpointthread.h"

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
#include "objectcache.h"
#include "persistentaction.h"

using namespace Herqq::Upnp;

/**
 * Fill UDSEntry @c entry,
 * setting uint @c property by retrieving
 * the value from the item/container @c object's meta-data
 * where the key is @c name.
 */
static inline void fillMetadata( KIO::UDSEntry &entry, uint property,
                          const DIDL::Object *object, const QString &name )
{
    if( object->data().contains(name) )
        entry.insert( property, object->data()[name] );
}

/**
 * Fill from resource attributes
 */
static inline void fillResourceMetadata( KIO::UDSEntry &entry, uint property,
                                         const DIDL::Item *object, const QString &name )
{
    if( object->resource().contains(name) )
        entry.insert( property, object->resource()[name] );
}

namespace SearchRegExp
{
// create and keep

// this isn't perfect, we skip certain things
QString wchar("[\t\v\n\r\f ]");
QString relOp("=|!=|<|<=|>|>=");

QString quotedVal( "\"[^\"]*\"" ); // for example, no escape double quote checks
QString stringOp("contains|doesNotContain|derivedfrom");
QString existsOp("exists");
QString boolVal("\"(?:true|false)\"");

QString binOp( "(?:(?:" + relOp + ")|(?:" + stringOp + "))" );

QString property( "\\S*" );

QString relExp1( "(" + property + ")" + wchar + "+" + binOp + wchar + "+" + quotedVal );
QString relExp2( "(" + property + ")" + wchar + "+" + existsOp + wchar + "+" + boolVal );

QString relExp( "(?:" + relExp1 + ")" + "|" + "(?:" + relExp2 + ")" );

QRegExp searchCriteria(
"\\("
+ wchar + "*"
+ "(" + relExp + ")"
+ wchar + "*"
+ "\\)"
    );
}

ControlPointThread::ControlPointThread( QObject *parent )
    : QThread( parent )
    , m_controlPoint( 0 )
    , m_searchListingCounter( 0 )
{
    //Herqq::Upnp::SetLoggingLevel( Herqq::Upnp::Debug );
    qRegisterMetaType<KIO::UDSEntry>();
    qRegisterMetaType<Herqq::Upnp::HActionArguments>();
    qDBusRegisterMetaType<DeviceInfo>();

    start();

    // necessary due to Qt's concept of thread affinity
    // since ControlPointThread is created in UPnPMS, it
    // belongs to the main thread. We reparent it to the
    // ControlPointThread.
    QObject::moveToThread(this);
}

ControlPointThread::~ControlPointThread()
{
}

void ControlPointThread::run()
{
    // Again for reasons of thread affinity
    // HControlPoint creation and destruction
    // should take place in run()
    HControlPointConfiguration config;
    config.setAutoDiscovery(false);
    m_controlPoint = new HControlPoint( config, this );
    connect(m_controlPoint,
            SIGNAL(rootDeviceOnline(Herqq::Upnp::HDeviceProxy *)),
            this,
            SLOT(rootDeviceOnline(Herqq::Upnp::HDeviceProxy *)));
    connect(m_controlPoint,
            SIGNAL(rootDeviceOffline(Herqq::Upnp::HDeviceProxy *)),
            this,
            SLOT(rootDeviceOffline(Herqq::Upnp::HDeviceProxy *)));

    if( !m_controlPoint->init() )
    {
      kDebug() << m_controlPoint->errorDescription();
      kDebug() << "Error initing control point";
    }

    exec();

    foreach( MediaServerDevice dev, m_devices ) {
        delete dev.cache;
        dev.cache = NULL;
    }
    delete m_controlPoint;
}

void ControlPointThread::rootDeviceOnline(HDeviceProxy *device) // SLOT
{
    kDebug() << "Received device " << device->deviceInfo().udn().toString();

    // NOTE as a reference!
    MediaServerDevice &dev = m_devices[device->deviceInfo().udn().toSimpleUuid()];
    dev.device = device;
    dev.cache = new ObjectCache( this );

    HStateVariable *systemUpdateID = contentDirectory(dev.device)->stateVariableByName( "SystemUpdateID" );
    connect( systemUpdateID,
             SIGNAL( valueChanged(const Herqq::Upnp::HStateVariableEvent&) ),
             this,
             SLOT( slotCDSUpdated(const Herqq::Upnp::HStateVariableEvent&) ) );
 
    HStateVariable *containerUpdates = contentDirectory(dev.device)->stateVariableByName( "ContainerUpdateIDs" );
    if( containerUpdates ) {
        bool ok = connect( containerUpdates,
                           SIGNAL( valueChanged(const Herqq::Upnp::HStateVariableEvent&) ),
                           this,
                           SLOT( slotContainerUpdates(const Herqq::Upnp::HStateVariableEvent&) ) );
        Q_ASSERT( ok );
    }
    else {
        kDebug() << dev.deviceInfo.friendlyName() << "does not support updates";
    }

    PersistentAction *action = new PersistentAction;

    HAction *searchCapAction = contentDirectory(dev.device)->actionByName( "GetSearchCapabilities" );
    Q_ASSERT( searchCapAction );

    connect( action,
             SIGNAL( invokeComplete( Herqq::Upnp::HActionArguments, Herqq::Upnp::HAsyncOp, bool, QString ) ),
             this,
             SLOT( searchCapabilitiesInvokeDone( Herqq::Upnp::HActionArguments, Herqq::Upnp::HAsyncOp, bool, QString ) ) );

    HActionArguments input = searchCapAction->inputArguments();

    action->invoke( searchCapAction, input, dev.device );
}

void ControlPointThread::searchCapabilitiesInvokeDone( Herqq::Upnp::HActionArguments output, Herqq::Upnp::HAsyncOp op, bool ok, QString errorString ) // SLOT
{
    Q_UNUSED( op );
    if( !ok ) {
        emit error( KIO::ERR_SLAVE_DEFINED, "Could not invoke GetSearchCapabilities(): " + errorString );
        return;
    }

    PersistentAction *action = static_cast<PersistentAction *>( QObject::sender() );
    action->deleteLater();

    QString reply = output["SearchCaps"]->value().toString();

    // NOTE as a reference!
    HDeviceProxy *device = (HDeviceProxy *) op.userData();
    Q_ASSERT( device );
    MediaServerDevice &dev = m_devices[device->deviceInfo().udn().toSimpleUuid()];
    dev.searchCapabilities = reply.split(",", QString::SkipEmptyParts);

    emit deviceReady();
}

void ControlPointThread::rootDeviceOffline(HDeviceProxy *device) // SLOT
{
    // if we aren't valid, we don't really care about
    // devices going offline
    // This slot can get called twice by HUpnp
    QString uuid = device->deviceInfo().udn().toSimpleUuid();
    if( m_devices.contains( uuid ) ) {
        kDebug() << "Removing" << uuid;
        if( m_currentDevice.device->deviceInfo().udn() == device->deviceInfo().udn() ) {
            kDebug() << "Was current device - invalidating";
            m_currentDevice.device = NULL;
            m_currentDevice.deviceInfo = DeviceInfo();
        }

        m_devices.remove( uuid );
    }
}

/**
 * Updates device information from Cagibi and gets
 * HUPnP to find the device.
 */
bool ControlPointThread::updateDeviceInfo( const KUrl& url )
{
    kDebug() << "Updating device info for " << url;

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface iface( "org.kde.Cagibi", "/org/kde/Cagibi", "org.kde.Cagibi", bus );
    QString udn = "uuid:" + url.host();
    QDBusReply<DeviceInfo> res = iface.call("deviceDetails", udn);
    if( !res.isValid() ) {
        kDebug() << "Invalid request" << res.error().message();
        emit error(KIO::ERR_COULD_NOT_CONNECT, udn);
        return false;
    }

    DeviceInfo deviceInfo = res.value();
    if( deviceInfo.udn().isEmpty() ) {
        emit error( KIO::ERR_COULD_NOT_MOUNT, i18n( "Device %1 is offline", url.host() ) );
        return false;
    }

    // the device is definitely present, so we let the scan fill in
    // remaining details
    MediaServerDevice dev;
    dev.device = NULL;
    dev.deviceInfo = deviceInfo;
    dev.cache = NULL;
    dev.searchCapabilities = QStringList();
    m_devices[url.host()] = dev;

    HDiscoveryType specific(deviceInfo.udn());
    // Stick to multicast, unicast is a UDA 1.1 feature
    // all devices don't support it
    // Thanks to Tuomo Penttinen for pointing that out
    if( !m_controlPoint->scan(specific) ) {
        kDebug() << m_controlPoint->errorDescription();
        emit error( KIO::ERR_COULD_NOT_MOUNT, i18n( "Device %1 is offline", url.host() ) );
        return false;
    }

    // local blocking event loop.
    // This is the only point at which the ControlPointThread
    // ever blocks. Until we have a device, there is no point
    // in continuing processing.
    QEventLoop local;
    connect(this,
            SIGNAL(deviceReady()),
            &local,
            SLOT(quit()));
    local.exec();

    kDebug() << "+++ REeturning from loop";
    return true;
}

/*
 * Returns a ContentDirectory service or 0
 */
HServiceProxy* ControlPointThread::contentDirectory(HDeviceProxy *forDevice) const
{
    HDeviceProxy *device = forDevice;
    if( !device )
        device = m_currentDevice.device;
    Q_ASSERT( device );
    HServiceProxy *contentDir = device->serviceProxyById( HServiceId("urn:schemas-upnp-org:serviceId:ContentDirectory") );
    if( !contentDir ) {
        contentDir = device->serviceProxyById( HServiceId( "urn:upnp-org:serviceId:ContentDirectory" ) );
    }
    return contentDir;
}

HAction* ControlPointThread::browseAction() const
{
    Q_ASSERT( contentDirectory() );
    return contentDirectory()->actionByName( "Browse" );
}

HAction* ControlPointThread::searchAction() const
{
    Q_ASSERT( contentDirectory() );
    return contentDirectory()->actionByName( "Search" );
}

bool ControlPointThread::ensureDevice( const KUrl &url )
{
    if( ("uuid:" + url.host()) == m_currentDevice.deviceInfo.udn() )
        return true;

    if( m_devices.contains( url.host() ) ) {
        kDebug() << "We already know of device" << url.host();
        m_currentDevice = m_devices[url.host()];
        return true;
    }

    if( updateDeviceInfo(url) ) {
        // make this the current device
        m_currentDevice = m_devices[url.host()];
        return true;
    }

    return false;
}

/////////////////////////
////       Stat      ////
/////////////////////////

void ControlPointThread::stat( const KUrl &url )
{
    if( !ensureDevice( url ) ) {
      emit error( KIO::ERR_DOES_NOT_EXIST, url.prettyUrl() );
      return;
    }

    QString path = url.path(KUrl::RemoveTrailingSlash);
    kDebug() << url << path;
    connect( m_currentDevice.cache, SIGNAL( pathResolved( const DIDL::Object * ) ),
             this, SLOT( statResolvedPath( const DIDL::Object * ) ) );

    m_currentDevice.cache->resolvePathToObject( path );
}

void ControlPointThread::statResolvedPath( const DIDL::Object *object ) // SLOT
{
    disconnect( m_currentDevice.cache, SIGNAL( pathResolved( const DIDL::Object * ) ),
             this, SLOT( statResolvedPath( const DIDL::Object * ) ) );
    KIO::UDSEntry entry;

    if( !object ) {
        kDebug() << "ERROR: idString null";
        emit error( KIO::ERR_DOES_NOT_EXIST, QString() );
        return;
    }

    entry.insert( KIO::UDSEntry::UDS_NAME, object->title() );
    entry.insert( KIO::UDSEntry::UDS_DISPLAY_NAME, QUrl::fromPercentEncoding( object->title().toAscii() ) );
    entry.insert( KIO::UPNP_CLASS, object->upnpClass() );
    if( object->type() == DIDL::SuperObject::Container )
        entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR );
    else {
        entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG );
        const DIDL::Item *item = static_cast<const DIDL::Item *>( object );
        if( item && item->hasResource() ) {
            DIDL::Resource res = item->resource();
            entry.insert( KIO::UDSEntry::UDS_TARGET_URL, res["uri"] );
            entry.insert( KIO::UDSEntry::UDS_SIZE, res["size"].toULongLong() );
        }
    }
    emit statEntry( entry );
}

/////////////////////////////////////////////
////          Directory listing          ////
/////////////////////////////////////////////

/**
 * secondArgument should be one of BROWSE_DIRECT_CHILDREN,
 * BROWSE_METADATA or a valid search string.
 */
void ControlPointThread::browseOrSearchObject( const DIDL::Object *obj,
                                               HAction *action,
                                               const QString &secondArgument,
                                               const QString &filter,
                                               const uint startIndex,
                                               const uint requestedCount,
                                               const QString &sortCriteria )
{
    if( !contentDirectory() ) {
        emit error( KIO::ERR_UNSUPPORTED_ACTION, "UPnP device " + m_currentDevice.deviceInfo.friendlyName() + " does not support browsing" );
    }

    PersistentAction *pAction = new PersistentAction;
   
    HActionArguments args = action->inputArguments();
  
    Q_ASSERT( obj );
    if( action->name() == "Browse" ) {
        args["ObjectID"]->setValue( obj->id() );
        args["BrowseFlag"]->setValue( secondArgument );
    }
    else if( action->name() == "Search" ) {
        args["ContainerID"]->setValue( obj->id() );
        args["SearchCriteria"]->setValue( secondArgument );
    }
    args["Filter"]->setValue( filter );
    args["StartingIndex"]->setValue( startIndex );
    args["RequestedCount"]->setValue( requestedCount );
    args["SortCriteria"]->setValue( sortCriteria );

    ActionStateInfo *info = new ActionStateInfo;
    info->on = obj;
    info->start = startIndex;

    connect( pAction,
             SIGNAL( invokeComplete( Herqq::Upnp::HActionArguments, Herqq::Upnp::HAsyncOp, bool, QString ) ),
             this,
             SLOT( browseInvokeDone( Herqq::Upnp::HActionArguments, Herqq::Upnp::HAsyncOp, bool, QString ) ) );

    pAction->invoke( action, args, info );
}

/**
 * Search capabilities
 *
 * Users of the kio-slave can check if searching is supported
 * by the remote MediaServer/CDS by passing the query option
 * 'searchcapabilities' to the slave.
 * Each capability is returned as a file entry with the name
 * being the exact proprety supported in the search.
 * It is recommended that a synchronous job be used to test this.
 * 
 * Errors are always reported by error(), so if you do not
 * receive any entries, that means 0 items matched the search.
 * 
 * A search can be run instead of a browse by passing the following
 * query parameters:
 *  - search : Required. activate search rather than browse.
 *  - query : Required. a valid UPnP Search query string.
 *  - query2 : Optional. A query logically ANDed with query1.
 *  - ......
 *  - queryN : Similar to query 2
 *  - resolvePath : Optional. Resolve paths to search results. ( Default: false ).
 *        If resolvePath is set, the search result's UDS_NAME
 *        is set to point to the relative path to the actual
 *        item, from the search root.
 *  - filter : Optional. A CSV string specifying which DIDL-Lite fields should
 *        be returned. The required fields are always returned.
 *  - getCount : Optional. Return only a count (TotalMatches) instead of actual entries.
 *        One item will be emitted with UDS_NAME being the count.
 *
 * NOTE: The path component of the URL is treated as the top-level
 * container against which the search is run.
 * Usually you will want to use '/', but try to be more
 * specific if possible since that will give faster results.
 * It is recommended that values be percent encoded.
 * Since CDS implementations can be quite flaky or rigid, Stick
 * to the SearchCriteria specified in the UPNP specification.
 * In addition the slave will check that only properties
 * supported by the server are used.
 */
void ControlPointThread::listDir( const KUrl &url )
{
    kDebug() << url;

    if( !ensureDevice( url ) ) {
      emit error( KIO::ERR_DOES_NOT_EXIST, url.prettyUrl() );
      return;
    }

    QString path = url.path(KUrl::RemoveTrailingSlash);

    if( !url.queryItem( "searchcapabilities" ).isNull() ) {
        foreach( QString capability, m_currentDevice.searchCapabilities ) {
            KIO::UDSEntry entry;
            entry.insert( KIO::UDSEntry::UDS_NAME, capability );
            entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG );
            emit listEntry( entry );
        }
        emit listingDone();
        return;
    }

    if( !url.queryItem( "search" ).isNull() ) {
        QMap<QString, QString> searchQueries = url.queryItems();
        m_baseSearchPath = url.path( KUrl::AddTrailingSlash );
        m_resolveSearchPaths = url.queryItems().contains("resolvePath");

        if( !searchQueries.contains( "query" ) ) {
            emit error( KIO::ERR_SLAVE_DEFINED, i18n( "Expected query parameter as a minimum requirement for searching" ) );
            return;
        }

        if( searchQueries.contains( "filter" ) )
            m_filter = searchQueries["filter"];
        else
            m_filter = "*";

        if( searchQueries.contains( "getCount" ) )
            m_getCount = true;
        else
            m_getCount = false;

        m_queryString = searchQueries["query"];
        QRegExp queryParam("query\\d+");
        foreach( QString key, searchQueries.keys() ) {
            if( queryParam.exactMatch(key) ) {
                m_queryString += " and " + searchQueries[key];
            }
        }

        m_queryString = m_queryString.trimmed();

        kDebug() << m_queryString;

        if( m_queryString == "*" && !m_currentDevice.searchCapabilities.contains("*") ) {
            emit error(KIO::ERR_SLAVE_DEFINED, "Bad search: parameter '*' unsupported by server" );
            return;
        }

        if( m_queryString != "*" ) {
            int offset = 0;
            while( SearchRegExp::searchCriteria.indexIn( m_queryString, offset ) != -1 ) {
                offset += SearchRegExp::searchCriteria.matchedLength();
                QString property = SearchRegExp::searchCriteria.cap(2);
                // caused due to relExp, the issue
                // is odd, because interchanging regExp1 and regExp2
                // will cause patterns matching regExp1
                // to have the wrong capture group
                // so this workaround
                if( property.isEmpty() )
                    property = SearchRegExp::searchCriteria.cap(3);

                QRegExp logicalOp("\\s*(and|or)\\s*");
                if( logicalOp.indexIn( m_queryString, offset ) != -1 ) {
                    offset += logicalOp.matchedLength();
                }
                else {
                    if( offset < m_queryString.length() ) {
                        emit error( KIO::ERR_SLAVE_DEFINED, "Bad search: Expected logical op at " + m_queryString.mid(offset, 10) );
                        return;
                    }
                }
                if( !m_currentDevice.searchCapabilities.contains( property ) ) {
                    emit error( KIO::ERR_SLAVE_DEFINED, "Bad search: unsupported property " + property );
                    return;
                }
            }
            if( offset < m_queryString.length() ) {
                emit error( KIO::ERR_SLAVE_DEFINED, "Bad search: Invalid query '" + m_queryString.mid(offset) + "'" );
                return;
            }
        }

        connect( m_currentDevice.cache, SIGNAL( pathResolved( const DIDL::Object * ) ),
                 this, SLOT( searchResolvedPath( const DIDL::Object * ) ) );
        m_currentDevice.cache->resolvePathToObject( path );
        return;
    }

    connect( m_currentDevice.cache, SIGNAL( pathResolved( const DIDL::Object * ) ),
             this, SLOT( browseResolvedPath( const DIDL::Object *) ) );
    m_currentDevice.cache->resolvePathToObject(path);
}

void ControlPointThread::browseResolvedPath( const DIDL::Object *object, uint start, uint count ) // SLOT
{
    disconnect( m_currentDevice.cache, SIGNAL( pathResolved( const DIDL::Object * ) ),
                this, SLOT( browseResolvedPath( const DIDL::Object *) ) );
    if( !object ) {
        kDebug() << "ERROR: idString null";
        emit error( KIO::ERR_DOES_NOT_EXIST, QString() );
        return;
    }

    Q_ASSERT(connect( this, SIGNAL( browseResult( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ),
                      this, SLOT( createDirectoryListing( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ) ));
    browseOrSearchObject( object,
                          browseAction(),
                          BROWSE_DIRECT_CHILDREN,
                          "*",
                          start,
                          count,
                          "" );
}

void ControlPointThread::browseInvokeDone( HActionArguments output, HAsyncOp invocationOp, bool ok, QString error ) // SLOT
{
    if( !ok ) {
        kDebug() << "browse failed" << error;
        m_lastErrorString = error;
    }
    else {
        Q_ASSERT( output["Result"] );
        m_lastErrorString = QString();
    }

    // delete the PersistentAction
    PersistentAction *action = static_cast<PersistentAction *>( QObject::sender() );
    action->deleteLater();

    ActionStateInfo *info = ( ActionStateInfo *)invocationOp.userData();
    Q_ASSERT( info );
    emit browseResult( output, info );
}

void ControlPointThread::createDirectoryListing( const HActionArguments &args, ActionStateInfo *info ) // SLOT
{
    bool ok = disconnect( this, SIGNAL( browseResult( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ),
                          this, SLOT( createDirectoryListing( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ) );
    Q_ASSERT( ok );
    Q_UNUSED( ok );
    if( !args["Result"] ) {
        emit error( KIO::ERR_SLAVE_DEFINED, m_lastErrorString );
        return;
    }

    QString didlString = args["Result"]->value().toString();
    kDebug() << didlString;
    DIDL::Parser parser;
    connect( &parser, SIGNAL(error( const QString& )), this, SLOT(slotParseError( const QString& )) );

    connect( &parser, SIGNAL(containerParsed(DIDL::Container *)), this, SLOT(slotListContainer(DIDL::Container *)) );
    connect( &parser, SIGNAL(itemParsed(DIDL::Item *)), this, SLOT(slotListItem(DIDL::Item *)) );
    parser.parse(didlString);

    // NOTE: it is possible to dispatch this call even before
    // the parsing begins, but perhaps this delay is good for
    // adding some 'break' to the network connections, so that
    // disconnection by the remote device can be avoided.
    Q_ASSERT( info );
    uint num = args["NumberReturned"]->value().toUInt();
    uint total = args["TotalMatches"]->value().toUInt();
    if( num > 0 && ( info->start + num < total ) ) {
        Q_ASSERT( info->on );
        msleep( 1000 );
        browseResolvedPath( info->on, info->start + num );
    }
    else {
        emit listingDone();
    }
}

///////////////////////////////
//// DIDL parsing handlers ////
///////////////////////////////
void ControlPointThread::slotParseError( const QString &errorString )
{
    emit error(KIO::ERR_SLAVE_DEFINED, errorString);
}

void ControlPointThread::fillCommon( KIO::UDSEntry &entry, DIDL::Object *obj )
{
    entry.insert( KIO::UDSEntry::UDS_NAME, obj->title() );
    entry.insert( KIO::UDSEntry::UDS_DISPLAY_NAME, QUrl::fromPercentEncoding( obj->title().toAscii() ) );
    long long access = 0;
    // perform all permissions checks here

    access |= S_IRUSR | S_IRGRP | S_IROTH;

    entry.insert( KIO::UDSEntry::UDS_ACCESS, access );

    if( !obj->upnpClass().isNull() ) {
        entry.insert( KIO::UPNP_CLASS, obj->upnpClass() );
    }
    entry.insert( KIO::UPNP_ID, obj->id() );
    entry.insert( KIO::UPNP_PARENT_ID, obj->parentId() );
}

void ControlPointThread::fillContainer( KIO::UDSEntry &entry, DIDL::Container *c )
{
    fillCommon( entry, c );
    entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR );

    fillMetadata(entry, KIO::UPNP_ALBUM_CHILDCOUNT, c, "childCount");
}

void ControlPointThread::fillItem( KIO::UDSEntry &entry, DIDL::Item *item )
{
    fillCommon( entry, item );
    entry.insert( KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG );
    if( item->hasResource() ) {
        DIDL::Resource res = item->resource();
        entry.insert( KIO::UDSEntry::UDS_MIME_TYPE, res["mimetype"] );
        entry.insert( KIO::UDSEntry::UDS_SIZE, res["size"].toULongLong() );
        entry.insert( KIO::UDSEntry::UDS_TARGET_URL, res["uri"] );
    }
    else {
        long long access = entry.numberValue( KIO::UDSEntry::UDS_ACCESS );
        // undo slotListFillCommon
        access ^= S_IRUSR | S_IRGRP | S_IROTH;
        entry.insert( KIO::UDSEntry::UDS_ACCESS, access );
    }

    if( !item->refId().isNull() )
        entry.insert( KIO::UPNP_REF_ID, item->refId() );

    fillMetadata(entry, KIO::UPNP_CREATOR, item, "creator");
    fillMetadata(entry, KIO::UPNP_ARTIST, item, "artist");
    fillMetadata(entry, KIO::UPNP_ALBUM, item, "album");
    fillMetadata(entry, KIO::UPNP_GENRE, item, "genre");
    fillMetadata(entry, KIO::UPNP_TRACK_NUMBER, item, "originalTrackNumber");
// TODO processing
    fillMetadata(entry, KIO::UPNP_DATE, item, "date");

    fillResourceMetadata(entry, KIO::UPNP_DURATION, item, "duration");
    fillResourceMetadata(entry, KIO::UPNP_BITRATE, item, "bitrate");
    fillResourceMetadata(entry, KIO::UPNP_IMAGE_RESOLUTION, item, "resolution");
    fillMetadata(entry, KIO::UPNP_CHANNEL_NAME, item, "channelName");
    fillMetadata(entry, KIO::UPNP_CHANNEL_NUMBER, item, "channelNr");
    fillMetadata(entry, KIO::UPNP_ALBUMART_URI, item, "albumArtURI");
}

void ControlPointThread::slotListContainer( DIDL::Container *c )
{
    KIO::UDSEntry entry;
    fillContainer( entry, c );
    emit listEntry( entry );
}

void ControlPointThread::slotListItem( DIDL::Item *item )
{
    KIO::UDSEntry entry;
    fillItem( entry, item );
    emit listEntry( entry );
}

////////////////////////////////////////////
//// ID/title/object mapping/resolution ////
////////////////////////////////////////////

///////////////////
////  Updates  //// 
///////////////////
void ControlPointThread::slotCDSUpdated( const HStateVariableEvent &event )
{
    kDebug() << "UPDATE" << event.newValue();
}

void ControlPointThread::slotContainerUpdates( const Herqq::Upnp::HStateVariableEvent& event )
{
    kDebug() << "UPDATED containers" << event.newValue();

    HDevice *deviceForEvent = event.eventSource()->parentService()->parentDevice();
    Q_ASSERT( deviceForEvent );
    QString uuid = deviceForEvent->deviceInfo().udn().toSimpleUuid();

    QStringList filesChanged;

    QStringList updates = event.newValue().toString().split(",");
    QStringList::const_iterator it;
    for( it = updates.begin(); it != updates.end(); /* see loop */ ) {
        QString id = *it;
        it++;
        QString updateValue = *it;
        it++;

// NOTE what about CDS's with tracking changes option?
// TODO implement later

        if( m_devices[uuid].cache->update( id, updateValue ) ) {
            QString updatedPath = m_devices[uuid].cache->pathForId( id );
            kDebug() << "ID" << id << "Path" << updatedPath;

            KUrl fullPath;

            fullPath.setProtocol( "upnp-ms" );
            fullPath.setHost( uuid );
            fullPath.setPath( updatedPath );
            filesChanged << fullPath.prettyUrl();
        }
    }
    kDebug() << "Files Changed" << filesChanged;
    OrgKdeKDirNotifyInterface::emitFilesChanged( filesChanged );
}

///////////////////
//// Searching ////
///////////////////

// TODO
// - Relative mapping
//   ie. if search turns up a/b/c then it should
//   actually point to a/b/c ( we need id->path mappings )
// - somehow use createDirectoryListing's slot-recursive action
//   invocation in a generalised manner

void ControlPointThread::searchResolvedPath( const DIDL::Object *object, uint start, uint count )
{
    disconnect( m_currentDevice.cache, SIGNAL( pathResolved( const DIDL::Object * ) ),
                this, SLOT( searchResolvedPath( const DIDL::Object *) ) );
    if( !object ) {
        kDebug() << "ERROR: idString null";
        emit error( KIO::ERR_DOES_NOT_EXIST, QString() );
        return;
    }

    Q_ASSERT(connect( this, SIGNAL( browseResult( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ),
                      this, SLOT( createSearchListing( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ) ));
    browseOrSearchObject( object,
                          searchAction(),
                          m_queryString,
                          m_filter,
                          start,
                          count,
                          "" );
}

void ControlPointThread::createSearchListing( const HActionArguments &args, ActionStateInfo *info ) // SLOT
{
    kDebug() << "DONE";
    bool ok = disconnect( this, SIGNAL( browseResult( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ),
                          this, SLOT( createSearchListing( const Herqq::Upnp::HActionArguments &, ActionStateInfo * ) ) );
    Q_ASSERT( ok );
    Q_UNUSED( ok );
    if( !args["Result"] ) {
        emit error( KIO::ERR_SLAVE_DEFINED, m_lastErrorString );
        return;
    }

    if( m_getCount ) {
        QString matches = args["TotalMatches"]->value().toString();
        KIO::UDSEntry entry;
        entry.insert( KIO::UDSEntry::UDS_NAME, matches );
        emit listEntry( entry );
        emit listingDone();
        return;
    }

    QString didlString = args["Result"]->value().toString();
    kDebug() << didlString;
    DIDL::Parser parser;
    connect( &parser, SIGNAL(error( const QString& )), this, SLOT(slotParseError( const QString& )) );

    if( m_resolveSearchPaths ) {
        connect( &parser, SIGNAL(containerParsed(DIDL::Container *)), this, SLOT(slotListSearchContainer(DIDL::Container *)) );
        connect( &parser, SIGNAL(itemParsed(DIDL::Item *)), this, SLOT(slotListSearchItem(DIDL::Item *)) );
    }
    else {
        connect( &parser, SIGNAL(containerParsed(DIDL::Container *)), this, SLOT(slotListContainer(DIDL::Container *)) );
        connect( &parser, SIGNAL(itemParsed(DIDL::Item *)), this, SLOT(slotListItem(DIDL::Item *)) );
        connect( &parser, SIGNAL(error( const QString& )), this, SLOT(slotParseError( const QString& )) );
    }
    parser.parse(didlString);

    // NOTE: it is possible to dispatch this call even before
    // the parsing begins, but perhaps this delay is good for
    // adding some 'break' to the network connections, so that
    // disconnection by the remote device can be avoided.
    Q_ASSERT( info );
    uint num = args["NumberReturned"]->value().toUInt();

    if( m_resolveSearchPaths )
        m_searchListingCounter += num;

    uint total = args["TotalMatches"]->value().toUInt();
    if( num > 0 && ( info->start + num < total ) ) {
        Q_ASSERT( info->on );
        msleep( 1000 );
        searchResolvedPath( info->on, info->start + num );
    }
    else {
        if( !m_resolveSearchPaths )
            emit listingDone();
    }
}

void ControlPointThread::slotListSearchContainer( DIDL::Container *c )
{
    KIO::UDSEntry entry;
    fillContainer( entry, c );

    // ugly hack to get around lack of closures in C++
    setProperty( ("upnp_id_" + c->id()).toAscii().constData(), QVariant::fromValue( entry ) );
    connect( m_currentDevice.cache, SIGNAL( idToPathResolved( const QString &, const QString & ) ),
             this, SLOT( slotEmitSearchEntry( const QString &, const QString & ) ), Qt::UniqueConnection );
    m_currentDevice.cache->resolveIdToPath( c->id() );
}

void ControlPointThread::slotListSearchItem( DIDL::Item *item )
{
    KIO::UDSEntry entry;
    fillItem( entry, item );
    setProperty( ("upnp_id_" + item->id()).toAscii().constData(), QVariant::fromValue( entry ) );
    connect( m_currentDevice.cache, SIGNAL( idToPathResolved( const QString &, const QString & ) ),
             this, SLOT( slotEmitSearchEntry( const QString &, const QString & ) ), Qt::UniqueConnection );
    m_currentDevice.cache->resolveIdToPath( item->id() );
}

void ControlPointThread::slotEmitSearchEntry( const QString &id, const QString &path )
{
    KIO::UDSEntry entry = property( ("upnp_id_" + id).toAscii().constData() ).value<KIO::UDSEntry>();
    // delete the property
    setProperty( ("upnp_id_" + id).toAscii().constData(), QVariant() );

    kDebug() << "RESOLVED PATH" << path;
    kDebug() << "BASE SEARCH PATH " << m_baseSearchPath;
    entry.insert( KIO::UDSEntry::UDS_NAME, QString(path).replace( m_baseSearchPath, "" ) );
    emit listEntry( entry );
    m_searchListingCounter--;

    if( m_searchListingCounter == 0 )
        emit listingDone();
}
