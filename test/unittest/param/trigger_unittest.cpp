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

#include "init_jobs_internal.h"
#include "init_param.h"
#include "init_unittest.h"
#include "init_utils.h"
#include "param_libuvadp.h"
#include "param_manager.h"
#include "param_service.h"
#include "param_stub.h"
#include "param_utils.h"
#include "securec.h"
#include "trigger_checker.h"
#include "trigger_manager.h"

using namespace testing::ext;
using namespace std;

const static int32_t g_maxCount = 6;
static int32_t g_repeatCount = 0;
static void TimerCallback(ParamTaskPtr timer, void *context)
{
    TestClient(g_repeatCount);
    g_repeatCount = g_repeatCount + 1;
    if (g_repeatCount >= g_maxCount) {
        ParamTaskClose(timer);
        StopParamService();
    }
}

static const int triggerBuffer = 512;
static uint32_t g_execCmdId = 0;
static int g_matchTrigger = 0;
static char g_matchTriggerName[triggerBuffer] = { 0 };
static void TestCmdExec(const TriggerNode *trigger, const CommandNode *cmd, const char *content, uint32_t size)
{
    if (cmd->cmdKeyIndex == CMD_INDEX_FOR_PARA_WAIT || cmd->cmdKeyIndex == CMD_INDEX_FOR_PARA_WATCH) {
        TriggerExtData *extData = TRIGGER_GET_EXT_DATA(trigger, TriggerExtData);
        if (extData != nullptr && extData->excuteCmd != nullptr) {
            extData->excuteCmd(extData, cmd->cmdKeyIndex, content);
        }
        return;
    }
    g_execCmdId = cmd->cmdKeyIndex;
}

static int TestTriggerExecute(TriggerNode *trigger, const char *content, uint32_t size)
{
    int ret = memcpy_s(g_matchTriggerName, (int)sizeof(g_matchTriggerName) - 1, trigger->name, strlen(trigger->name));
    EXPECT_EQ(ret, 0);
    g_matchTriggerName[strlen(trigger->name)] = '\0';
    g_matchTrigger++;
    return 0;
}

class TriggerUnitTest : public ::testing::Test {
public:
    TriggerUnitTest() {}
    virtual ~TriggerUnitTest() {}

    void SetUp() {}
    void TearDown() {}
    void TestBody() {}

    int TestRunClient()
    {
        TestClient(0);
        ParamTaskPtr timer = nullptr;
        ParamTimerCreate(&timer, TimerCallback, nullptr);
        ParamTimerStart(timer, 500, g_maxCount); // 500ms
        StartParamService();
        return 0;
    }

    int ParseInitCfg(const char *configFile)
    {
        char *fileBuf = ReadFileToBuf(configFile);
        INIT_ERROR_CHECK(fileBuf != nullptr, return -1, "Failed to read file content %s", configFile);
        cJSON *fileRoot = cJSON_Parse(fileBuf);
        INIT_ERROR_CHECK(fileRoot != nullptr, return -1, "Failed to parse json file %s", configFile);
        ParseTriggerConfig(fileRoot);
        cJSON_Delete(fileRoot);
        free(fileBuf);
        fileBuf = nullptr;
        return 0;
    }

    int TestLoadTrigger()
    {
        int cmdKeyIndex = 0;
        const char *matchCmd = GetMatchCmd("setparam aaaa aaaa", &cmdKeyIndex);
        printf("cmd %d \n", matchCmd != nullptr);
        EXPECT_EQ(matchCmd != 0, 1);

        ReadConfig();
        ParseInitCfg(PARAM_DEFAULT_PATH "/trigger_test.cfg");
        // trigger
        PostTrigger(EVENT_TRIGGER_BOOT, "pre-init", strlen("pre-init"));
        PostTrigger(EVENT_TRIGGER_BOOT, "init", strlen("init"));
        PostTrigger(EVENT_TRIGGER_BOOT, "post-init", strlen("post-init"));
        LibuvEventTask *task = (LibuvEventTask *)(GetTriggerWorkSpace()->eventHandle);
        if (task != nullptr) { // 触发队列执行
            task->process(1, nullptr, 0);
        }
        return 0;
    }

    TriggerHeader *GetTriggerHeader(int type)
    {
        return &GetTriggerWorkSpace()->triggerHead[type];
    }

    int TestAddTriggerForBoot()
    {
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_BOOT), "init-later", "", 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), "init-later");
        EXPECT_EQ(node, trigger);
        if (trigger == nullptr) {
            return -1;
        }
        EXPECT_EQ(strcmp(trigger->name, "init-later"), 0);

        // add command
        int cmdIndex = 0;
        GetMatchCmd("reboot ", &cmdIndex);
        int ret = AddCommand(trigger, cmdIndex, nullptr);
        EXPECT_EQ(ret, 0);
        ret = AddCommand(trigger, cmdIndex, "update: aaaaaaa");
        EXPECT_EQ(ret, 0);
        return 0;
    }

    int TestAddTriggerForParm()
    {
        const char *triggerName = "param:test_param.000";
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, "test_param.000=1", 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);
        if (trigger == nullptr) {
            return -1;
        }
        EXPECT_EQ(strcmp(trigger->name, triggerName), 0);

        // add command
        int cmdIndex = 0;
        GetMatchCmd("reboot ", &cmdIndex);
        int ret = AddCommand(trigger, cmdIndex, nullptr);
        EXPECT_EQ(ret, 0);
        ret = AddCommand(trigger, cmdIndex, "update: aaaaaaa");
        EXPECT_EQ(ret, 0);
        return 0;
    }

    int TestParamEvent()
    {
        PostParamTrigger(EVENT_TRIGGER_PARAM, "net.tcp.default_init_rwnd", "60");
        const char *sysctrl = "ohos.startup.powerctrl=reboot, shutdown";
        PostTrigger(EVENT_TRIGGER_PARAM, sysctrl, strlen(sysctrl));
        PostParamTrigger(EVENT_TRIGGER_PARAM, "ohos.startup.powerctrl", "reboot, shutdown");

        const char *startCmd = "ohos.ctl.start=hdc -t";
        PostTrigger(EVENT_TRIGGER_PARAM, startCmd, strlen(startCmd));
        PostParamTrigger(EVENT_TRIGGER_PARAM, "ohos.ctl.start", "hdc -t");

        const char *stopCmd = "ohos.ctl.stop=hdc -t";
        PostTrigger(EVENT_TRIGGER_PARAM, stopCmd, strlen(stopCmd));
        PostParamTrigger(EVENT_TRIGGER_PARAM, "ohos.ctl.stop", "hdc -t");
        return 0;
    }

    int TestBootEvent(const char *boot)
    {
        PostTrigger(EVENT_TRIGGER_BOOT, boot, strlen(boot));
        return 0;
    }

    int TestCheckParamTrigger1()
    {
        const char *triggerName = "param:test_param.111";
        const char *param = "test_param.aaa.111.2222";
        const char *value = "1";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);

        g_matchTrigger = 0;
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, "2");
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, buffer, strlen(buffer), TestTriggerExecute);
        EXPECT_EQ(0, g_matchTrigger);

        g_matchTrigger = 0;
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, buffer, strlen(buffer), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));
        return 0;
    }

    int TestCheckParamTrigger2()
    {
        const char *triggerName = "param:test_param.222";
        const char *param = "test_param.aaa.222.2222";
        const char *value = "1";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=*", param);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);

        g_matchTrigger = 0;
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, "2");
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, buffer, strlen(buffer), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));

        g_matchTrigger = 0;
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, param, strlen(param), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));
        return 0;
    }

    int TestCheckParamTrigger3()
    {
        const char *triggerName = "param:test_param.333";
        const char *param1 = "test_param.aaa.333.2222=1";
        const char *param2 = "test_param.aaa.333.3333=2";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s || %s", param1, param2);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);

        g_matchTrigger = 0;
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, param1, strlen(param1), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));

        g_matchTrigger = 0;
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, param2, strlen(param2), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));
        return 0;
    }

    int TestCheckParamTrigger4()
    {
        const char *triggerName = "param:test_param.444";
        const char *param1 = "test_param.aaa.444.2222";
        const char *param2 = "test_param.aaa.444.3333";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=%s && %s=%s", param1, "1", param2, "2");
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);

        g_matchTrigger = 0;
        SystemWriteParam(param1, "1");
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param1, "1");
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, buffer, strlen(buffer), TestTriggerExecute);
        EXPECT_EQ(0, g_matchTrigger);

        SystemWriteParam(param2, "2");
        g_matchTrigger = 0;
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param2, "2");
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, buffer, strlen(buffer), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));
        return 0;
    }

    // test for trigger aaaa:test_param.aaa 被加入unknown执行
    int TestCheckParamTrigger5()
    {
        const char *triggerName = "aaaa:test_param.aaa";
        const char *param1 = "test_param.aaa.aaa.2222";
        const char *param2 = "test_param.aaa.aaa.3333";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "aaaa && %s=%s && %s=%s", param1, "1", param2, "2");
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_UNKNOW), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);

        g_matchTrigger = 0;
        SystemWriteParam(param1, "1");
        ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param1, "1");
        EXPECT_GE(ret, 0);
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_PARAM, buffer, strlen(buffer), TestTriggerExecute);
        EXPECT_EQ(0, g_matchTrigger);

        SystemWriteParam(param2, "2");
        g_matchTrigger = 0;
        CheckTrigger(GetTriggerWorkSpace(), TRIGGER_UNKNOW, "aaaa", strlen("aaaa"), TestTriggerExecute);
        EXPECT_EQ(1, g_matchTrigger);
        EXPECT_EQ(0, strcmp(triggerName, g_matchTriggerName));
        return 0;
    }

    int TestComputeCondition(const char *condition)
    {
        u_int32_t size = strlen(condition) + CONDITION_EXTEND_LEN;
        char *prefix = (char *)malloc(size);
        if (prefix == nullptr) {
            printf("prefix is null.\n");
            return -1;
        }
        ConvertInfixToPrefix(condition, prefix, size);
        printf("prefix %s \n", prefix);
        free(prefix);
        return 0;
    }

    // 普通的属性trigger
    int TestExecuteParamTrigger1()
    {
        const char *triggerName = "aaaa:test_param.eee";
        const char *param = "test_param.eee.aaa.1111";
        const char *value = "eee";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);

        const uint32_t cmdIndex = 100;
        ret = AddCommand(trigger, cmdIndex, value);
        EXPECT_EQ(ret, 0);
        // 修改命令为测试执行
        GetTriggerWorkSpace()->cmdExec = TestCmdExec;
        LibuvEventTask *task = (LibuvEventTask *)(GetTriggerWorkSpace()->eventHandle);
        SystemWriteParam(param, value);
        // 触发队列执行
        task->process(1, nullptr, 0);
        EXPECT_EQ(g_execCmdId, cmdIndex);
        return 0;
    }

    int TestExecuteParamTrigger2()
    {
        const char *triggerName = "param:test_param.dddd";
        const char *param = "test_param.dddd.aaa.2222";
        const char *value = "2222";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);
        const uint32_t cmdIndex = 102;
        ret = AddCommand(trigger, cmdIndex, value);
        EXPECT_EQ(ret, 0);
        // 修改命令为测试执行
        GetTriggerWorkSpace()->cmdExec = TestCmdExec;
        LibuvEventTask *task = (LibuvEventTask *)(GetTriggerWorkSpace()->eventHandle);
        task->beforeProcess(EVENT_TRIGGER_PARAM, buffer, strlen(buffer));
        // 触发队列执行
        task->process(EVENT_TRIGGER_PARAM, nullptr, 0);
        EXPECT_EQ(g_execCmdId, cmdIndex);
        return 0;
    }

    // 测试执行后立刻删除
    int TestExecuteParamTrigger3()
    {
        const char *triggerName = "param:test_param.3333";
        const char *param = "test_param.dddd.aaa.3333";
        const char *value = "3333";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);
        if (trigger == nullptr) {
            return -1;
        }
        const uint32_t cmdIndex = 103;
        ret = AddCommand(trigger, cmdIndex, value);
        EXPECT_EQ(ret, 0);
        TRIGGER_SET_FLAG(trigger, TRIGGER_FLAGS_ONCE);

        // 修改命令为测试执行
        GetTriggerWorkSpace()->cmdExec = TestCmdExec;
        LibuvEventTask *task = (LibuvEventTask *)(GetTriggerWorkSpace()->eventHandle);
        task->beforeProcess(EVENT_TRIGGER_PARAM, buffer, strlen(buffer));
        // 触发队列执行
        task->process(EVENT_TRIGGER_PARAM, nullptr, 0);
        EXPECT_EQ(g_execCmdId, cmdIndex);
        trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        if (trigger != nullptr) {
            EXPECT_EQ(1, 0);
        }
        return 0;
    }

    // 测试删除队列中的trigger
    int TestExecuteParamTrigger4()
    {
        const char *triggerName = "param:test_param.4444";
        const char *param = "test_param.dddd.aaa.4444";
        const char *value = "4444";
        char buffer[triggerBuffer];
        int ret = sprintf_s(buffer, sizeof(buffer), "%s=%s", param, value);
        EXPECT_GE(ret, 0);
        TriggerNode *node = AddTrigger(GetTriggerHeader(TRIGGER_PARAM), triggerName, buffer, 0);
        TriggerNode *trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        EXPECT_EQ(trigger, node);
        if (trigger == nullptr) {
            return -1;
        }
        const uint32_t cmdIndex = 105;
        ret = AddCommand(trigger, cmdIndex, value);
        EXPECT_EQ(ret, 0);
        TRIGGER_SET_FLAG(trigger, TRIGGER_FLAGS_ONCE);

        // 修改命令为测试执行
        GetTriggerWorkSpace()->cmdExec = TestCmdExec;
        LibuvEventTask *task = (LibuvEventTask *)(GetTriggerWorkSpace()->eventHandle);
        task->beforeProcess(EVENT_TRIGGER_PARAM, buffer, strlen(buffer));

        // 删除对应的trigger
        FreeTrigger(trigger);

        // 触发队列执行
        task->process(EVENT_TRIGGER_PARAM, nullptr, 0);
        EXPECT_NE(g_execCmdId, cmdIndex);
        trigger = GetTriggerByName(GetTriggerWorkSpace(), triggerName);
        if (trigger != nullptr) {
            EXPECT_EQ(1, 0);
        }
        return 0;
    }

    // 测试执行后检查子trigger执行
    int TestExecuteParamTrigger5()
    {
        const char *boot = "boot2";
        const char *triggerName = "boot2:test_param.5555";
        const char *param = "test_param.dddd.aaa.5555";
        const char *value = "5555";
        TriggerNode *trigger = AddTrigger(GetTriggerHeader(TRIGGER_BOOT), boot, nullptr, 0);
        const int testCmdIndex = 1105;
        int ret = AddCommand(trigger, testCmdIndex, value);
        EXPECT_EQ(ret, 0);
        if (trigger == nullptr) {
            return -1;
        }
        TRIGGER_SET_FLAG(trigger, TRIGGER_FLAGS_SUBTRIGGER);

        char buffer[triggerBuffer];
        ret = sprintf_s(buffer, sizeof(buffer), "boot2 && %s=%s", param, value);
        EXPECT_GE(ret, 0);
        trigger = AddTrigger(GetTriggerHeader(TRIGGER_UNKNOW), triggerName, buffer, 0);
        const int testCmdIndex2 = 105;
        ret = AddCommand(trigger, testCmdIndex2, value);

        // 修改命令为测试执行
        GetTriggerWorkSpace()->cmdExec = TestCmdExec;
        // 设置属性值
        SystemWriteParam(param, value);

        // 触发boot
        TestBootEvent(boot);

        LibuvEventTask *task = (LibuvEventTask *)(GetTriggerWorkSpace()->eventHandle);
        // 触发队列执行
        task->process(EVENT_TRIGGER_PARAM, nullptr, 0);
        // 连续执行两个trigger
        EXPECT_EQ(g_execCmdId, (uint32_t)testCmdIndex2);
        return 0;
    }

    int TestDumpTrigger()
    {
        DumpTrigger(GetTriggerWorkSpace());
        return 0;
    }
};

HWTEST_F(TriggerUnitTest, TestLoadTrigger, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestLoadTrigger();
}

HWTEST_F(TriggerUnitTest, TestBootEvent, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestBootEvent("pre-init");
    test.TestBootEvent("init");
    test.TestBootEvent("post-init");
    test.TestBootEvent("early-init");
}

HWTEST_F(TriggerUnitTest, TestAddTriggerForBoot, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestAddTriggerForBoot();
}

HWTEST_F(TriggerUnitTest, TestAddTriggerForParm, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestAddTriggerForParm();
}

HWTEST_F(TriggerUnitTest, TestCheckParamTrigger1, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestCheckParamTrigger1();
}

HWTEST_F(TriggerUnitTest, TestCheckParamTrigger2, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestCheckParamTrigger2();
}

HWTEST_F(TriggerUnitTest, TestCheckParamTrigger3, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestCheckParamTrigger3();
}

HWTEST_F(TriggerUnitTest, TestCheckParamTrigger4, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestCheckParamTrigger4();
}

HWTEST_F(TriggerUnitTest, TestCheckParamTrigger5, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestCheckParamTrigger5();
}

HWTEST_F(TriggerUnitTest, TestParamEvent, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestParamEvent();
}

HWTEST_F(TriggerUnitTest, ComputerCondition, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestComputeCondition("aaa=111||aaa=222||aaa=333");
    test.TestComputeCondition("aaa=111||aaa=222&&aaa=333");
    test.TestComputeCondition("(aaa=111||aaa=222)&&aaa=333");
    test.TestComputeCondition("aaa=111||(aaa=222&&aaa=333)");
}

HWTEST_F(TriggerUnitTest, TestExecuteParamTrigger1, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestExecuteParamTrigger1();
}

HWTEST_F(TriggerUnitTest, TestExecuteParamTrigger2, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestExecuteParamTrigger2();
}

HWTEST_F(TriggerUnitTest, TestExecuteParamTrigger3, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestExecuteParamTrigger3();
}

HWTEST_F(TriggerUnitTest, TestExecuteParamTrigger4, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestExecuteParamTrigger4();
}

HWTEST_F(TriggerUnitTest, TestExecuteParamTrigger5, TestSize.Level0)
{
    TriggerUnitTest test;
    test.TestExecuteParamTrigger5();
}

HWTEST_F(TriggerUnitTest, TestRunClient, TestSize.Level0)
{
    TriggerUnitTest test;
    int ret = test.TestRunClient();
    EXPECT_EQ(ret, 0);
}
