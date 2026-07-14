#pragma once

#include <pch.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <string>
#include <locale>
#include <codecvt>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace winrt::StarlightGUI::implementation {
    void InitializeConfig();

    template<typename T>
    void SaveConfig(std::string key, T s_value) {
        try
        {
            auto userFolder = fs::path(GetInstalledLocationPath());
            auto configFilePath = userFolder / "StarlightGUI.json";
            json configData = json::object();

            if (fs::exists(configFilePath))
            {
                try {
                    std::ifstream configFile(configFilePath);
                    configFile >> configData;
                    if (!configData.is_object()) configData = json::object();
                }
                catch (...) {
                    configData = json::object();
                }
            }

            configData[key] = s_value;

            std::ofstream configFile(configFilePath);
            configFile << configData.dump(4);
        }
        catch (...)
        {
        }
        InitializeConfig();
    }

    template<typename T>
    T ReadConfig(std::string key, T defaultValue) {
        try
        {
            auto userFolder = fs::path(GetInstalledLocationPath());
            auto configFilePath = userFolder / "StarlightGUI.json";
            json configData = json::object();
            bool needWriteBack = false;

            if (fs::exists(configFilePath))
            {
                try {
                    std::ifstream configFile(configFilePath);
                    configFile >> configData;
                    if (!configData.is_object()) {
                        configData = json::object();
                        needWriteBack = true;
                    }
                }
                catch (...) {
                    configData = json::object();
                    needWriteBack = true;
                }
            }
            else
            {
                needWriteBack = true;
            }

            if (configData.contains(key))
            {
                try {
                    return configData[key].get<T>();
                }
                catch (...) {
                    configData[key] = defaultValue;
                    needWriteBack = true;
                }
            }
            else
            {
                configData[key] = defaultValue;
                needWriteBack = true;
            }

            if (needWriteBack)
            {
                try {
                    std::ofstream configFile(configFilePath);
                    configFile << configData.dump(4);
                }
                catch (...) {
                }
            }

            return defaultValue;
        }
        catch (...)
        {
            return defaultValue;
        }
    }
}
