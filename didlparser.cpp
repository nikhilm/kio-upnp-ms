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

#include "didlparser.h"

#include <QStringList>
#include <QXmlStreamReader>
#include <klocale.h>

#include <kdebug.h>

#include "didlobjects.h"

namespace DIDL {
Parser::Parser()
    : QObject(NULL)
    , m_reader(NULL)
{
}

Parser::~Parser()
{
}

bool Parser::interpretRestricted(const QStringRef &res)
{
    if( res == "1" )
        return true;
    return false;
}

void Parser::raiseError( const QString &errorStr )
{
    m_reader->raiseError( errorStr );
    emit error( errorStr );
    m_reader->clear();
}

QString Parser::parseTitle()
{
    m_reader->readNextStartElement();
    if( m_reader->name() == "title" ) {
        return m_reader->readElementText();
    }
    raiseError("Expected dc:title");
    return QString();
}

Resource Parser::parseResource()
{
    Resource r;
    QString protocolInfo = m_reader->attributes().value("protocolInfo").toString();
    if( !protocolInfo.isEmpty() ) {
        QStringList fields = protocolInfo.split(":");
        if( fields.length() != 4 ) {
            raiseError( i18n("Bad protocolInfo %1", (protocolInfo)) );
            return Resource();
        }
        r["mimetype"] = fields[2];
    }

    foreach( QXmlStreamAttribute attr, m_reader->attributes() ) {
        r[attr.name().toString()] = attr.value().toString();
    }
    r["uri"] = m_reader->readElementText();

    return r;
}

void Parser::parseItem()
{
    QXmlStreamAttributes attributes = m_reader->attributes();
    Item *item = new Item(
        attributes.value("id").toString(),
        attributes.value("parentID").toString(),
        interpretRestricted( attributes.value("restricted") ) );

    item->setTitle( parseTitle() );

    while( m_reader->readNextStartElement() ) {
        if( m_reader->name() == "res" ) {
            item->addResource( parseResource() );
        }
        else if( m_reader->name() == "class" ) {
            item->setUpnpClass( m_reader->readElementText() );
        }
        else {
            m_reader->skipCurrentElement();
        }
    }

    emit itemParsed( item );
}

void Parser::parseContainer()
{
    QXmlStreamAttributes attributes = m_reader->attributes();
    Container *container = new Container(
        attributes.value("id").toString(),
        attributes.value("parentID").toString(),
        interpretRestricted( attributes.value("restricted") ) );

    container->setTitle( parseTitle() );

    while( m_reader->readNextStartElement() ) {
        kDebug() << m_reader->name();
        if( m_reader->name() == "class" ) {
            container->setUpnpClass( m_reader->readElementText() );
        }
        else {
            m_reader->skipCurrentElement();
        }
    }

    emit containerParsed( container );
}

void Parser::parseDescription()
{
    QXmlStreamAttributes attributes = m_reader->attributes();
    Description *description = new Description(
        attributes.value("id").toString(),
        attributes.value("nameSpace").toString() );
    description->setDescription( m_reader->readElementText() );
}

void Parser::parse(const QString &input)
{
    // minimal parsing just to test the resolver
    if( m_reader ) {
        // TODO should probably just create this on the stack
        delete m_reader;
    }
    m_reader = new QXmlStreamReader(input);
    while( !m_reader->atEnd() ) {
        if( !m_reader->readNextStartElement() )
            break;
        Object *object = NULL;
        if( m_reader->name() == "item" ) {
            parseItem();
        }
        else if( m_reader->name() == "container" ) {
            parseContainer();
        }
        else if( m_reader->name() == "description" ) {
            parseDescription();
        }
        else if( m_reader->name() == "DIDL-Lite" ) {
        }
        else {
            raiseError("Unexpected element" + m_reader->name().toString() );
        }
    }

    if( m_reader->hasError() )
        raiseError(m_reader->errorString());
    else
        emit done();
}

} //~ namespace

