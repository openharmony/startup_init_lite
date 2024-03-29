/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
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

#include "param_service.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "init_param.h"
#include "init_utils.h"
#include "param_message.h"
#include "param_manager.h"
#include "param_request.h"
#include "trigger_manager.h"

static ParamWorkSpace g_paramWorkSpace = { 0, {}, NULL, {}, NULL, NULL };

static void OnClose(ParamTaskPtr client)
{
    PARAM_LOGD("OnClose %p", client);
    ParamWatcher *watcher = (ParamWatcher *)ParamGetTaskUserData(client);
    ClearWatcherTrigger(watcher);
    ListRemove(&watcher->node);
}

static int AddParam(WorkSpace *workSpace, const char *name, const char *value, uint32_t *dataIndex)
{
    ParamTrieNode *node = AddTrieNode(workSpace, name, strlen(name));
    PARAM_CHECK(node != NULL, return PARAM_CODE_REACHED_MAX, "Failed to add node");
    ParamNode *entry = (ParamNode *)GetTrieNode(workSpace, node->dataIndex);
    if (entry == NULL) {
        uint32_t offset = AddParamNode(workSpace, name, strlen(name), value, strlen(value));
        PARAM_CHECK(offset > 0, return PARAM_CODE_REACHED_MAX, "Failed to allocate name %s", name);
        SaveIndex(&node->dataIndex, offset);
    }
    *dataIndex = node->dataIndex;
    return 0;
}

static int UpdateParam(const WorkSpace *workSpace, uint32_t *dataIndex, const char *name, const char *value)
{
    ParamNode *entry = (ParamNode *)GetTrieNode(workSpace, *dataIndex);
    PARAM_CHECK(entry != NULL, return PARAM_CODE_REACHED_MAX, "Failed to update param value %s %u", name, *dataIndex);
    PARAM_CHECK(entry->keyLength == strlen(name), return PARAM_CODE_INVALID_NAME, "Failed to check name len %s", name);

    uint32_t valueLen = strlen(value);
    uint32_t commitId = atomic_load_explicit(&entry->commitId, memory_order_relaxed);
    atomic_store_explicit(&entry->commitId, commitId | PARAM_FLAGS_MODIFY, memory_order_relaxed);

    if (entry->valueLength < PARAM_VALUE_LEN_MAX && valueLen < PARAM_VALUE_LEN_MAX) {
        int ret = memcpy_s(entry->data + entry->keyLength + 1, PARAM_VALUE_LEN_MAX, value, valueLen + 1);
        PARAM_CHECK(ret == EOK, return PARAM_CODE_INVALID_VALUE, "Failed to copy value");
        entry->valueLength = valueLen;
    }
    uint32_t flags = commitId & ~PARAM_FLAGS_COMMITID;
    atomic_store_explicit(&entry->commitId, (++commitId) | flags, memory_order_release);
    futex_wake(&entry->commitId, INT_MAX);
    return 0;
}

static int CheckParamValue(const WorkSpace *workSpace, const ParamTrieNode *node, const char *name, const char *value)
{
    if (IS_READY_ONLY(name)) {
        PARAM_CHECK(strlen(value) < PARAM_CONST_VALUE_LEN_MAX,
            return PARAM_CODE_INVALID_VALUE, "Illegal param value %s", value);
        if (node != NULL && node->dataIndex != 0) {
            PARAM_LOGE("Read-only param was already set %s", name);
            return PARAM_CODE_READ_ONLY;
        }
    } else {
        // 限制非read only的参数，防止参数值修改后，原空间不能保存
        PARAM_CHECK(strlen(value) < PARAM_VALUE_LEN_MAX,
            return PARAM_CODE_INVALID_VALUE, "Illegal param value %s", value);
    }
    return 0;
}

int WriteParam(const WorkSpace *workSpace, const char *name, const char *value, uint32_t *dataIndex, int onlyAdd)
{
    PARAM_CHECK(workSpace != NULL && dataIndex != NULL, return PARAM_CODE_INVALID_PARAM, "Invalid workSpace");
    PARAM_CHECK(value != NULL && name != NULL, return PARAM_CODE_INVALID_PARAM, "Invalid name or value");
    ParamTrieNode *node = FindTrieNode(workSpace, name, strlen(name), NULL);
    int ret = CheckParamValue(workSpace, node, name, value);
    PARAM_CHECK(ret == 0, return ret, "Invalid param value param: %s=%s", name, value);
    if (node != NULL && node->dataIndex != 0) {
        *dataIndex = node->dataIndex;
        if (onlyAdd) {
            return 0;
        }
        return UpdateParam(workSpace, &node->dataIndex, name, value);
    }
    return AddParam((WorkSpace *)workSpace, name, value, dataIndex);
}

PARAM_STATIC int AddSecurityLabel(const ParamAuditData *auditData, void *context)
{
    PARAM_CHECK(auditData != NULL && auditData->name != NULL, return -1, "Invalid auditData");
    PARAM_CHECK(context != NULL, return -1, "Invalid context");
    ParamWorkSpace *workSpace = (ParamWorkSpace *)context;
    int ret = CheckParamName(auditData->name, 1);
    PARAM_CHECK(ret == 0, return ret, "Illegal param name %s", auditData->name);

    ParamTrieNode *node = FindTrieNode(&workSpace->paramSpace, auditData->name, strlen(auditData->name), NULL);
    if (node == NULL) {
        node = AddTrieNode(&workSpace->paramSpace, auditData->name, strlen(auditData->name));
    }
    PARAM_CHECK(node != NULL, return PARAM_CODE_REACHED_MAX, "Failed to add node %s", auditData->name);
    if (node->labelIndex == 0) { // can not support update for label
        uint32_t offset = AddParamSecruityNode(&workSpace->paramSpace, auditData);
        PARAM_CHECK(offset != 0, return PARAM_CODE_REACHED_MAX, "Failed to add label");
        SaveIndex(&node->labelIndex, offset);
    } else {
#ifdef STARTUP_INIT_TEST
        ParamSecruityNode *label = (ParamSecruityNode *)GetTrieNode(&workSpace->paramSpace, node->labelIndex);
        label->mode = auditData->dacData.mode;
        label->uid = auditData->dacData.uid;
        label->gid = auditData->dacData.gid;
#endif
        PARAM_LOGE("Error, repeate to add label for name %s", auditData->name);
    }
    PARAM_LOGD("AddSecurityLabel label gid %d uid %d mode %o name: %s", auditData->dacData.gid, auditData->dacData.uid,
               auditData->dacData.mode, auditData->name);
    return 0;
}

static char *GetServiceCtrlName(const char *name, const char *value)
{
    static char *ctrlParam[] = {
        "ohos.ctl.start",
        "ohos.ctl.stop"
    };
    static char *powerCtrlArg[][2] = {
        {"reboot,shutdown", "reboot.shutdown"},
        {"reboot,updater", "reboot.updater"},
        {"reboot,flashd", "reboot.flashd"},
        {"reboot", "reboot"},
    };
    char *key = NULL;
    if (strcmp("ohos.startup.powerctrl", name) == 0) {
        for (size_t i = 0; i < ARRAY_LENGTH(powerCtrlArg); i++) {
            if (strncmp(value, powerCtrlArg[i][0], strlen(powerCtrlArg[i][0])) == 0) {
                uint32_t keySize = strlen(powerCtrlArg[i][1]) + strlen(OHOS_SERVICE_CTRL_PREFIX) + 1;
                key = (char *)malloc(keySize + 1);
                PARAM_CHECK(key != NULL, return NULL, "Failed to alloc memory for %s", name);
                int ret = sprintf_s(key, keySize, "%s%s", OHOS_SERVICE_CTRL_PREFIX, powerCtrlArg[i][1]);
                PARAM_CHECK(ret > EOK, free(key);
                    return NULL, "Failed to format key for %s", name);
                return key;
            }
        }
    } else {
        for (size_t i = 0; i < ARRAY_LENGTH(ctrlParam); i++) {
            if (strcmp(name, ctrlParam[i]) == 0) {
                uint32_t keySize = strlen(value) + strlen(OHOS_SERVICE_CTRL_PREFIX) + 1;
                key = (char *)malloc(keySize + 1);
                PARAM_CHECK(key != NULL, return NULL, "Failed to alloc memory for %s", name);
                int ret = sprintf_s(key, keySize, "%s%s", OHOS_SERVICE_CTRL_PREFIX, value);
                PARAM_CHECK(ret > EOK, free(key);
                    return NULL, "Failed to format key for %s", name);
                return key;
            }
        }
    }
    return key;
}

static void CheckAndSendTrigger(ParamWorkSpace *workSpace, uint32_t dataIndex, const char *name, const char *value)
{
    ParamNode *entry = (ParamNode *)GetTrieNode(&workSpace->paramSpace, dataIndex);
    PARAM_CHECK(entry != NULL, return, "Failed to get data %s ", name);
    uint32_t trigger = 1;
    if ((atomic_load_explicit(&entry->commitId, memory_order_relaxed) & PARAM_FLAGS_TRIGGED) != PARAM_FLAGS_TRIGGED) {
        trigger = (CheckAndMarkTrigger(TRIGGER_PARAM, name) != 0) ? 1 : 0;
    }
    if (trigger) {
        atomic_store_explicit(&entry->commitId,
            atomic_load_explicit(&entry->commitId, memory_order_relaxed) | PARAM_FLAGS_TRIGGED, memory_order_release);
        // notify event to process trigger
        PostParamTrigger(EVENT_TRIGGER_PARAM, name, value);
    }

    int wait = 1;
    if ((atomic_load_explicit(&entry->commitId, memory_order_relaxed) & PARAM_FLAGS_WAITED) != PARAM_FLAGS_WAITED) {
        wait = (CheckAndMarkTrigger(TRIGGER_PARAM_WAIT, name) != 0) ? 1 : 0;
    }
    if (wait) {
        atomic_store_explicit(&entry->commitId,
            atomic_load_explicit(&entry->commitId, memory_order_relaxed) | PARAM_FLAGS_WAITED, memory_order_release);
        PostParamTrigger(EVENT_TRIGGER_PARAM_WAIT, name, value);
    }
    PostParamTrigger(EVENT_TRIGGER_PARAM_WATCH, name, value);
}

static int SystemSetParam(const char *name, const char *value, const ParamSecurityLabel *srcLabel)
{
    PARAM_LOGD("SystemSetParam name %s value: %s", name, value);
    int ret = CheckParamName(name, 0);
    PARAM_CHECK(ret == 0, return ret, "Illegal param name %s", name);

    int serviceCtrl = 0;
    char *key = GetServiceCtrlName(name, value);
    if (srcLabel != NULL) {
        ret = CheckParamPermission(&g_paramWorkSpace, srcLabel, (key == NULL) ? name : key, DAC_WRITE);
    }
    if (key != NULL) {
        serviceCtrl = 1;
        free(key);
    }
    PARAM_CHECK(ret == 0, return ret, "Forbit to set parameter %s", name);

    if (serviceCtrl) {
        ret = CheckParamValue(&g_paramWorkSpace.paramSpace, NULL, name, value);
        PARAM_CHECK(ret == 0, return ret, "Invalid param value param: %s=%s", name, value);
        PostParamTrigger(EVENT_TRIGGER_PARAM, name, value);
    } else {
        uint32_t dataIndex = 0;
        ret = WriteParam(&g_paramWorkSpace.paramSpace, name, value, &dataIndex, 0);
        PARAM_CHECK(ret == 0, return ret, "Failed to set param %d name %s %s", ret, name, value);
        ret = WritePersistParam(&g_paramWorkSpace, name, value);
        PARAM_CHECK(ret == 0, return ret, "Failed to set persist param name %s", name);
        CheckAndSendTrigger(&g_paramWorkSpace, dataIndex, name, value);
    }

    // watcher stoped
    if (strcmp(name, "init.svc.param_watcher") == 0 && strcmp(value, "stopped") == 0) {
        ParamWatcher *watcher = GetParamWatcher(NULL);
        PARAM_LOGD("ClearWatcherTrigger");
        ClearWatcherTrigger(watcher);
    }
    return ret;
}

static int SendResponseMsg(ParamTaskPtr worker, const ParamMessage *msg, int result)
{
    ParamResponseMessage *response = NULL;
    response = (ParamResponseMessage *)CreateParamMessage(msg->type, msg->key, sizeof(ParamResponseMessage));
    PARAM_CHECK(response != NULL, return PARAM_CODE_ERROR, "Failed to alloc memory for response");
    response->msg.id.msgId = msg->id.msgId;
    response->result = result;
    response->msg.msgSize = sizeof(ParamResponseMessage);
    ParamTaskSendMsg(worker, (ParamMessage *)response);
    return 0;
}

static int SendWatcherNotifyMessage(const TriggerExtData *extData, int cmd, const char *content)
{
    UNUSED(cmd);
    PARAM_CHECK(content != NULL, return -1, "Invalid content");
    PARAM_CHECK(extData != NULL && extData->watcher != NULL, return -1, "Invalid extData");
    uint32_t msgSize = sizeof(ParamMessage) + PARAM_ALIGN(strlen(content) + 1);
    ParamMessage *msg = (ParamMessage *)CreateParamMessage(MSG_NOTIFY_PARAM, "*", msgSize);
    PARAM_CHECK(msg != NULL, return -1, "Failed to create msg ");

    uint32_t offset = 0;
    int ret;
    char *tmp = strstr(content, "=");
    if (tmp != NULL) {
        ret = strncpy_s(msg->key, sizeof(msg->key) - 1, content, tmp - content);
        PARAM_CHECK(ret == 0, free(msg);
            return -1, "Failed to fill value");
        tmp++;
        ret = FillParamMsgContent(msg, &offset, PARAM_VALUE, tmp, strlen(tmp));
        PARAM_CHECK(ret == 0, free(msg);
            return -1, "Failed to fill value");
    } else {
        ret = FillParamMsgContent(msg, &offset, PARAM_VALUE, tmp, strlen(content));
        PARAM_CHECK(ret == 0, free(msg);
            return -1, "Failed to fill value");
    }

    msg->id.msgId = extData->watcherId;
    msg->msgSize = sizeof(ParamMessage) + offset;
    PARAM_LOGD("SendWatcherNotifyMessage cmd %s, watcherId %d msgSize %d para: %s",
        (cmd == CMD_INDEX_FOR_PARA_WAIT) ? "wait" : "watcher",
        extData->watcherId, msg->msgSize, content);
    ParamTaskSendMsg(extData->watcher->stream, msg);
    return 0;
}

static int HandleParamSet(const ParamTaskPtr worker, const ParamMessage *msg)
{
    uint32_t offset = 0;
    ParamMsgContent *valueContent = GetNextContent(msg, &offset);
    PARAM_CHECK(valueContent != NULL, return -1, "Invalid msg for %s", msg->key);
    int ret;
    ParamMsgContent *lableContent =  GetNextContent(msg, &offset);
    ParamSecurityLabel *srcLabel = NULL;
    if (lableContent != NULL && lableContent->contentSize != 0) {
        PARAM_CHECK(g_paramWorkSpace.paramSecurityOps.securityDecodeLabel != NULL,
            return -1, "Can not get decode function");
        ret = g_paramWorkSpace.paramSecurityOps.securityDecodeLabel(&srcLabel,
            lableContent->content, lableContent->contentSize);
        PARAM_CHECK(ret == 0, return ret,
            "Failed to decode param %d name %s %s", ret, msg->key, valueContent->content);
    }

    ret = SystemSetParam(msg->key, valueContent->content, srcLabel);
    if (srcLabel != NULL && g_paramWorkSpace.paramSecurityOps.securityFreeLabel != NULL) {
        g_paramWorkSpace.paramSecurityOps.securityFreeLabel(srcLabel);
    }
    return SendResponseMsg(worker, msg, ret);
}

static ParamNode *CheckMatchParamWait(const ParamWorkSpace *worksapce, const char *name, const char *value)
{
    uint32_t nameLength = strlen(name);
    ParamTrieNode *node = FindTrieNode(&worksapce->paramSpace, name, nameLength, NULL);
    if (node == NULL || node->dataIndex == 0) {
        return NULL;
    }
    ParamNode *param = (ParamNode *)GetTrieNode(&worksapce->paramSpace, node->dataIndex);
    if (param == NULL) {
        return NULL;
    }
    if ((param->keyLength != nameLength) || (strncmp(param->data, name, nameLength) != 0)) { // compare name
        return NULL;
    }
    atomic_store_explicit(&param->commitId,
        atomic_load_explicit(&param->commitId, memory_order_relaxed) | PARAM_FLAGS_WAITED, memory_order_release);
    if ((strncmp(value, "*", 1) == 0) || (strcmp(param->data + nameLength + 1, value) == 0)) { // compare value
        return param;
    }
    char *tmp = strstr(value, "*");
    if (tmp != NULL && (strncmp(param->data + nameLength + 1, value, tmp - value) == 0)) {
        return param;
    }
    return NULL;
}

static int HandleParamWaitAdd(const ParamWorkSpace *worksapce, const ParamTaskPtr worker, const ParamMessage *msg)
{
    PARAM_CHECK(msg != NULL, return -1, "Invalid message");
    uint32_t offset = 0;
    uint32_t timeout = DEFAULT_PARAM_WAIT_TIMEOUT;
    ParamMsgContent *valueContent = GetNextContent(msg, &offset);
    PARAM_CHECK(valueContent != NULL, return -1, "Invalid msg");
    PARAM_CHECK(valueContent->contentSize <= PARAM_CONST_VALUE_LEN_MAX, return -1, "Invalid msg");
    ParamMsgContent *timeoutContent = GetNextContent(msg, &offset);
    if (timeoutContent != NULL) {
        timeout = *((uint32_t *)(timeoutContent->content));
    }

    PARAM_LOGD("HandleParamWaitAdd name %s timeout %d", msg->key, timeout);
    ParamWatcher *watcher = GetParamWatcher(worker);
    PARAM_CHECK(watcher != NULL, return -1, "Failed to get param watcher data");
    watcher->timeout = timeout;

    TriggerExtData extData = {};
    extData.excuteCmd = SendWatcherNotifyMessage;
    extData.watcherId = msg->id.watcherId;
    extData.watcher = watcher;
    // first check match, if match send response to client
    ParamNode *param = CheckMatchParamWait(worksapce, msg->key, valueContent->content);
    if (param != NULL) {
        SendWatcherNotifyMessage(&extData, CMD_INDEX_FOR_PARA_WAIT, param->data);
        return 0;
    }

    uint32_t buffSize = strlen(msg->key) + valueContent->contentSize + 1 + 1;
    char *condition = calloc(1, buffSize);
    PARAM_CHECK(condition != NULL, return -1, "Failed to create condition for %s", msg->key);
    int ret = sprintf_s(condition, buffSize - 1, "%s=%s", msg->key, valueContent->content);
    PARAM_CHECK(ret > EOK, free(condition);
        return -1, "Failed to copy name for %s", msg->key);
    TriggerNode *trigger = AddWatcherTrigger(watcher, TRIGGER_PARAM_WAIT, msg->key, condition, &extData);
    PARAM_CHECK(trigger != NULL, free(condition);
        return -1, "Failed to add trigger for %s", msg->key);
    free(condition);
    return 0;
}

static int HandleParamWatcherAdd(ParamWorkSpace *workSpace, const ParamTaskPtr worker, const ParamMessage *msg)
{
    PARAM_CHECK(msg != NULL, return -1, "Invalid message");
    TriggerExtData extData = {};
    extData.excuteCmd = SendWatcherNotifyMessage;
    extData.watcherId = msg->id.watcherId;
    int ret = 0;
    do {
        ParamWatcher *watcher = GetParamWatcher(NULL);
        PARAM_CHECK(watcher != NULL, ret = -1;
            break, "Failed to get param watcher data");
        watcher->stream = worker;
        TriggerNode *trigger = AddWatcherTrigger(watcher, TRIGGER_PARAM_WATCH, msg->key, NULL, &extData);
        PARAM_CHECK(trigger != NULL, ret = -1;
            break, "Failed to add trigger for %s", msg->key);
    } while (0);
    PARAM_LOGD("HandleParamWatcherAdd name %s watcher: %d", msg->key, msg->id.watcherId);
    return SendResponseMsg(worker, msg, ret);
}

static int HandleParamWatcherDel(ParamWorkSpace *workSpace, const ParamTaskPtr worker, const ParamMessage *msg)
{
    PARAM_CHECK(msg != NULL, return -1, "Invalid message");
    ParamWatcher *watcher = GetParamWatcher(NULL);
    PARAM_CHECK(watcher != NULL, return -1, "Failed to get param watcher data");
    PARAM_LOGD("HandleParamWatcherDel name %s watcher: %d", msg->key, msg->id.watcherId);
    DelWatcherTrigger(watcher, msg->id.watcherId);
    return SendResponseMsg(worker, msg, 0);
}

PARAM_STATIC int ProcessMessage(const ParamTaskPtr worker, const ParamMessage *msg)
{
    PARAM_CHECK(msg != NULL, return -1, "Invalid msg");
    PARAM_CHECK(worker != NULL, return -1, "Invalid worker");
    int ret = PARAM_CODE_INVALID_PARAM;
    switch (msg->type) {
        case MSG_SET_PARAM:
            ret = HandleParamSet(worker, msg);
            break;
        case MSG_WAIT_PARAM:
            ret = HandleParamWaitAdd(&g_paramWorkSpace, worker, msg);
            break;
        case MSG_ADD_WATCHER:
            ret = HandleParamWatcherAdd(&g_paramWorkSpace, worker, msg);
            break;
        case MSG_DEL_WATCHER:
            ret = HandleParamWatcherDel(&g_paramWorkSpace, worker, msg);
            break;
        default:
            break;
    }
    PARAM_CHECK(ret == 0, return -1, "Failed to process message ret %d", ret);
    return 0;
}

static int LoadDefaultParam_(const char *fileName, uint32_t mode, const char *exclude[], uint32_t count)
{
    uint32_t paramNum = 0;
    FILE *fp = fopen(fileName, "r");
    PARAM_CHECK(fp != NULL, return -1, "Open file %s fail", fileName);
    char *buff = calloc(1, sizeof(SubStringInfo) * (SUBSTR_INFO_VALUE + 1) + PARAM_BUFFER_SIZE);
    PARAM_CHECK(buff != NULL, (void)fclose(fp);
        return -1, "Failed to alloc memory for load %s", fileName);

    SubStringInfo *info = (SubStringInfo *)(buff + PARAM_BUFFER_SIZE);
    while (fgets(buff, PARAM_BUFFER_SIZE, fp) != NULL) {
        buff[PARAM_BUFFER_SIZE - 1] = '\0';
        int subStrNumber = GetSubStringInfo(buff, strlen(buff), '=', info, SUBSTR_INFO_VALUE + 1);
        if (subStrNumber <= SUBSTR_INFO_VALUE) {
            continue;
        }
        // 过滤
        for (uint32_t i = 0; i < count; i++) {
            if (strncmp(info[0].value, exclude[i], strlen(exclude[i])) == 0) {
                PARAM_LOGI("Do not set %s parameters", info[0].value);
                continue;
            }
        }
        int ret = CheckParamName(info[0].value, 0);
        PARAM_CHECK(ret == 0, continue, "Illegal param name %s", info[0].value);
        PARAM_LOGI("Add default parameter %s  %s", info[0].value, info[1].value);
        uint32_t dataIndex = 0;
        ret = WriteParam(&g_paramWorkSpace.paramSpace,
            info[0].value, info[1].value, &dataIndex, mode & LOAD_PARAM_ONLY_ADD);
        PARAM_CHECK(ret == 0, continue, "Failed to set param %d %s", ret, buff);
        paramNum++;
    }
    (void)fclose(fp);
    free(buff);
    PARAM_LOGI("Load parameters success %s total %u", fileName, paramNum);
    return 0;
}

static int OnIncomingConnect(const ParamTaskPtr server, uint32_t flags)
{
    PARAM_LOGD("OnIncomingConnect %p", server);
    ParamStreamInfo info = {};
    info.flags = WORKER_TYPE_CLIENT | flags;
    info.server = NULL;
    info.close = OnClose;
    info.recvMessage = ProcessMessage;
    info.incomingConnect = NULL;
    ParamTaskPtr client = NULL;
    int ret = ParamStreamCreate(&client, server, &info, sizeof(ParamWatcher));
    PARAM_CHECK(ret == 0, return -1, "Failed to create client");

    ParamWatcher *watcher = (ParamWatcher *)ParamGetTaskUserData(client);
    PARAM_CHECK(watcher != NULL, ParamTaskClose(client);
        return -1, "Failed to get watcher");
    ListInit(&watcher->node);
    PARAM_TRIGGER_HEAD_INIT(watcher->triggerHead);
    ListAddTail(&GetTriggerWorkSpace()->waitList, &watcher->node);
    watcher->stream = client;
    watcher->timeout = UINT32_MAX;
    return 0;
}

static void TimerCallback(ParamTaskPtr timer, void *context)
{
    UNUSED(context);
    PARAM_CHECK(GetTriggerWorkSpace() != NULL, return, "Invalid wrokspace");
    ParamWatcher *watcher = GetNextParamWatcher(GetTriggerWorkSpace(), NULL);
    while (watcher != NULL) {
        ParamWatcher *next = GetNextParamWatcher(GetTriggerWorkSpace(), watcher);
        if (watcher->timeout > 0) {
            watcher->timeout--;
        } else {
            PARAM_LOGD("TimerCallback watcher->timeout %p ", watcher);
            ParamTaskClose(watcher->stream);
        }
        watcher = next;
    }
}

static int GetParamValueFromBuffer(const char *name, const char *buffer, char *value, int length)
{
    size_t bootLen = strlen(OHOS_BOOT);
    const char *tmpName = name + bootLen;
    int ret = GetProcCmdlineValue(tmpName, buffer, value, length);
    return ret;
}

static int LoadParamFromCmdLine(void)
{
    static const char *cmdLines[] = {
        OHOS_BOOT"hardware",
        OHOS_BOOT"reboot_reason",
#ifdef STARTUP_INIT_TEST
        OHOS_BOOT"mem",
        OHOS_BOOT"console",
        OHOS_BOOT"mmz",
        OHOS_BOOT"androidboot.selinux",
        OHOS_BOOT"init",
        OHOS_BOOT"root",
        OHOS_BOOT"uuid",
        OHOS_BOOT"aaaaa",
        OHOS_BOOT"rootfstype",
        OHOS_BOOT"blkdevparts"
#endif
    };
    char *data = ReadFileData(PARAM_CMD_LINE);
    PARAM_CHECK(data != NULL, return -1, "Failed to read file %s", PARAM_CMD_LINE);
    char *value = calloc(1, PARAM_CONST_VALUE_LEN_MAX + 1);
    PARAM_CHECK(value != NULL, free(data);
        return -1, "Failed to read file %s", PARAM_CMD_LINE);

    for (size_t i = 0; i < ARRAY_LENGTH(cmdLines); i++) {
        int ret = GetParamValueFromBuffer(cmdLines[i], data, value, PARAM_CONST_VALUE_LEN_MAX);
        if (ret == 0) {
            PARAM_LOGD("Add param from cmdline %s %s", cmdLines[i], value);
            ret = CheckParamName(cmdLines[i], 0);
            PARAM_CHECK(ret == 0, break, "Invalid name %s", cmdLines[i]);
            uint32_t dataIndex = 0;
            PARAM_LOGE("**** cmdLines[%d] %s, value %s", i, cmdLines[i], value);
            ret = WriteParam(&g_paramWorkSpace.paramSpace, cmdLines[i], value, &dataIndex, 0);
            PARAM_CHECK(ret == 0, break, "Failed to write param %s %s", cmdLines[i], value);
        } else {
            PARAM_LOGE("Can not find arrt %s", cmdLines[i]);
        }
    }
    PARAM_LOGD("Parse cmdline finish %s", PARAM_CMD_LINE);
    free(data);
    free(value);
    return 0;
}

int SystemWriteParam(const char *name, const char *value)
{
    PARAM_CHECK(name != NULL && value != NULL, return -1, "The name is null");
    return SystemSetParam(name, value, g_paramWorkSpace.securityLabel);
}

int SystemReadParam(const char *name, char *value, unsigned int *len)
{
    PARAM_CHECK(name != NULL && len != NULL, return -1, "The name is null");
    ParamHandle handle = 0;
    int ret = ReadParamWithCheck(&g_paramWorkSpace, name, DAC_READ, &handle);
    if (ret == 0) {
        ret = ReadParamValue(&g_paramWorkSpace, handle, value, len);
    }
    return ret;
}

int LoadPersistParams(void)
{
    return LoadPersistParam(&g_paramWorkSpace);
}

static int ProcessParamFile(const char *fileName, void *context)
{
    static const char *exclude[] = {"ctl.", "selinux.restorecon_recursive"};
    uint32_t mode = *(int *)context;
    return LoadDefaultParam_(fileName, mode, exclude, ARRAY_LENGTH(exclude));
}

int LoadDefaultParams(const char *fileName, uint32_t mode)
{
    PARAM_CHECK(fileName != NULL, return -1, "Invalid fielname for load");
    if (!PARAM_TEST_FLAG(g_paramWorkSpace.flags, WORKSPACE_FLAGS_INIT)) {
        return PARAM_CODE_NOT_INIT;
    }
    PARAM_LOGI("load default parameters %s.", fileName);
    int ret = 0;
    struct stat st;
    if ((stat(fileName, &st) == 0) && !S_ISDIR(st.st_mode)) {
        ret = ProcessParamFile(fileName, &mode);
    } else {
        ret = ReadFileInDir(fileName, ".para", ProcessParamFile, &mode);
    }

    // load security label
    ParamSecurityOps *ops = &g_paramWorkSpace.paramSecurityOps;
    if (ops->securityGetLabel != NULL) {
        ret = ops->securityGetLabel(AddSecurityLabel, fileName, (void *)&g_paramWorkSpace);
    }
    return ret;
}

void InitParamService(void)
{
    PARAM_LOGI("InitParamService pipe: %s.", PIPE_NAME);
    CheckAndCreateDir(PIPE_NAME);
    int ret = InitParamWorkSpace(&g_paramWorkSpace, 0);
    PARAM_CHECK(ret == 0, return, "Init parameter workspace fail");
    ret = InitPersistParamWorkSpace(&g_paramWorkSpace);
    PARAM_CHECK(ret == 0, return, "Init persist parameter workspace fail");
    if (g_paramWorkSpace.serverTask == NULL) {
        ParamStreamInfo info = {};
        info.flags = WORKER_TYPE_SERVER;
        info.server = PIPE_NAME;
        info.close = NULL;
        info.recvMessage = NULL;
        info.incomingConnect = OnIncomingConnect;
        ret = ParamServerCreate(&g_paramWorkSpace.serverTask, &info);
        PARAM_CHECK(ret == 0, return, "Failed to create server");
        PARAM_LOGD("OnIncomingConnect %p", g_paramWorkSpace.serverTask);
    }

    if (g_paramWorkSpace.timer == NULL) {
        ParamTimerCreate(&g_paramWorkSpace.timer, TimerCallback, &g_paramWorkSpace);
        ParamTimerStart(g_paramWorkSpace.timer, 1, MS_UNIT);
        PARAM_LOGD("Start timer %p", g_paramWorkSpace.timer);
    }
    ret = InitTriggerWorkSpace();
    PARAM_CHECK(ret == 0, return, "Failed to init trigger");

    ParamAuditData auditData = {};
    auditData.name = "#";
    auditData.label = NULL;
    auditData.dacData.gid = getegid();
    auditData.dacData.uid = geteuid();
    auditData.dacData.mode = DAC_ALL_PERMISSION;
    ret = AddSecurityLabel(&auditData, (void *)&g_paramWorkSpace);
    PARAM_CHECK(ret == 0, return, "Failed to add default dac label");

    // 读取cmdline的参数
    LoadParamFromCmdLine();
}

int StartParamService(void)
{
    return ParamServiceStart();
}

void StopParamService(void)
{
    PARAM_LOGI("StopParamService.");
    ClosePersistParamWorkSpace();
    CloseParamWorkSpace(&g_paramWorkSpace);
    CloseTriggerWorkSpace();
    ParamTaskClose(g_paramWorkSpace.serverTask);
    g_paramWorkSpace.serverTask = NULL;
    ParamServiceStop();
}

ParamWorkSpace *GetParamWorkSpace(void)
{
    return &g_paramWorkSpace;
}

void DumpParametersAndTriggers(void)
{
    DumpParameters(&g_paramWorkSpace, 1);
    if (GetTriggerWorkSpace() != NULL) {
        DumpTrigger(GetTriggerWorkSpace());
    }
}