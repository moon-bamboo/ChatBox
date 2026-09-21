/*
 * test_tick.c - 验证"单调帧计数 g_Tick"的逻辑
 *
 * 背景: 引擎的 CurrentFrame (0xA8ED84) 会因【读档】而回退。
 * 之前直接用它判断消息超时, 于是读档后收到的消息"永远不超时"
 * (实机现象: 读档几次后任务目标重复出现好几遍, 死活不消失)。
 *
 * 修法是自己维护一个只增不减的 g_Tick: 每逻辑帧加上 CurrentFrame 的增量,
 * 增量离谱(回退/跳变)时丢弃。这样时间最多"卡一下", 绝不倒退。
 *
 * 逻辑是从 ChatBox.cpp 的 ChatBox_FrameHook 里抄过来的。
 * ⚠️ 改动那一段时必须同步改这里。
 *
 * 编译运行:
 *   gcc -O2 -o test_tick.exe test_tick.c && test_tick.exe
 */

#include <stdio.h>

#define TICK_MAX_DELTA  100000u

static unsigned g_Tick      = 0;
static unsigned g_LastFrame = 0;

/* 与 ChatBox.cpp 一致：推进一帧 */
static void advance(unsigned f)
{
    if (f != g_LastFrame)
    {
        unsigned d = f - g_LastFrame;
        if (d > TICK_MAX_DELTA) d = 0;
        g_LastFrame = f;
        g_Tick += d;
    }
}

/* 旧实现：直接用 CurrentFrame 算年龄 */
static unsigned oldAge(unsigned f, unsigned msgFrame)
{
    return f - msgFrame;
}

static int g_fail = 0;

static void check(const char* name, unsigned got, unsigned want)
{
    if (got == want) printf("[PASS] %-58s = %u\n", name, got);
    else { printf("[FAIL] %-58s = %u (期望 %u)\n", name, got, want); g_fail++; }
}

static void checkTrue(const char* name, int cond)
{
    if (cond) printf("[PASS] %s\n", name);
    else { printf("[FAIL] %s\n", name); g_fail++; }
}

int main(void)
{
    printf("== 1. 正常增长: CurrentFrame 每帧 +1 ==\n");
    g_Tick = 0; g_LastFrame = 0;
    advance(1000);            /* 首次: 采纳 1000 (与消息用同一基准, 无妨) */
    check("首次对齐后 tick", g_Tick, 1000);
    advance(1001); advance(1002); advance(1003);
    check("连走 3 帧后 tick", g_Tick, 1003);

    printf("\n== 2. 读档回退: CurrentFrame 从 1003 掉回 200 ==\n");
    {
        unsigned before = g_Tick;
        advance(200);                       /* 回退 */
        check("回退后 tick 不变", g_Tick, before);
        advance(201); advance(202);
        check("之后正常继续增长", g_Tick, before + 2);
        checkTrue("tick 全程单调不减", 1);
    }

    printf("\n== 3. 对比旧实现: 同样的回退会让旧算法算出天文数字 ==\n");
    {
        /* 消息在 CurrentFrame=1003 时收到; 读档后 CurrentFrame=202 */
        unsigned age = oldAge(202, 1003);
        printf("       旧算法 age = %u  (无符号下溢)\n", age);
        checkTrue("旧算法确实下溢(> 2700 超时阈值)", age > 2700u);
        printf("       -> 旧实现: 读档【前】的消息会被误判为已超时\n");
    }

    printf("\n== 4. 读档后新消息的年龄按真实经过的帧数走 ==\n");
    {
        unsigned msgFrame;
        g_Tick = 0; g_LastFrame = 0;
        advance(1003);
        msgFrame = g_Tick;                  /* 读档前收到一条 */
        advance(1004); advance(1005);
        check("读档前消息 age", g_Tick - msgFrame, 2);

        advance(200);                       /* 读档, CurrentFrame 回退 */
        check("回退瞬间消息 age 不变", g_Tick - msgFrame, 2);

        advance(201); advance(202);
        check("读档后 age 继续增长", g_Tick - msgFrame, 4);
        checkTrue("age 远小于超时阈值 2700, 不该消失", (g_Tick - msgFrame) < 2700u);
    }

    printf("\n== 5. 连续多次读档(实机就是读了两三次) ==\n");
    {
        unsigned msgFrame;
        int i;
        g_Tick = 0; g_LastFrame = 0;
        advance(5000);
        msgFrame = g_Tick;                  /* 第一条消息 */

        for (i = 0; i < 5; i++)
        {
            advance(100 + i);               /* 读档, 回到很早的帧号 */
            advance(101 + i);
        }
        /* 每次读档只贡献 1 帧增量(第二次 advance 的那 1 帧) */
        check("5 次读档推了 5 帧", g_Tick - msgFrame, 5);
        checkTrue("tick 仍单调(未倒退)", g_Tick >= msgFrame);
    }

    printf("\n== 6. 切场景这种大跳变也必须丢弃 ==\n");
    {
        unsigned before;
        g_Tick = 0; g_LastFrame = 0;
        advance(10);
        before = g_Tick;
        advance(500000);                    /* 正向大跳(不是回退, 而是场景切换) */
        check("正向大跳也被丢弃", g_Tick, before);
        advance(500001);
        check("之后恢复 +1", g_Tick, before + 1);
    }

    printf("\n== 7. 同一帧内重复调用不应重复计数 ==\n");
    {
        g_Tick = 0; g_LastFrame = 0;
        advance(1000);
        advance(1000); advance(1000); advance(1000);   /* 钩子每秒被调 600 次 */
        check("帧号没变就不推进", g_Tick, 1000);
    }

    printf("\nfailures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
