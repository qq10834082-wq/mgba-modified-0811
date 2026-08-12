/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>

struct mCore;

namespace QGBA {

class ConfigController;
class CoreController;

class AgentServer : public QObject {
Q_OBJECT

public:
	AgentServer(ConfigController* config, QObject* parent = nullptr);
	~AgentServer();

	void reload();
	void setController(std::shared_ptr<CoreController> controller);
	void setRomPath(const QString& path);

private slots:
	void onNewConnection();
	void onReadyRead();
	void onDisconnected();

private:
	struct Client {
		QTcpSocket* socket;
		QByteArray buffer;
	};

	QJsonObject dispatch(const QJsonObject& request);
	QJsonObject makeResponse(const QJsonValue& id, const QJsonValue& result);
	QJsonObject makeError(const QJsonValue& id, int code, const QString& message);

	ConfigController* m_config;
	std::shared_ptr<CoreController> m_controller;
	QString m_romPath;
	QTcpServer m_server;
	QHash<QTcpSocket*, Client> m_clients;
};

}
