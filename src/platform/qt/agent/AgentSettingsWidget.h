/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace QGBA {

class ConfigController;

class AgentSettingsWidget : public QWidget {
Q_OBJECT

public:
	AgentSettingsWidget(ConfigController* config, QWidget* parent = nullptr);

	void load();
	void save();

signals:
	void settingsChanged();

private:
	ConfigController* m_config;

	QCheckBox* m_enabled;
	QLineEdit* m_host;
	QSpinBox* m_port;
};

}
