/* Copyright 2013 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "OneSixVersionBuilder.h"

#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QDir>
#include <QDebug>

#include "VersionFinal.h"
#include "OneSixInstance.h"
#include "OneSixRule.h"
#include "VersionFile.h"
#include "MMCJson.h"
#include "modutils.h"
#include "logger/QsLog.h"

OneSixVersionBuilder::OneSixVersionBuilder()
{
}

void OneSixVersionBuilder::build(VersionFinal *version, OneSixInstance *instance, const bool onlyVanilla, const QStringList &external)
{
	OneSixVersionBuilder builder;
	builder.m_version = version;
	builder.m_instance = instance;
	builder.buildInternal(onlyVanilla, external);
}

void OneSixVersionBuilder::readJsonAndApplyToVersion(VersionFinal *version, const QJsonObject &obj)
{
	OneSixVersionBuilder builder;
	builder.m_version = version;
	builder.m_instance = 0;
	builder.readJsonAndApply(obj);
}

void OneSixVersionBuilder::buildInternal(const bool onlyVanilla, const QStringList &external)
{
	m_version->clear();

	QDir root(m_instance->instanceRoot());
	QDir patches(root.absoluteFilePath("patches/"));

	// if we do external files, do just those.
	if(!external.isEmpty()) for (auto fileName : external)
	{
		QLOG_INFO() << "Reading" << fileName;
		VersionFile file = parseJsonFile(QFileInfo(fileName), false, fileName.endsWith("pack.json"));
		file.name = QFileInfo(fileName).fileName();
		file.fileId = "org.multimc.external." + file.name;
		file.version = QString();
		file.mcVersion = QString();
		file.applyTo(m_version);
	}
	// else, if there's custom json, we just do that.
	else if (QFile::exists(root.absoluteFilePath("custom.json")))
	{
		QLOG_INFO() << "Reading custom.json";
		VersionFile file = parseJsonFile(QFileInfo(root.absoluteFilePath("custom.json")), false);
		file.name = "custom.json";
		file.filename = "custom.json";
		file.fileId = "org.multimc.custom.json";
		file.version = QString();
		file.applyTo(m_version);
		// QObject::tr("The version descriptors of this instance are not compatible with the current version of MultiMC"));
		// QObject::tr("Error while applying %1. Please check MultiMC-0.log for more info.")
	}
	// version.json -> patches/*.json -> user.json
	else do
	{
		// version.json
		QLOG_INFO() << "Reading version.json";
		VersionFile file;
		parseJsonFile(QFileInfo(root.absoluteFilePath("version.json")), false, &file);
		file.name = "Minecraft";
		file.fileId = "org.multimc.version.json";
		file.version = m_instance->intendedVersionId();
		file.mcVersion = m_instance->intendedVersionId();
		file.applyTo(m_version);
		// QObject::tr("Error while applying %1. Please check MultiMC-0.log for more info.").arg(root.absoluteFilePath("version.json")));

		if (onlyVanilla)
			break;

		// patches/
		// load all, put into map for ordering, apply in the right order
		QMap<QString, int> overrideOrder = readOverrideOrders(m_instance);

		QMap<int, QPair<QString, VersionFile>> files;
		for (auto info : patches.entryInfoList(QStringList() << "*.json", QDir::Files))
		{
			QLOG_INFO() << "Reading" << info.fileName();
			VersionFile file = parseJsonFile(info, true);
			if (overrideOrder.contains(file.fileId))
			{
				file.order = overrideOrder.value(file.fileId);
			}
			if (files.contains(file.order))
			{
				throw VersionBuildError(QObject::tr("%1 has the same order as %2").arg(file.fileId, files[file.order].second.fileId));
			}
			files.insert(file.order, qMakePair(info.fileName(), file));
		}
		for (auto order : files.keys())
		{
			QLOG_DEBUG() << "Applying file with order" << order;
			auto filePair = files[order];
			filePair.second.applyTo(m_version);
		}
	} while(0);

	// some final touches
	finalizeVersion();
}

void OneSixVersionBuilder::finalizeVersion()
{
	if (m_version->assets.isEmpty())
	{
		m_version->assets = "legacy";
	}
	if (m_version->minecraftArguments.isEmpty())
	{
		QString toCompare = m_version->processArguments.toLower();
		if (toCompare == "legacy")
		{
			m_version->minecraftArguments = " ${auth_player_name} ${auth_session}";
		}
		else if (toCompare == "username_session")
		{
			m_version->minecraftArguments =
				"--username ${auth_player_name} --session ${auth_session}";
		}
		else if (toCompare == "username_session_version")
		{
			m_version->minecraftArguments = "--username ${auth_player_name} "
											"--session ${auth_session} "
											"--version ${profile_name}";
		}
	}
}

void OneSixVersionBuilder::readJsonAndApply(const QJsonObject &obj)
{
	m_version->clear();

	VersionFile file = VersionFile::fromJson(QJsonDocument(obj), QString(), false);
	// QObject::tr("Error while reading. Please check MultiMC-0.log for more info."));

	file.applyTo(m_version);
	// QObject::tr("Error while applying. Please check MultiMC-0.log for more info."));
	// QObject::tr("The version descriptors of this instance are not compatible with the current version of MultiMC"));
}

VersionFile OneSixVersionBuilder::parseJsonFile(const QFileInfo &fileInfo, const bool requireOrder, bool isFTB)
{
	QFile file(fileInfo.absoluteFilePath());
	if (!file.open(QFile::ReadOnly))
	{
		throw JSONValidationError(QObject::tr("Unable to open %1: %2").arg(file.fileName(), file.errorString()));
	}
	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError)
	{
		throw JSONValidationError(QObject::tr("Unable to parse %1: %2 at %3")
								  .arg(file.fileName(), error.errorString())
								  .arg(error.offset));
	}
	return VersionFile::fromJson(doc, file.fileName(), requireOrder, isFTB);
	// QObject::tr("Error while reading %1. Please check MultiMC-0.log for more info.").arg(file.fileName());
}

QMap<QString, int> OneSixVersionBuilder::readOverrideOrders(OneSixInstance *instance)
{
	QMap<QString, int> out;
	if (QDir(instance->instanceRoot()).exists("order.json"))
	{
		QFile orderFile(instance->instanceRoot() + "/order.json");
		if (!orderFile.open(QFile::ReadOnly))
		{
			QLOG_ERROR() << "Couldn't open" << orderFile.fileName()
						 << " for reading:" << orderFile.errorString();
			QLOG_WARN() << "Ignoring overriden order";
		}
		else
		{
			QJsonParseError error;
			QJsonDocument doc = QJsonDocument::fromJson(orderFile.readAll(), &error);
			if (error.error != QJsonParseError::NoError || !doc.isObject())
			{
				QLOG_ERROR() << "Couldn't parse" << orderFile.fileName() << ":"
							 << error.errorString();
				QLOG_WARN() << "Ignoring overriden order";
			}
			else
			{
				QJsonObject obj = doc.object();
				for (auto it = obj.begin(); it != obj.end(); ++it)
				{
					if (it.key().startsWith("org.multimc."))
					{
						continue;
					}
					out.insert(it.key(), it.value().toDouble());
				}
			}
		}
	}
	return out;
}

bool OneSixVersionBuilder::writeOverrideOrders(const QMap<QString, int> &order,
											   OneSixInstance *instance)
{
	QJsonObject obj;
	for (auto it = order.cbegin(); it != order.cend(); ++it)
	{
		if (it.key().startsWith("org.multimc."))
		{
			continue;
		}
		obj.insert(it.key(), it.value());
	}
	QFile orderFile(instance->instanceRoot() + "/order.json");
	if (!orderFile.open(QFile::WriteOnly))
	{
		QLOG_ERROR() << "Couldn't open" << orderFile.fileName()
					 << "for writing:" << orderFile.errorString();
		return false;
	}
	orderFile.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
	return true;
}

