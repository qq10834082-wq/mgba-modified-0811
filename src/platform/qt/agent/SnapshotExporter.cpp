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
#include <mgba/core/interface.h>
#include <mgba/core/sync.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>
#include <mgba-util/vfs.h>

#ifdef USE_PNG
#include <mgba-util/png-io.h>
#endif

using namespace QGBA;

namespace {

struct CaptureWork {
	QString romPath;
	AgentExportRegions regions;

	uint32_t frame = 0;
	unsigned width = 0;
	unsigned height = 0;
	size_t stride = 0;
	QByteArray pixels;
	QByteArray memory;
	QJsonArray regionArray;
	QString romTitle;
	QString romCode;
	QString platform;
	bool paused = false;

	QString outputDir;
	bool success = false;
	QString error;
};

// Shared (not thread_local): mCoreThreadRunFunction runs the callback on the
// emulation thread, while the caller sets this pointer on the UI/RPC thread.
static QMutex s_exportMutex;
static CaptureWork* s_captureWork = nullptr;

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

// Emulation thread: same-frame capture only (pixels + memory + metadata fields).
// No directory creation, PNG encode, or disk I/O here — those stall the game loop.
static void runCapture(mCoreThread* context) {
	CaptureWork* work = s_captureWork;
	if (!work) {
		return;
	}
	mCore* core = context->core;
	if (!core || !mCoreThreadHasStarted(context)) {
		work->error = QStringLiteral("no game loaded");
		return;
	}

	work->frame = core->frameCounter(core);

	QString romPath = work->romPath;
	if (romPath.isEmpty()) {
		romPath = QString::fromUtf8(core->dirs.baseName);
	}
	if (romPath.isEmpty()) {
		work->error = QStringLiteral("invalid ROM path");
		return;
	}
	work->romPath = romPath;

	core->desiredVideoDimensions(core, &work->width, &work->height);
	if (!work->width || !work->height) {
		work->error = QStringLiteral("invalid video dimensions");
		return;
	}

	const void* pixels = nullptr;
	size_t stride = 0;
	core->getPixels(core, &pixels, &stride);
	if (!pixels || !stride) {
		work->error = QStringLiteral("failed to capture pixels");
		return;
	}
	work->stride = stride;
	const int pixelBytes = static_cast<int>(stride * work->height * BYTES_PER_PIXEL);
	work->pixels = QByteArray(static_cast<const char*>(pixels), pixelBytes);

	const mCoreMemoryBlock* blocks;
	const size_t nBlocks = core->listMemoryBlocks(core, &blocks);

	int totalMemory = 0;
	for (size_t i = 0; i < nBlocks; ++i) {
		const mCoreMemoryBlock& block = blocks[i];
		if (!work->regions.shouldExport(block)) {
			continue;
		}
		size_t blockSize = 0;
		const void* data = core->getMemoryBlock(core, block.id, &blockSize);
		if (data && blockSize) {
			totalMemory += static_cast<int>(blockSize);
		}
	}

	work->memory.clear();
	work->memory.reserve(totalMemory);
	work->regionArray = QJsonArray();
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

		work->memory.append(data, static_cast<int>(blockSize));

		QJsonObject region;
		region.insert(QStringLiteral("name"), QString::fromUtf8(block.internalName));
		region.insert(QStringLiteral("address"), static_cast<qint64>(block.start));
		region.insert(QStringLiteral("offset"), static_cast<qint64>(offset));
		region.insert(QStringLiteral("size"), static_cast<qint64>(blockSize));
		work->regionArray.append(region);

		offset += blockSize;
	}

	char title[17] = {};
	char code[5] = {};
	core->getGameTitle(core, title);
	core->getGameCode(core, code);
	work->romTitle = QString::fromUtf8(title).trimmed();
	work->romCode = QString::fromUtf8(code).trimmed();
	work->platform = platformName(core->platform(core));
	work->success = true;
}

// mCoreThreadRunFunction clears audioWait/videoFrameWait for the whole callback via
// _waitPrologue. DisplayGL may see that window and call swapInterval(0). Re-assert
// controller sync and wake waiters so export cannot leave pacing permanently slow.
static void restoreEmulationSync(CoreController* controller) {
	if (!controller || !controller->thread() || !controller->thread()->impl) {
		return;
	}
	controller->setSync(true);
	mCoreSync* sync = &controller->thread()->impl->sync;
	mCoreSyncForceFrame(sync);
	mCoreSyncLockAudio(sync);
	mCoreSyncConsumeAudio(sync);
}

static bool writeSnapshotFiles(CaptureWork& work, QString* errorOut) {
	auto setError = [&](const QString& err) {
		work.error = err;
		if (errorOut) {
			*errorOut = err;
		}
	};

	QString baseDir = resolveSnapshotBaseDir(work.romPath);
	const QString dirName = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))
		+ QStringLiteral("_f") + QString::number(work.frame);
	work.outputDir = baseDir + QDir::separator() + dirName;

	QDir dir;
	if (!dir.mkpath(work.outputDir)) {
		const QString leaf = QFileInfo(work.romPath).completeBaseName().isEmpty()
			? QStringLiteral("snapshot")
			: QFileInfo(work.romPath).completeBaseName();
		baseDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
			+ QDir::separator() + QStringLiteral("mGBA-agent-snapshots")
			+ QDir::separator() + leaf;
		work.outputDir = baseDir + QDir::separator() + dirName;
		if (!dir.mkpath(work.outputDir)) {
			setError(QStringLiteral("failed to create output directory"));
			return false;
		}
	}

#ifdef USE_PNG
	const QString screenshotPath = work.outputDir + QDir::separator() + QStringLiteral("screenshot.png");
	VFile* screenshotVf = VFileDevice::open(screenshotPath, O_CREAT | O_TRUNC | O_WRONLY);
	if (!screenshotVf) {
		setError(QStringLiteral("failed to open screenshot file"));
		return false;
	}
	png_structp png = PNGWriteOpen(screenshotVf);
	png_infop info = PNGWriteHeader(png, work.width, work.height);
	const bool pngOk = PNGWritePixels(png, work.width, work.height, work.stride, work.pixels.constData());
	PNGWriteClose(png, info);
	screenshotVf->close(screenshotVf);
	if (!pngOk) {
		setError(QStringLiteral("failed to write screenshot"));
		return false;
	}
#else
	setError(QStringLiteral("PNG support not available"));
	return false;
#endif

	const QString memoryPath = work.outputDir + QDir::separator() + QStringLiteral("memory.bin");
	QFile memoryFile(memoryPath);
	if (!memoryFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		setError(QStringLiteral("failed to open memory file"));
		return false;
	}
	if (memoryFile.write(work.memory) != work.memory.size()) {
		setError(QStringLiteral("failed to write memory file"));
		return false;
	}
	memoryFile.close();

	QJsonObject metadata;
	metadata.insert(QStringLiteral("format_version"), 1);
	metadata.insert(QStringLiteral("mgba_version"), QString::fromUtf8(projectVersion));
	metadata.insert(QStringLiteral("rom_path"), work.romPath);
	metadata.insert(QStringLiteral("rom_title"), work.romTitle);
	metadata.insert(QStringLiteral("rom_code"), work.romCode);
	metadata.insert(QStringLiteral("platform"), work.platform);
	metadata.insert(QStringLiteral("snapshot_time"), QDateTime::currentDateTime().toString(Qt::ISODate));
	metadata.insert(QStringLiteral("frame"), static_cast<qint64>(work.frame));
	metadata.insert(QStringLiteral("capture_method"), QStringLiteral("atomic"));
	metadata.insert(QStringLiteral("paused"), work.paused);

	QJsonObject screen;
	screen.insert(QStringLiteral("width"), static_cast<int>(work.width));
	screen.insert(QStringLiteral("height"), static_cast<int>(work.height));
	metadata.insert(QStringLiteral("screen"), screen);

	metadata.insert(QStringLiteral("memory_regions"), work.regionArray);

	QJsonObject files;
	files.insert(QStringLiteral("screenshot"), QStringLiteral("screenshot.png"));
	files.insert(QStringLiteral("memory"), QStringLiteral("memory.bin"));
	metadata.insert(QStringLiteral("files"), files);

	const QString metadataPath = work.outputDir + QDir::separator() + QStringLiteral("metadata.json");
	QFile metaFile(metadataPath);
	if (!metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		setError(QStringLiteral("failed to open metadata file"));
		return false;
	}
	metaFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
	metaFile.close();

	return true;
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

	CaptureWork work;
	work.romPath = romPath;
	work.regions = regions;
	work.paused = controller->isPaused();

	mCoreThread* thread = controller->thread();
	{
		QMutexLocker locker(&s_exportMutex);
		s_captureWork = &work;
		// Use Interrupter instead of mCoreThreadRunFunction:
		// RunFunction keeps audioWait/videoFrameWait cleared for the entire callback
		// (_waitPrologue), and DisplayGL may latch swapInterval(0) in that window —
		// which shows up as a sustained ~40 FPS until a native save/pause recovers it.
		// Interrupter only clears waits during the brief transition, then restores them
		// while the core is safely frozen for same-frame capture.
		CoreController::Interrupter interrupter(controller);
		runCapture(thread);
		s_captureWork = nullptr;
	}

	restoreEmulationSync(controller);

	if (!work.success) {
		const QString err = work.error.isEmpty() ? QStringLiteral("export failed") : work.error;
		setError(err);
		LOG(QT, WARN) << QObject::tr("Agent snapshot export failed: %1").arg(err);
		return QString();
	}

	// Disk I/O + PNG encode after the core has resumed with sync restored.
	QString writeError;
	if (!writeSnapshotFiles(work, &writeError)) {
		restoreEmulationSync(controller);
		const QString err = writeError.isEmpty() ? QStringLiteral("export failed") : writeError;
		setError(err);
		LOG(QT, WARN) << QObject::tr("Agent snapshot export failed: %1").arg(err);
		return QString();
	}

	restoreEmulationSync(controller);
	return work.outputDir;
}
