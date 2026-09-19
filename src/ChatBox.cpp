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

#define CB_VERSION      "0.2.1-window"
#define CB_SECTION      "ChatBox"

#define MAX_PATH_LEN    260
#define LOG_LINE_MAX    4096      /* 日志单行缓冲 */
#define MAX_TEXT_CHARS  1024      /* 单条消息最多取这么多字符 */
#define DIR_NAME        "MsgLog"

/* ---------------- 配置 ---------------- */

#define DEF_ENABLE      1
#define DEF_LOG_RAW     1         /* 阶段 1 的核心开关: 记录全部参数 */

typedef struct {
    int Enable;      /* ChatBox=1          总开关 */
    int LogRaw;      /* LogRaw=1           记录全部 7 个参数 + 返回地址 */
    int LogText;     /* LogText=1          记录消息文本(关掉则只记长度) */
    int LogUtf8;     /* LogUtf8=0          1 = 日志用 UTF-8(带 BOM), 0 = 本地代码页 */

    /* ---- 阶段 2: 窗口 ---- */
    int ShowWindow;     /* ShowWindow=1     1 = 接管绘制, 显示自制消息框;
                         *                   0 = 完全不碰画面(原版消息照旧显示) */
    int MaxLines;       /* MaxLines=8       折叠态最多同时显示几条 */
    int BoardOpacity;   /* BoardOpacity=45  底板不透明度 0~100 */
    int PosX;           /* PosX=8           窗口左上角 X */
    int PosY;           /* PosY=8           窗口左上角 Y */
    int MaxWidth;       /* MaxWidth=0       消息框最大宽度(像素); 0 = 自动 */
    int ExpandLines;    /* ExpandLines=30   展开态最多显示多少行 */
    int SysUseMyColor;  /* SysUseMyColor=1  系统消息用当前玩家颜色(0 = 用消息自带颜色) */
    int FilterSilent;   /* FilterSilent=1   1 = 不进消息框(操作提示: 快捷键/路径点) */
    int ShowLogHint;    /* ShowLogHint=1    1 = 把操作提示照旧用原版样式画出来 */
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
    g_Cfg.MaxWidth     = GetPrivateProfileIntA(CB_SECTION, "MaxWidth",     0,  g_IniPath);
    g_Cfg.ExpandLines  = GetPrivateProfileIntA(CB_SECTION, "ExpandLines", 30,  g_IniPath);
    g_Cfg.SysUseMyColor= GetPrivateProfileIntA(CB_SECTION, "SysUseMyColor", 1, g_IniPath);
    g_Cfg.FilterSilent = GetPrivateProfileIntA(CB_SECTION, "FilterSilent", 1,  g_IniPath);
    g_Cfg.ShowLogHint  = GetPrivateProfileIntA(CB_SECTION, "ShowLogHint",  0,  g_IniPath);

    g_Cfg.Enable  = g_Cfg.Enable  ? 1 : 0;
    g_Cfg.LogRaw  = g_Cfg.LogRaw  ? 1 : 0;
    g_Cfg.LogText = g_Cfg.LogText ? 1 : 0;
    g_Cfg.LogUtf8 = g_Cfg.LogUtf8 ? 1 : 0;

    g_Cfg.ShowWindow    = g_Cfg.ShowWindow    ? 1 : 0;
    g_Cfg.SysUseMyColor = g_Cfg.SysUseMyColor ? 1 : 0;
    g_Cfg.FilterSilent = g_Cfg.FilterSilent ? 1 : 0;
    g_Cfg.ShowLogHint  = g_Cfg.ShowLogHint  ? 1 : 0;
    if (g_Cfg.MaxLines     < 1)   g_Cfg.MaxLines     = 1;
    if (g_Cfg.MaxLines     > 40)  g_Cfg.MaxLines     = 40;
    if (g_Cfg.BoardOpacity < 0)   g_Cfg.BoardOpacity = 0;
    if (g_Cfg.BoardOpacity > 100) g_Cfg.BoardOpacity = 100;
    if (g_Cfg.MaxWidth < 0)       g_Cfg.MaxWidth     = 0;
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

#define MAX_MSGS    200
#define MAX_TEXT_W  512
#define MAX_NAME_W  64

typedef struct {
    wchar_t  text[MAX_TEXT_W];   /* 消息正文 */
    wchar_t  name[MAX_NAME_W];   /* 来源(仅玩家聊天非空) */
    int      color;              /* ColorSchemeIdx -> 玩家颜色 */
    unsigned retAddr;            /* 调用者返回地址(分类用) */
    unsigned frame;              /* 收到时的游戏帧号 */
    int      timeout;            /* 超时帧数; -1 = 永不消失 */
    int      silent;             /* 1 = 操作提示 */
} ChatMsg;

static ChatMsg g_Msgs[MAX_MSGS];
static int     g_MsgHead  = 0;   /* 下一条要写入的槽位 */
static int     g_MsgCount = 0;   /* 当前有效条数 */

/* i = 0 表示最旧的一条 */
static ChatMsg* MsgAt(int i)
{
    int idx = g_MsgHead - g_MsgCount + i;
    while (idx < 0) idx += MAX_MSGS;
    return &g_Msgs[idx % MAX_MSGS];
}

static void CopyW(wchar_t* dst, const wchar_t* src, int cap)
{
    int i = 0;
    if (!src || !IsPlausiblePtr(src)) { dst[0] = 0; return; }
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void MsgPush(const wchar_t* name, const wchar_t* text, int color,
                    unsigned retAddr, unsigned frame, int timeout, int silent)
{
    ChatMsg* m = &g_Msgs[g_MsgHead];
    CopyW(m->text, text, MAX_TEXT_W);
    CopyW(m->name, name, MAX_NAME_W);
    m->color   = color;
    m->retAddr = retAddr;
    m->frame   = frame;
    m->timeout = timeout;
    m->silent  = silent;

    g_MsgHead = (g_MsgHead + 1) % MAX_MSGS;
    if (g_MsgCount < MAX_MSGS) g_MsgCount++;
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

#define MAX_PICKED  100    /* 一次最多参与排版的"消息条数"(展开态要能往回翻) */
#define MAX_LPP       8    /* 单条消息最多折成几行(超出部分截断) */

typedef struct {
    const wchar_t* start;
    int            len;
} TextLine;

static TextLine g_LineBuf[MAX_PICKED][MAX_LPP];
static int      g_LineCnt[MAX_PICKED];
static int      g_Picked[MAX_PICKED];    /* 本轮要显示的消息下标, [0] = 最新 */
static int      g_RowBase[MAX_PICKED];   /* 每条消息第一行的行号 */
static wchar_t  g_Compose[MAX_NAME_W + MAX_TEXT_W + 4];   /* 拼 "名字: 内容" 用 */

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
    static unsigned char s_logged = 0;

    if (!g_Cfg.Enable) return;

    /* Ctrl + M : 切换展开 / 收起 */
    {
        static unsigned char s_pCtrlM = 0;
        unsigned char now = (unsigned char)(KeyDown(VK_CONTROL) && KeyDown('M'));
        if (now && !s_pCtrlM)
        {
            g_Expanded = !g_Expanded;
            g_Scroll   = 0;                 /* 每次切换都回到最新 */
            LogLine(g_Expanded ? "ui: expanded (Ctrl+M)" : "ui: collapsed (Ctrl+M)");
        }
        s_pCtrlM = now;
    }

    if (!g_Expanded) return;

    if (KeyPressedEdge(VK_UP,   &s_pUp))   g_Scroll++;
    if (KeyPressedEdge(VK_DOWN, &s_pDown)) { if (g_Scroll > 0) g_Scroll--; }

    if (KeyPressedEdge(VK_PRIOR, &s_pPgUp))
        g_Scroll += g_Cfg.MaxLines;                       /* PageUp: 往历史翻 */
    if (KeyPressedEdge(VK_NEXT,  &s_pPgDn))
    {
        g_Scroll -= g_Cfg.MaxLines;
        if (g_Scroll < 0) g_Scroll = 0;
    }

    if (KeyPressedEdge(VK_HOME, &s_pHome)) g_Scroll = 99999;   /* 到最旧 */
    if (KeyPressedEdge(VK_END,  &s_pEnd))  g_Scroll = 0;       /* 到最新 */

    /* 上界夹紧放到绘制时按实际行数做, 因为这里不知道总共有多少行 */
    if (!s_logged) { s_logged = 1; }
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

static RectangleStruct g_BoxRect;    /* 上一帧消息框的屏幕矩形(命中判定用) */
static int             g_BoxValid = 0;

static int MouseInBox(void)
{
    void* m;
    int   mx, my;

    if (!g_BoxValid) return 0;
    m = *(void**)ADDR_WWMOUSE_INSTANCE;
    if (!m) return 0;

    mx = *(int*)((char*)m + OFF_WWMOUSE_XY1 + 0);
    my = *(int*)((char*)m + OFF_WWMOUSE_XY1 + 4);

    return (mx >= g_BoxRect.X && mx < g_BoxRect.X + g_BoxRect.Width &&
            my >= g_BoxRect.Y && my < g_BoxRect.Y + g_BoxRect.Height);
}

/* 滚轮滚动。dir > 0 = 往历史翻(上), dir < 0 = 往新消息(下) */
static void ScrollBy(int dir)
{
    int lines = g_Cfg.MaxLines;
    if (lines < 1) lines = 1;

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

    /* 键盘/鼠标交互放在最前面: 即使当前没消息, 也要能响应切换热键 */
    PollKeys();
    PollMouse();

    g_BoxValid = 0;      /* 本轮若没画出消息框, 命中判定就应为空 */
    surf = *(void**)ADDR_DSURFACE_COMPOSITE;
    if (!surf) return;
    if (g_MsgCount <= 0) return;

    pFont = *(unsigned char**)ADDR_BITFONT_INSTANCE;
    lineH = pFont ? *(int*)(pFont + FONT_OFF_HEIGHT) : 14;
    if (lineH < 6 || lineH > 64) lineH = 14;

    /* 首次绘制时把字体度量记进日志, 方便排查排版问题 */
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

    /* 把当前生效的宽度也记一次(排查排版问题用) */
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

    frame = *(unsigned*)ADDR_CURRENT_FRAME;

    /* --- ① 挑选要进入排版的消息(从最新往回) ---
     * 折叠态: 消息超时就跳过 —— 效果接近原版"显示一段时间后自动消失"。
     * 展开态: 【忽略超时】, 显示完整历史 —— 否则翻历史时旧消息会自己消失。
     * 注意内存历史本身从不删除, 只是"显示/不显示"的区别。 */
    for (i = g_MsgCount - 1; i >= 0 && picked < MAX_PICKED; i--)
    {
        ChatMsg* m = MsgAt(i);
        if (!g_Expanded && m->timeout >= 0 && m->timeout > 0)
        {
            if (frame - m->frame > (unsigned)m->timeout) break;   /* 该消失了 */
        }
        g_Picked[picked++] = i;      /* g_Picked[0] 是最新的一条 */
    }
    if (picked <= 0) return;

    x = g_Cfg.PosX;
    y = g_Cfg.PosY;
    /* 消息框宽度: MaxWidth>0 时直接用它, 否则取屏幕宽度的一半(上限 520)。
     * 夹紧规则: 至少 80 像素, 且不超出屏幕右侧。 */
    maxW = 420;
    {
        RectangleStruct* vb = (RectangleStruct*)ADDR_DSURFACE_VIEWBOUNDS;
        int screenW = (vb && vb->Width > 200) ? vb->Width : 640;

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
    }
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
        ChatMsg*       m = MsgAt(g_Picked[k]);
        const wchar_t* body;
        int            cnt;

        if (m->name[0])
        {
            /* 玩家聊天: 拼成 "名字: 内容" 再折行 */
            int p = 0, q = 0;
            while (m->name[q] && p < MAX_NAME_W + MAX_TEXT_W) { g_Compose[p++] = m->name[q++]; }
            if (p < MAX_NAME_W + MAX_TEXT_W - 2) { g_Compose[p++] = L':'; g_Compose[p++] = L' '; }
            q = 0;
            while (m->text[q] && p < MAX_NAME_W + MAX_TEXT_W - 1) { g_Compose[p++] = m->text[q++]; }
            g_Compose[p] = 0;
            body = g_Compose;
        }
        else
        {
            body = m->text;
        }

        cnt = WrapText(body, textW, g_LineBuf[k], MAX_LPP);
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

    /* --- ④ 底板(覆盖本轮实际显示的行) --- */
    rect.X = x - 3;
    rect.Y = y - 2;
    rect.Width  = maxW + 6;
    rect.Height = totalRows * lineH + 4;
    board.R = 0; board.G = 0; board.B = 0;
    FillRectTransVt(surf, &rect, &board, g_Cfg.BoardOpacity);

    /* 记下本帧的矩形 —— 鼠标命中判定(点击切换、滚轮、吞点击)都用它 */
    g_BoxRect  = rect;
    g_BoxValid = 1;

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
         *   · 系统/剧情消息(name 为空): 默认用【当前玩家(我)】的颜色方案,
         *     让消息框整体呈现"我的界面"观感; SysUseMyColor=0 可关掉改回原样。
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

/* 绘制接管: 钩在 0x4F455D (原版 call MessageListClass::Draw 的位置)
 *
 * 返回 0x4F4562 = 跳过那条 call, 自己画; 返回 0 = 执行原 call, 走原版。
 * 无论哪条路, AddMessage 都照常跑过 —— 消息该进链表进链表、该响提示音响提示音。
 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatBox_MessageDrawHook(void* regs)
{
    (void)regs;
    EnsureInit();

    if (g_Cfg.Enable && g_Cfg.ShowWindow)
    {
        DrawMessages();
        return ADDR_AFTER_DRAW_CALL;   /* 跳过原版 MessageListClass::Draw */
    }
    return 0;                          /* 执行原指令, 原版照旧显示 */
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

    /* --- 拼日志行 --- */
    p = AppendCh(line, p, '#', sizeof(line));
    p += UtoA(g_Seq, line + p);

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

    nameLen = WcsLenLimited(argName, MAX_TEXT_CHARS);
    textLen = WcsLenLimited(argMsg,  MAX_TEXT_CHARS);

    (void)b1; (void)b2; (void)b3; (void)b4; (void)b5; (void)b6;

    if (g_Cfg.LogRaw)
    {
        p = AppendCStr(line, p, " nameLen=", sizeof(line));
        p += UtoA(nameLen, line + p);
        p = AppendCStr(line, p, " textLen=", sizeof(line));
        p += UtoA(textLen, line + p);
    }

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

    /* --- 阶段 2: 存进内存历史 ---
     * FilterSilent=1 时, 操作提示(silent=1) 不进消息框 —— 快捷键提示频率极高,
     * 放进来会把真正要看的内容冲掉。它们仍然照常写日志。 */
    if (!(g_Cfg.FilterSilent && argSP))
    {
        MsgPush(argName, argMsg, (int)argColor, retAddr,
                *(unsigned*)ADDR_CURRENT_FRAME, (int)argTimeout, (int)argSP);
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
