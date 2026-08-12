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

	auto* memoryGroup = new QGroupBox(tr("Snapshot memory regions"), this);
	auto* memoryLayout = new QVBoxLayout(memoryGroup);
	m_exportEwram = new QCheckBox(tr("EWRAM / WRAM (256 KiB)"), memoryGroup);
	m_exportIwram = new QCheckBox(tr("IWRAM (32 KiB)"), memoryGroup);
	m_exportVram = new QCheckBox(tr("VRAM (96 KiB)"), memoryGroup);
	m_exportOam = new QCheckBox(tr("OAM"), memoryGroup);
	m_exportPalette = new QCheckBox(tr("Palette"), memoryGroup);
	m_exportIo = new QCheckBox(tr("MMIO"), memoryGroup);
	m_exportSram = new QCheckBox(tr("SRAM / Flash"), memoryGroup);
	m_exportBios = new QCheckBox(tr("BIOS"), memoryGroup);
	m_exportHram = new QCheckBox(tr("HRAM (GB)"), memoryGroup);
	memoryLayout->addWidget(m_exportEwram);
	memoryLayout->addWidget(m_exportIwram);
	memoryLayout->addWidget(m_exportVram);
	memoryLayout->addWidget(m_exportOam);
	memoryLayout->addWidget(m_exportPalette);
	memoryLayout->addWidget(m_exportIo);
	memoryLayout->addWidget(m_exportSram);
	memoryLayout->addWidget(m_exportBios);
	memoryLayout->addWidget(m_exportHram);
	layout->addWidget(memoryGroup);

	layout->addStretch();

	load();
}

void AgentSettingsWidget::load() {
	loadBool(m_config, m_enabled, "agent.enabled", true);
	m_host->setText(m_config->getOption("agent.host", QStringLiteral("127.0.0.1")));
	m_port->setValue(m_config->getOption("agent.port", 8765).toInt());

	loadBool(m_config, m_exportEwram, "agent.export.ewram", true);
	loadBool(m_config, m_exportIwram, "agent.export.iwram", true);
	loadBool(m_config, m_exportVram, "agent.export.vram", true);
	loadBool(m_config, m_exportOam, "agent.export.oam", false);
	loadBool(m_config, m_exportPalette, "agent.export.palette", false);
	loadBool(m_config, m_exportIo, "agent.export.io", false);
	loadBool(m_config, m_exportSram, "agent.export.sram", false);
	loadBool(m_config, m_exportBios, "agent.export.bios", false);
	loadBool(m_config, m_exportHram, "agent.export.hram", false);
}

void AgentSettingsWidget::save() {
	saveBool(m_config, m_enabled, "agent.enabled");
	m_config->setOption("agent.host", m_host->text());
	m_config->updateOption("agent.host");
	m_config->setOption("agent.port", m_port->value());
	m_config->updateOption("agent.port");

	saveBool(m_config, m_exportEwram, "agent.export.ewram");
	saveBool(m_config, m_exportIwram, "agent.export.iwram");
	saveBool(m_config, m_exportVram, "agent.export.vram");
	saveBool(m_config, m_exportOam, "agent.export.oam");
	saveBool(m_config, m_exportPalette, "agent.export.palette");
	saveBool(m_config, m_exportIo, "agent.export.io");
	saveBool(m_config, m_exportSram, "agent.export.sram");
	saveBool(m_config, m_exportBios, "agent.export.bios");
	saveBool(m_config, m_exportHram, "agent.export.hram");

	emit settingsChanged();
}
