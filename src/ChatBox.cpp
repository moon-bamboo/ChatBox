/*
 * ChatBox.cpp - 消息历史窗口 for 尤里的复仇 1.001
 *
 * ================ 阶段 1: 只捕获, 不显示 ================
 *
 * 目标: 钩住 MessageListClass::AddMessage 的【函数入口】, 把每一条消息
 *       的全部参数 + 调用者返回地址写进日志文件。
 *
 * 为什么先做这个:
 *   1. 验证"钩函数入口能抓到全部消息"(Phobos 只挂了一个调用点, 只能抓到
 *      触发器文本, 漏掉玩家聊天和 PrintMessage 类消息)
 *   2. 找出【快捷键提示】("选中画面相同单位"之类) 的特征 —— 这类消息频率
 *      极高, 必须识别出来并分流, 否则会把消息框淹没
 *   3. 确认 ColorSchemeIdx 是否就等于发言玩家的颜色(用于按来源着色)
 *
 * 阶段 1 完全不下发任何事件、不碰任何游戏状态, 只读 + 写文件。
 *
 * ================ 构建 ================
 *   32 位 mingw, C++ 受限子集(无 STL / 无异常 / 无 RTTI), 不依赖 CRT。
 *   工具链必须放在纯 ASCII 路径下编译(见 build.bat)。
 */

#include <windows.h>
#include "offsets.h"

#define CB_VERSION      "0.5.0"
#define CB_SECTION      "ChatBox"

#define MAX_PATH_LEN    260
#define LOG_LINE_MAX    4096      /* 日志单行缓冲 */
#define MAX_TEXT_CHARS  1024      /* 单条消息最多取这么多字符 */
#define DIR_NAME        "MsgLog"

/* ---------------- 配置 ---------------- */

#define DEF_ENABLE      1
#define DEF_LOG_RAW     0         /* 默认不记 nameLen/textLen(那是阶段1标定用的) */

/* 一次可回看条数的上限 —— 排版用的数组按它分配, 所以必须在 LoadConfig 之前定义。
 * 实际生效值由配置 MaxHistory 决定(默认 100)。 */
#define MAX_HISTORY_LIMIT 500

/* "一直显示、不随超时消失"的最新消息条数上限(配置 KeepLast)。
 * 上限取 50 而不是 500: 这个值的用途是"消息全部过期后屏幕上仍留点东西",
 * 50 条已经远超一屏能放下的量(MaxLines 最多 40 行); 设太大只会挤占屏幕。 */
#define MAX_KEEP_LAST    50

typedef struct {
    int Enable;      /* ChatBox=1          总开关 */
    int LogRaw;      /* LogRaw=0           1 = 额外记录 nameLen/textLen */
    int LogText;     /* LogText=1          记录消息文本(关掉则只记长度) */
    int LogUtf8;     /* LogUtf8=0          1 = 日志用 UTF-8(带 BOM), 0 = 本地代码页 */

    /* ---- 阶段 2: 窗口 ---- */
    int ShowWindow;     /* ShowWindow=1     1 = 接管绘制, 显示自制消息框;
                         *                   0 = 完全不碰画面(原版消息照旧显示) */
    int MaxLines;       /* MaxLines=8       折叠态最多同时显示几条 */
    int BoardOpacity;   /* BoardOpacity=45  底板不透明度 0~100 */
    int PosX;           /* PosX=8           窗口左上角 X */
    int PosY;           /* PosY=8           窗口左上角 Y */
    int TopGapLines;    /* TopGapLines=1    顶部预留几行空档(给回车输入框让位) */
    int KeepLast;       /* KeepLast=1       最新的 N 条【永远显示】, 不随超时消失(0~50) */
    int MaxHistory;     /* MaxHistory=100   一次最多回看多少条消息 */
    int MaxWidth;       /* MaxWidth=0       消息框最大宽度(像素); 0 = 自动 */
    int ExpandLines;    /* ExpandLines=15   展开态最多显示多少行 */
    int ScrollStep;     /* ScrollStep=5     翻页/滚轮一次的步长(行) */
    int SysUseMyColor;  /* SysUseMyColor=0  1 = 系统消息用当前玩家颜色 */
    int FilterSilent;   /* FilterSilent=1   1 = 操作提示不进【主历史】(改由提示区显示) */

    /* ---- 操作提示区(快捷键/路径点提示的独立显示区, 贴在主框下方) ---- */
    int HintArea;       /* HintArea=1     1 = 开启提示区; 0 = 回到旧行为(见 FilterSilent) */
    int HintGap;        /* HintGap=4      与主框底边之间的缝隙(像素) */
    int HintLines;      /* HintLines=4    提示区最多显示几行 */
    int HintOpacity;    /* HintOpacity=45 提示区底板不透明度 0~100 (0 = 不画底板) */
} ChatBoxConfig;

static ChatBoxConfig g_Cfg;
static char          g_IniPath[MAX_PATH_LEN];
static char          g_GameDir[MAX_PATH_LEN];
static char          g_LogPath[MAX_PATH_LEN];
static unsigned      g_Seq    = 0;   /* 本次运行的消息序号 */

/* ==================== 无 CRT 工具函数 ==================== */

/* 无符号整数 -> 十进制, 返回写入长度 (p 至少 12 字节) */
static unsigned UtoA(unsigned v, char* p)
{
    char tmp[12];
    int i = 0, j = 0;
    if (!v) tmp[i++] = '0';
    while (v && i < 11) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) p[j++] = tmp[--i];
    p[j] = 0;
    return (unsigned)j;
}

/* 无符号整数 -> "0x1A2B3C4D", 返回写入长度 (p 至少 12 字节) */
static unsigned HtoA(unsigned v, char* p)
{
    static const char* hex = "0123456789ABCDEF";
    unsigned n = 0;
    int i;
    p[n++] = '0'; p[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) p[n++] = hex[(v >> i) & 0xF];
    p[n] = 0;
    return n;
}

/* 追加字符串 (带容量保护) */
static unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap)
{
    while (*src && pos < cap - 1) dst[pos++] = *src++;
    dst[pos] = 0;
    return pos;
}

/* 追加一个字符 */
static unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap)
{
    if (pos < cap - 1) { dst[pos++] = c; dst[pos] = 0; }
    return pos;
}

static unsigned WcsLenLimited(const wchar_t* w, unsigned maxLen)
{
    unsigned n = 0;
    if (!w) return 0;
    while (w[n] && n < maxLen) n++;
    return n;
}

/*
 * 指针合理性检查 —— 阶段 1 的重要保险。
 *
 * 我们是在函数入口按【推断出来的栈偏移】读参数的。万一日后偏移猜错，
 * 读到的东西可能是 0、小整数或未对齐的垃圾，直接送进 WideCharToMultiByte
 * 就是野指针解引用 -> 崩游戏。
 *
 * 这里只做最低限度的筛查: 落在用户态地址范围、且按 4 字节对齐。
 * 挡不住"有效但内容不是字符串"的情况, 但那种最多读到乱码, 不会崩。
 */
static int IsPlausiblePtr(const void* p)
{
    unsigned u = (unsigned)(size_t)p;
    return (u >= 0x00010000u && u < 0x7FF00000u && (u & 3u) == 0);
}

/*
 * 宽字符串 -> 本地代码页(中文系统即 GBK), 并转义控制字符。
 *
 * 为什么转义: 日志是"一条消息一行", 消息正文里的换行必须转成字面 "\n",
 * 否则会破坏日志的行结构, 后续没法用 grep 分析。
 */
static unsigned WideToLocalEscaped(const wchar_t* w, char* dst, unsigned cap)
{
    char tmp[MAX_TEXT_CHARS * 4];
    unsigned pos = 0;
    unsigned wlen;
    int n, i;

    if (!w) { dst[0] = 0; return 0; }
    if (!IsPlausiblePtr(w)) { AppendCStr(dst, 0, "<bad-ptr>", cap); return 0; }

    /* 先自己限长, 再交给 API —— 绝不让它去无限扫描内存 */
    wlen = WcsLenLimited(w, MAX_TEXT_CHARS);
    if (wlen == 0) { dst[0] = 0; return 0; }

    n = WideCharToMultiByte(g_Cfg.LogUtf8 ? CP_UTF8 : CP_ACP, 0, w, (int)wlen,
                            tmp, sizeof(tmp) - 2, NULL, NULL);
    if (n <= 0) { dst[0] = 0; return 0; }
    if (n > (int)sizeof(tmp) - 2) n = sizeof(tmp) - 2;

    for (i = 0; i < n && tmp[i]; i++)
    {
        char c = tmp[i];
        if      (c == '\n') pos = AppendCStr(dst, pos, "\\n", cap);
        else if (c == '\r') pos = AppendCStr(dst, pos, "\\r", cap);
        else if (c == '\t') pos = AppendCStr(dst, pos, "\\t", cap);
        else if (c == '"')  pos = AppendCStr(dst, pos, "\\\"", cap);
        else if (c == '\\') pos = AppendCStr(dst, pos, "\\\\", cap);
        else if ((unsigned char)c < 0x20) { /* 其余控制字符丢弃 */ }
        else pos = AppendCh(dst, pos, c, cap);
    }
    dst[pos] = 0;
    return pos;
}

/* ==================== 日志 ==================== */

static HANDLE           g_Log       = NULL;
static CRITICAL_SECTION g_LogCS;
static int              g_LogCSReady = 0;

/* 前向声明: LogOpen 写头部时要调 LogLine, 而 LogLine 首次又要点 LogOpen */
static void LogLine(const char* body);

/* 拼出日志文件路径: <游戏目录>\MsgLog\YYYY-MM-DD_HH-MM-SS.log */
static void BuildLogPath(void)
{
    SYSTEMTIME st;
    char dir[MAX_PATH_LEN];
    char name[64];
    unsigned k = 0;

    k = AppendCStr(dir, 0, g_GameDir, MAX_PATH_LEN);
    AppendCStr(dir, k, "\\" DIR_NAME, MAX_PATH_LEN);
    CreateDirectoryA(dir, NULL);          /* 已存在则失败, 无所谓 */

    GetLocalTime(&st);
    k = 0;
    k += UtoA((unsigned)st.wYear,  name + k);
    k = AppendCh(name, k, '-', sizeof(name));
    if (st.wMonth < 10) k = AppendCh(name, k, '0', sizeof(name));
    k += UtoA((unsigned)st.wMonth, name + k);
    k = AppendCh(name, k, '-', sizeof(name));
    if (st.wDay < 10) k = AppendCh(name, k, '0', sizeof(name));
    k += UtoA((unsigned)st.wDay,   name + k);
    k = AppendCh(name, k, '_', sizeof(name));
    if (st.wHour < 10) k = AppendCh(name, k, '0', sizeof(name));
    k += UtoA((unsigned)st.wHour,  name + k);
    k = AppendCh(name, k, '-', sizeof(name));
    if (st.wMinute < 10) k = AppendCh(name, k, '0', sizeof(name));
    k += UtoA((unsigned)st.wMinute, name + k);
    k = AppendCh(name, k, '-', sizeof(name));
    if (st.wSecond < 10) k = AppendCh(name, k, '0', sizeof(name));
    k += UtoA((unsigned)st.wSecond, name + k);
    AppendCStr(name, k, ".log", sizeof(name));

    k = AppendCStr(g_LogPath, 0, dir, MAX_PATH_LEN);
    k = AppendCh(g_LogPath, k, '\\', MAX_PATH_LEN);
    AppendCStr(g_LogPath, k, name, MAX_PATH_LEN);
}

/*
 * 首次【真正要写日志】时才创建文件并写头部。
 *
 * 为什么不在一开始就建: 启动器进程也会加载本 DLL, 但它一条消息都不产生 ——
 * 那样就会留下一份只有头部的空日志(实测确实踩到过, 一次启动出现两份 log)。
 *
 * 注意调用关系: LogLine() 在 g_Log 为空时会调本函数, 而本函数在写头部之前
 * 已经给 g_Log 赋好值, 所以不会递归。
 */
static void LogOpen(void)
{
    DWORD written = 0;
    char  b1[16], b2[16], b3[16], b4[16];
    char  buf[512];
    unsigned p;

    if (g_Log) return;
    if (!g_LogCSReady) { InitializeCriticalSection(&g_LogCS); g_LogCSReady = 1; }
    BuildLogPath();
    g_Log = CreateFileA(g_LogPath, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Log == INVALID_HANDLE_VALUE) { g_Log = NULL; return; }

    /* UTF-8 时先写 BOM, 记事本才会正确识别 */
    if (g_Cfg.LogUtf8)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        WriteFile(g_Log, bom, 3, &written, NULL);
    }

    UtoA((unsigned)g_Cfg.Enable,  b1);
    UtoA((unsigned)g_Cfg.LogRaw,  b2);
    UtoA((unsigned)g_Cfg.LogText, b3);
    UtoA((unsigned)g_Cfg.LogUtf8, b4);

    LogLine("==================================================");
    LogLine("ChatBox " CB_VERSION " start");

    p = 0;
    p = AppendCStr(buf, p, "config: ChatBox=",  sizeof(buf));
    p = AppendCStr(buf, p, b1,                   sizeof(buf));
    p = AppendCStr(buf, p, " LogRaw=",           sizeof(buf));
    p = AppendCStr(buf, p, b2,                   sizeof(buf));
    p = AppendCStr(buf, p, " LogText=",          sizeof(buf));
    p = AppendCStr(buf, p, b3,                   sizeof(buf));
    p = AppendCStr(buf, p, " LogUtf8=",          sizeof(buf));
    p = AppendCStr(buf, p, b4,                   sizeof(buf));
    LogLine(buf);

    p = 0;
    p = AppendCStr(buf, p, "log file: ", sizeof(buf));
    p = AppendCStr(buf, p, g_LogPath,    sizeof(buf));
    LogLine(buf);

    LogLine("--------------------------------------------------");
}

/* 写一行: 自动加 [MM-DD HH:MM:SS] 时间戳前缀 */
static void LogLine(const char* body)
{
    char buf[LOG_LINE_MAX];
    unsigned n = 0;
    DWORD written = 0;
    SYSTEMTIME st;
    const char* p;

    if (!g_Log) LogOpen();      /* 首次真正要写时才建文件(含头部) */
    if (!g_Log) return;

    GetLocalTime(&st);
    buf[n++] = '[';
    if (st.wMonth < 10) buf[n++] = '0';
    n += UtoA((unsigned)st.wMonth, buf + n);  buf[n++] = '-';
    if (st.wDay < 10) buf[n++] = '0';
    n += UtoA((unsigned)st.wDay, buf + n);
    buf[n++] = ' ';
    if (st.wHour < 10) buf[n++] = '0';
    n += UtoA((unsigned)st.wHour, buf + n);   buf[n++] = ':';
    if (st.wMinute < 10) buf[n++] = '0';
    n += UtoA((unsigned)st.wMinute, buf + n); buf[n++] = ':';
    if (st.wSecond < 10) buf[n++] = '0';
    n += UtoA((unsigned)st.wSecond, buf + n);
    buf[n++] = ']'; buf[n++] = ' ';

    for (p = body; *p && n < LOG_LINE_MAX - 3; p++) buf[n++] = *p;
    buf[n++] = 13; buf[n++] = 10;

    EnterCriticalSection(&g_LogCS);
    SetFilePointer(g_Log, 0, NULL, FILE_END);
    WriteFile(g_Log, buf, n, &written, NULL);
    LeaveCriticalSection(&g_LogCS);
}

/* ==================== 路径与配置 ==================== */

static void BuildPaths(void)
{
    char exe[MAX_PATH_LEN];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH_LEN);
    DWORD i;
    unsigned k;

    if (n == 0 || n >= MAX_PATH_LEN) { exe[0] = 0; n = 0; }
    i = n;
    while (i > 0 && exe[i] != '\\' && exe[i] != '/') i--;
    exe[i] = 0;

    k = 0;
    k = AppendCStr(g_GameDir, k, exe, MAX_PATH_LEN);

    k = 0;
    k = AppendCStr(g_IniPath, k, exe, MAX_PATH_LEN);
    AppendCStr(g_IniPath, k, "\\ChatBox.ini", MAX_PATH_LEN);
}

static void LoadConfig(void)
{
    g_Cfg.Enable  = GetPrivateProfileIntA(CB_SECTION, "ChatBox", DEF_ENABLE,  g_IniPath);
    g_Cfg.LogRaw  = GetPrivateProfileIntA(CB_SECTION, "LogRaw",  DEF_LOG_RAW, g_IniPath);
    g_Cfg.LogText = GetPrivateProfileIntA(CB_SECTION, "LogText", 1,           g_IniPath);
    g_Cfg.LogUtf8 = GetPrivateProfileIntA(CB_SECTION, "LogUtf8", 0,           g_IniPath);

    g_Cfg.ShowWindow   = GetPrivateProfileIntA(CB_SECTION, "ShowWindow",   1,  g_IniPath);
    g_Cfg.MaxLines     = GetPrivateProfileIntA(CB_SECTION, "MaxLines",     8,  g_IniPath);
    g_Cfg.BoardOpacity = GetPrivateProfileIntA(CB_SECTION, "BoardOpacity", 45, g_IniPath);
    g_Cfg.PosX         = GetPrivateProfileIntA(CB_SECTION, "PosX",         8,  g_IniPath);
    g_Cfg.PosY         = GetPrivateProfileIntA(CB_SECTION, "PosY",         8,  g_IniPath);
    g_Cfg.TopGapLines  = GetPrivateProfileIntA(CB_SECTION, "TopGapLines",  1,  g_IniPath);
    g_Cfg.KeepLast     = GetPrivateProfileIntA(CB_SECTION, "KeepLast",     1,  g_IniPath);
    g_Cfg.MaxHistory   = GetPrivateProfileIntA(CB_SECTION, "MaxHistory",  100, g_IniPath);
    g_Cfg.MaxWidth     = GetPrivateProfileIntA(CB_SECTION, "MaxWidth",     0,  g_IniPath);
    g_Cfg.ExpandLines  = GetPrivateProfileIntA(CB_SECTION, "ExpandLines", 15,  g_IniPath);
    g_Cfg.ScrollStep   = GetPrivateProfileIntA(CB_SECTION, "ScrollStep",  5,   g_IniPath);
    g_Cfg.SysUseMyColor= GetPrivateProfileIntA(CB_SECTION, "SysUseMyColor", 0, g_IniPath);
    g_Cfg.FilterSilent = GetPrivateProfileIntA(CB_SECTION, "FilterSilent", 1,  g_IniPath);
    g_Cfg.HintArea     = GetPrivateProfileIntA(CB_SECTION, "HintArea",     1,  g_IniPath);
    g_Cfg.HintGap      = GetPrivateProfileIntA(CB_SECTION, "HintGap",      4,  g_IniPath);
    g_Cfg.HintLines    = GetPrivateProfileIntA(CB_SECTION, "HintLines",    4,  g_IniPath);
    g_Cfg.HintOpacity  = GetPrivateProfileIntA(CB_SECTION, "HintOpacity",  45, g_IniPath);

    g_Cfg.Enable  = g_Cfg.Enable  ? 1 : 0;
    g_Cfg.LogRaw  = g_Cfg.LogRaw  ? 1 : 0;
    g_Cfg.LogText = g_Cfg.LogText ? 1 : 0;
    g_Cfg.LogUtf8 = g_Cfg.LogUtf8 ? 1 : 0;

    g_Cfg.ShowWindow    = g_Cfg.ShowWindow    ? 1 : 0;
    g_Cfg.SysUseMyColor = g_Cfg.SysUseMyColor ? 1 : 0;
    g_Cfg.FilterSilent = g_Cfg.FilterSilent ? 1 : 0;
    g_Cfg.HintArea     = g_Cfg.HintArea     ? 1 : 0;
    if (g_Cfg.MaxLines     < 1)   g_Cfg.MaxLines     = 1;
    if (g_Cfg.MaxLines     > 40)  g_Cfg.MaxLines     = 40;
    if (g_Cfg.BoardOpacity < 0)   g_Cfg.BoardOpacity = 0;
    if (g_Cfg.BoardOpacity > 100) g_Cfg.BoardOpacity = 100;
    if (g_Cfg.HintGap      < 0)   g_Cfg.HintGap      = 0;
    if (g_Cfg.HintGap      > 200) g_Cfg.HintGap      = 200;
    if (g_Cfg.HintLines    < 1)   g_Cfg.HintLines    = 1;
    if (g_Cfg.HintLines    > 20)  g_Cfg.HintLines    = 20;
    if (g_Cfg.HintOpacity  < 0)   g_Cfg.HintOpacity  = 0;
    if (g_Cfg.HintOpacity  > 100) g_Cfg.HintOpacity  = 100;
    if (g_Cfg.MaxWidth < 0)       g_Cfg.MaxWidth     = 0;
    if (g_Cfg.TopGapLines < 0)    g_Cfg.TopGapLines  = 0;
    if (g_Cfg.TopGapLines > 10)   g_Cfg.TopGapLines  = 10;
    if (g_Cfg.KeepLast < 0)       g_Cfg.KeepLast     = 0;
    if (g_Cfg.KeepLast > MAX_KEEP_LAST) g_Cfg.KeepLast = MAX_KEEP_LAST;
    if (g_Cfg.MaxHistory < 10)    g_Cfg.MaxHistory   = 10;
    if (g_Cfg.MaxHistory > MAX_HISTORY_LIMIT) g_Cfg.MaxHistory = MAX_HISTORY_LIMIT;
    if (g_Cfg.ExpandLines < 2)    g_Cfg.ExpandLines  = 2;
    if (g_Cfg.ExpandLines > 200)  g_Cfg.ExpandLines  = 200;
}

/* ==================== 惰性初始化 ==================== */

static int g_InitDone = 0;

static void EnsureInit(void)
{
    if (g_InitDone) return;
    g_InitDone = 1;

    BuildPaths();
    LoadConfig();
    /* 日志故意不在这里打开 —— 留到第一条消息到达时再建,
     * 免得"加载了 DLL 但没产生消息"的进程留下一份空日志。 */
}

/* ==================== 消息存储（阶段 2）====================
 *
 * 固定大小的环形缓冲, 不做动态分配:
 *   - 内存历史本来就可以有上限(用户已确认), 200 条 × (512+64) 宽字符 ≈ 230KB
 *   - 无 CRT 环境下自己管内存容易出错, 定长数组最稳
 *   - 操作提示消息(silent=1)默认根本不进这里, 所以 200 条实际很够用
 */

#define MAX_MSGS    500    /* 内存历史容量 —— 必须 >= MAX_HISTORY_LIMIT 才能翻满 */
#define MAX_TEXT_W  512
#define MAX_NAME_W  64

/* "名字: 内容" 拼好后的最大长度 */
#define MAX_DISPLAY_W (MAX_NAME_W + MAX_TEXT_W + 4)

typedef struct {
    wchar_t  name[MAX_NAME_W];        /* 来源(仅玩家聊天非空) —— 着色判断用 */
    wchar_t  display[MAX_DISPLAY_W];  /* 拼好的显示文本: "名字: 内容" 或纯正文 */
    int      color;              /* ColorSchemeIdx -> 玩家颜色 */
    unsigned retAddr;            /* 调用者返回地址(分类用) */
    unsigned frame;              /* 收到时的 g_Tick(单调帧计数, 见下) */
    int      timeout;            /* 超时帧数; -1 = 永不消失 */
    int      silent;             /* 1 = 操作提示 */
    int      clipped;            /* 1 = 正文过长被截断过 */
} ChatMsg;

static ChatMsg g_Msgs[MAX_MSGS];
static int     g_MsgHead  = 0;   /* 下一条要写入的槽位 */
static int     g_MsgCount = 0;   /* 当前有效条数 */

/* ==================== 单调递增的"逻辑帧"计数 ====================
 *
 * 超时判断需要一个【只会前进】的时间基准, 而引擎的 CurrentFrame (0xA8ED84)
 * 会【回退】。
 *
 * 实测 (2026-09-21 日志): 读档会把 CurrentFrame 拉回存档时的值。于是每次读档
 * 之后收到的消息, 记下的 m->frame 都是"回退后的小值 + 一点增量", 而当前 frame
 * 也在同一量级 —— 两者之差始终很小, 消息【永远不超时】。
 * 玩家看到的现象就是: 读档两三次后, 那几条任务目标重复出现好几遍, 而且死活
 * 不消失(折叠态一直显示, 展开态反而正常)。
 *
 * 更早用 break 而非 continue 时, 读档还会让"读档前"的消息因无符号下溢被判定
 * 为已超时 —— 同一个根因的另一种表现。
 *
 * 所以这里自己维护一个只增不减的 g_Tick: 每个逻辑帧加上 CurrentFrame 的
 * 【增量】。增量若因回退/跳变而算出天文数字, 直接丢弃(当 0 处理) ——
 * 于是读档最多让时间"卡一下", 绝不会倒退。
 *
 * 消息的年龄 = g_Tick - m->frame, 两个量都单调, 差值因此永远有意义。
 */

/* 单次增量超过它就认定 CurrentFrame 跳变了(读档/切场景)。
 * 依据: CurrentFrame 是【逻辑帧号】, 每逻辑帧 +1; 正常帧率 15~60 fps,
 * 本钩子每秒被调用约 600 次 ⇒ 正常情况下每次看到的增量就是 1,
 * 最多因卡顿跳几帧。放大到 10 万帧(约 1 小时)仍远小于"读档回退"
 * 造成的那种 40 亿级下溢, 所以这个阈值既不会误杀、也不会漏判。 */
#define TICK_MAX_DELTA  100000u

static unsigned g_Tick      = 0;   /* 单调递增的逻辑帧计数 */
static unsigned g_LastFrame = 0;   /* 上一次看到的 CurrentFrame 原值 */

/* i = 0 表示最旧的一条 */
static ChatMsg* MsgAt(int i)
{
    int idx = g_MsgHead - g_MsgCount + i;
    while (idx < 0) idx += MAX_MSGS;
    return &g_Msgs[idx % MAX_MSGS];
}

/* ==================== 操作提示区(快捷键/路径点提示) ====================
 *
 * 这些消息(silent=1)的特点是【又频繁又啰嗦】—— 实测一局里"选取部队横越地图"
 * 这类提示就有 20 多条。放进主历史会把真正要看的聊天和剧情冲掉, 所以默认
 * 不进主历史。
 *
 * 但它们也不是全无用处(路径点模式、通信信标之类确实需要看一眼), 所以给它们
 * 一个【独立的小缓冲】, 显示在主消息框正下方的一块独立区域里:
 *
 *   - 与主历史完全隔离: 不占 MaxHistory、不影响行号、不参与滚动
 *   - 每条各带自己的超时, 到点自然消失 —— 不需要用户手动清
 *   - 主框底边在哪, 它就跟到哪(见 g_BoxBottom), 两者间距固定
 *
 * 用独立的环形缓冲而不是复用 g_Msgs, 是因为两者语义差别很大:
 * 主历史要能翻、要能展开、要"永不过期"; 提示区只要求"最新的几条、会自己消失"。
 * 硬塞进一个数组只会把两边的逻辑都搞复杂。 */
#define MAX_HINTS    24    /* 提示缓冲容量(环形, 超出覆盖最旧的) */
#define MAX_HINT_W   256   /* 单条提示最多存这么多字符 */

typedef struct {
    wchar_t  text[MAX_HINT_W];
    unsigned frame;        /* 收入时的 g_Tick */
    int      timeout;      /* 超时帧数; <= 0 当作不自动消失 */
    int      color;        /* ColorScheme 索引(取色与主框一致) */
} HintMsg;

static HintMsg g_Hints[MAX_HINTS];
static int     g_HintHead  = 0;   /* 下一条要写入的槽位 */
static int     g_HintCount = 0;   /* 当前有效条数 */

/* i = 0 表示最旧的一条 */
static HintMsg* HintAt(int i)
{
    int idx = g_HintHead - g_HintCount + i;
    while (idx < 0) idx += MAX_HINTS;
    return &g_Hints[idx % MAX_HINTS];
}

/* 消息框底边(提示区跟着它走) + 本帧字高与框宽(三个绘制函数共用) */
static int g_BoxBottom = 0;
static int g_LineH     = 0;
static int g_BoxWidth  = 0;

/* 拷贝宽字符串; 超长则截断并补 "..." , 让"被截断"一眼可见。
 * 返回 1 表示发生了截断。
 *
 * 用 ASCII 的 "..." 而不是 U+2026 "…": 引擎的 BitFont 未必收录那个码位,
 * 显示成方块反而更糟。 */
static int CopyW(wchar_t* dst, const wchar_t* src, int cap)
{
    int i = 0;
    if (cap <= 0) return 0;
    if (!src || !IsPlausiblePtr(src)) { dst[0] = 0; return 0; }

    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }

    if (src[i] && cap >= 4)          /* 源串还有剩余 -> 被截断了 */
    {
        if (i > cap - 4) i = cap - 4;
        dst[i++] = L'.'; dst[i++] = L'.'; dst[i++] = L'.';
        dst[i] = 0;
        return 1;
    }
    dst[i] = 0;
    return 0;
}

static void MsgPush(const wchar_t* name, const wchar_t* text, int color,
                    unsigned retAddr, unsigned frame, int timeout, int silent)
{
    ChatMsg* m = &g_Msgs[g_MsgHead];
    int p = 0, q = 0;

    CopyW(m->name, name, MAX_NAME_W);

    /* 在这里就拼好显示文本(而不是等到绘制时):
     * 每条消息只存一份文本, 绘制时无需大缓冲, 历史条数才能放大。 */
    if (m->name[0])
    {
        while (m->name[q] && p < MAX_NAME_W - 2) { m->display[p++] = m->name[q++]; }
        m->display[p++] = L':';
        m->display[p++] = L' ';
    }
    m->clipped = CopyW(m->display + p, text, MAX_DISPLAY_W - p);

    m->color   = color;
    m->retAddr = retAddr;
    m->frame   = frame;
    m->timeout = timeout;
    m->silent  = silent;

    g_MsgHead = (g_MsgHead + 1) % MAX_MSGS;
    if (g_MsgCount < MAX_MSGS) g_MsgCount++;
}

/* 收一条操作提示进提示区。
 * 与 MsgPush 的区别: 不拼来源名(这类消息实测 name 恒为空)、不计入主历史。 */
static void HintPush(const wchar_t* text, int color, int timeout)
{
    HintMsg* h = &g_Hints[g_HintHead];

    CopyW(h->text, text, MAX_HINT_W);
    h->frame   = g_Tick;
    h->timeout = timeout;
    h->color   = color;

    g_HintHead = (g_HintHead + 1) % MAX_HINTS;
    if (g_HintCount < MAX_HINTS) g_HintCount++;
}

/* ==================== 颜色 ==================== */

/* DynamicVectorClass<ColorScheme*> 布局与 AutoKit 实测的容器一致:
 *   +0x04 Items   +0x10 Count                                            */
#define OFF_CSCHEME_ITEMS   0x04
#define OFF_CSCHEME_COUNT   0x10

/* 取"颜色方案对象"的指针。
 *
 * 原版绘制要的就是这个指针 —— 引擎自己会从中取出正确的显示色
 * (内部处理了 BaseColor -> 显示色 的换算与明暗档位), 我们不需要、
 * 也不应该自己去算 RGB。
 *
 * ColorScheme::Array 是 DynamicVectorClass<ColorScheme*>,
 * Items 在 +0x04 (实测: 原版 0x5D4A47 处读的正是 0xB054D4 = 0xB054D0+4)。 */
static void* SchemePtr(int cs)
{
    void** items;
    int    count;

    if (cs < 0) return NULL;

    items = *(void***)(ADDR_COLORSCHEME_ARRAY + OFF_CSCHEME_ITEMS);
    count = *(int*)(ADDR_COLORSCHEME_ARRAY + OFF_CSCHEME_COUNT);
    if (!items || count <= 0 || count > 1000 || cs >= count) return NULL;

    return items[cs];
}

/* Fancy_Text_Print_Wide 的 ColorScheme 重载 (cdecl, 8 个栈参数) */
typedef Point2D* (__cdecl *FancyTextCSFn)(
    Point2D* retBuffer, const wchar_t* text, void* surface,
    RectangleStruct* bounds, Point2D* location,
    void* foreScheme, void* backScheme, int flags);

/* ==================== 折行（阶段 2b）====================
 *
 * 需求(用户第 5 点): 无换行的超长文本, 在消息框内到达宽度上限也要自动换行。
 *
 * 用引擎自己的 BitFont 折行函数算"这段文本在给定像素宽度下能放几个字符":
 *   int __thiscall 0x433F50(BitFont* this, const wchar_t* text, int width, int, int)
 * 实测: 入口 `mov %ecx,%edx`(用 ECX 传 this), 结尾 `ret $0x10`(4 个栈参数)。
 * 这正是原版 MessageListClass::Manage 折行时用的同一个函数。
 *
 * 硬换行(文本里自带的 \n)会优先断开 —— 与原版"带换行的消息按原样换行"一致。
 */

#define MAX_LPP             8    /* 单条消息最多折成几行(超出部分截断) */

typedef struct {
    const wchar_t* start;
    int            len;
} TextLine;

static TextLine g_LineBuf[MAX_HISTORY_LIMIT][MAX_LPP];
static int      g_LineCnt[MAX_HISTORY_LIMIT];
static int      g_Picked[MAX_HISTORY_LIMIT];   /* 本轮要显示的消息下标, [0] = 最新 */
static int      g_RowBase[MAX_HISTORY_LIMIT];  /* 每条消息第一行的行号 */

/* 注: 这里曾有一个 g_Compose[MAX_PICKED][...] 拼接缓冲(100 条约 580KB)。
 * 现在"名字: 内容"的拼接提前到了 MsgPush, 存在 ChatMsg::display 里,
 * 绘制时直接取用 —— 既少了 580KB, 也让历史条数可以放心放大。 */

/* 引擎折行函数: __fastcall 表达 this(thiscall), 第 2 个参数是占位 EDX */
typedef int (__fastcall *TextFitFn)(void* font, void* edxUnused,
                                    const wchar_t* text, int width, int a4, int a5);

static int TextCharsThatFit(const wchar_t* text, int width)
{
    void*     font = *(void**)ADDR_BITFONT_INSTANCE;
    TextFitFn fn   = (TextFitFn)ADDR_TEXT_CHARS_FIT;
    int       r;
    if (!font || !fn || width <= 0) return 0;
    r = fn(font, NULL, text, width, 111, 1);
    return r;
}

/*
 * 把 text 按宽度(并尊重硬换行)切成若干行, 结果写进 out, 返回行数。
 * 每行只是原缓冲区里的一个 [start, start+len) 片段 —— 不复制字符串,
 * 绘制时按 len 逐字符画即可。
 */
static int WrapText(const wchar_t* text, int width, TextLine* out, int maxOut)
{
    int n = 0;
    const wchar_t* p;

    if (!text || !*text || maxOut <= 0) return 0;
    p = text;

    while (*p && n < maxOut)
    {
        int hardLen = 0;   /* 到本段硬换行/结尾的长度 */
        int fit;

        while (p[hardLen] && p[hardLen] != L'\n') hardLen++;

        fit = TextCharsThatFit(p, width);
        if (fit <= 0) fit = 1;                  /* 宽到放不下一个字符: 至少前进, 防死循环 */
        if (fit > hardLen) fit = hardLen;       /* 不跨过硬换行 */

        out[n].start = p;
        out[n].len   = fit;
        n++;

        p += fit;
        if (*p == L'\n') p++;                   /* 吃掉换行符 */
    }
    return n;
}

/* ==================== 交互（阶段 3a：键盘）====================
 *
 * 暂不继承引擎的 GadgetClass —— 消息框的交互需求其实很窄:
 *   · 切换展开/收起
 *   · 滚动
 *   · 命中判定 = 一个"点在矩形内"的判断
 * 走 GadgetClass 要额外处理 C++ 对象构造、虚表、GameAllocator、生命周期,
 * 风险与收益不成比例。自己做输入反而更可控。
 *
 * 鼠标部分(点击/滚轮/吞掉穿透点击)留到阶段 3b —— 那才是真正需要小心的地方:
 * 点击若不被吞掉, 会穿透到战场去选中/命令底下的单位。
 *
 * 这里只读按键, 不改游戏任何状态。
 */

static int g_Expanded = 0;   /* 0 = 折叠态(只显示最新几行), 1 = 展开态(可翻历史) */
static int g_Scroll   = 0;   /* 展开态的滚动量: 0 = 看到最新, 越大越往历史翻 */

/* ==================== 一键清空 (Ctrl+L) ====================
 *
 * 只清【内存里的历史缓冲】, 日志文件一个字都不动 —— 已经落盘的东西照旧可查。
 *
 * 清空后消息框会消失(挑选结果为 0), 新消息来了自然重新出现。这是预期的:
 * 按这个键就是要立刻把屏幕清干净。
 *
 * 两条"看起来会出事、其实不会"的地方, 记下来免得后人担心:
 *   - 热键不会跟着失灵 —— 输入处理在帧钩子(0x55D360)上, 与"有没有消息"无关
 *     (这正是 0.4.3 修过的那个坑)。
 *   - 不会留下一个看不见却还在吞点击的空框 —— 命中判定靠 g_BoxFrame 帧号保鲜,
 *     框不再绘制后会自动失效。
 */
static void ClearHistory(void)
{
    g_MsgHead   = 0;
    g_MsgCount  = 0;
    g_HintHead  = 0;
    g_HintCount = 0;
    g_Scroll    = 0;
    g_Expanded  = 0;

    /* 不受 LogRaw 控制: 这是用户主动操作的痕迹, 一行就是一行 */
    LogLine("ui: history cleared (Ctrl+L)");
}

/* 前向声明: PollKeys 要用, 定义在同组函数的后面 */
static int ScrollStep(void);

static int KeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

/* 只在"按下那一瞬间"触发一次(上升沿) */
static int KeyPressedEdge(int vk, unsigned char* pPrev)
{
    unsigned char now = (unsigned char)KeyDown(vk);
    int r = (now && !*pPrev);
    *pPrev = now;
    return r;
}

static void PollKeys(void)
{
    static unsigned char s_pUp = 0, s_pDown = 0;
    static unsigned char s_pPgUp = 0, s_pPgDn = 0, s_pHome = 0, s_pEnd = 0;

    if (!g_Cfg.Enable) return;

    /* 诊断(LogRaw=1): 记录 Ctrl / M 的按键状态变化。
     * 排查"Ctrl+M 没反应"用 —— 能看出是完全检测不到按键,
     * 还是检测到了但切换逻辑没走。 */
    if (g_Cfg.LogRaw)
    {
        static unsigned char s_lastCtrl = 0, s_lastM = 0;
        unsigned char c = (unsigned char)KeyDown(VK_CONTROL);
        unsigned char m = (unsigned char)KeyDown('M');
        if (c != s_lastCtrl || m != s_lastM)
        {
            char b1[4], b2[4], buf[64];
            unsigned q = 0;
            s_lastCtrl = c; s_lastM = m;
            b1[0] = c ? '1' : '0'; b1[1] = 0;
            b2[0] = m ? '1' : '0'; b2[1] = 0;
            q = AppendCStr(buf, q, "key: ctrl=", sizeof(buf));
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " m=", sizeof(buf));
            q = AppendCStr(buf, q, b2, sizeof(buf));
            LogLine(buf);
        }
    }

    /* Ctrl + M : 切换展开 / 收起 */
    {
        static unsigned char s_pCtrlM = 0;
        unsigned char now = (unsigned char)(KeyDown(VK_CONTROL) && KeyDown('M'));
        if (now && !s_pCtrlM)
        {
            g_Expanded = !g_Expanded;
            g_Scroll   = 0;                 /* 每次切换都回到最新 */
            /* 不受 LogRaw 控制: 这是用户主动操作的痕迹, 一行就是一行,
             * 不会刷屏 —— 而且"按了没反应"时, 有没有这行是最关键的证据。 */
            LogLine(g_Expanded ? "ui: expanded (Ctrl+M)" : "ui: collapsed (Ctrl+M)");
        }
        s_pCtrlM = now;
    }

    /* Ctrl + L : 清空内存历史与提示区(日志文件不受影响) */
    {
        static unsigned char s_pCtrlL = 0;
        unsigned char now = (unsigned char)(KeyDown(VK_CONTROL) && KeyDown('L'));
        if (now && !s_pCtrlL) ClearHistory();
        s_pCtrlL = now;
    }

    if (!g_Expanded) return;

    if (KeyPressedEdge(VK_UP,   &s_pUp))   g_Scroll++;
    if (KeyPressedEdge(VK_DOWN, &s_pDown)) { if (g_Scroll > 0) g_Scroll--; }

    if (KeyPressedEdge(VK_PRIOR, &s_pPgUp))
        g_Scroll += ScrollStep();                         /* PageUp: 往历史翻一屏 */
    if (KeyPressedEdge(VK_NEXT,  &s_pPgDn))
    {
        g_Scroll -= ScrollStep();
        if (g_Scroll < 0) g_Scroll = 0;
    }

    if (KeyPressedEdge(VK_HOME, &s_pHome)) g_Scroll = 99999;   /* 到最旧 */
    if (KeyPressedEdge(VK_END,  &s_pEnd))  g_Scroll = 0;       /* 到最新 */

    /* 上界夹紧放到绘制时按实际行数做, 因为这里不知道总共有多少行 */
}

/* ==================== 鼠标交互（阶段 3b）====================
 *
 * 三个钩子共同保证"点消息框不会穿透到战场":
 *   0x692419  命中消息框 -> 跳过点击坐标处理   (点击不被战场接收)
 *   0x533F50  悬停消息框 -> 跳过侧栏滚动       (滚轮只滚消息)
 *   0x777998  读滚轮方向 -> 自己滚动消息
 * 另加左键点击框内 = 切换展开/收起(在 PollMouse 里轮询)。
 *
 * 这三个钩子都【只读】输入、只在命中消息框时改变自身状态, 不碰游戏对象。
 */

static RectangleStruct g_BoxRect;    /* 上一次画出的消息框矩形(命中判定用) */
static unsigned        g_BoxFrame = 0; /* 画它时的帧号 —— 用于"保鲜" */

/*
 * 鼠标是否落在消息框上。
 *
 * ⚠️ 必须按帧号保鲜, 不能只看"画过没有"。
 *
 * DrawMessages 只在钩子被调用时执行, 而钩子只在原版消息链表非空时才被调用;
 * 一旦某轮它提前 return, 那个"画过"的标志就会永远停在 1、矩形停在最后一次的
 * 位置 —— 结果那块区域【永久吞点击】, 表现为"消息框已经没了, 但左上角一片
 * 区域再也选不中单位"。(实测反馈过的现象。)
 *
 * 所以只有"最近 2 帧内画过"才算命中, 过期自动失效。
 */
#define BOX_HIT_FRESH_FRAMES 2

static int MouseInBox(void)
{
    void*    m;
    int      mx, my;
    unsigned now;

    if (!g_BoxFrame) return 0;

    now = g_Tick;   /* 用单调帧计数: CurrentFrame 会因读档回退, 会让保鲜判定错乱 */
    if (now - g_BoxFrame > BOX_HIT_FRESH_FRAMES) return 0;   /* 框已经不在画了 */

    m = *(void**)ADDR_WWMOUSE_INSTANCE;
    if (!m) return 0;

    mx = *(int*)((char*)m + OFF_WWMOUSE_XY1 + 0);
    my = *(int*)((char*)m + OFF_WWMOUSE_XY1 + 4);

    return (mx >= g_BoxRect.X && mx < g_BoxRect.X + g_BoxRect.Width &&
            my >= g_BoxRect.Y && my < g_BoxRect.Y + g_BoxRect.Height);
}

/* 翻页 / 滚轮一次的步长 = ScrollStep 行(默认 5)。
 *
 * 这个值走过两个极端, 最后定在 5:
 *   · 固定 MaxLines(8) —— 展开态一屏 15 行, 一次翻不到一屏
 *   · 跟随可见行数(15) —— 一次翻一整屏, 但"瞬间翻页没有中间过程",
 *     容易让人怀疑自己是不是翻过头了
 * 5 行的好处是内容能看到连续移动, 又不会翻得太慢。
 */
static int ScrollStep(void)
{
    int n = g_Cfg.ScrollStep;
    if (n < 1)  n = 1;
    if (n > 50) n = 50;
    return n;
}

/* 滚轮滚动。dir > 0 = 往历史翻(上), dir < 0 = 往新消息(下) */
static void ScrollBy(int dir)
{
    int lines = ScrollStep();

    if (dir > 0)
    {
        if (!g_Expanded) g_Expanded = 1;   /* 往上滚自动展开, 符合聊天框直觉 */
        g_Scroll += lines;
    }
    else
    {
        g_Scroll -= lines;
        if (g_Scroll < 0) g_Scroll = 0;
    }
}

/* 左键点击框内 -> 切换展开/收起; 展开时点框外 -> 收起 */
static void PollMouse(void)
{
    static unsigned char s_pLMB = 0;
    unsigned char now;

    if (!g_Cfg.Enable) return;

    now = (unsigned char)((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0);
    if (now && !s_pLMB)
    {
        if (MouseInBox())
        {
            g_Expanded = !g_Expanded;
            g_Scroll   = 0;
            LogLine(g_Expanded ? "ui: expanded (click)" : "ui: collapsed (click)");
        }
        else if (g_Expanded)
        {
            g_Expanded = 0;
            g_Scroll   = 0;
            LogLine("ui: collapsed (click outside)");
        }
    }
    s_pLMB = now;
}

/* ---- 钩子 3: 滚轮方向 (0x777998) ----
 *
 * 进入时 ECX = 【原始 WParam】。注意: 0x777998 处那条 `shr $0x10,%ecx`
 * 正是被本钩子覆盖掉的原指令, 所以此刻 ECX 还没被移位。
 * WM_MOUSEWHEEL 的 HIWORD(WParam) 是【有符号】的:
 *   bit31 = 0 -> 向前/向上滚 ; bit31 = 1 -> 向后/向下滚
 * 也就是要判 bit31, 不是移位后的 bit15。
 *
 * ⚠️ 曾经写成 `wparam & 0x8000u`(移位后的位), 结果该位恒为 0 ——
 *    无论往哪滚都走"向上"分支, 表现为"向上滚正常、向下滚没反应"。
 *    Phobos 的参考实现用的是 `WParam & 0x80000000u`, 照抄即可, 不要自己推。
 *
 * 返回 0 让原版继续 —— 它会去滚侧栏, 由下一个钩子按命中情况拦下。 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_ScrollWheelHook(void* regs)
{
    unsigned wparam;

    EnsureInit();
    if (!g_Cfg.Enable || !g_Cfg.ShowWindow) return 0;

    wparam = *(unsigned*)((char*)regs + OFF_REG_ECX);

    if (MouseInBox())
        ScrollBy((wparam & 0x80000000u) ? -1 : +1);

    return 0;
}

/* ---- 钩子 4: 跳过侧栏滚动 (0x533F50) ---- */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_ScrollSidebarHook(void* regs)
{
    (void)regs;
    EnsureInit();

    if (g_Cfg.Enable && g_Cfg.ShowWindow && MouseInBox())
        return ADDR_SKIP_SCROLL_SIDEBAR;   /* 滚轮不要同时滚侧栏 */

    return 0;
}

/* ---- 钩子 5: 吞掉穿透到战场的点击 (0x692419) ---- */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_ClickCoordsHook(void* regs)
{
    (void)regs;
    EnsureInit();

    if (g_Cfg.Enable && g_Cfg.ShowWindow && MouseInBox())
        return ADDR_CLICK_DONOTHING;       /* 这次点击由消息框消费 */

    return 0;
}

/* 当前玩家(我)的颜色方案索引; 取不到返回 -1(调用方会退回白色) */
static int CurrentPlayerScheme(void)
{
    void* p = *(void**)ADDR_CURRENT_PLAYER;
    if (!p) return -1;
    return *(int*)((char*)p + OFF_HOUSE_COLORSCHEME);
}

/* ==================== 绘制（阶段 2）==================== */

/* Simple_Text_Print_Wide @ 0x4A5EB0 的函数指针类型
 *
 * ⚠️ 这里是第二个调用约定坑: 它【不是】stdcall, 而是 __fastcall ——
 *   反汇编 0x4A5EC2 有 `mov %ecx,0x14(%esp)`, 说明第一个参数走 ECX;
 *   函数结尾 `ret $0x1C` 清理 7 个栈参数, 加上 ECX/EDX 恰好 9 个,
 *   与 YRpp 的声明 (Point2D*, wchar_t*, Surface*, RectangleStruct*,
 *   Point2D*, unsigned, unsigned, TextPrintType, int) 一一对应。
 *   (YRpp 里写成 JMP_STD 有误导性, 它标的是 __fastcall。)
 *
 * 参数分配: ECX=pRetVal, EDX=pText, 其余 7 个走栈, 被调方清栈。 */
typedef Point2D* (__fastcall *SimpleTextPrintFn)(
    Point2D* pRetVal, const wchar_t* pText, void* pSurface,
    RectangleStruct* pBounds, Point2D* pLocation,
    unsigned nForeColor, unsigned nBackColor, int nFlags, int nUnused);

#define TEXT_FLAGS      0x4046   /* 与实测日志里所有消息的 style 一致 */
#define FONT_OFF_HEIGHT 0x1C     /* BitFont::field_1C = 字高(Phobos 同用法) */
#define BAR_WIDTH       4        /* 滚动条宽度(像素) */

/* 用引擎的 Surface::FillRectTrans 画半透明底板 (虚表偏移见 offsets.h)
 *
 * ⚠️ 调用约定是这个项目踩过的第一个大坑:
 *   FillRectTrans 是【成员函数】, this 必须在 ECX 里。
 *   最初写成 __stdcall(把 this 当第一个"栈"参数传), 结果 ECX 保持垃圾值,
 *   游戏在 0x4BB83D 处 `mov (%edi),%eax` 崩溃 —— 崩溃报告里 ECX=1、
 *   "tried to read from 0x00000001", 与垃圾 this 完全吻合。
 *
 *   改用 __fastcall 表达: 前两个参数走 ECX/EDX, 其余走栈,
 *   且栈由被调方清理(与成员函数一致)。第二个参数是占位的 EDX。
 *
 * 反向印证: 0x4BB830 结尾是 `ret $0xC`(清 12 字节 = 3 个栈参数),
 * 与 FillRectTrans 的 (RectangleStruct*, ColorStruct*, int) 完全一致。 */
static void FillRectTransVt(void* surf, RectangleStruct* pRect, ColorStruct* pColor, int opacity)
{
    unsigned char* vt;
    typedef int (__fastcall *Fn)(void*, void*, RectangleStruct*, ColorStruct*, int);
    Fn fn;

    if (!surf) return;
    vt = *(unsigned char**)surf;
    if (!vt) return;
    fn = (Fn)(*(void**)(vt + VT_FILL_RECT_TRANS));
    if (fn) fn(surf, NULL, pRect, pColor, opacity);
}

/* 消息框宽度: MaxWidth>0 时直接用它, 否则取屏幕宽度的一半(上限 520)。
 * 夹紧规则: 至少 80 像素, 且不超出屏幕右侧。
 *
 * 抽成函数是因为【提示区要跟主框一样宽】—— 两处各算一遍迟早会不一致。 */
static int CalcBoxWidth(int x)
{
    RectangleStruct* vb = (RectangleStruct*)ADDR_DSURFACE_VIEWBOUNDS;
    int screenW = (vb && vb->Width > 200) ? vb->Width : 640;
    int maxW    = 420;

    if (g_Cfg.MaxWidth > 0)
        maxW = g_Cfg.MaxWidth;
    else
    {
        maxW = screenW / 2;
        if (maxW > 520) maxW = 520;
    }

    if (maxW < 80) maxW = 80;
    if (maxW > screenW - x - 8) maxW = screenW - x - 8;
    if (maxW < 80) maxW = 80;

    return maxW;
}

static void DrawMessages(void)
{
    void*    surf;
    unsigned frame;
    int      i, k, r, drawn = 0;
    int      picked = 0, totalAll = 0, totalRows = 0, rowCursor;
    int      rowBegin = 0, rowEnd = 0, limit = 0, skip = 0;
    int      x, y, lineH, maxW, textW;
    RectangleStruct rect;
    ColorStruct     board;
    Point2D  loc, ret;
    FancyTextCSFn fnCS;
    unsigned char* pFont;

    if (!g_Cfg.ShowWindow) return;

    /* 两处"本该在这里、但已经搬走/删掉"的代码, 留个记号免得被好心加回来:
     *   - 键盘/鼠标交互 -> 已移到帧钩子 ChatBox_FrameHook(每帧必调)。
     *     本函数所在的钩子在原版消息链表为空时不会执行, 放这儿会一起失灵。
     *   - 命中标志的复位   -> 已由 MouseInBox() 按 g_BoxFrame 帧号保鲜取代,
     *     不再需要每帧手动清零。 */

    surf = *(void**)ADDR_DSURFACE_COMPOSITE;

    /* 诊断(LogRaw=1): 记录进入绘制时的关键状态, 变化才记一行。
     * 用于定位"消息框整个不出现"这类问题 —— 能看出是卡在
     * 没有消息(msgs=0)、没有画布(surf=NULL), 还是消息被全部过滤。 */
    if (g_Cfg.LogRaw)
    {
        static int s_lastMsgs = -1, s_lastSurf = -1;
        int curSurf = surf ? 1 : 0;
        if (g_MsgCount != s_lastMsgs || curSurf != s_lastSurf)
        {
            char b1[16], b2[16], b3[16], b4[16], buf[220];
            unsigned q = 0;
            s_lastMsgs = g_MsgCount; s_lastSurf = curSurf;
            UtoA((unsigned)g_MsgCount, b1);
            UtoA((unsigned)g_Cfg.KeepLast, b2);
            UtoA((unsigned)g_Cfg.MaxHistory, b3);
            UtoA((unsigned)g_Cfg.MaxLines, b4);
            q = AppendCStr(buf, q, "draw-state: msgs=", sizeof(buf));
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " surf=", sizeof(buf));
            q = AppendCStr(buf, q, curSurf ? "OK" : "NULL", sizeof(buf));
            q = AppendCStr(buf, q, " keepLast=", sizeof(buf));
            q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " maxHist=", sizeof(buf));
            q = AppendCStr(buf, q, b3, sizeof(buf));
            q = AppendCStr(buf, q, " maxLines=", sizeof(buf));
            q = AppendCStr(buf, q, b4, sizeof(buf));
            LogLine(buf);
        }
    }

    if (!surf) return;

    pFont = *(unsigned char**)ADDR_BITFONT_INSTANCE;
    lineH = pFont ? *(int*)(pFont + FONT_OFF_HEIGHT) : 14;
    if (lineH < 6 || lineH > 64) lineH = 14;

    /* 把字高与框宽记成全局 —— 提示区要用同一套度量, 否则两块框对不齐 */
    g_LineH    = lineH;
    g_BoxWidth = CalcBoxWidth(g_Cfg.PosX);

    /* 提示区跟随用的基准, 先按"消息框 0 行高"记。
     * 真正画完后再更新成实际底边(见函数末尾) —— 这样即使本轮一条消息都没有
     * (函数提前返回), 提示区也贴在"消息框本该在的位置", 不会乱跳。 */
    g_BoxBottom = g_Cfg.PosY + g_Cfg.TopGapLines * lineH + 2;

    /* 首次绘制时记录字体度量与宽度配置 —— 纯诊断信息, 只在 LogRaw=1 时输出 */
    if (g_Cfg.LogRaw)
    {
        static int s_metricLogged = 0;
        if (!s_metricLogged)
        {
            char b1[16], b2[16], buf[160];
            unsigned q = 0;
            s_metricLogged = 1;
            UtoA((unsigned)lineH, b1);
            UtoA((unsigned)(pFont ? *(int*)(pFont + FONT_OFF_HEIGHT) : 0xFFFFFFFFu), b2);
            q = AppendCStr(buf, q, "draw: lineH=", sizeof(buf));
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " (BitFont+0x1C=", sizeof(buf));
            q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, ")", sizeof(buf));
            LogLine(buf);
        }
    }

    if (g_Cfg.LogRaw)
    {
        static int s_widthLogged = 0;
        if (!s_widthLogged)
        {
            s_widthLogged = 1;
            /* maxW 在下面才算出来, 所以这里只记配置值 */
            char b1[16], buf[120];
            unsigned q = 0;
            UtoA((unsigned)g_Cfg.MaxWidth, b1);
            q = AppendCStr(buf, q, "draw: cfg MaxWidth=", sizeof(buf));
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " (0 = auto)", sizeof(buf));
            LogLine(buf);
        }
    }

    /* 用单调帧计数而不是 CurrentFrame —— 后者会因读档回退(见 g_Tick 说明) */
    frame = g_Tick;

    /* --- ① 挑选要进入排版的消息(从最新往回) ---
     * 折叠态: 消息超时就跳过 —— 效果接近原版"显示一段时间后自动消失"。
     * 展开态: 【忽略超时】, 显示完整历史 —— 否则翻历史时旧消息会自己消失。
     * 注意内存历史本身从不删除, 只是"显示/不显示"的区别。 */
    for (i = g_MsgCount - 1; i >= 0 && picked < g_Cfg.MaxHistory; i--)
    {
        ChatMsg* m = MsgAt(i);

        /* 最新的 KeepLast 条【永远显示】, 即使已经超时。
         *
         * 为什么必须这样: 否则所有临时消息过期后 picked 会变成 0,
         * 函数直接 return、什么都不画 —— 看起来就像"消息框消失了,
         * 按 Ctrl+M 也唤不出来"(其实 g_Expanded 已经切换了, 只是没东西可画,
         * 要等下一条消息到达才显现)。
         *
         * 判定写成 "picked >= KeepLast" 而不是"比较序号": 因为我们是从最新
         * 往旧遍历的, 前 KeepLast 条收完之后 picked 就等于 KeepLast,
         * 从第 KeepLast+1 条开始才按超时淘汰。
         *
         * ⚠️ 这里必须是 continue 而不是 break。
         *    一开始想当然写了 break(理由: 消息按时间有序, 老的先超时,
         *    见到超时的就不用再看了)。但 timeout = -1 的消息【永不消失】,
         *    它可能比后面某条已超时的消息更旧 —— 一旦 break, 这条永久消息
         *    就被"更新的、已超时的"那条挡住了, 永远显示不出来。
         *    改成 continue 只是多遍历几条, 代价可忽略(上限 MaxHistory)。
         *    该边界由 test/test_keep.c 覆盖。 */
        if (picked >= g_Cfg.KeepLast && !g_Expanded && m->timeout >= 0 && m->timeout > 0)
        {
            if (frame - m->frame > (unsigned)m->timeout) continue;   /* 跳过这条, 继续看更旧的 */
        }
        g_Picked[picked++] = i;      /* g_Picked[0] 是最新的一条 */
    }
    if (picked <= 0) return;

    x = g_Cfg.PosX;
    /* 顶部按行预留空档: 原版按回车时会在屏幕顶部显示"从【玩家名】："输入框,
     * 并把消息列表下推。消息框也相应下移, 免得盖住输入框。
     * 用"行数"而不是像素, 是因为用户不需要知道字高是多少。 */
    y = g_Cfg.PosY + g_Cfg.TopGapLines * lineH;
    /* 宽度与提示区共用同一份计算(见 CalcBoxWidth) */
    maxW = g_BoxWidth;
    /* 可用文本宽度 = maxW。
     *
     * 底板宽 maxW+6、左内边距 3, 所以文字从 x 起、右边界 x+maxW,
     * 左右各留 3 像素, 对称。
     * ⚠️ 曾经写成 maxW-6(本意"左右各留 3"), 但底板已按 +6 算过、
     *    文字起点又偏移了 3 —— 等于减了两次, 结果右侧空白 9 像素、
     *    比左侧的 3 像素明显宽一截。 */
    textW = maxW;
    if (textW < 40) textW = 40;

    /* --- ② 折行: 先把所有 picked 消息都折好 ---
     * 展开态要能往回翻历史, 所以不能像折叠态那样"凑够行数就停"。 */
    for (k = 0; k < picked; k++)
    {
        ChatMsg* m = MsgAt(g_Picked[k]);
        int      cnt;

        /* 直接折行已经拼好的显示文本 —— 拼接在 MsgPush 里就做完了 */
        cnt = WrapText(m->display, textW, g_LineBuf[k], MAX_LPP);
        if (cnt <= 0) cnt = 1;
        g_LineCnt[k] = cnt;
        totalAll    += cnt;
    }
    if (totalAll <= 0) return;

    /* ★ 向上翻页时冻结自动滚动(用户第 9 点)。
     *
     * g_Scroll 的含义是"从最新一行往回跳过多少行"。新消息到达会让 totalAll
     * 变大, 于是同样的 skip 就指向了别的内容 —— 正在读历史时会被不断顶走:
     *     原 totalAll=100, skip=20 -> 显示 [50,80)
     *     来 2 行 -> totalAll=102 -> 显示 [52,82)   ← 内容被顶走
     * 做法: 正在看历史(skip>0)时, 把新增的行数补偿进 g_Scroll, 使显示区间不变。
     * 看最新时 skip==0, 不补偿, 新消息自然滚动出现。 */
    {
        static int s_prevTotalAll = 0;
        if (g_Expanded && g_Scroll > 0 && totalAll > s_prevTotalAll)
            g_Scroll += (totalAll - s_prevTotalAll);
        s_prevTotalAll = totalAll;
    }

    /* 每条消息的起始行号。
     *
     * ⚠️ 关键点: g_Picked[0] 是【最新】的一条, 而我们要"最新在最下面",
     *    所以最新那条必须拿到【最大】的行号 —— 因此从 totalAll 往回减。
     *
     * 曾经写成从 0 往上加(以为 picked[0] 是最旧), 一个符号错误同时造成三个症状:
     *   · 新消息跑到最上面
     *   · 折叠态"取最新几行"实际取到的是最旧的几行, 新消息永远看不到
     *   · 翻页时底板像窗帘一样伸缩(因为显示区间与滚动方向相反) */
    rowCursor = totalAll;
    for (k = 0; k < picked; k++)
    {
        rowCursor   -= g_LineCnt[k];
        g_RowBase[k] = rowCursor;
    }

    /* --- ③ 按展开/滚动状态决定显示哪一段 ---
     * 最新一行永远在消息框最下面, 所以从"最新"往回跳 skip 行、再取 limit 行。 */
    limit = g_Expanded ? g_Cfg.ExpandLines : g_Cfg.MaxLines;
    skip  = g_Expanded ? g_Scroll          : 0;

    if (limit > totalAll) limit = totalAll;
    if (skip < 0) skip = 0;
    /* 最多只跳到"最旧的一整屏" —— 否则 Home 会只剩一行 */
    if (skip > totalAll - limit) skip = totalAll - limit;

    /* ★ 必须把夹紧后的值【写回】 g_Scroll。
     *
     * 否则"按过头"的余量会积压: 比如上限是 50, 连按 10 次 PageUp 后
     * g_Scroll 变成 80, 多出来的 30 不会消失 —— 这时按 PageDown 只是把 80
     * 减到 72, 仍然被夹在 50, 画面纹丝不动, 必须按够 4 次才能抵消掉那 30。
     * (实测反馈: "PageUp 到顶后, PageDown 要按同样多次数才生效"。)
     *
     * 这也是个通用教训: 带边界的累加量, 夹紧之后一定要落回状态本身,
     * 不能只在消费端夹紧。 */
    if (g_Expanded) g_Scroll = skip;

    rowEnd   = totalAll - skip;
    rowBegin = rowEnd - limit;
    if (rowBegin < 0) rowBegin = 0;
    if (rowEnd <= rowBegin) return;
    totalRows = rowEnd - rowBegin;

    /* 滚动状态诊断(仅 LogRaw=1, 且只在状态变化时记一行)
     * 排查"翻页时某行不动"这类问题用: 能看到 rows 区间是否随 skip 正常移动。 */
    if (g_Cfg.LogRaw)
    {
        static int s_pB = -1, s_pE = -1, s_pT = -1, s_pP = -1;
        if (rowBegin != s_pB || rowEnd != s_pE || totalAll != s_pT || picked != s_pP)
        {
            char b1[16], b2[16], b3[16], b4[16], b5[16], b6[16], buf[240];
            unsigned q = 0;
            s_pB = rowBegin; s_pE = rowEnd; s_pT = totalAll; s_pP = picked;
            UtoA((unsigned)picked,   b1);
            UtoA((unsigned)totalAll, b2);
            UtoA((unsigned)limit,    b3);
            UtoA((unsigned)skip,     b4);
            UtoA((unsigned)rowBegin, b5);
            UtoA((unsigned)rowEnd,   b6);
            q = AppendCStr(buf, q, "scroll: picked=", sizeof(buf));
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " totalAll=", sizeof(buf));
            q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " limit=", sizeof(buf));
            q = AppendCStr(buf, q, b3, sizeof(buf));
            q = AppendCStr(buf, q, " skip=", sizeof(buf));
            q = AppendCStr(buf, q, b4, sizeof(buf));
            q = AppendCStr(buf, q, " rows=[", sizeof(buf));
            q = AppendCStr(buf, q, b5, sizeof(buf));
            q = AppendCh(buf, q, ',', sizeof(buf));
            q = AppendCStr(buf, q, b6, sizeof(buf));
            q = AppendCh(buf, q, ')', sizeof(buf));
            LogLine(buf);
        }
    }

    /* --- ④ 底板(覆盖本轮实际显示的行) --- */
    rect.X = x - 3;
    rect.Y = y - 2;
    rect.Width  = maxW + 6;
    rect.Height = totalRows * lineH + 4;
    board.R = 0; board.G = 0; board.B = 0;
    FillRectTransVt(surf, &rect, &board, g_Cfg.BoardOpacity);

    /* 记下本帧的矩形 + 帧号 —— 鼠标命中判定(点击切换、滚轮、吞点击)都用它。
     * 帧号用于保鲜: 消息框不再绘制后, 命中判定会自动失效(见 MouseInBox)。 */
    g_BoxRect  = rect;
    g_BoxFrame = g_Tick;

    /* 提示区跟着【底板的下边缘】走, 两者之间留 HintGap 像素的缝隙。
     * rect.Height = totalRows*lineH + 4, rect.Y = y-2 ⇒ 下边缘 = y + totalRows*lineH + 2。 */
    g_BoxBottom = rect.Y + rect.Height;

    /* --- ④b 滚动条 ---
     *
     * 贴在底板【右边缘外侧】1 像素处, 而不是画在框内:
     * 画在框内就得给文字让出宽度, 而折行宽度一变、文本会跟着重排,
     * 滚动时整屏文字都会跳。放框外则完全不影响排版。
     *
     * 方向: 新消息在下, 所以 skip=0(看到最新)时滑块在【底部】。 */
    if (totalAll > limit)
    {
        int maxSkip  = totalAll - limit;
        int barX     = rect.X + rect.Width + 1;
        int barY     = rect.Y + 2;
        int barH     = rect.Height - 4;
        int thumbH, thumbY;
        RectangleStruct bar;
        ColorStruct     barColor;

        if (barH > 12)
        {
            /* 轨道 */
            bar.X = barX; bar.Y = barY;
            bar.Width = BAR_WIDTH; bar.Height = barH;
            barColor.R = 30; barColor.G = 30; barColor.B = 30;
            FillRectTransVt(surf, &bar, &barColor, 55);

            /* 滑块: 高度按"可见行 / 总行"比例 */
            thumbH = barH * limit / totalAll;
            if (thumbH < 8) thumbH = 8;
            if (thumbH > barH) thumbH = barH;

            /* skip 越大 = 越往历史翻 = 滑块越靠上 */
            thumbY = barY + (barH - thumbH) * (maxSkip - skip) / maxSkip;

            bar.X = barX; bar.Y = thumbY;
            bar.Width = BAR_WIDTH; bar.Height = thumbH;
            barColor.R = 170; barColor.G = 170; barColor.B = 170;
            FillRectTransVt(surf, &bar, &barColor, 90);
        }
    }

    /* --- ⑤ 逐行画 ---
     *
     * ⚠️ pBounds / pLocation 的语义(第三次才彻底搞对):
     *   pBounds   = 【整个消息框】。它既是坐标原点(实际位置 = pBounds 原点 + pLocation),
     *               又是裁剪区: 内部构造 (X, Y, X+Width, Y+Height) 用于裁剪
     *               (见 0x4A5F96~0x4A5FAB 的加法)。
     *   pLocation = 【框内】的相对偏移。
     *   曾经把 pBounds 固定在 (0,0) 且高度只给 2 倍行高, 结果第 1 行完整、
     *   第 2 行被裁掉下半截、第 3 行起全空白。 */
    fnCS = (FancyTextCSFn)ADDR_FANCY_TEXT_CS;

    for (k = 0; k < picked && drawn < totalRows; k++)
    {
        ChatMsg* m = MsgAt(g_Picked[k]);
        int      schemeIdx;
        void*    scheme;

        /* 取色规则:
         *   · 玩家聊天(name 非空): 用【发言者】的颜色方案 —— 与原版一致
         *   · 系统/剧情消息(name 为空): 默认也用【消息自带的】颜色方案(原版行为);
         *     SysUseMyColor=1 可改成用【当前玩家(我)】的颜色, 让消息框整体
         *     呈现"我的界面"观感。
         * 这里只决定"用哪个颜色方案", 具体 RGB 由引擎自己从方案里取 —— 这样才能
         * 和原版表现完全一致(自己算 RGB 会踩到 BaseColor/换算/通道顺序一连串问题)。 */
        if (m->name[0] || !g_Cfg.SysUseMyColor)
            schemeIdx = m->color;
        else
            schemeIdx = CurrentPlayerScheme();

        scheme = SchemePtr(schemeIdx);
        if (!scheme) scheme = SchemePtr(0);      /* 兜底: 用第 0 个方案 */
        if (!scheme) continue;

        for (r = 0; r < g_LineCnt[k]; r++)
        {
            TextLine* ln = &g_LineBuf[k][r];
            wchar_t   tmp[MAX_NAME_W + MAX_TEXT_W + 4];
            int       t, globalRow;

            globalRow = g_RowBase[k] + r;
            if (globalRow < rowBegin || globalRow >= rowEnd) continue;

            for (t = 0; t < ln->len && t < MAX_NAME_W + MAX_TEXT_W; t++)
                tmp[t] = ln->start[t];
            tmp[t] = 0;

            loc.X = 3;
            loc.Y = (globalRow - rowBegin) * lineH + 2;

            fnCS(&ret, tmp, surf, &rect, &loc, scheme, NULL, TEXT_FLAGS);
            drawn++;
        }
    }
}

/* ==================== 操作提示区绘制 ====================
 *
 * 贴在主消息框【正下方】, 中间留 HintGap 像素缝隙。
 * 位置来自 g_BoxBottom —— 那是主框底板的下边缘, DrawMessages 每帧更新,
 * 所以主框一折叠/展开/消息变多变少, 这一块就跟着上下移动, 间距始终不变。
 *
 * 与主框刻意的区别:
 *   - 不滚动、不展开、没有 KeepLast 兜底 —— 全过期就整块消失
 *   - 不参与主历史的行号, 互不影响
 *   - 取色沿用消息自带的 ColorScheme, 看起来与主框是同一套体系
 *
 * 行数不够时【保留最新的】: 从最新往回收集, 收不下就停 ——
 * 宁可少显示几条旧的, 也不能让最新那条看不到。
 */
static void DrawHints(void)
{
    /* 折行结果放静态区: 24 条 × 8 行 × 8 字节 ≈ 1.5 KB, 不必占栈 */
    static TextLine lines[MAX_HINTS][MAX_LPP];
    static int      colors[MAX_HINTS];
    static int      lineCnt[MAX_HINTS];

    void*   surf;
    Point2D loc, ret;
    FancyTextCSFn fnCS;
    ColorStruct   board;
    RectangleStruct rect;
    RectangleStruct* vb;
    int x, y, lineH, screenH, maxRows;
    int i, k, r, cnt = 0, totalRows = 0, row;
    unsigned tt;

    if (!g_Cfg.ShowWindow || !g_Cfg.HintArea) return;
    if (g_HintCount <= 0) return;

    surf = *(void**)ADDR_DSURFACE_COMPOSITE;
    if (!surf) return;

    lineH = g_LineH;
    if (lineH <= 0) return;
    if (g_BoxWidth < 40) return;   /* 主框还没画过, 宽度未知(ShowWindow=0 时会这样) */

    x = g_Cfg.PosX;
    y = g_BoxBottom + g_Cfg.HintGap;

    /* 屏幕底部放不下的部分直接不要(宁缺毋滥, 也不做裁剪一半的字) */
    vb      = (RectangleStruct*)ADDR_DSURFACE_VIEWBOUNDS;
    screenH = (vb && vb->Height > 100) ? vb->Height : 480;
    maxRows = (screenH - 4 - y) / lineH;
    if (maxRows < 1) return;                                  /* 主框太高, 底下没地方了 */
    if (maxRows > g_Cfg.HintLines) maxRows = g_Cfg.HintLines;

    tt = g_Tick;

    /* --- 从最新往回收集还没消失的提示 --- */
    for (i = g_HintCount - 1; i >= 0 && cnt < MAX_HINTS; i--)
    {
        HintMsg* h = HintAt(i);
        int n;

        /* 到点就"自然消失" —— 这里不做删除, 只是不显示, 环形缓冲自己会覆盖 */
        if (h->timeout > 0 && tt - h->frame > (unsigned)h->timeout) continue;

        n = WrapText(h->text, g_BoxWidth, lines[cnt], MAX_LPP);
        if (n <= 0) continue;

        if (totalRows + n > maxRows) break;   /* 这块放不下了 */

        colors[cnt]  = h->color;
        lineCnt[cnt] = n;
        totalRows   += n;
        cnt++;
    }

    /* 诊断(LogRaw=1): 提示区状态变化时记一行。
     * 用来回答"提示区怎么不出现/怎么少了几条" —— 能看出是缓冲里没有(total=0)、
     * 还是都被超时滤掉了(shown=0)、还是行数不够放不下(rows 顶到 HintLines)。 */
    if (g_Cfg.LogRaw)
    {
        static int s_lastTotal = -1, s_lastShown = -1;
        if (g_HintCount != s_lastTotal || cnt != s_lastShown)
        {
            char b1[16], b2[16], b3[16], b4[16], buf[160];
            unsigned q = 0;
            s_lastTotal = g_HintCount; s_lastShown = cnt;
            UtoA((unsigned)g_HintCount, b1);
            UtoA((unsigned)cnt,         b2);
            UtoA((unsigned)totalRows,   b3);
            UtoA((unsigned)maxRows,     b4);
            q = AppendCStr(buf, q, "hint: total=", sizeof(buf));
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " shown=", sizeof(buf));
            q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " rows=", sizeof(buf));
            q = AppendCStr(buf, q, b3, sizeof(buf));
            q = AppendCStr(buf, q, "/", sizeof(buf));
            q = AppendCStr(buf, q, b4, sizeof(buf));
            LogLine(buf);
        }
    }

    if (cnt <= 0) return;

    /* --- 底板: 与主框同一套尺寸约定(左右各多 3 像素, 上下各多 2 像素) --- */
    rect.X      = x - 3;
    rect.Y      = y - 2;
    rect.Width  = g_BoxWidth + 6;
    rect.Height = totalRows * lineH + 4;

    if (g_Cfg.HintOpacity > 0)
    {
        board.R = 0; board.G = 0; board.B = 0;
        FillRectTransVt(surf, &rect, &board, g_Cfg.HintOpacity);
    }

    fnCS = (FancyTextCSFn)ADDR_FANCY_TEXT_CS;

    /* --- 从最旧的一条开始画, 这样最新的落在最下面(与主框一致) --- */
    row = 0;
    for (k = cnt - 1; k >= 0; k--)
    {
        void* scheme = SchemePtr(colors[k]);

        for (r = 0; r < lineCnt[k]; r++)
        {
            TextLine* ln = &lines[k][r];
            wchar_t   tmp[MAX_HINT_W];
            int       t;

            for (t = 0; t < ln->len && t < MAX_HINT_W - 1; t++)
                tmp[t] = ln->start[t];
            tmp[t] = 0;

            /* pBounds 传整块底板, loc 传框内偏移 —— 与主框完全相同的用法 */
            loc.X = 3;
            loc.Y = row * lineH + 2;

            fnCS(&ret, tmp, surf, &rect, &loc, scheme, NULL, TEXT_FLAGS);
            row++;
        }
    }
}

/* ==================== 帧钩子: 输入处理 ====================
 *
 * 输入处理必须放在【每帧必被调用】的地方。
 *
 * ⚠️ 曾经把 PollKeys/PollMouse 放在 DrawMessages 里(而它在 MessageListClass::Draw
 *    内部的钩子上)。那个钩子只在【原版消息链表非空】时才执行 —— 于是所有消息
 *    过期、链表清空之后, 绘制钩子连同输入处理一起停止工作: 按 Ctrl+M 没反应、
 *    日志里连一行记录都没有(实测反馈)。
 *
 * 帧钩子 0x55D360 每个逻辑帧都会被调用, 与"有没有消息"无关, 所以输入放这里。
 * (AutoKit / AutoLoad 用的也是这个地址。)
 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_FrameHook(void* regs)
{
    (void)regs;
    EnsureInit();

    /* 活性诊断: 只写一次。这一行能直接回答"帧钩子到底有没有被执行" ——
     * 排查 0.4.2 那个"消息过期后热键全失灵且日志空白"的问题时,
     * 最缺的就是这么一条证据。刻意不受 LogRaw 控制(仅一次, 不刷屏)。 */
    {
        static unsigned char s_alive = 0;
        if (!s_alive) { s_alive = 1; LogLine("hook: frame alive (0x55D360)"); }
    }

    /* ---- 推进单调帧计数 g_Tick(见它的说明) ----
     * 本钩子每秒被调用约 600 次, 但只有 CurrentFrame 真正变化时才推进,
     * 所以 g_Tick 的增速就是逻辑帧率, 与钩子调用频率无关。 */
    {
        unsigned f = *(unsigned*)ADDR_CURRENT_FRAME;

        if (f != g_LastFrame)
        {
            unsigned old    = g_LastFrame;
            unsigned d      = f - old;
            int      jumped = 0;

            /* 增量大到不可能是"一帧一帧走过来的" -> CurrentFrame 发生了跳变
             * (读档、切换场景等)。丢弃这次增量, 绝不让时间倒退。 */
            if (d > TICK_MAX_DELTA) { jumped = 1; d = 0; }

            g_LastFrame = f;
            g_Tick     += d;

            if (jumped && g_Cfg.LogRaw)
            {
                char b1[16], b2[16], buf[160];
                unsigned q = 0;
                UtoA(old,    b1);
                UtoA(f,      b2);
                q = AppendCStr(buf, q, "frame jumped: ", sizeof(buf));
                q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCStr(buf, q, " -> ", sizeof(buf));
                q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCStr(buf, q, "  (时间不倒退, 这次增量丢弃)", sizeof(buf));
                LogLine(buf);
            }
        }

        /* 心跳(仅 LogRaw=1): 每 5 秒【真实时间】记一行, 报告 CurrentFrame
         * 和 g_Tick 各自前进了多少。
         *
         * 这一行是排查"消息不过期"的钥匙:
         *   dFrame=+0  ->  CurrentFrame 根本没动(暂停/地址失效)
         *   dFrame 很大而 dTick=+0  ->  一直在跳变, 每次都被当成读档丢掉了
         *   两者都正常增长  ->  超时逻辑本身有问题 */
        if (g_Cfg.LogRaw)
        {
            static DWORD    s_beat     = 0;
            static unsigned s_beatF    = 0;
            static unsigned s_beatTick = 0;
            DWORD now = GetTickCount();

            if (s_beat == 0)
            {
                s_beat = now; s_beatF = f; s_beatTick = g_Tick;
            }
            else if (now - s_beat >= 5000)
            {
                char b1[16], b2[16], b3[16], b4[16], b5[16], buf[200];
                unsigned q = 0;
                UtoA(now - s_beat,      b1);   /* 实际经过的毫秒 */
                UtoA(f - s_beatF,       b2);   /* CurrentFrame 增量 */
                UtoA(g_Tick - s_beatTick, b3); /* 我们采纳的增量 */
                UtoA((unsigned)g_MsgCount, b4);
                UtoA(g_Tick,            b5);

                q = AppendCStr(buf, q, "beat: ", sizeof(buf));
                q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCStr(buf, q, "ms dFrame=+", sizeof(buf));
                q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCStr(buf, q, " dTick=+", sizeof(buf));
                q = AppendCStr(buf, q, b3, sizeof(buf));
                q = AppendCStr(buf, q, " tick=", sizeof(buf));
                q = AppendCStr(buf, q, b5, sizeof(buf));
                q = AppendCStr(buf, q, " msgs=", sizeof(buf));
                q = AppendCStr(buf, q, b4, sizeof(buf));
                LogLine(buf);

                s_beat = now; s_beatF = f; s_beatTick = g_Tick;
            }
        }
    }

    if (g_Cfg.Enable)
    {
        PollKeys();
        PollMouse();
    }
    return 0;
}

/* ==================== 绘制 ====================
 *
 * 分两个钩子配合, 各管一件事:
 *
 *   0x4F4558  ChatBox_DrawCallHook   —— 每帧必到的绘制调用点【前一条】指令。
 *             `b9 60 bc a8 00` = mov $0xa8bc60,%ecx  (5 字节, 绝对立即数)
 *             这里 4F4558 是【无条件】每帧执行的, 所以画框的时机可靠
 *             (不再依赖"消息链表非空")。我们在这里画自己的消息框, 返回 0
 *             让原指令执行, 原版 Draw 继续跑 —— 它负责画【输入框】。
 *
 *   0x5D4A94  ChatBox_MessageDrawHook —— Draw 内部画【消息列表】的那三条指令。
 *             返回 0x5D4A9B 跳过它, 于是原版消息列表不画(由我们的框替代),
 *             而同一个函数里的输入框照旧。
 *
 * 之所以不让第一个钩子直接跳过原版 Draw: 那样会连输入框一起跳掉
 * (联机按回车时"从【玩家名】："就不见了)。
 *
 * ⚠️ 钩点绝不能选 0x4F455D —— 那是 `call rel32`, Syringe 把原指令抄到
 *    trampoline 执行时不重定位相对偏移, 会跳到 0x02970464 崩溃(实测)。
 *    详见 offsets.h 里 ADDR_DRAW_CALL_SITE 上面那段。 */

/* 画自己的消息框 —— 挂在无条件执行的调用点上 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_DrawCallHook(void* regs)
{
    (void)regs;
    EnsureInit();

    /* 活性诊断: 只写一次(见 FrameHook 处说明) */
    {
        static unsigned char s_alive = 0;
        if (!s_alive) { s_alive = 1; LogLine("hook: draw alive (0x4F4558)"); }
    }

    if (g_Cfg.Enable && g_Cfg.ShowWindow)
    {
        DrawMessages();
        /* 必须【在主框之后】—— 提示区的位置依赖 g_BoxBottom, 那是主框刚更新的 */
        DrawHints();
    }

    return 0;   /* 让原版 Draw 继续 —— 它要画输入框 */
}

/* 只跳过原版的消息列表绘制 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_MessageDrawHook(void* regs)
{
    (void)regs;
    EnsureInit();

    if (g_Cfg.Enable && g_Cfg.ShowWindow)
        return ADDR_AFTER_MSGLIST_DRAW;   /* 只跳过消息列表, 保留输入框 */

    return 0;                             /* 执行原指令, 原版照旧显示 */
}

/* ==================== 核心: AddMessage 钩子 ==================== */

/*
 * 钩在 MessageListClass::AddMessage 的【函数入口】(0x5D3BA0)。
 *
 * 此时函数的第一条指令 sub $0x14c,%esp 尚未执行, 所以:
 *   - ECX  = this (MessageListClass::Instance)
 *   - ESP  = 指向返回地址, 参数依次在 ESP+4 .. ESP+1C
 * 这正是我们能在"消息产生的第一时间"拿到全部原始参数的原因。
 *
 * 返回 0 表示"执行被覆盖的原指令后继续" —— 我们从不干预原版行为,
 * 阶段 1 纯观察。
 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_AddMessageHook(void* regs)
{
    unsigned char* esp;
    const wchar_t* argName;
    const wchar_t* argMsg;
    unsigned retAddr, argID, argColor, argStyle, argTimeout, argSP;
    char  b1[16], b2[16], b3[16], b4[16], b5[16], b6[16];
    char  nbuf[MAX_TEXT_CHARS * 4 + 8];
    char  tbuf[MAX_TEXT_CHARS * 4 + 8];
    char  line[LOG_LINE_MAX];
    unsigned p = 0;
    unsigned nameLen = 0, textLen = 0;

    EnsureInit();
    if (!g_Cfg.Enable) return 0;

    esp = *(unsigned char**)((char*)regs + OFF_REG_ESP);
    if (!esp) return 0;

    retAddr    = *(unsigned*)(esp + STK_RETURN_ADDR);
    argName    = *(const wchar_t**)(esp + STK_ARG_NAME);
    argID      = *(unsigned*)(esp + STK_ARG_ID);
    argMsg     = *(const wchar_t**)(esp + STK_ARG_MESSAGE);
    argColor   = *(unsigned*)(esp + STK_ARG_COLORSCHEME);
    argStyle   = *(unsigned*)(esp + STK_ARG_STYLE);
    argTimeout = *(unsigned*)(esp + STK_ARG_TIMEOUT);
    argSP      = *(unsigned*)(esp + STK_ARG_SINGLEPLAYER);

    g_Seq++;

    nameLen = WcsLenLimited(argName, MAX_TEXT_CHARS);
    textLen = WcsLenLimited(argMsg,  MAX_TEXT_CHARS);

    /* --- 拼日志行 ---
     *
     * 两种格式, 由 LogRaw 决定:
     *
     *   LogRaw=0 (默认) —— 只给人看的内容:
     *       #12 moonbamboo: test
     *       #13 难度： 终结
     *
     *   LogRaw=1 —— 完整字段, 排查/标定用:
     *       #12 ret=0x0055F0FA id=2 cs=7 style=0x4046 to=1259 sp=0
     *           nameLen=10 textLen=4 name="moonbamboo" text="test"
     *
     * ret/id/cs/style/to/sp 这些对日常阅读没有意义(它们是分流的判据、
     * 颜色方案索引等等), 所以统一收进 LogRaw, 默认不输出。 */
    p = AppendCh(line, p, '#', sizeof(line));
    p += UtoA(g_Seq, line + p);

    if (g_Cfg.LogRaw)
    {
        p = AppendCStr(line, p, " ret=",    sizeof(line));
        p += HtoA(retAddr, line + p);

        p = AppendCStr(line, p, " id=",     sizeof(line));
        p += UtoA(argID, line + p);

        p = AppendCStr(line, p, " cs=",     sizeof(line));
        p += UtoA(argColor, line + p);

        p = AppendCStr(line, p, " style=",  sizeof(line));
        p += HtoA(argStyle, line + p);

        p = AppendCStr(line, p, " to=",     sizeof(line));
        p += UtoA(argTimeout, line + p);

        p = AppendCStr(line, p, " sp=",     sizeof(line));
        p += UtoA(argSP, line + p);

        p = AppendCStr(line, p, " nameLen=", sizeof(line));
        p += UtoA(nameLen, line + p);
        p = AppendCStr(line, p, " textLen=", sizeof(line));
        p += UtoA(textLen, line + p);

        if (g_Cfg.LogText)
        {
            WideToLocalEscaped(argName, nbuf, sizeof(nbuf));
            WideToLocalEscaped(argMsg,  tbuf, sizeof(tbuf));

            p = AppendCStr(line, p, " name=\"",  sizeof(line));
            p = AppendCStr(line, p, nbuf,        sizeof(line));
            p = AppendCStr(line, p, "\" text=\"", sizeof(line));
            p = AppendCStr(line, p, tbuf,        sizeof(line));
            p = AppendCh(line, p, '"',           sizeof(line));
        }
    }
    else
    {
        p = AppendCh(line, p, ' ', sizeof(line));

        if (g_Cfg.LogText)
        {
            /* 玩家聊天带 "名字: " 前缀; 系统消息没有名字, 直接给正文 */
            if (nameLen > 0)
            {
                WideToLocalEscaped(argName, nbuf, sizeof(nbuf));
                p = AppendCStr(line, p, nbuf,   sizeof(line));
                p = AppendCStr(line, p, ": ",   sizeof(line));
            }
            WideToLocalEscaped(argMsg, tbuf, sizeof(tbuf));
            p = AppendCStr(line, p, tbuf, sizeof(line));
        }
        else
        {
            p = AppendCStr(line, p, "(text omitted)", sizeof(line));
        }
    }

    (void)b1; (void)b2; (void)b3; (void)b4; (void)b5; (void)b6;

    /* --- 阶段 2: 分流 ---
     *
     * 消息按第 7 个参数(silent, 实测可靠性 100%)分成两类:
     *
     *   真消息(name/聊天/剧情/系统)  -> 主历史 g_Msgs, 可翻可展开
     *   操作提示(silent=1: 快捷键/路径点) -> 独立的提示区 g_Hints, 会自己消失
     *
     * 操作提示实测一局 20 多条, 混进主历史会把真正要看的内容冲掉; 但它们也
     * 不是全无用处, 所以给一块独立区域显示, 而不是直接丢掉。
     *
     * HintArea=0 时退回旧行为: 由 FilterSilent 决定"丢掉"还是"混进主历史"。 */
    if (!argSP)
    {
        /* 时间戳用单调帧计数 g_Tick, 不用 CurrentFrame(读档会回退, 见其说明) */
        MsgPush(argName, argMsg, (int)argColor, retAddr,
                g_Tick, (int)argTimeout, (int)argSP);
    }
    else if (g_Cfg.HintArea)
    {
        HintPush(argMsg, (int)argColor, (int)argTimeout);
    }
    else if (!g_Cfg.FilterSilent)
    {
        MsgPush(argName, argMsg, (int)argColor, retAddr,
                g_Tick, (int)argTimeout, (int)argSP);
    }

    LogLine(line);

    return 0;   /* 0 = 继续执行原指令, 原版行为完全不变 */
}

/* ==================== Syringe 接口 ==================== */

typedef struct {
    int cbSize;
    int num_hooks;
    unsigned int checksum;
    DWORD exeFilesize;
    DWORD exeTimestamp;
    unsigned int exeCRC;
    int cchMessage;
    char* Message;
} SyringeHandshakeInfo;

extern "C" __declspec(dllexport) HRESULT __cdecl SyringeHandshake(SyringeHandshakeInfo* pInfo)
{
    if (!pInfo || pInfo->cbSize < (int)sizeof(SyringeHandshakeInfo))
        return E_FAIL;

    EnsureInit();

    if (pInfo->Message && pInfo->cchMessage > 0)
    {
        const char* msg = "ChatBox " CB_VERSION
                          ": message log + on-screen message box (client-side only).";
        int i = 0;
        while (msg[i] && i < pInfo->cchMessage - 1) { pInfo->Message[i] = msg[i]; i++; }
        pInfo->Message[i] = '\0';
    }
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        /* 不做重活: 若 Syringe 未调用 handshake, 首次消息钩子会惰性初始化 */
    }
    return TRUE;
}
