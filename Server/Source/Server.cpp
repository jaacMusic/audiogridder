/*
 * Copyright (c) 2020 Andreas Pohl
 * Licensed under MIT (https://github.com/apohl79/audiogridder/blob/master/COPYING)
 *
 * Author: Andreas Pohl
 */

#ifdef JUCE_MAC
#include <sys/socket.h>
#endif

#include "Server.hpp"
#include "Utils.hpp"
#include "json.hpp"

namespace e47 {

using json = nlohmann::json;

Server::Server() : Thread("Server") { loadConfig(); }

void Server::loadConfig() {
    logln("starting server...");
    File cfg(SERVER_CONFIG_FILE);
    if (cfg.exists()) {
        FileInputStream fis(cfg);
        json j = json::parse(fis.readEntireStreamAsString().toStdString());
        if (j.find("ID") != j.end()) {
            m_id = j["ID"].get<int>();
        }
        if (j.find("AU") != j.end()) {
            m_enableAU = j["AU"].get<bool>();
            logln("AudioUnit support " << (m_enableAU ? "enabled" : "disabled"));
        }
        if (j.find("VST") != j.end()) {
            m_enableVST = j["VST"].get<bool>();
            logln("VST3 support " << (m_enableVST ? "enabled" : "disabled"));
        }
        if (j.find("VST2") != j.end()) {
            m_enableVST2 = j["VST2"].get<bool>();
            logln("VST2 support " << (m_enableVST2 ? "enabled" : "disabled"));
        }
        if (j.find("ScreenQuality") != j.end()) {
            m_screenJpgQuality = j["ScreenQuality"].get<float>();
        }
        if (j.find("ScreenDiffDetection") != j.end()) {
            m_screenDiffDetection = j["ScreenDiffDetection"].get<bool>();
            logln("screen capture difference detection " << (m_screenDiffDetection ? "enabled" : "disabled"));
        }
        if (j.find("ExcludePlugins") != j.end()) {
            for (auto& s : j["ExcludePlugins"]) {
                m_pluginexclude.insert(s.get<std::string>());
            }
        }
    }
    File deadmanfile(DEAD_MANS_FILE);
    if (deadmanfile.exists()) {
        StringArray lines;
        deadmanfile.readLines(lines);
        for (auto& line : lines) {
            m_pluginlist.addToBlacklist(line);
        }
        deadmanfile.deleteFile();
        saveConfig();
    }
}

void Server::saveConfig() {
    json j;
    j["ID"] = m_id;
    j["AU"] = m_enableAU;
    j["VST"] = m_enableVST;
    j["VST2"] = m_enableVST2;
    j["ScreenQuality"] = m_screenJpgQuality;
    j["ScreenDiffDetection"] = m_screenDiffDetection;
    j["ExcludePlugins"] = json::array();
    for (auto& p : m_pluginexclude) {
        j["ExcludePlugins"].push_back(p.toStdString());
    }

    File cfg(SERVER_CONFIG_FILE);
    cfg.deleteFile();
    FileOutputStream fos(cfg);
    fos.writeText(j.dump(4), false, false, "\n");
}

void Server::loadKnownPluginList() { loadKnownPluginList(m_pluginlist); }

void Server::loadKnownPluginList(KnownPluginList& plist) {
    File file(KNOWN_PLUGINS_FILE);
    if (file.exists()) {
        auto xml = XmlDocument::parse(file);
        plist.recreateFromXml(*xml);
    }
}

void Server::saveKnownPluginList() { saveKnownPluginList(m_pluginlist); }

void Server::saveKnownPluginList(KnownPluginList& plist) {
    File file(KNOWN_PLUGINS_FILE);
    auto xml = plist.createXml();
    xml->writeTo(file);
}

Server::~Server() {
    if (m_masterSocket.isConnected()) {
        m_masterSocket.close();
    }
    stopThread(-1);
    m_pluginlist.clear();
    logln("server terminated");
}

void Server::shutdown() {
    m_masterSocket.close();
    for (auto& w : m_workers) {
        logln("shutting down worker, isRunning=" << (int)w->isThreadRunning());
        w->shutdown();
        w->waitForThreadToExit(-1);
    }
    signalThreadShouldExit();
}

bool Server::shouldExclude(const String& name) {
    std::vector<String> emptylist;
    return shouldExclude(name, emptylist);
}

bool Server::shouldExclude(const String& name, const std::vector<String>& include) {
    if (name.containsIgnoreCase("AGridder") || name.containsIgnoreCase("AudioGridder")) {
        return true;
    }
    if (include.size() > 0) {
        for (auto& incl : include) {
            if (!name.compare(incl)) {
                return false;
            }
        }
        return true;
    } else {
        for (auto& excl : m_pluginexclude) {
            if (!name.compare(excl)) {
                return true;
            }
        }
    }
    return false;
}

void Server::addPlugins(const std::vector<String>& names, std::function<void(bool)> fn) {
    std::thread([this, names, fn] {
        scanForPlugins(names);
        saveConfig();
        saveKnownPluginList();
        if (fn) {
            for (auto& name : names) {
                bool found = false;
                for (auto& p : m_pluginlist.getTypes()) {
                    if (!name.compare(p.descriptiveName)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    fn(false);
                    return;
                }
            }
            fn(true);
        }
    }).detach();
}

bool Server::scanPlugin(const String& id, const String& format) {
    std::unique_ptr<AudioPluginFormat> fmt;
    if (!format.compare("VST")) {
        fmt = std::make_unique<VSTPluginFormat>();
    } else if (!format.compare("VST3")) {
        fmt = std::make_unique<VST3PluginFormat>();
#ifdef JUCE_MAC
    } else if (!format.compare("AudioUnit")) {
        fmt = std::make_unique<AudioUnitPluginFormat>();
#endif
    } else {
        return false;
    }
    KnownPluginList plist;
    loadKnownPluginList(plist);
    logln_static("scanning id=" << id << " fmt=" << format);
    bool success = true;
    PluginDirectoryScanner scanner(plist, *fmt, {}, true, File(DEAD_MANS_FILE));
    scanner.setFilesOrIdentifiersToScan({id});
    String name;
    scanner.scanNextFile(true, name);
    for (auto& f : scanner.getFailedFiles()) {
        plist.addToBlacklist(f);
        success = false;
    }
    saveKnownPluginList(plist);
    return success;
}

void Server::scanNextPlugin(const String& id, const String& fmt) {
    String fileFmt = id;
    fileFmt << "|" << fmt;
    ChildProcess proc;
    StringArray args;
    args.add(File::getSpecialLocation(File::currentExecutableFile).getFullPathName());
    args.add("-scan");
    args.add(fileFmt);
    if (proc.start(args)) {
        proc.waitForProcessToFinish(30000);
        if (proc.isRunning()) {
            logln("error: scan timeout, killing scan process");
            proc.kill();
        } else {
            auto ec = proc.getExitCode();
            if (ec != 0) {
                logln("error: scan failed with exit code " << as<int>(ec));
            }
        }
    } else {
        logln("error: failed to start scan process");
    }
}

void Server::scanForPlugins() {
    std::vector<String> emptylist;
    scanForPlugins(emptylist);
}

void Server::scanForPlugins(const std::vector<String>& include) {
    logln("scanning for plugins...");
    std::vector<std::unique_ptr<AudioPluginFormat>> fmts;
#ifdef JUCE_MAC
    if (m_enableAU) {
        fmts.push_back(std::make_unique<AudioUnitPluginFormat>());
    }
#endif
    if (m_enableVST) {
        fmts.push_back(std::make_unique<VST3PluginFormat>());
    }
    if (m_enableVST2) {
        fmts.push_back(std::make_unique<VSTPluginFormat>());
    }

    std::set<String> neverSeenList = m_pluginexclude;

    loadKnownPluginList();

    for (auto& fmt : fmts) {
        auto fileOrIds = fmt->searchPathsForPlugins(fmt->getDefaultLocationsToSearch(), true);
        for (auto& fileOrId : fileOrIds) {
            auto name = fmt->getNameOfPluginFromIdentifier(fileOrId);
            auto plugindesc = m_pluginlist.getTypeForFile(fileOrId);
            if ((nullptr == plugindesc || fmt->pluginNeedsRescanning(*plugindesc)) &&
                !m_pluginlist.getBlacklistedFiles().contains(fileOrId) && !shouldExclude(name, include)) {
                logln("  scanning: " << name);
                getApp().setSplashInfo(String("Scanning plugin ") + name + "...");
                scanNextPlugin(fileOrId, fmt->getName());
            } else {
                logln("  (skipping: " << name << ")");
            }
            neverSeenList.erase(name);
        }
    }

    loadKnownPluginList();
    m_pluginlist.sort(KnownPluginList::sortAlphabetically, true);

    for (auto& name : neverSeenList) {
        m_pluginexclude.erase(name);
    }
    logln("scan for plugins finished.");
}

void Server::run() {
    scanForPlugins();
    saveConfig();
    saveKnownPluginList();

    getApp().hideSplashWindow();

#ifdef JUCE_MAC
    setsockopt(m_masterSocket.getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, nullptr, 0);
#endif

    logln("creating listener " << (m_host.length() == 0? "*": m_host) << ":" << (m_port + m_id));
    if (m_masterSocket.createListener(m_port + m_id, m_host)) {
        logln("server started: ID=" << m_id << ", PORT=" << m_port + m_id);
        while (!currentThreadShouldExit()) {
            auto* clnt = m_masterSocket.waitForNextConnection();
            if (nullptr != clnt) {
                logln("new client " << clnt->getHostName());
                m_workers.emplace_back(std::make_unique<Worker>(clnt));
                m_workers.back()->startThread();
                // lazy cleanup
                std::shared_ptr<WorkerList> deadWorkers = std::make_shared<WorkerList>();
                for (auto it = m_workers.begin(); it < m_workers.end();) {
                    if (!(*it)->isThreadRunning()) {
                        deadWorkers->push_back(std::move(*it));
                        m_workers.erase(it);
                    } else {
                        it++;
                    }
                }
                MessageManager::callAsync([deadWorkers] { deadWorkers->clear(); });
            }
        }
    } else {
        logln("failed to create listener");
    }
}

}  // namespace e47
