/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "agent/AgentSettings.h"

#include "ConfigController.h"

using namespace QGBA;

static bool optionBool(const ConfigController* config, const char* key, bool defaultVal) {
	QString value = config->getOption(key, defaultVal ? 1 : 0);
	if (value.isNull()) {
		return defaultVal;
	}
	return value != QLatin1String("0");
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
	return settings;
}
