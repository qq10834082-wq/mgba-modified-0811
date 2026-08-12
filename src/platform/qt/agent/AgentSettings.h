/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <mgba/core/interface.h>

#include <QString>

namespace QGBA {

class ConfigController;

struct AgentExportRegions {
	bool ewram = true;
	bool iwram = true;
	bool vram = true;
	bool oam = false;
	bool palette = false;
	bool io = false;
	bool sram = false;
	bool bios = false;
	bool hram = false;

	static AgentExportRegions fromConfig(const ConfigController* config);
	bool shouldExport(const mCoreMemoryBlock& block) const;
};

struct AgentSettings {
	bool enabled = true;
	QString host = QStringLiteral("127.0.0.1");
	quint16 port = 8765;
	AgentExportRegions regions;

	static AgentSettings fromConfig(const ConfigController* config);
};

}
