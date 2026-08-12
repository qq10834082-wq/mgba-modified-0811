/* Copyright (c) 2026 mGBA modified
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "agent/AgentSettingsWidget.h"

#include "ConfigController.h"

using namespace QGBA;

static void loadBool(ConfigController* config, QCheckBox* box, const char* key, bool defaultVal) {
	QString value = config->getOption(key, defaultVal ? 1 : 0);
	box->setChecked(value.isNull() ? defaultVal : value != QLatin1String("0"));
}

static void saveBool(ConfigController* config, const QCheckBox* box, const char* key) {
	config->setOption(key, box->isChecked() ? 1 : 0);
	config->updateOption(key);
}

AgentSettingsWidget::AgentSettingsWidget(ConfigController* config, QWidget* parent)
	: QWidget(parent)
	, m_config(config)
{
	auto* layout = new QVBoxLayout(this);

	auto* serverGroup = new QGroupBox(tr("Agent server"), this);
	auto* serverForm = new QFormLayout(serverGroup);
	m_enabled = new QCheckBox(tr("Enable agent server on startup"), serverGroup);
	m_host = new QLineEdit(serverGroup);
	m_port = new QSpinBox(serverGroup);
	m_port->setRange(1, 65535);
	serverForm->addRow(m_enabled);
	serverForm->addRow(tr("Host"), m_host);
	serverForm->addRow(tr("Port"), m_port);
	layout->addWidget(serverGroup);

	layout->addStretch();

	load();
}

void AgentSettingsWidget::load() {
	loadBool(m_config, m_enabled, "agent.enabled", true);
	m_host->setText(m_config->getOption("agent.host", QStringLiteral("127.0.0.1")));
	m_port->setValue(m_config->getOption("agent.port", 8765).toInt());
}

void AgentSettingsWidget::save() {
	saveBool(m_config, m_enabled, "agent.enabled");
	m_config->setOption("agent.host", m_host->text());
	m_config->updateOption("agent.host");
	m_config->setOption("agent.port", m_port->value());
	m_config->updateOption("agent.port");

	emit settingsChanged();
}
