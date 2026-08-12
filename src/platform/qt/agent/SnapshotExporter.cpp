/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "agent/SnapshotExporter.h"

#include "CoreController.h"
#include "LogController.h"
#include "VFileDevice.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>
#include <mgba-util/vfs.h>

#ifdef USE_PNG
#include <mgba-util/png-io.h>
#endif

using namespace QGBA;

namespace {

struct ExportWork {
	QString romPath;
	AgentExportRegions regions;
	QString outputDir;
	bool success = false;
	QString error;
};

// Shared (not thread_local): mCoreThreadRunFunction runs the callback on the
// emulation thread, while the caller sets this pointer on the UI/RPC thread.
static QMutex s_exportMutex;
static ExportWork* s_exportWork = nullptr;

static QString platformName(mPlatform platform) {
	switch (platform) {
	case mPLATFORM_GBA:
		return QStringLiteral("gba");
	case mPLATFORM_GB:
		return QStringLiteral("gb");
	default:
		return QStringLiteral("unknown");
	}
}

// Resolve a writable base directory for snapshots.
// Loose ROM: <romDir>/<romStem>/
// Archive path (e.g. C:/roms/game.zip/inner.gba): <romDir>/<zipStem>_<innerStem>/
// Fallback: Documents/mGBA-agent-snapshots/<stem>/
static QString resolveSnapshotBaseDir(const QString& romPath) {
	QFileInfo romInfo(romPath);
	if (romInfo.isFile()) {
		return romInfo.absolutePath() + QDir::separator() + romInfo.completeBaseName();
	}

	QString leaf = romInfo.completeBaseName();
	if (leaf.isEmpty()) {
		leaf = QStringLiteral("snapshot");
	}

	QString probe = QDir::cleanPath(romPath);
	while (!probe.isEmpty() && probe != QStringLiteral(".") && probe != QStringLiteral("/")) {
		QFileInfo fi(probe);
		if (fi.isDir()) {
			return fi.absoluteFilePath() + QDir::separator() + leaf;
		}
		if (fi.isFile()) {
			// Hit an archive container (e.g. .zip) — write beside it.
			return fi.absolutePath() + QDir::separator() + fi.completeBaseName() + QLatin1Char('_') + leaf;
		}
		const QString parent = fi.path();
		if (parent == probe) {
			break;
		}
		probe = parent;
	}

	const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	return docs + QDir::separator() + QStringLiteral("mGBA-agent-snapshots") + QDir::separator() + leaf;
}

static void runExport(mCoreThread* context) {
	ExportWork* work = s_exportWork;
	if (!work) {
		return;
	}
	mCore* core = context->core;
	if (!core || !mCoreThreadHasStarted(context)) {
		work->error = QStringLiteral("no game loaded");
		return;
	}

	const uint32_t frame = core->frameCounter(core);

	QString romPath = work->romPath;
	if (romPath.isEmpty()) {
		romPath = QString::fromUtf8(core->dirs.baseName);
	}
	if (romPath.isEmpty()) {
		work->error = QStringLiteral("invalid ROM path");
		return;
	}
	work->romPath = romPath;

	QString baseDir = resolveSnapshotBaseDir(romPath);
	const QString dirName = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))
		+ QStringLiteral("_f") + QString::number(frame);
	work->outputDir = baseDir + QDir::separator() + dirName;

	QDir dir;
	if (!dir.mkpath(work->outputDir)) {
		// Last resort: always-writable Documents location.
		const QString leaf = QFileInfo(romPath).completeBaseName().isEmpty()
			? QStringLiteral("snapshot")
			: QFileInfo(romPath).completeBaseName();
		baseDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
			+ QDir::separator() + QStringLiteral("mGBA-agent-snapshots")
			+ QDir::separator() + leaf;
		work->outputDir = baseDir + QDir::separator() + dirName;
		if (!dir.mkpath(work->outputDir)) {
			work->error = QStringLiteral("failed to create output directory");
			return;
		}
	}

#ifdef USE_PNG
	const QString screenshotPath = work->outputDir + QDir::separator() + QStringLiteral("screenshot.png");
	// VFileDevice(QString, OpenMode) maps WriteOnly to O_WRONLY only (no O_CREAT).
	VFile* screenshotVf = VFileDevice::open(screenshotPath, O_CREAT | O_TRUNC | O_WRONLY);
	if (!screenshotVf) {
		work->error = QStringLiteral("failed to open screenshot file");
		return;
	}
	if (!mCoreTakeScreenshotVF(core, screenshotVf)) {
		screenshotVf->close(screenshotVf);
		work->error = QStringLiteral("failed to write screenshot");
		return;
	}
	screenshotVf->close(screenshotVf);
#else
	work->error = QStringLiteral("PNG support not available");
	return;
#endif

	const mCoreMemoryBlock* blocks;
	const size_t nBlocks = core->listMemoryBlocks(core, &blocks);

	QByteArray memory;
	QJsonArray regionArray;
	quint64 offset = 0;

	for (size_t i = 0; i < nBlocks; ++i) {
		const mCoreMemoryBlock& block = blocks[i];
		if (!work->regions.shouldExport(block)) {
			continue;
		}

		size_t blockSize = 0;
		const char* data = static_cast<const char*>(core->getMemoryBlock(core, block.id, &blockSize));
		if (!data || !blockSize) {
			continue;
		}

		memory.append(data, blockSize);

		QJsonObject region;
		region.insert(QStringLiteral("name"), QString::fromUtf8(block.internalName));
		region.insert(QStringLiteral("address"), static_cast<qint64>(block.start));
		region.insert(QStringLiteral("offset"), static_cast<qint64>(offset));
		region.insert(QStringLiteral("size"), static_cast<qint64>(blockSize));
		regionArray.append(region);

		offset += blockSize;
	}

	const QString memoryPath = work->outputDir + QDir::separator() + QStringLiteral("memory.bin");
	QFile memoryFile(memoryPath);
	if (!memoryFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		work->error = QStringLiteral("failed to open memory file");
		return;
	}
	if (memoryFile.write(memory) != memory.size()) {
		work->error = QStringLiteral("failed to write memory file");
		return;
	}
	memoryFile.close();

	char title[17] = {};
	char code[5] = {};
	core->getGameTitle(core, title);
	core->getGameCode(core, code);

	unsigned width = 0;
	unsigned height = 0;
	core->desiredVideoDimensions(core, &width, &height);

	QJsonObject metadata;
	metadata.insert(QStringLiteral("format_version"), 1);
	metadata.insert(QStringLiteral("mgba_version"), QString::fromUtf8(projectVersion));
	metadata.insert(QStringLiteral("rom_path"), work->romPath);
	metadata.insert(QStringLiteral("rom_title"), QString::fromUtf8(title).trimmed());
	metadata.insert(QStringLiteral("rom_code"), QString::fromUtf8(code).trimmed());
	metadata.insert(QStringLiteral("platform"), platformName(core->platform(core)));
	metadata.insert(QStringLiteral("snapshot_time"), QDateTime::currentDateTime().toString(Qt::ISODate));
	metadata.insert(QStringLiteral("frame"), static_cast<qint64>(frame));
	metadata.insert(QStringLiteral("capture_method"), QStringLiteral("atomic"));
	metadata.insert(QStringLiteral("paused"), mCoreThreadIsPaused(context));

	QJsonObject screen;
	screen.insert(QStringLiteral("width"), static_cast<int>(width));
	screen.insert(QStringLiteral("height"), static_cast<int>(height));
	metadata.insert(QStringLiteral("screen"), screen);

	metadata.insert(QStringLiteral("memory_regions"), regionArray);

	QJsonObject files;
	files.insert(QStringLiteral("screenshot"), QStringLiteral("screenshot.png"));
	files.insert(QStringLiteral("memory"), QStringLiteral("memory.bin"));
	metadata.insert(QStringLiteral("files"), files);

	const QString metadataPath = work->outputDir + QDir::separator() + QStringLiteral("metadata.json");
	QFile metaFile(metadataPath);
	if (!metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		work->error = QStringLiteral("failed to open metadata file");
		return;
	}
	const QJsonDocument doc(metadata);
	metaFile.write(doc.toJson(QJsonDocument::Indented));
	metaFile.close();

	work->success = true;
}

} // namespace

QString SnapshotExporter::exportSnapshot(CoreController* controller, const QString& romPath, const AgentExportRegions& regions, QString* errorOut) {
	auto setError = [errorOut](const QString& err) {
		if (errorOut) {
			*errorOut = err;
		}
	};

	if (!controller) {
		setError(QStringLiteral("no controller"));
		return QString();
	}
	if (!controller->hasStarted()) {
		setError(QStringLiteral("no game running"));
		LOG(QT, WARN) << QObject::tr("Cannot export snapshot: no game running");
		return QString();
	}

	ExportWork work;
	work.romPath = romPath;
	work.regions = regions;

	mCoreThread* thread = controller->thread();
	QMutexLocker locker(&s_exportMutex);
	s_exportWork = &work;
	mCoreThreadRunFunction(thread, runExport);
	s_exportWork = nullptr;

	if (!work.success) {
		const QString err = work.error.isEmpty() ? QStringLiteral("export failed") : work.error;
		setError(err);
		LOG(QT, WARN) << QObject::tr("Agent snapshot export failed: %1").arg(err);
		return QString();
	}

	return work.outputDir;
}
