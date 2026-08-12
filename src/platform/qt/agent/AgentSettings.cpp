/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "agent/AgentSettings.h"

#include "ConfigController.h"

#include <cstring>

using namespace QGBA;

static bool optionBool(const ConfigController* config, const char* key, bool defaultVal) {
	QString value = config->getOption(key, defaultVal ? 1 : 0);
	if (value.isNull()) {
		return defaultVal;
	}
	return value != QLatin1String("0");
}

AgentExportRegions AgentExportRegions::fromConfig(const ConfigController* config) {
	AgentExportRegions regions;
	regions.ewram = optionBool(config, "agent.export.ewram", true);
	regions.iwram = optionBool(config, "agent.export.iwram", true);
	regions.vram = optionBool(config, "agent.export.vram", true);
	regions.oam = optionBool(config, "agent.export.oam", false);
	regions.palette = optionBool(config, "agent.export.palette", false);
	regions.io = optionBool(config, "agent.export.io", false);
	regions.sram = optionBool(config, "agent.export.sram", false);
	regions.bios = optionBool(config, "agent.export.bios", false);
	regions.hram = optionBool(config, "agent.export.hram", false);
	return regions;
}

bool AgentExportRegions::shouldExport(const mCoreMemoryBlock& block) const {
	if (block.id < 0) {
		return false;
	}
	if (!(block.flags & mCORE_MEMORY_MAPPED)) {
		return false;
	}
	if (block.flags & mCORE_MEMORY_WORM) {
		return false;
	}
	if (!block.internalName) {
		return false;
	}
	if (!strcmp(block.internalName, "wram")) {
		return ewram;
	}
	if (!strcmp(block.internalName, "iwram")) {
		return iwram;
	}
	if (!strcmp(block.internalName, "vram")) {
		return vram;
	}
	if (!strcmp(block.internalName, "oam")) {
		return oam;
	}
	if (!strcmp(block.internalName, "palette")) {
		return palette;
	}
	if (!strcmp(block.internalName, "io")) {
		return io;
	}
	if (!strcmp(block.internalName, "sram")) {
		return sram;
	}
	if (!strcmp(block.internalName, "bios")) {
		return bios;
	}
	if (!strcmp(block.internalName, "hram")) {
		return hram;
	}
	return false;
}

AgentSettings AgentSettings::fromConfig(const ConfigController* config) {
	AgentSettings settings;
	settings.enabled = optionBool(config, "agent.enabled", true);
	settings.host = config->getOption("agent.host", QStringLiteral("127.0.0.1"));
	if (settings.host.isEmpty()) {
		settings.host = QStringLiteral("127.0.0.1");
	}
	settings.port = config->getOption("agent.port", 8765).toUInt();
	if (!settings.port) {
		settings.port = 8765;
	}
	settings.regions = AgentExportRegions::fromConfig(config);
	return settings;
}
