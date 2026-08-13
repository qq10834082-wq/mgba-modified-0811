/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QVector>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdint>
#include <memory>

struct mCore;
struct mCoreThread;

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

	struct Watch {
		int id = 0;
		uint32_t address = 0;
		uint32_t length = 0;
		QString access;
		bool pauseOnHit = true;
		QByteArray last;
		uint64_t hits = 0;
	};

	QJsonObject dispatch(const QJsonObject& request);
	QJsonObject makeResponse(const QJsonValue& id, const QJsonValue& result);
	QJsonObject makeError(const QJsonValue& id, int code, const QString& message);
	QJsonArray checkWatches(mCoreThread* thread);
	bool sampleMemory(mCoreThread* thread, uint32_t address, uint32_t length, QByteArray* bytes);

	ConfigController* m_config;
	std::shared_ptr<CoreController> m_controller;
	QString m_romPath;
	uint32_t m_inputMask = 0;
	QVector<Watch> m_watches;
	int m_nextWatchId = 1;
	bool m_diffInitialized = false;
	uint32_t m_diffAddress = 0;
	QByteArray m_diffBytes;
	QTcpServer m_server;
	QHash<QTcpSocket*, Client> m_clients;
};

}
