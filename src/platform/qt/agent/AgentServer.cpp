/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "agent/AgentServer.h"

#include "agent/AgentSettings.h"
#include "agent/SnapshotExporter.h"
#include "ConfigController.h"
#include "CoreController.h"
#include "LogController.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>

#include <mgba/core/core.h>
#include <mgba/core/thread.h>

using namespace QGBA;

namespace {

enum class CoreOp {
	None,
	GetInfo,
	Read,
	ReadRange,
	Write,
	FrameCounter,
};

struct CoreRequest {
	CoreOp op = CoreOp::None;
	uint32_t address = 0;
	uint32_t value = 0;
	uint32_t length = 0;
	int width = 0;
	bool ok = false;
	uint32_t u32 = 0;
	QByteArray bytes;
	QJsonObject info;
};

// Shared (not thread_local): mCoreThreadRunFunction runs on the emulation
// thread; the caller sets this pointer on the UI/RPC thread.
static QMutex s_coreRequestMutex;
static CoreRequest* s_coreRequest = nullptr;

static void runCoreRequest(mCoreThread* context) {
	CoreRequest* req = s_coreRequest;
	if (!req || !context->core || !mCoreThreadHasStarted(context)) {
		return;
	}
	mCore* core = context->core;

	switch (req->op) {
	case CoreOp::GetInfo: {
		char title[17] = {};
		char code[5] = {};
		core->getGameTitle(core, title);
		core->getGameCode(core, code);
		req->info.insert(QStringLiteral("title"), QString::fromUtf8(title).trimmed());
		req->info.insert(QStringLiteral("code"), QString::fromUtf8(code).trimmed());
		req->info.insert(QStringLiteral("frame"), static_cast<qint64>(core->frameCounter(core)));
		req->info.insert(QStringLiteral("platform"), core->platform(core) == mPLATFORM_GBA ? QStringLiteral("gba") : QStringLiteral("gb"));
		req->info.insert(QStringLiteral("paused"), mCoreThreadIsPaused(context));
		req->ok = true;
		break;
	}
	case CoreOp::Read:
		switch (req->width) {
		case 1:
			req->u32 = core->busRead8(core, req->address);
			break;
		case 2:
			req->u32 = core->busRead16(core, req->address);
			break;
		case 4:
			req->u32 = core->busRead32(core, req->address);
			break;
		}
		req->ok = true;
		break;
	case CoreOp::ReadRange:
		req->bytes.clear();
		req->bytes.reserve(req->length);
		for (uint32_t i = 0; i < req->length; ++i) {
			req->bytes.append(char(core->busRead8(core, req->address + i) & 0xFF));
		}
		req->ok = true;
		break;
	case CoreOp::Write:
		switch (req->width) {
		case 1:
			core->busWrite8(core, req->address, req->value);
			break;
		case 2:
			core->busWrite16(core, req->address, req->value);
			break;
		case 4:
			core->busWrite32(core, req->address, req->value);
			break;
		default:
			return;
		}
		req->ok = true;
		break;
	case CoreOp::FrameCounter:
		req->u32 = core->frameCounter(core);
		req->ok = true;
		break;
	case CoreOp::None:
		break;
	}
}

static bool runCore(mCoreThread* thread, CoreRequest& request) {
	QMutexLocker locker(&s_coreRequestMutex);
	s_coreRequest = &request;
	mCoreThreadRunFunction(thread, runCoreRequest);
	s_coreRequest = nullptr;
	return request.ok;
}

} // namespace

AgentServer::AgentServer(ConfigController* config, QObject* parent)
	: QObject(parent)
	, m_config(config)
{
	connect(&m_server, &QTcpServer::newConnection, this, &AgentServer::onNewConnection);
	reload();
}

AgentServer::~AgentServer() {
	m_server.close();
}

void AgentServer::reload() {
	m_server.close();
	AgentSettings settings = AgentSettings::fromConfig(m_config);
	if (!settings.enabled) {
		return;
	}
	QHostAddress address(settings.host);
	if (!m_server.listen(address, settings.port)) {
		LOG(QT, WARN) << tr("Agent server failed to listen on %1:%2").arg(settings.host).arg(settings.port);
	} else {
		LOG(QT, INFO) << tr("Agent server listening on %1:%2").arg(settings.host).arg(settings.port);
	}
}

void AgentServer::setController(std::shared_ptr<CoreController> controller) {
	m_controller = controller;
}

void AgentServer::setRomPath(const QString& path) {
	m_romPath = path;
}

void AgentServer::onNewConnection() {
	while (QTcpSocket* socket = m_server.nextPendingConnection()) {
		Client client;
		client.socket = socket;
		m_clients.insert(socket, client);
		connect(socket, &QTcpSocket::readyRead, this, &AgentServer::onReadyRead);
		connect(socket, &QTcpSocket::disconnected, this, &AgentServer::onDisconnected);
	}
}

void AgentServer::onReadyRead() {
	QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
	if (!socket || !m_clients.contains(socket)) {
		return;
	}
	Client& client = m_clients[socket];
	client.buffer.append(socket->readAll());

	while (true) {
		int newline = client.buffer.indexOf('\n');
		if (newline < 0) {
			break;
		}
		QByteArray line = client.buffer.left(newline).trimmed();
		client.buffer.remove(0, newline + 1);
		if (line.isEmpty()) {
			continue;
		}

		QJsonParseError parseError;
		const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
		QJsonObject response;
		if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
			response = makeError(QJsonValue(), -32700, QStringLiteral("parse error"));
		} else {
			response = dispatch(doc.object());
		}
		socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
		socket->write("\n");
	}
}

void AgentServer::onDisconnected() {
	QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
	if (socket) {
		m_clients.remove(socket);
		socket->deleteLater();
	}
}

QJsonObject AgentServer::makeResponse(const QJsonValue& id, const QJsonValue& result) {
	QJsonObject response;
	if (!id.isUndefined()) {
		response.insert(QStringLiteral("id"), id);
	}
	response.insert(QStringLiteral("result"), result);
	return response;
}

QJsonObject AgentServer::makeError(const QJsonValue& id, int code, const QString& message) {
	QJsonObject response;
	if (!id.isUndefined()) {
		response.insert(QStringLiteral("id"), id);
	}
	QJsonObject error;
	error.insert(QStringLiteral("code"), code);
	error.insert(QStringLiteral("message"), message);
	response.insert(QStringLiteral("error"), error);
	return response;
}

QJsonObject AgentServer::dispatch(const QJsonObject& request) {
	const QJsonValue id = request.value(QStringLiteral("id"));
	const QString method = request.value(QStringLiteral("method")).toString();
	const QJsonObject params = request.value(QStringLiteral("params")).toObject();

	if (method == QStringLiteral("ping")) {
		return makeResponse(id, QStringLiteral("pong"));
	}

	if (!m_controller || !m_controller->hasStarted()) {
		if (method == QStringLiteral("get_info")) {
			QJsonObject info;
			info.insert(QStringLiteral("rom_loaded"), false);
			return makeResponse(id, info);
		}
		return makeError(id, -32000, QStringLiteral("no ROM loaded"));
	}

	mCoreThread* thread = m_controller->thread();

	if (method == QStringLiteral("get_info")) {
		CoreRequest req;
		req.op = CoreOp::GetInfo;
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("get_info failed"));
		}
		QJsonObject info = req.info;
		info.insert(QStringLiteral("rom_loaded"), true);
		return makeResponse(id, info);
	}

	if (method == QStringLiteral("read8") || method == QStringLiteral("read16") || method == QStringLiteral("read32")) {
		CoreRequest req;
		req.op = CoreOp::Read;
		req.address = params.value(QStringLiteral("address")).toVariant().toUInt();
		req.width = method == QStringLiteral("read8") ? 1 : method == QStringLiteral("read16") ? 2 : 4;
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("read failed"));
		}
		return makeResponse(id, static_cast<qint64>(req.u32));
	}

	if (method == QStringLiteral("read_range")) {
		CoreRequest req;
		req.op = CoreOp::ReadRange;
		req.address = params.value(QStringLiteral("address")).toVariant().toUInt();
		req.length = params.value(QStringLiteral("length")).toVariant().toUInt();
		if (!req.length || req.length > 4096) {
			return makeError(id, -32602, QStringLiteral("length must be 1-4096"));
		}
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("read_range failed"));
		}
		QJsonArray bytes;
		for (unsigned char byte : req.bytes) {
			bytes.append(byte);
		}
		return makeResponse(id, bytes);
	}

	if (method == QStringLiteral("write8") || method == QStringLiteral("write16") || method == QStringLiteral("write32")) {
		CoreRequest req;
		req.op = CoreOp::Write;
		req.address = params.value(QStringLiteral("address")).toVariant().toUInt();
		req.value = params.value(QStringLiteral("value")).toVariant().toUInt();
		req.width = method == QStringLiteral("write8") ? 1 : method == QStringLiteral("write16") ? 2 : 4;
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("write failed"));
		}
		return makeResponse(id, true);
	}

	if (method == QStringLiteral("advance_frames")) {
		int count = params.value(QStringLiteral("count")).toInt(1);
		if (count < 1) {
			count = 1;
		}
		for (int i = 0; i < count; ++i) {
			m_controller->frameAdvance();
		}
		CoreRequest req;
		req.op = CoreOp::FrameCounter;
		runCore(thread, req);
		return makeResponse(id, static_cast<qint64>(req.u32));
	}

	if (method == QStringLiteral("pause")) {
		m_controller->setPaused(true);
		return makeResponse(id, true);
	}

	if (method == QStringLiteral("unpause")) {
		m_controller->setPaused(false);
		return makeResponse(id, true);
	}

	if (method == QStringLiteral("reset")) {
		m_controller->reset();
		return makeResponse(id, true);
	}

	if (method == QStringLiteral("export_snapshot")) {
		const AgentSettings settings = AgentSettings::fromConfig(m_config);
		QString error;
		const QString outDir = SnapshotExporter::exportSnapshot(m_controller.get(), m_romPath, settings.regions, &error);
		if (outDir.isEmpty()) {
			return makeError(id, -32603, error.isEmpty() ? QStringLiteral("export failed") : error);
		}
		QJsonObject result;
		result.insert(QStringLiteral("path"), outDir);
		return makeResponse(id, result);
	}

	return makeError(id, -32601, QStringLiteral("unknown method: ") + method);
}
