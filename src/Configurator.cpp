// Copyright (c) 2009-2023 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "BusClient.h"
#include "Configurator.h"
#include "dirent.h"
#include <fstream>
#include <streambuf>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#include <algorithm>

using namespace std;

static std::string Replace(std::string input, const std::string& substr, const std::string& replacement)
{
	size_t i;
	size_t l;

	i = input.find(substr);
	l = substr.length();
	while (i != string::npos) {
		input.replace(i, l, replacement);
		i = input.find(substr, i + l);
	}

	return input;
}


ConfiguratorCallback::ConfiguratorCallback(Configurator* configurator, const std::string& filePath)
	: m_slot(this, &ConfiguratorCallback::ResponseWrapper),
	  m_config(filePath),
	  m_handler(configurator),
	  m_delegateInvoked(false),
	  m_unconfigure(false),
	  m_configure(false),
      m_defaultCacheBehaviourUsed(false)
{
	assert(m_handler.get() != NULL);
}

ConfiguratorCallback::~ConfiguratorCallback()
{
}

MojErr ConfiguratorCallback::DelegateResponse(MojObject& response, MojErr err)
{
	if (m_delegateInvoked)
		return MojErrAccessDenied;
	m_delegateInvoked = true;
	m_defaultCacheBehaviourUsed = false;
	return m_handler->BusResponseAsync(m_config, response, err, &m_defaultCacheBehaviourUsed);
}

void ConfiguratorCallback::MarkConfigured()
{
	assert(!m_unconfigure);
	m_configure = true;
}

void ConfiguratorCallback::UnmarkConfigured()
{
	assert(!m_configure);
	m_unconfigure = true;
}

MojErr ConfiguratorCallback::ResponseWrapper(MojObject &response, MojErr err)
{
	MojErr result = MojErrNone;
	try {
		m_slot.cancel();
		result = Response(response, err);
	}  catch (const std::exception& e){
		MojErrThrowMsg(MojErrInternal, "%s", e.what());
	} catch (...) {
		MojErrThrowMsg(MojErrInternal, "Uncaught exception in Configurator::BusResponse!");
	}

	if (!m_delegateInvoked)
		result = DelegateResponse(response, result == MojErrNone ? err : result);

	if (!m_defaultCacheBehaviourUsed) {
		if (m_unconfigure) {
			LOG_DEBUG("Unmarking %s as configured", m_config.c_str());
			m_handler->UnmarkConfigured(m_config);
		} else if (m_configure) {
			LOG_DEBUG("Marking %s as configured", m_config.c_str());
			m_handler->MarkConfigured(m_config);
		}
	}
	return result;
}

DefaultConfiguratorCallback::DefaultConfiguratorCallback(Configurator *configurator, const std::string &filePath)
	: ConfiguratorCallback(configurator, filePath)
{
}

DefaultConfiguratorCallback::~DefaultConfiguratorCallback()
{
}

MojErr DefaultConfiguratorCallback::Response(MojObject &response, MojErr err)
{
	// no action - do the default response
	return MojErrNone;
}

Configurator::ConfigCollection Configurator::m_configureOk;
Configurator::ConfigCollection Configurator::m_configureFailed;

void Configurator::ResetConfigStats()
{
	m_configureOk.clear();
	m_configureFailed.clear();
}

const Configurator::ConfigCollection& Configurator::ConfigureOk()
{
	return m_configureOk;
}

const Configurator::ConfigCollection& Configurator::ConfigureFailure()
{
	return m_configureFailed;
}

Configurator::Configurator(const string& id, ConfigType confType, RunType type, BusClient& busClient, const string& configDirectory)
: m_busClient(busClient),
  m_id(id),
	m_confType(confType),
  m_currentType(type),
  m_completed(false),
	m_configDir(configDirectory),
	m_scanned(false),
    m_emptyConfigurator(false)
{
	InitCacheDir();
}

Configurator::~Configurator()
{
	LOG_DEBUG("Destroying configurator %p", this);
}

ConfiguratorCallback* Configurator::CreateCallback(const std::string &filePath)
{
	return new DefaultConfiguratorCallback(this, filePath);
}

void Configurator::InitCacheDir() const
{
	MojMkDir(kCacheDir, kCacheDirPerms);
	MojMkDir(kConfCacheDir, kCacheStampPerm);
}

bool Configurator::IsAlreadyConfigured(const std::string& confFile) const
{
	if (!this->CanCacheConfiguratorStatus(confFile)) {
		LOG_DEBUG("Configurator ignores caching - returning false");
		return false;
	}

	std::string stamp = (kConfCacheDir ? kConfCacheDir : std::string("")) + Replace(confFile, "/", "_");
	MojStatT stampInfo, confInfo;

	if (MojErrNone != MojStat(stamp.c_str(), &stampInfo))
		return false;

	if (MojErrNone != MojStat(confFile.c_str(), &confInfo))
		return false;

	LOG_DEBUG("%s may already be configured - %s exists", confFile.c_str(), stamp.c_str());
	return stampInfo.st_mtime >= confInfo.st_mtime;
}

void Configurator::MarkConfigured(const std::string &confFile) const
{
	if (!CanCacheConfiguratorStatus(confFile))
		return;

	LOG_DEBUG("Attempting to mark '%s' as configured", confFile.c_str());

	MojStatT confFileInfo;
	MojErr err;

	err = MojStat(confFile.c_str(), &confFileInfo);

#ifndef UTIME_NOW
	/*
		If we don't have futimens() then use futimes with msec resolution. Also the
		last-access time will be set to 0 instead of "now".
	*/
	struct timeval *times;
	struct timeval inherited[2];

	if (err == MojErrNone) {
		inherited[0].tv_usec = 0; // don't care about atime
		inherited[0].tv_sec = 0;
		inherited[1].tv_usec = confFileInfo.st_mtim.tv_nsec / 1000;
		inherited[1].tv_sec = confFileInfo.st_mtim.tv_sec + 1;

		times = inherited;

//		MojLogDebug(m_log, "Inheriting timestamp of %d s, %d ms", (int)inherited[1].tv_sec, (int)inherited[1].tv_msec);
	} else {
		LOG_WARNING(MSG_CONFIGURATOR_WARNING, 1,
				PMLOGKS("error", strerror(errno)),
				"Using current time as timestamp - couldn't get timestamp of conf file (%s)", strerror(errno));
		times = NULL;
	}

	string stamp = kConfCacheDir + Replace(confFile, "/", "_");

	int stampFd = open(stamp.c_str(), O_CREAT | O_WRONLY | O_NOATIME, kCacheStampPerm);
	if (stampFd == -1) {
		MojLogError(m_log, "Failed to mark %s as configured: %s", confFile.c_str(), strerror(errno));
		return;
	}

	if (-1 == futimes(stampFd, times)) {
		// fall through to close()
		unlink(stamp.c_str());
		LOG_ERROR(MSGID_CONFIGURATOR_ERROR, 2,
				PMLOGKS("file", confFile.c_str())),
				PMLOGKS("error", strerror(errno)),
				"Failed to create configured stamp for %s (timestamp change failed: %s)", confFile.c_str(), strerror(errno));
	} else {
		LOG_DEBUG("'%s' marked as configured (stamp '%s' created)", confFile.c_str(), stamp.c_str());
	}

#else
	struct timespec *times;
	struct timespec inherited[2];

	if (err == MojErrNone) {
		inherited[0].tv_nsec = UTIME_NOW; // don't care about atime - linux bug prevents me from using UTIME_OMIT
		inherited[0].tv_sec = 0; // linux bug
		inherited[1].tv_nsec = confFileInfo.st_mtim.tv_nsec;
		inherited[1].tv_sec = confFileInfo.st_mtim.tv_sec + 1;

		times = inherited;

//		MojLogDebug(m_log, "Inheriting timestamp of %d s, %d ns", (int)inherited[1].tv_sec, (int)inherited[1].tv_nsec);
	} else {
		LOG_WARNING(MSGID_CONFIGURATOR_WARNING, 1,
				PMLOGKS("error", strerror(errno)),
				"Using current time as timestamp - couldn't get timestamp of conf file (%s)", strerror(errno));
		times = NULL;
	}

	string stamp = (kConfCacheDir ? kConfCacheDir : std::string("")) + Replace(confFile, "/", "_");

	int stampFd = open(stamp.c_str(), O_CREAT | O_WRONLY | O_NOATIME, kCacheStampPerm);
	if (stampFd == -1) {
		LOG_ERROR(MSGID_CONFIGURATOR_ERROR, 2,
				PMLOGKS("file", confFile.c_str()),
				PMLOGKS("error", strerror(errno)),
				"Failed to mark %s as configured: %s", confFile.c_str(), strerror(errno));
		return;
	}

	if (-1 == futimens(stampFd, times)) {
		// fall through to close()
		unlink(stamp.c_str());
		LOG_ERROR(MSGID_CONFIGURATOR_ERROR, 2,
				PMLOGKS("file", confFile.c_str()),
				PMLOGKS("error", strerror(errno)),
				"Failed to create configured stamp for %s (timestamp change failed: %s)", confFile.c_str(), strerror(errno));
	} else {
		LOG_DEBUG("'%s' marked as configured (stamp '%s' created)", confFile.c_str(), stamp.c_str());
	}

	close(stampFd);
#endif
}

void Configurator::UnmarkConfigured(const std::string &confFile) const
{
	if (!CanCacheConfiguratorStatus(confFile))
		return;

	string stamp = (kConfCacheDir ? kConfCacheDir : std::string("")) + Replace(confFile, "/", "_");
	if (unlink(stamp.c_str()) == 0)
    {
		LOG_DEBUG("removed configured stamp for '%s'", confFile.c_str());
    }
	else
    {
		LOG_WARNING(MSGID_CONFIGURATOR_WARNING, 2,
				PMLOGKS("file", confFile.c_str()),
				PMLOGKS("stamp", stamp.c_str()),
				"failed to remove configured stamp for '%s' ('%s')", confFile.c_str(), stamp.c_str());
    }
}

const std::string& Configurator::ParentId(const std::string& filePath) const
{
	ConfigMap::const_iterator i = m_parentDirMap.find(filePath);
	if (i == m_parentDirMap.end() || i->second.empty())
		return m_id;
	return i->second;
}

bool Configurator::CanCacheConfiguratorStatus(const std::string &) const
{
	LOG_TRACE("Entering function %s", __FUNCTION__);
	return true;
}

bool Configurator::Run()
{
	LOG_TRACE("Entering function %s", __FUNCTION__);

	if (!m_scanned) {
        bool folderFound = GetConfigFiles("", m_configDir);
		if (m_configs.empty()) {
            if (folderFound) // Prevents double logging when folder is missing
                LOG_DEBUG("No configurations found in %s", m_configDir.c_str());
			m_emptyConfigurator = true;
		} else {
			m_emptyConfigurator = false;
		}
		m_scanned = true;
	}

	if (m_configs.empty()) {
		if (m_pendingConfigs.empty() && !m_completed) {
			if (!m_emptyConfigurator) {
				LOG_DEBUG("%s :: No more configurations", ConfiguratorName());
			}
			Complete();
		} else {
			LOG_DEBUG("%s :: %zu configurations pending, m_completed = %d", ConfiguratorName(), m_pendingConfigs.size(), m_completed);
		}
		// nothing to do - already sent out all the requests
		// just waiting for responses from services
		return m_configs.empty();
	}
	// Read the config file
	string filePath = m_configs.back();
	m_configs.pop_back();
	m_pendingConfigs.push_back(filePath);
	string config = ReadFile(filePath);

	LOG_DEBUG("%s :: Configuring '%s'", ConfiguratorName(), filePath.c_str());

	// process it
	MojErr err = MojErrNone;
	switch (m_currentType) {
	case Configure:
	case Reconfigure:
		err = ProcessConfig(filePath, config);
		break;
	case RemoveConfiguration:
		err = ProcessConfigRemoval(filePath, config);
		break;
	}

	if (err) {
		if (MojErrInProgress == err) {
			m_configureOk.push_back(config);
			LOG_DEBUG("Skipping config file: %s", filePath.c_str());
		}
		else
		{
			MojString errorMsg;
			MojErrToString(err, errorMsg);
			LOG_ERROR(MSGID_CONFIGURATOR_ERROR, 2,
					PMLOGKS("config", config.c_str()),
					PMLOGKS("error", errorMsg.data()),
					"Failed to process config: %s (error: %s)", config.c_str(), errorMsg.data());
			// Skip this file and keep going!
			m_configureFailed.push_back(filePath);
		}
		m_pendingConfigs.pop_back();
		return Run();
	}
	return m_configs.empty();
}

MojErr Configurator::ProcessConfig(const std::string &filePath, const std::string &json)
{
	MojObject parsed;
	MojErr err = parsed.fromJson(json.c_str());
	MojErrCheck(err);

	return ProcessConfig(filePath, parsed);
}

MojErr Configurator::ProcessConfigRemoval(const std::string &filePath, const std::string &json)
{
	MojObject parsed;
	MojErr err = parsed.fromJson(json.c_str());
	MojErrCheck(err);

	return ProcessConfigRemoval(filePath, parsed);
}

// returns whether folder exists or not
bool Configurator::GetConfigFiles(const string& parent, const string& directory)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);

	DIR* dp = NULL;
	struct dirent* dirp = NULL;
	struct stat stat_buf;

	if((dp  = opendir(directory.c_str())) == NULL) {
		LOG_WARNING(MSGID_CONFIGURATOR_WARNING, 2,
				PMLOGKS("directory", directory.c_str()),
				PMLOGKS("parent", parent.c_str()),
				"Failed to open directory: %s, under %s", directory.c_str(), parent.c_str());
        return false;
	}

    LOG_DEBUG("Reading config files in '%s' under '%s'", directory.c_str(), parent.c_str());

    while ((dirp = readdir(dp)) != NULL) {
		string filename = dirp->d_name;
		if (filename != "." && filename != "..") {
			string filePath = directory;
			filePath.append("/");
			filePath.append(filename);
			if(0 == stat(filePath.c_str(), &stat_buf)) {
				if (S_ISDIR(stat_buf.st_mode)) {
					GetConfigFiles(filename, filePath);
				} else {
					if (! parent.empty())
						m_parentDirMap[filePath] = parent;

					// Check if the config file has already been processed
					if (!(m_currentType == Configure && IsAlreadyConfigured(filePath))) {
						LOG_DEBUG("Found configuration '%s'", filePath.c_str());
						m_configs.push_back(filePath);
					} else {
						LOG_DEBUG("Skipping configuration '%s' because it has already run (cache stamp in %s exists)", filePath.c_str(), kConfCacheDir);
					}
				}
			}
			else {
				LOG_ERROR(MSGID_CONFIGURATOR_ERROR, 0, "Failed to get file information on: %s", filename.c_str());
				break;
			}
		}
	}
	closedir(dp);
    return true;
}

const string Configurator::ReadFile(const string& filePath)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);

	string contents;
	ifstream file;
	file.open(filePath.c_str());

	if (!file.good())
		return contents;

	try {
		file.seekg(0, ios::end);
		contents.reserve(file.tellg());
		file.seekg(0, ios::beg);
	} catch (...) {
	}

	contents.assign((istreambuf_iterator<char>(file)),
                     istreambuf_iterator<char>());

	file.close();
	return contents;
}

void Configurator::Complete()
{
	m_busClient.ConfiguratorComplete(this);
	m_completed = true;
}

MojErr Configurator::BusResponseAsync(const std::string& config, MojObject& response, MojErr err, bool *cacheConfigured)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);

	try {
		// remove the config from the list
		ConfigCollection::iterator i = find(m_pendingConfigs.begin(), m_pendingConfigs.end(), config);
		if (i == m_pendingConfigs.end()) {
			LOG_WARNING(MSGID_CONFIGURATOR_WARNING, 1,
					PMLOGKS("for", config.c_str()),
					"Response for %s but not in pending list", config.c_str());
		} else {
			LOG_DEBUG("Response for %s - removing from pending list", config.c_str());
			m_pendingConfigs.erase(i);
		}

		bool success = true;
		response.get("returnValue", success);

		if (err || !success) {
			m_configureFailed.push_back(config);

			MojString json;
			MojErrCheck(response.toJson(json));
			LOG_ERROR(MSGID_CONFIGURATOR_ERROR, 2,
					PMLOGKS("config", config.c_str()),
					PMLOGKFV("error", "%d", err),
					"%s: %s (MojErr: %i)", config.c_str(), json.data(), err);
		} else {
			m_configureOk.push_back(config);

			*cacheConfigured = true;
			if (m_currentType != RemoveConfiguration)
				MarkConfigured(config);
			else
				UnmarkConfigured(config);
		}

		// do the next config
		Run();
	} catch (const std::exception& e){
		MojErrThrowMsg(MojErrInternal, "%s", e.what());
	} catch (...) {
		MojErrThrowMsg(MojErrInternal, "Uncaught exception in Configurator::BusResponse!");
	}
	return MojErrNone;
}
