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
#include "GBAApp.h"
#include "LogController.h"
#include "Window.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <cmath>
#include <cstring>
#include <limits>

#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/core/thread.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/input.h>
#endif

using namespace QGBA;

namespace {

enum class CoreOp {
	None,
	GetInfo,
	Read,
	ReadRange,
	Write,
	FrameCounter,
	Input,
	RunFrame,
	SaveState,
	LoadState,
};

struct CoreRequest {
	CoreOp op = CoreOp::None;
	uint32_t address = 0;
	uint32_t value = 0;
	uint32_t length = 0;
	uint32_t key = 0;
	int slot = 0;
	int width = 0;
	bool pressed = false;
	bool ok = false;
	uint32_t u32 = 0;
	QByteArray bytes;
	QJsonObject info;
};

// Shared (not thread_local): mCoreThreadRunFunction runs on the emulation
// thread; the caller sets this pointer on the UI/RPC thread.
static QMutex s_coreRequestMutex;
static CoreRequest* s_coreRequest = nullptr;

static bool jsonUint(const QJsonValue& value, uint32_t maximum, uint32_t* out) {
	if (!value.isDouble()) {
		return false;
	}
	const double number = value.toDouble();
	if (!std::isfinite(number) || number < 0 || number > maximum || std::floor(number) != number) {
		return false;
	}
	*out = static_cast<uint32_t>(number);
	return true;
}

static bool jsonBool(const QJsonObject& params, const QString& name, bool defaultValue, bool* out) {
	if (!params.contains(name)) {
		*out = defaultValue;
		return true;
	}
	const QJsonValue value = params.value(name);
	if (!value.isBool()) {
		return false;
	}
	*out = value.toBool();
	return true;
}

static QJsonArray inputNames(uint32_t mask) {
	static const char* names[] = {
		"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"
	};
	QJsonArray result;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		if (mask & (1u << i)) {
			result.append(QString::fromLatin1(names[i]));
		}
	}
	return result;
}

static bool parseInputKey(const QJsonObject& params, uint32_t* key) {
	const QJsonValue value = params.value(QStringLiteral("key"));
	if (!value.isString()) {
		return false;
	}
	const QString name = value.toString().trimmed().toUpper();
#ifdef M_CORE_GBA
	static const QHash<QString, uint32_t> keys = {
		{QStringLiteral("A"), GBA_KEY_A},
		{QStringLiteral("B"), GBA_KEY_B},
		{QStringLiteral("SELECT"), GBA_KEY_SELECT},
		{QStringLiteral("START"), GBA_KEY_START},
		{QStringLiteral("RIGHT"), GBA_KEY_RIGHT},
		{QStringLiteral("LEFT"), GBA_KEY_LEFT},
		{QStringLiteral("UP"), GBA_KEY_UP},
		{QStringLiteral("DOWN"), GBA_KEY_DOWN},
		{QStringLiteral("R"), GBA_KEY_R},
		{QStringLiteral("L"), GBA_KEY_L},
	};
	const auto it = keys.constFind(name);
	if (it == keys.constEnd()) {
		return false;
	}
	*key = it.value();
	return true;
#else
	Q_UNUSED(name);
	Q_UNUSED(key);
	return false;
#endif
}

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
		uint32_t registerValue = 0;
		if (core->readRegister && core->readRegister(core, "pc", &registerValue)) {
			req->info.insert(QStringLiteral("pc"), static_cast<qint64>(registerValue));
		}
		if (core->readRegister && core->readRegister(core, "lr", &registerValue)) {
			req->info.insert(QStringLiteral("lr"), static_cast<qint64>(registerValue));
		}
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
	case CoreOp::ReadRange: {
		req->bytes.resize(static_cast<int>(req->length));
		char* out = req->bytes.data();
		uint32_t remaining = req->length;
		uint32_t addr = req->address;
		while (remaining > 0) {
			size_t blockAvail = 0;
			void* ptr = mCoreGetMemoryBlock(core, addr, &blockAvail);
			if (ptr && blockAvail > 0) {
				const uint32_t n = remaining < blockAvail ? remaining : static_cast<uint32_t>(blockAvail);
				memcpy(out, ptr, n);
				out += n;
				addr += n;
				remaining -= n;
			} else {
				// Unmapped / I/O: preserve bus semantics one byte at a time.
				*out++ = static_cast<char>(core->busRead8(core, addr) & 0xFF);
				++addr;
				--remaining;
			}
		}
		req->ok = true;
		break;
	}
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
	case CoreOp::Input:
#ifdef M_CORE_GBA
		if (core->platform(core) != mPLATFORM_GBA || req->key >= GBA_KEY_MAX) {
			return;
		}
		if (req->pressed) {
			core->addKeys(core, 1u << req->key);
		} else {
			core->clearKeys(core, 1u << req->key);
		}
		req->u32 = core->frameCounter(core);
		req->ok = true;
#endif
		break;
	case CoreOp::RunFrame:
		if (!core->runFrame || !req->length) {
			return;
		}
		core->runFrame(core);
		req->u32 = core->frameCounter(core);
		req->ok = true;
		break;
	case CoreOp::SaveState:
		req->ok = mCoreSaveState(core, req->slot, SAVESTATE_SCREENSHOT | SAVESTATE_SAVEDATA | SAVESTATE_RTC | SAVESTATE_METADATA);
		break;
	case CoreOp::LoadState:
		req->ok = mCoreLoadState(core, req->slot, SAVESTATE_SCREENSHOT | SAVESTATE_RTC | SAVESTATE_METADATA);
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
	if (m_controller.get() != controller.get()) {
		m_inputMask = 0;
		m_watches.clear();
		m_diffInitialized = false;
	}
	m_controller = controller;
}

void AgentServer::setRomPath(const QString& path) {
	m_romPath = path;
}

bool AgentServer::sampleMemory(mCoreThread* thread, uint32_t address, uint32_t length, QByteArray* bytes) {
	CoreRequest request;
	request.op = CoreOp::ReadRange;
	request.address = address;
	request.length = length;
	if (!runCore(thread, request)) {
		return false;
	}
	if (bytes) {
		*bytes = request.bytes;
	}
	return true;
}

QJsonArray AgentServer::checkWatches(mCoreThread* thread) {
	QJsonArray hits;
	for (Watch& watch : m_watches) {
		QByteArray current;
		if (!sampleMemory(thread, watch.address, watch.length, &current)) {
			continue;
		}
		if (watch.last.isEmpty()) {
			watch.last = current;
			continue;
		}
		if (current == watch.last) {
			continue;
		}
		QJsonObject hit;
		hit.insert(QStringLiteral("id"), watch.id);
		hit.insert(QStringLiteral("address"), static_cast<qint64>(watch.address));
		hit.insert(QStringLiteral("length"), static_cast<qint64>(watch.length));
		hit.insert(QStringLiteral("access"), watch.access);
		hit.insert(QStringLiteral("pause_on_hit"), watch.pauseOnHit);
		hit.insert(QStringLiteral("old"), QString::fromLatin1(watch.last.toHex()));
		hit.insert(QStringLiteral("new"), QString::fromLatin1(current.toHex()));
		++watch.hits;
		hit.insert(QStringLiteral("hits"), static_cast<qint64>(watch.hits));
		hits.append(hit);
		watch.last = current;
		if (watch.pauseOnHit) {
			mCoreThreadPause(thread);
		}
	}
	return hits;
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
	if (client.buffer.size() > 1024 * 1024) {
		// Do not retain an unbounded unterminated request from a local client.
		socket->disconnectFromHost();
		return;
	}

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
	if (method.isEmpty()) {
		return makeError(id, -32600, QStringLiteral("method is required"));
	}
	if (request.contains(QStringLiteral("params")) && !request.value(QStringLiteral("params")).isObject()) {
		return makeError(id, -32602, QStringLiteral("params must be an object"));
	}
	const QJsonObject params = request.value(QStringLiteral("params")).toObject();

	if (method == QStringLiteral("ping")) {
		return makeResponse(id, QStringLiteral("pong"));
	}

	if (method == QStringLiteral("load_rom")) {
		const QJsonValue pathValue = params.value(QStringLiteral("path"));
		if (!pathValue.isString() || pathValue.toString().trimmed().isEmpty()) {
			return makeError(id, -32602, QStringLiteral("path must be a non-empty absolute local ROM path"));
		}
		const QString path = pathValue.toString();
		QFileInfo info(path);
		if (!info.isAbsolute()) {
			return makeError(id, -32602, QStringLiteral("path must be an absolute local ROM path"));
		}
		if (!info.isReadable()) {
			return makeError(id, -32001, QStringLiteral("ROM file is not readable: ") + path);
		}
		const QList<Window*> windows = GBAApp::app() ? GBAApp::app()->windows() : QList<Window*>();
		if (windows.isEmpty()) {
			return makeError(id, -32000, QStringLiteral("no mGBA window is available"));
		}
		QString error;
		Window* window = windows.first();
		if (!window->loadRomFromAgent(info.absoluteFilePath(), &error)) {
			return makeError(id, -32001, error.isEmpty() ? QStringLiteral("could not load ROM") : error);
		}
		m_controller = window->controller();
		m_romPath = window->windowFilePath();
		if (!m_controller || !m_controller->hasStarted()) {
			return makeError(id, -32001, QStringLiteral("ROM loaded but controller did not start"));
		}
		CoreRequest req;
		req.op = CoreOp::GetInfo;
		if (!runCore(m_controller->thread(), req)) {
			return makeError(id, -32603, QStringLiteral("ROM loaded but info query failed"));
		}
		QJsonObject result = req.info;
		result.insert(QStringLiteral("rom_loaded"), true);
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(m_controller->thread()));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		result.insert(QStringLiteral("rom_path"), m_romPath);
		return makeResponse(id, result);
	}

	if (!m_controller || !m_controller->hasStarted()) {
		if (method == QStringLiteral("get_info")) {
			QJsonObject info;
			info.insert(QStringLiteral("rom_loaded"), false);
			info.insert(QStringLiteral("frame"), 0);
			info.insert(QStringLiteral("paused"), true);
			info.insert(QStringLiteral("input"), inputNames(m_inputMask));
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
		info.insert(QStringLiteral("input"), inputNames(m_inputMask));
		return makeResponse(id, info);
	}

	if (method == QStringLiteral("save_state") || method == QStringLiteral("load_state")) {
		uint32_t slot = 0;
		if (!jsonUint(params.value(QStringLiteral("slot")), 9, &slot) || slot < 1) {
			return makeError(id, -32602, QStringLiteral("slot must be an integer in 1-9"));
		}
		if (method == QStringLiteral("load_state")) {
			mCoreThreadPause(thread);
		}
		CoreRequest req;
		req.op = method == QStringLiteral("save_state") ? CoreOp::SaveState : CoreOp::LoadState;
		req.slot = static_cast<int>(slot);
		if (!runCore(thread, req)) {
			return makeError(id, -32603, method == QStringLiteral("save_state")
				? QStringLiteral("save_state failed for slot %1").arg(slot)
				: QStringLiteral("load_state failed for slot %1").arg(slot));
		}
		if (method == QStringLiteral("load_state")) {
			m_controller->setPaused(true);
		}
		CoreRequest infoRequest;
		infoRequest.op = CoreOp::GetInfo;
		if (!runCore(thread, infoRequest)) {
			return makeError(id, -32603, QStringLiteral("state operation succeeded but info query failed"));
		}
		QJsonObject result = infoRequest.info;
		result.insert(QStringLiteral("rom_loaded"), true);
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		result.insert(QStringLiteral("slot"), static_cast<int>(slot));
		result.insert(QStringLiteral("operation"), method);
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("watch_add")) {
		if (m_watches.size() >= 32) {
			return makeError(id, -32602, QStringLiteral("watch limit is 32"));
		}
		uint32_t address = 0;
		uint32_t length = 0;
		if (!jsonUint(params.value(QStringLiteral("address")), std::numeric_limits<uint32_t>::max(), &address)
			|| !jsonUint(params.value(QStringLiteral("length")), 4096, &length) || !length) {
			return makeError(id, -32602, QStringLiteral("address must be 32-bit and length must be 1-4096"));
		}
		if (static_cast<quint64>(address) + length > (static_cast<quint64>(std::numeric_limits<uint32_t>::max()) + 1)) {
			return makeError(id, -32602, QStringLiteral("address plus length exceeds the 32-bit address range"));
		}
		QString access = params.value(QStringLiteral("access")).toString().trimmed().toLower();
		if (access.isEmpty()) {
			access = QStringLiteral("write");
		}
		if (access != QStringLiteral("write") && access != QStringLiteral("read") && access != QStringLiteral("rw")) {
			return makeError(id, -32602, QStringLiteral("access must be write, read, or rw"));
		}
		bool pauseOnHit = true;
		if (!jsonBool(params, QStringLiteral("pause_on_hit"), true, &pauseOnHit)) {
			return makeError(id, -32602, QStringLiteral("pause_on_hit must be boolean"));
		}
		Watch watch;
		watch.id = m_nextWatchId++;
		watch.address = address;
		watch.length = length;
		watch.access = access;
		watch.pauseOnHit = pauseOnHit;
		if (!sampleMemory(thread, address, length, &watch.last)) {
			return makeError(id, -32603, QStringLiteral("watch baseline read failed"));
		}
		m_watches.append(watch);
		QJsonObject result;
		result.insert(QStringLiteral("id"), watch.id);
		result.insert(QStringLiteral("address"), static_cast<qint64>(address));
		result.insert(QStringLiteral("length"), static_cast<qint64>(length));
		result.insert(QStringLiteral("access"), access);
		result.insert(QStringLiteral("pause_on_hit"), pauseOnHit);
		result.insert(QStringLiteral("precision"), QStringLiteral("frame-boundary polling; changes may be missed within a frame"));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("watch_remove")) {
		uint32_t watchId = 0;
		if (!jsonUint(params.value(QStringLiteral("id")), std::numeric_limits<uint32_t>::max(), &watchId)) {
			return makeError(id, -32602, QStringLiteral("id must be an integer"));
		}
		for (int i = 0; i < m_watches.size(); ++i) {
			if (m_watches.at(i).id == static_cast<int>(watchId)) {
				m_watches.removeAt(i);
				return makeResponse(id, true);
			}
		}
		return makeError(id, -32004, QStringLiteral("watch not found"));
	}

	if (method == QStringLiteral("watch_list")) {
		QJsonArray watches;
		for (const Watch& watch : m_watches) {
			QJsonObject item;
			item.insert(QStringLiteral("id"), watch.id);
			item.insert(QStringLiteral("address"), static_cast<qint64>(watch.address));
			item.insert(QStringLiteral("length"), static_cast<qint64>(watch.length));
			item.insert(QStringLiteral("access"), watch.access);
			item.insert(QStringLiteral("pause_on_hit"), watch.pauseOnHit);
			item.insert(QStringLiteral("hits"), static_cast<qint64>(watch.hits));
			watches.append(item);
		}
		QJsonObject result;
		result.insert(QStringLiteral("watches"), watches);
		result.insert(QStringLiteral("precision"), QStringLiteral("frame-boundary polling; changes may be missed within a frame"));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("read_block")) {
		uint32_t address = 0;
		uint32_t length = 0;
		if (!jsonUint(params.value(QStringLiteral("address")), std::numeric_limits<uint32_t>::max(), &address)
			|| !jsonUint(params.value(QStringLiteral("length")), 65536, &length) || !length) {
			return makeError(id, -32602, QStringLiteral("address must be 32-bit and length must be 1-65536"));
		}
		if (static_cast<quint64>(address) + length > (static_cast<quint64>(std::numeric_limits<uint32_t>::max()) + 1)) {
			return makeError(id, -32602, QStringLiteral("address plus length exceeds the 32-bit address range"));
		}
		QByteArray bytes;
		if (!sampleMemory(thread, address, length, &bytes)) {
			return makeError(id, -32603, QStringLiteral("read_block failed"));
		}
		QJsonObject result;
		result.insert(QStringLiteral("address"), static_cast<qint64>(address));
		result.insert(QStringLiteral("length"), static_cast<qint64>(bytes.size()));
		result.insert(QStringLiteral("base64"), QString::fromLatin1(bytes.toBase64()));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("memory_diff")) {
		uint32_t address = 0;
		uint32_t length = 0;
		uint32_t maxChanges = 256;
		if (!jsonUint(params.value(QStringLiteral("address")), std::numeric_limits<uint32_t>::max(), &address)
			|| !jsonUint(params.value(QStringLiteral("length")), 4096, &length) || !length
			|| (params.contains(QStringLiteral("max_changes"))
				&& !jsonUint(params.value(QStringLiteral("max_changes")), 1024, &maxChanges)) || !maxChanges) {
			return makeError(id, -32602, QStringLiteral("address/length are required and max_changes must be 1-1024"));
		}
		if (static_cast<quint64>(address) + length > (static_cast<quint64>(std::numeric_limits<uint32_t>::max()) + 1)) {
			return makeError(id, -32602, QStringLiteral("address plus length exceeds the 32-bit address range"));
		}
		QByteArray current;
		if (!sampleMemory(thread, address, length, &current)) {
			return makeError(id, -32603, QStringLiteral("memory_diff sample failed"));
		}
		QJsonArray changes;
		const bool initialized = !m_diffInitialized || m_diffAddress != address || m_diffBytes.size() != current.size();
		if (!initialized) {
			for (int i = 0; i < current.size() && changes.size() < static_cast<int>(maxChanges); ++i) {
				const unsigned char oldByte = static_cast<unsigned char>(m_diffBytes.at(i));
				const unsigned char newByte = static_cast<unsigned char>(current.at(i));
				if (oldByte == newByte) {
					continue;
				}
				QJsonObject change;
				change.insert(QStringLiteral("address"), static_cast<qint64>(address + static_cast<uint32_t>(i)));
				change.insert(QStringLiteral("old"), oldByte);
				change.insert(QStringLiteral("new"), newByte);
				changes.append(change);
			}
		}
		m_diffInitialized = true;
		m_diffAddress = address;
		m_diffBytes = current;
		QJsonObject result;
		result.insert(QStringLiteral("initialized"), initialized);
		result.insert(QStringLiteral("address"), static_cast<qint64>(address));
		result.insert(QStringLiteral("length"), static_cast<qint64>(length));
		result.insert(QStringLiteral("changes"), changes);
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("press_key")) {
		uint32_t key = 0;
		if (!parseInputKey(params, &key)) {
			return makeError(id, -32602, QStringLiteral("key must be one of A, B, L, R, Start, Select, Up, Down, Left, Right"));
		}
		uint32_t holdFrames = 1;
		uint32_t waitFrames = 0;
		if (params.contains(QStringLiteral("hold_frames"))
			&& !jsonUint(params.value(QStringLiteral("hold_frames")), 1000, &holdFrames)) {
			return makeError(id, -32602, QStringLiteral("hold_frames must be an integer in 0-1000"));
		}
		if (params.contains(QStringLiteral("wait_frames"))
			&& !jsonUint(params.value(QStringLiteral("wait_frames")), 1000, &waitFrames)) {
			return makeError(id, -32602, QStringLiteral("wait_frames must be an integer in 0-1000"));
		}
		bool pauseAfter = true;
		if (!jsonBool(params, QStringLiteral("pause_after"), true, &pauseAfter)) {
			return makeError(id, -32602, QStringLiteral("pause_after must be boolean"));
		}
		if (static_cast<quint64>(holdFrames) + waitFrames > 1000) {
			return makeError(id, -32602, QStringLiteral("hold_frames plus wait_frames must be at most 1000"));
		}

		CoreRequest keyRequest;
		keyRequest.op = CoreOp::Input;
		keyRequest.key = key;
		keyRequest.pressed = true;
		m_controller->addKey(static_cast<int>(key));
		if (!runCore(thread, keyRequest)) {
			return makeError(id, -32603, QStringLiteral("key press failed"));
		}
		m_inputMask |= 1u << key;

		mCoreThreadPause(thread);
		CoreRequest startRequest;
		startRequest.op = CoreOp::FrameCounter;
		if (!runCore(thread, startRequest)) {
			return makeError(id, -32603, QStringLiteral("could not read starting frame"));
		}
		const uint32_t startFrame = startRequest.u32;
		uint32_t frame = startFrame;
		QJsonArray watchHits;
		bool watchPause = false;
		const uint32_t totalFrames = holdFrames + waitFrames;
		for (uint32_t i = 0; i < totalFrames; ++i) {
			if (i == holdFrames) {
				CoreRequest releaseRequest;
				releaseRequest.op = CoreOp::Input;
				releaseRequest.key = key;
				releaseRequest.pressed = false;
				m_controller->clearKey(static_cast<int>(key));
				if (!runCore(thread, releaseRequest)) {
					return makeError(id, -32603, QStringLiteral("key release failed"));
				}
				m_inputMask &= ~(1u << key);
			}
			CoreRequest frameRequest;
			frameRequest.op = CoreOp::RunFrame;
			frameRequest.length = 1;
			if (!runCore(thread, frameRequest)) {
				return makeError(id, -32603, QStringLiteral("press_key frame advance failed"));
			}
			frame = frameRequest.u32;
			const QJsonArray frameHits = checkWatches(thread);
			for (const QJsonValue& hit : frameHits) {
				watchHits.append(hit);
				if (hit.toObject().value(QStringLiteral("pause_on_hit")).toBool()) {
					watchPause = true;
				}
			}
		}
		if (m_inputMask & (1u << key)) {
			CoreRequest releaseRequest;
			releaseRequest.op = CoreOp::Input;
			releaseRequest.key = key;
			releaseRequest.pressed = false;
			m_controller->clearKey(static_cast<int>(key));
			runCore(thread, releaseRequest);
			m_inputMask &= ~(1u << key);
		}
		if (!pauseAfter && !watchPause) {
			m_controller->setPaused(false);
		}
		QJsonObject result;
		result.insert(QStringLiteral("start_frame"), static_cast<qint64>(startFrame));
		result.insert(QStringLiteral("end_frame"), static_cast<qint64>(frame));
		result.insert(QStringLiteral("frame"), static_cast<qint64>(frame));
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		result.insert(QStringLiteral("watch_hits"), watchHits);
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("read8") || method == QStringLiteral("read16") || method == QStringLiteral("read32")) {
		CoreRequest req;
		req.op = CoreOp::Read;
		req.width = method == QStringLiteral("read8") ? 1 : method == QStringLiteral("read16") ? 2 : 4;
		const uint32_t maximum = std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(req.width - 1);
		if (!jsonUint(params.value(QStringLiteral("address")), maximum, &req.address)) {
			return makeError(id, -32602, QStringLiteral("address must be an integer in the 32-bit address range"));
		}
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("read failed"));
		}
		return makeResponse(id, static_cast<qint64>(req.u32));
	}

	if (method == QStringLiteral("read_range")) {
		CoreRequest req;
		req.op = CoreOp::ReadRange;
		if (!jsonUint(params.value(QStringLiteral("address")), std::numeric_limits<uint32_t>::max(), &req.address)) {
			return makeError(id, -32602, QStringLiteral("address must be an integer in the 32-bit address range"));
		}
		if (!jsonUint(params.value(QStringLiteral("length")), 4096, &req.length) || !req.length) {
			return makeError(id, -32602, QStringLiteral("length must be 1-4096"));
		}
		if (static_cast<quint64>(req.address) + req.length > (static_cast<quint64>(std::numeric_limits<uint32_t>::max()) + 1)) {
			return makeError(id, -32602, QStringLiteral("address plus length exceeds the 32-bit address range"));
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
		req.width = method == QStringLiteral("write8") ? 1 : method == QStringLiteral("write16") ? 2 : 4;
		const uint32_t addressMaximum = std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(req.width - 1);
		const uint32_t valueMaximum = req.width == 1 ? 0xFFu : req.width == 2 ? 0xFFFFu : std::numeric_limits<uint32_t>::max();
		if (!jsonUint(params.value(QStringLiteral("address")), addressMaximum, &req.address)) {
			return makeError(id, -32602, QStringLiteral("address must be an integer in the 32-bit address range"));
		}
		if (!jsonUint(params.value(QStringLiteral("value")), valueMaximum, &req.value)) {
			return makeError(id, -32602, QStringLiteral("value is outside the selected width"));
		}
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("write failed"));
		}
		return makeResponse(id, true);
	}

	if (method == QStringLiteral("key_down") || method == QStringLiteral("key_up")) {
		uint32_t key = 0;
		if (!parseInputKey(params, &key)) {
			return makeError(id, -32602, QStringLiteral("key must be one of A, B, L, R, Start, Select, Up, Down, Left, Right"));
		}
		CoreRequest req;
		req.op = CoreOp::Input;
		req.key = key;
		req.pressed = method == QStringLiteral("key_down");
		if (req.pressed) {
			m_controller->addKey(static_cast<int>(key));
		} else {
			m_controller->clearKey(static_cast<int>(key));
		}
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("input update failed"));
		}
		if (req.pressed) {
			m_inputMask |= 1u << key;
		} else {
			m_inputMask &= ~(1u << key);
		}
		QJsonObject result;
		result.insert(QStringLiteral("frame"), static_cast<qint64>(req.u32));
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("pause")) {
		mCoreThreadPause(thread);
		CoreRequest req;
		req.op = CoreOp::FrameCounter;
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("pause failed"));
		}
		QJsonObject result;
		result.insert(QStringLiteral("frame"), static_cast<qint64>(req.u32));
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("unpause") || method == QStringLiteral("resume")) {
		m_controller->setPaused(false);
		CoreRequest req;
		req.op = CoreOp::FrameCounter;
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("unpause failed"));
		}
		QJsonObject result;
		result.insert(QStringLiteral("frame"), static_cast<qint64>(req.u32));
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("reset")) {
		m_controller->reset();
		CoreRequest req;
		req.op = CoreOp::FrameCounter;
		if (!runCore(thread, req)) {
			return makeError(id, -32603, QStringLiteral("reset failed"));
		}
		QJsonObject result;
		result.insert(QStringLiteral("frame"), static_cast<qint64>(req.u32));
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("step_frame") || method == QStringLiteral("advance_frames") || method == QStringLiteral("run_frames")) {
		uint32_t count = 1;
		if (method != QStringLiteral("step_frame") && params.contains(QStringLiteral("count"))
			&& (!jsonUint(params.value(QStringLiteral("count")), 1000, &count) || !count)) {
			return makeError(id, -32602, QStringLiteral("count must be an integer in 1-1000"));
		}
		if (method == QStringLiteral("step_frame") && params.contains(QStringLiteral("count"))) {
			return makeError(id, -32602, QStringLiteral("step_frame always advances exactly one frame"));
		}

		bool pauseAfter = true;
		if (!jsonBool(params, QStringLiteral("pause_after"), true, &pauseAfter)) {
			return makeError(id, -32602, QStringLiteral("pause_after must be boolean"));
		}
		if (method == QStringLiteral("step_frame") && !pauseAfter) {
			return makeError(id, -32602, QStringLiteral("step_frame always pauses after the frame"));
		}

		uint32_t fps = 0;
		uint32_t intervalMs = 0;
		const bool hasFps = params.contains(QStringLiteral("fps"));
		const bool hasInterval = params.contains(QStringLiteral("frame_interval_ms"));
		if (hasFps && hasInterval) {
			return makeError(id, -32602, QStringLiteral("specify fps or frame_interval_ms, not both"));
		}
		if (hasFps && !jsonUint(params.value(QStringLiteral("fps")), 240, &fps)) {
			return makeError(id, -32602, QStringLiteral("fps must be an integer in 0-240"));
		}
		if (hasInterval && !jsonUint(params.value(QStringLiteral("frame_interval_ms")), 1000, &intervalMs)) {
			return makeError(id, -32602, QStringLiteral("frame_interval_ms must be an integer in 0-1000"));
		}
		const quint64 durationMs = hasFps ? (fps ? (static_cast<quint64>(count) * 1000 + fps - 1) / fps : 0)
			: hasInterval ? static_cast<quint64>(count) * intervalMs : 0;
		if (durationMs > 60000) {
			return makeError(id, -32602, QStringLiteral("requested run duration exceeds 60 seconds"));
		}

		mCoreThreadPause(thread);
		CoreRequest startRequest;
		startRequest.op = CoreOp::FrameCounter;
		if (!runCore(thread, startRequest)) {
			return makeError(id, -32603, QStringLiteral("could not read starting frame"));
		}
		const uint32_t startFrame = startRequest.u32;
		QElapsedTimer timer;
		timer.start();
		uint32_t frame = 0;
		bool watchPause = false;
		QJsonArray watchHits;
		for (uint32_t i = 0; i < count; ++i) {
			CoreRequest req;
			req.op = CoreOp::RunFrame;
			req.length = 1;
			if (!runCore(thread, req)) {
				return makeError(id, -32603, QStringLiteral("frame advance failed"));
			}
			frame = req.u32;
			const QJsonArray frameHits = checkWatches(thread);
			for (const QJsonValue& hit : frameHits) {
				watchHits.append(hit);
				if (hit.toObject().value(QStringLiteral("pause_on_hit")).toBool()) {
					watchPause = true;
				}
			}
			const quint64 targetMs = hasFps && fps
				? (static_cast<quint64>(i + 1) * 1000 + fps - 1) / fps
				: hasInterval ? static_cast<quint64>(i + 1) * intervalMs : 0;
			const qint64 remaining = static_cast<qint64>(targetMs) - timer.elapsed();
			if (remaining > 0) {
				QThread::msleep(static_cast<unsigned long>(remaining));
			}
		}
		if (!pauseAfter && !watchPause) {
			m_controller->setPaused(false);
		}
		QJsonObject result;
		result.insert(QStringLiteral("start_frame"), static_cast<qint64>(startFrame));
		result.insert(QStringLiteral("end_frame"), static_cast<qint64>(frame));
		result.insert(QStringLiteral("frame"), static_cast<qint64>(frame));
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		result.insert(QStringLiteral("input"), inputNames(m_inputMask));
		result.insert(QStringLiteral("watch_hits"), watchHits);
		return makeResponse(id, result);
	}

	if (method == QStringLiteral("export_snapshot")) {
		const AgentSettings settings = AgentSettings::fromConfig(m_config);
		QVector<uint32_t> focusAddresses;
		if (params.contains(QStringLiteral("focus_addresses"))) {
			const QJsonValue values = params.value(QStringLiteral("focus_addresses"));
			if (!values.isArray() || values.toArray().size() > 16) {
				return makeError(id, -32602, QStringLiteral("focus_addresses must be an array of at most 16 addresses"));
			}
			for (const QJsonValue& value : values.toArray()) {
				uint32_t address = 0;
				if (!jsonUint(value, std::numeric_limits<uint32_t>::max(), &address)) {
					return makeError(id, -32602, QStringLiteral("focus_addresses must contain 32-bit integers"));
				}
				focusAddresses.append(address);
			}
		}
		QString error;
		const QString outDir = SnapshotExporter::exportSnapshot(m_controller.get(), m_romPath, settings.regions, focusAddresses, &error);
		if (outDir.isEmpty()) {
			return makeError(id, -32603, error.isEmpty() ? QStringLiteral("export failed") : error);
		}
		QJsonObject result;
		result.insert(QStringLiteral("path"), outDir);
		CoreRequest req;
		req.op = CoreOp::FrameCounter;
		if (runCore(thread, req)) {
			result.insert(QStringLiteral("frame"), static_cast<qint64>(req.u32));
		}
		result.insert(QStringLiteral("paused"), mCoreThreadIsPaused(thread));
		return makeResponse(id, result);
	}

	return makeError(id, -32601, QStringLiteral("unknown method: ") + method);
}
