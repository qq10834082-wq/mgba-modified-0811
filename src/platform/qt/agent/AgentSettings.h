/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QString>

namespace QGBA {

class ConfigController;

struct AgentSettings {
	bool enabled = true;
	QString host = QStringLiteral("127.0.0.1");
	quint16 port = 8765;

	static AgentSettings fromConfig(const ConfigController* config);
};

}
