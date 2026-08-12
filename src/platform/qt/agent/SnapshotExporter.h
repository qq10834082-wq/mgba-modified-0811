/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "agent/AgentSettings.h"

#include <QString>

namespace QGBA {

class CoreController;

class SnapshotExporter {
public:
	// Atomically captures screenshot, memory, and metadata for one emulated frame.
	// Returns the output directory path, or an empty string on failure.
	// If errorOut is non-null, it receives a short failure reason.
	static QString exportSnapshot(CoreController* controller, const QString& romPath, const AgentExportRegions& regions, QString* errorOut = nullptr);
};

}
