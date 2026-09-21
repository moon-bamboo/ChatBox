/*
 * test_keep.c - 验证 DrawMessages() 里"挑选要显示的消息"那段逻辑
 *
 * 这段逻辑有三个边界很容易写错，而且错了以后症状很隐蔽
 * (消息框要么全空、要么该消失的不消失)。所以单独抽出来测。
 *
 * 逻辑是从 ChatBox.cpp 的 DrawMessages() 里 1:1 抄过来的。
 * ⚠️ 改动那一段时必须同步改这里 —— 否则测试通过也说明不了任何事
 *    (已经因为不同步而误报过一次)。
 *
 * 编译运行:
 *   gcc -O2 -o test_keep.exe test_keep.c && test_keep.exe
 */

#include <stdio.h>

/* ---- 被测量的逻辑（与 ChatBox.cpp 保持一致）----
 *
 * timeouts[i] : 第 i 条消息的超时帧数 (-1 = 永不消失)
 * ages[i]     : 第 i 条消息已经存在了多少帧
 * 消息下标越大 = 越新。返回被选中的条数，out[] 里 [0] 是最新的一条。
 */
static int pick(int nMsgs, int keepLast, int expanded, int maxHistory,
                const int* timeouts, const int* ages, int* out)
{
    int picked = 0;
    int i;

    for (i = nMsgs - 1; i >= 0 && picked < maxHistory; i--)
    {
        if (picked >= keepLast && !expanded && timeouts[i] >= 0 && timeouts[i] > 0)
        {
            if (ages[i] > timeouts[i]) continue;   /* 跳过这条, 继续看更旧的 */
        }
        out[picked++] = i;
    }
    return picked;
}

static int g_fail = 0;

static void check(const char* name, int got, int want)
{
    if (got == want)
    {
        printf("[PASS] %-52s = %d\n", name, got);
    }
    else
    {
        printf("[FAIL] %-52s = %d (期望 %d)\n", name, got, want);
        g_fail++;
    }
}

int main(void)
{
    int out[64];
    int n, got;

    /* 5 条消息，全部已超时 (超时 1259 帧，已存在 5000 帧) */
    int to5[5]  = { 1259, 1259, 1259, 1259, 1259 };
    int age5[5] = { 5000, 5000, 5000, 5000, 5000 };

    printf("== 全部已超时的 5 条消息，看 KeepLast 保留几条 ==\n");

    n = pick(5, 0, 0, 100, to5, age5, out);
    check("KeepLast=0  -> 一条都不留(消息框会消失)", n, 0);

    n = pick(5, 1, 0, 100, to5, age5, out);
    check("KeepLast=1  -> 留最新 1 条", n, 1);
    check("  且留下的确实是【最新】那条", out[0], 4);

    n = pick(5, 3, 0, 100, to5, age5, out);
    check("KeepLast=3  -> 留最新 3 条", n, 3);
    check("  顺序: 最新在前", out[0] == 4 && out[1] == 3 && out[2] == 2, 1);

    n = pick(5, 5, 0, 100, to5, age5, out);
    check("KeepLast=5  -> 全部 5 条都留", n, 5);

    n = pick(5, 50, 0, 100, to5, age5, out);
    check("KeepLast=50 -> 消息只有 5 条，就留 5 条(不越界)", n, 5);

    printf("\n== 半超时的情况 ==\n");

    /* 最新 2 条还没超时，旧 3 条超时了 */
    {
        int to[5]  = { 1259, 1259, 1259, 1259, 1259 };
        int age[5] = { 5000, 5000, 5000,  100,  100 };

        n = pick(5, 0, 0, 100, to, age, out);
        check("KeepLast=0, 最新2条未超时 -> 显示 2 条", n, 2);

        n = pick(5, 3, 0, 100, to, age, out);
        check("KeepLast=3, 同上 -> 3 条(多留1条已超时的)", n, 3);

        n = pick(5, 1, 0, 100, to, age, out);
        check("KeepLast=1, 同上 -> 2 条(常驻名额被未超时的占了)", n, 2);
    }

    printf("\n== timeout = -1 (永不消失) 的消息不能被挡住 ==\n");
    {
        /* index 1 是永久消息, 却比 index 2 更旧; index 2/3 是普通消息。
         * 用 break 实现时会停在 index 2, 把永久消息误挡在外(已修)。 */
        int to[4]  = { 1259,  -1, 1259, 1259 };
        int age[4] = { 99999, 99999, 99999, 10 };

        n = pick(4, 1, 0, 100, to, age, out);
        check("KeepLast=1 -> 最新那条 + 永久消息, 共 2 条", n, 2);
        check("  永久消息确实在结果里", (n == 2 && (out[0] == 3 && out[1] == 1)), 1);

        n = pick(4, 0, 0, 100, to, age, out);
        check("KeepLast=0 -> 未超时的最新条 + 永久消息, 共 2 条", n, 2);
    }

    printf("\n== 展开态忽略超时 / MaxHistory 封顶 ==\n");
    {
        int to[5]     = { 1259, 1259, 1259, 1259, 1259 };
        int age32[5]  = { 5000, 5000, 5000, 5000, 5000 };   /* 全部已超时 */
        int ageNow[5] = {   10,   10,   10,   10,   10 };   /* 全部未超时 */

        n = pick(5, 0, 1, 100, to, age32, out);
        check("展开态, KeepLast=0 -> 全部 5 条都显示", n, 5);

        /* 测 MaxHistory 封顶必须用【未超时】的数据, 否则先被超时逻辑拦掉 */
        n = pick(5, 0, 0, 3, to, ageNow, out);
        check("折叠态, 未超时, MaxHistory=3 -> 封顶 3 条", n, 3);
    }

    printf("\n== timeout = 0 视为不超时(不淘汰) ==\n");
    {
        int to[3]  = { 0, 0, 0 };
        int age[3] = { 99999, 99999, 99999 };

        n = pick(3, 0, 0, 100, to, age, out);
        check("timeout=0 -> 不按超时淘汰, 3 条全留", n, 3);
    }

    printf("\nfailures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
