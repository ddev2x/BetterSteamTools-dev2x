#pragma once

#include "Steam/Types.h"
#include <vector>
#include <string>

namespace NiuWanCollection {
    // 注册来自于 .np 文件的 AppId
    void RegisterNpAppId(AppId_t appId);

    // 注销来自于 .np 文件的 AppId
    void UnregisterNpAppId(AppId_t appId);

    // 判断某 AppId 是否来源于 .np 文件
    bool IsNpAppId(AppId_t appId);

    // 获取当前记录的所有来源于 .np 文件的 AppId 列表
    std::vector<AppId_t> GetNpAppIds();

    // 执行集合同步到当前用户的 cloudstorage 中（增量或全量维护“牛玩”分类）
    // steamInstallPath 传入 Steam 的根安装目录
    void SyncCollection(const char* steamInstallPath);
}
