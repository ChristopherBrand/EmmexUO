// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2016 Hotride

#pragma once

#include <common/str.h>
#include <xuocore/mappedfile.h>
#include <map>
#include <unordered_map>
#include <vector>
#include "../BaseQueue.h"

class CCliloc : public CBaseQueueItem
{
    bool Load(uint32_t id, astr_t &result);
    void BuildIndex();
    astr_t GetX(int id, bool toCamelCase, const char *default_value);

public:
    astr_t Language;
    bool Loaded = false;
    CMappedFile m_File;
    // View used for lookups: the mapped file, or m_Decoded for BWT-compressed clilocs.
    CDataReader m_Data;
    std::vector<uint8_t> m_Decoded;
    // id -> (offset of the text in m_Data, length), built once at load
    std::unordered_map<uint32_t, std::pair<uint32_t, uint16_t>> m_Index;

    CCliloc(const astr_t &lang);
    virtual ~CCliloc();
    astr_t GetA(int id, bool toCamelCase = false, const char *default_value = nullptr);
    wstr_t GetW(int id, bool toCamelCase = false, const char *default_value = nullptr);
};

class CClilocManager : public CBaseQueue
{
    CCliloc *m_LastCliloc = nullptr;
    CCliloc *m_ENUCliloc = nullptr;
    wstr_t ParseArgumentsToCliloc(int cliloc, bool toCamelCase, wstr_t args);

public:
    CClilocManager() = default;
    virtual ~CClilocManager();
    CCliloc *Cliloc(const astr_t &lang);
    wstr_t ParseArgumentsToClilocString(int cliloc, bool toCamelCase, wstr_t args);
    wstr_t ParseXmfHtmlArgumentsToCliloc(int cliloc, bool toCamelCase, wstr_t args);
};

extern CClilocManager g_ClilocManager;
