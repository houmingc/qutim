/****************************************************************************
 * wcontact.h
 *
 *  Copyright (c) 2010 by Belov Nikita <null@deltaz.org>
 *
 ***************************************************************************
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************
*****************************************************************************/

#ifndef WCONTACT_H
#define WCONTACT_H

#include <QApplication>

#include <qutim/contact.h>
#include <qutim/icon.h>
#include <qutim/message.h>
#include <qutim/messagesession.h>
#include <qutim/tooltip.h>

#include "wmanager.h"

using namespace qutim_sdk_0_3;

class WContact : public Contact
{
	Q_OBJECT

public:
	WContact( const QString &city, Account *account );
	~WContact();

	bool sendMessage( const Message &message );

	void setName( const QString &name );
	void setNamev2( const QString &name );
	void setTags( const QStringList &tags );

	QString id() const;
	QString name() const;
	QString title() const;
	QStringList tags() const;
	Status status() const;

	bool isInList() const;
	void setInList( bool inList );

	QString avatar() const;

	void update();
	void updateStatus();

protected:
	virtual bool event( QEvent *ev );

private slots:
	void finished();
	void getWeather();
	void getForecast();

private:
	QString getFileData( const QString &path );

	WManager *m_wmanager;
	bool m_forecast;
	bool m_forStatus;

	Status m_status;

//	QMenu *m_menu;

	QString m_city;
	QString m_name;
	QStringList m_tags;
	bool m_inList;
};

#endif // WCONTACT_H
