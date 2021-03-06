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

#ifndef DIDL_PARSER_H
#define DIDL_PARSER_H

#include <QObject>
#include <QHash>

class QXmlStreamReader;

namespace DIDL {

class Container;
class Item;
class Description;
class SuperObject;
class Object;

typedef QHash<QString, QString> Resource;

/**
 * This class implements a parser for the 
 * DIDL-Lite XML schema. It is meant to be
 * used with UPnP ContentDirectory service replies.
 * Required elements are checked for, and an error
 * occurs in case of absence.
 * The class emits certain signals when top-level elements
 * are found. Top-level means immediate children of the DIDL
 * root element. Their children can be accessed through the
 * elements themselves.
 * 
 * @see DIDL::Container
 * @see DIDL::Item
 * @see DIDL::Description
 * 
 * @author Nikhil Marathe
 */
class Parser : public QObject
{
  Q_OBJECT
  public:
    Parser();
    ~Parser();

  public slots:
    /**
     * This is NOT a push parser.
     * @c parse() should only be called once
     * with the complete XML input until one
     * off @c done() or @c error() is called.
     * 
     * Calling this again will restart the parser.
     */
    void parse(const QString &input);

  signals:
    /**
     * Emitted in case of an error at any point
     * The parser will stop parsing after this.
     */
    void error(const QString &errorString);

    /**
     * Emitted when parsing is finished.
     */
    void done();

    // NOTE should we emit pointers?
    /**
     * Emitted when a top-level <item> is completely parsed
     * The caller should delete the item.
     */
    void itemParsed(DIDL::Item *);

    /**
     * Emitted when a top-level <container> is completely parsed.
     * The caller should delete the container.
     */
    void containerParsed(DIDL::Container *);

    /**
     * Emitted for a top-level <desc> element is parsed.
     * The caller should delete the description.
     */
    void descriptionParsed(DIDL::Description *);

 private:
    /**
     * The restricted attribute of a DIDL object is a "1"
     * or "0", convert to a boolean
     */
    bool interpretRestricted(const QStringRef &res);
    void parseItem();
    void parseContainer();
    void parseDescription();
    Resource parseResource();

   /**
     * returns true if it actually parsed something,
     * false otherwise
     */
    bool parseObjectCommon( Object *o );

    // emits error() and stops the parser
    void raiseError( const QString &errorStr=QString() );

    QXmlStreamReader *m_reader;
};

} //~ namespace

#endif
