/*
 * offsets.h - ChatBox 引擎地址常量
 *
 * 目标引擎: gamemd.exe (尤里的复仇 1.001, CRC32 = 0x54CC0A13)
 * 与 AutoKit 同一个游戏版本, 地址可互相参照。
 *
 * 来源:
 *   - YRpp/MessageListClass.h (类结构与函数地址)
 *   - YRpp/Syringe.h (REGISTERS 结构)
 *   - objdump 实测反汇编校验 (见 开发记录.md)
 */

#pragma once

/* ============ 引擎结构（只声明本插件用到的几个）============
 *
 * 刻意不复用 YRpp 的头: 那些是为 MSVC 写的 C++ 类, GCC 下要一堆 override
 * (AutoKit 的 probe/ 里踩过), 而我们只用几个纯 POD 结构, 自己写更省事。
 *
 * 字段顺序/类型与 YRpp 一致:
 *   Point2D         YRpp/GeneralDefinitions.h
 *   RectangleStruct 同上 (X, Y, Width, Height)
 *   ColorStruct     YRpp/GeneralStructures.h (R, G, B)
 */

typedef struct { int X, Y; } Point2D;
typedef struct { int X, Y, Width, Height; } RectangleStruct;
typedef struct { unsigned char R, G, B; } ColorStruct;

/* ============ 函数地址 ============ */

/* MessageListClass::AddMessage —— 游戏里【所有】文本消息的唯一入口。
 * 玩家聊天、触发器文本、快捷键提示("选中画面相同单位")、超武提示全走这里。
 *
 * __thiscall:
 *   ECX = this (= MessageListClass::Instance)
 *   栈  = 7 个参数 (见下)
 *
 * 签名 (YRpp/MessageListClass.h:30):
 *   TextLabelClass* AddMessage(const wchar_t* Name, int ID, const wchar_t* Message,
 *                              int ColorSchemeIdx, TextPrintType Style,
 *                              int Timeout, bool SinglePlayer);
 *
 * 实测: 该地址是一个干净的函数入口, 首指令为
 *       81 EC 4C 01 00 00   sub $0x14c,%esp   (正好 6 字节, 可下 6 字节钩子)
 *       前面 0x5D3B92~0x5D3B9F 是 14 个 0x90(nop) 填充。
 */
#define ADDR_ADD_MESSAGE            0x005D3BA0u

/* MessageListClass::Instance (全局单例, 同 AddMessage 的 ECX) */
#define ADDR_MESSAGELIST_INSTANCE   0x00A8BC60u

/* HouseClass::CurrentPlayer */
#define ADDR_CURRENT_PLAYER         0x00A83D4Cu

/* HouseClass::ColorSchemeIndex —— 该玩家使用的颜色方案索引。
 * 由 probe/probe_house.cpp 实测 = 0x16054 (同次探针的三个锚点
 * Balance=0x30C / ArrayIndex=0x30 / Buildings=0x68 与 AutoKit 一致, 可信)。 */
#define OFF_HOUSE_COLORSCHEME       0x16054

/* 游戏主循环每逻辑帧入口 (AutoKit 同址; 将来画窗口/处理输入用) */
#define ADDR_FRAME_HOOK             0x0055D360u

/* 绘制接管 —— 钩在【调用点】而不是被调函数里 (objdump 实测):
 *
 *   4f4558: b9 60 bc a8 00   mov  $0xa8bc60,%ecx     ; MessageListClass::Instance
 *   4f455d: e8 3e 04 0e 00   call 0x5d49a0          ; ← 钩这里, 正好 5 字节
 *   4f4562: 8b 0d 68 73 88 00                        ; 返回后继续
 *
 * 为什么钩调用点而不是 MessageListClass::Draw(0x5D49A0) 本身:
 *   1. Draw 的入口是 sub $0x24,%esp / push %esi / mov %ecx,%esi, 要"跳过整个
 *      函数"就得手工补栈清理, 容易出错;
 *   2. 钩调用点只需 return 0x4F4562 即可干净跳过, 无需碰栈。
 *
 * 这样做的最大好处: 原版流程【一点都不动】——
 *   AddMessage 照常执行, 所以消息照常进链表、照常超时消失、提示音照常播放,
 *   我们只是不让它画出来。
 *   (Phobos 的做法是让消息根本不进原版链表, 代价是得自己补提示音。)
 */
#define ADDR_DRAW_CALL_SITE         0x004F455Du
#define ADDR_AFTER_DRAW_CALL        0x004F4562u
#define HOOK_SIZE_DRAW_CALL         5
#define ADDR_MESSAGELIST_DRAW       0x005D49A0u   /* 被跳过的函数本身(备查) */

/* 绘制接管点: GScreenClass::NewMessageListDraw (Phobos 同址)
 * 实测指令: A1 1C 73 88 00   mov 0x88731c,%eax   (5 字节)
 * 位于上面那个 call 之后, 本插件【不使用】它 —— 留作参考。 */
#define ADDR_NEW_MESSAGE_DRAW       0x004F4589u

/* 生命周期接管点: MainLoop_FrameStep_NewMessageListManage (Phobos 同址)
 * 实测指令: B9 60 BC A8 00   mov $0xa8bc60,%ecx   (5 字节) */
#define ADDR_MESSAGE_MANAGE         0x0055DDA0u

/* ============ REGISTERS 结构 ============
 *
 * Syringe 把指向 REGISTERS 的指针作为参数传给钩子函数。
 * 布局见 YRpp/Syringe.h:115 (private 成员顺序即内存顺序):
 *
 *   +0x00  DWORD origin
 *   +0x04  DWORD flags
 *   +0x08  _EDI
 *   +0x0C  _ESI
 *   +0x10  _EBP
 *   +0x14  _ESP   <-- 我们需要的就是这个
 *   +0x18  _EBX
 *   +0x1C  _EDX
 *   +0x20  _ECX
 *   +0x24  _EAX
 *   ---- 共 0x28 字节 ----
 */
#define OFF_REG_ESP                 0x14
#define OFF_REG_ECX                 0x20   /* +0x18 EBX, +0x1C EDX, +0x20 ECX, +0x24 EAX */

/* ============ AddMessage 的栈参数偏移 ============
 *
 * 我们在【函数入口】下钩, 此时 sub $0x14c,%esp 尚未执行,
 * 所以 ESP 正好指向返回地址:
 *
 *   ESP + 0x00  返回地址 (调用者 = 消息是谁发的, 用于分流)
 *   ESP + 0x04  参数 1  const wchar_t* Name          消息来源(通常为发言者名字)
 *   ESP + 0x08  参数 2  int            ID
 *   ESP + 0x0C  参数 3  const wchar_t* Message       消息正文
 *   ESP + 0x10  参数 4  int            ColorSchemeIdx 颜色方案索引
 *   ESP + 0x14  参数 5  int            Style           TextPrintType
 *   ESP + 0x18  参数 6  int            Timeout         超时帧数(-1 = 永不消失)
 *   ESP + 0x1C  参数 7  int            SinglePlayer
 */
#define STK_RETURN_ADDR             0x00
#define STK_ARG_NAME                0x04
#define STK_ARG_ID                  0x08
#define STK_ARG_MESSAGE             0x0C
#define STK_ARG_COLORSCHEME         0x10
#define STK_ARG_STYLE               0x14
#define STK_ARG_TIMEOUT             0x18
#define STK_ARG_SINGLEPLAYER        0x1C

/* 覆盖长度: sub $0x14c,%esp 是 6 字节 */
#define HOOK_SIZE_ADD_MESSAGE       6

/* ============ 绘制相关（阶段 2）============
 *
 * 全部来自 YRpp/Surface.h, 并用 objdump 反汇编交叉验证过。
 */

/* 全局当前帧号 (AutoKit 同址), 用来判断消息是否超时 */
#define ADDR_CURRENT_FRAME          0x00A8ED84u

/* DSurface 的几个画布指针 (YRpp/Surface.h:207-213)
 * 反汇编 0x4F4589 处的原版绘制代码为  mov 0x88731c,%eax  ——  正是 Composite,
 * 说明该处就是"画消息列表"的地方。 */
#define ADDR_DSURFACE_COMPOSITE     0x0088731Cu
#define ADDR_DSURFACE_TEMP          0x00887314u
#define ADDR_DSURFACE_PRIMARY       0x00887308u

/* 战场可视区域矩形 (YRpp/Surface.h:216) */
#define ADDR_DSURFACE_VIEWBOUNDS    0x00886FA0u

/* BitFont::Instance (YRpp/BitFont.h:8) */
#define ADDR_BITFONT_INSTANCE       0x0089C4D0u

/* Point2D* Simple_Text_Print_Wide(Point2D* ret, const wchar_t* text, Surface* surf,
 *                                 RectangleStruct* bounds, Point2D* loc,
 *                                 unsigned foreColor, unsigned backColor,
 *                                 int flags, int unused)      —— stdcall
 * 底层文本绘制 (YRpp/Surface.h:195)
 *
 * 另一条路是 Fancy_Text_Print_Wide @ 0x4A60E0（Surface::DrawText 内部走它），
 * 参数里有 C++ 引用, 用函数指针表达不方便, 故本插件用上面这个。 */
#define ADDR_SIMPLE_TEXT_PRINT      0x004A5EB0u

/* Fancy_Text_Print_Wide 的【ColorScheme 重载】@ 0x4A61C0 —— 原版画消息文本用的就是它。
 *
 * 实测原版 MessageListClass::Draw (0x5D49A0) 的调用现场:
 *     mov  0x34(%eax),%edx       ; 消息的颜色方案索引
 *     mov  0xb054d4,%eax         ; ColorScheme::Array.Items
 *     mov  (%eax,%edx,4),%ecx    ; ecx = Items[索引] = ColorScheme*
 *     push %ecx                  ; ★ 传的是【指针】, 不是 RGB 值
 *     ...
 *     call 0x4a61c0
 *     add  $0x20,%esp            ; 8 个栈参数, 调用方清栈 => __cdecl
 *
 * 签名 (YRpp/Surface.h:183, 引用在 ABI 上即指针):
 *   Point2D* Fancy_Text_Print_Wide(Point2D* ret, const wchar_t* text, Surface* surf,
 *                                  RectangleStruct* bounds, Point2D* loc,
 *                                  ColorScheme* fore, ColorScheme* back,
 *                                  TextPrintType flags)
 *
 * 为什么必须走这条: 另一条路 0x4A5EB0 接受"颜色 dword", 但我把算好的
 * (255,240,0)=黄色 传进去却显示成粉紫色 —— 那条路的颜色语义与 RGB/COLORREF
 * 都不是简单对应。改用 ColorScheme* 后颜色换算全交给引擎, 且天然与原版一致。 */
#define ADDR_FANCY_TEXT_CS          0x004A61C0u

/* BitFont 折行/截断: 算"这段文本在给定像素宽度下能放几个字符"
 *   int __thiscall 0x433F50(BitFont* this, const wchar_t* text,
 *                           int width, int a4, int a5)
 * 实测: 入口 `mov %ecx,%edx`(ECX 传 this), 结尾 `ret $0x10`(4 个栈参数)。
 * a4/a5 原版传 111 与 1(Phobos 同)。返回可容纳的字符数。 */
#define ADDR_TEXT_CHARS_FIT         0x00433F50u

/* ============ 鼠标交互（阶段 3b）============ */

/* WWMouseClass::Instance (YRpp/WWMouseClass.h:14)
 * 其 XY1 字段是鼠标的【游戏绘制坐标】, 与 DSurface::Composite 同一坐标系
 * (Phobos 就是这么用它与消息区做命中判断的)。 */
#define ADDR_WWMOUSE_INSTANCE       0x00887640u
/* XY1 偏移: vptr(4) + Image(4) + ImageFrameIndex(4) + RefCount(4)
 *           + field_10..13(4) + field_14(8) = 0x1C */
#define OFF_WWMOUSE_XY1             0x1C

/* 滚轮: Game_WndProc_ScrollMouseWheel
 * 实测 0x777998 处: shr $0x10,%ecx (3) + test %cx,%cx (3) = 6 字节, 边界整齐。
 * 进入时 ECX = WParam >> 16, 其 bit15 为 1 表示向下滚(与 Phobos 判据等价)。 */
#define ADDR_SCROLL_WHEEL_HOOK      0x00777998u
#define HOOK_SIZE_SCROLL_WHEEL      6

/* 侧栏滚动: 悬停在消息框上时要跳过它, 否则滚轮会同时滚动侧栏。
 * (Phobos 同址; 跳过目标 0x533FC3) */
#define ADDR_SCROLL_SIDEBAR         0x00533F50u
#define ADDR_SKIP_SCROLL_SIDEBAR    0x00533FC3u
#define HOOK_SIZE_SCROLL_SIDEBAR    5

/* 点击坐标处理: 命中消息框时跳到 DoNothing, 避免点击穿透到战场
 * (否则点消息框会同时选中/命令底下的单位)。
 * 实测 0x692419 处: movswl (%esi),%ecx (3) + movswl 0x2(%esi),%edx (4) = 7 字节。 */
#define ADDR_CLICK_COORDS_HOOK      0x00692419u
#define ADDR_CLICK_DONOTHING        0x006925FCu
#define HOOK_SIZE_CLICK_COORDS      7

/* ColorScheme 相关 (YRpp/ColorScheme.h) */
#define ADDR_COLORSCHEME_ARRAY      0x00B054D0u  /* DynamicVectorClass<ColorScheme*> */

/* ColorScheme::BaseColor —— 由 ChatBox/probe/probe_color.cpp 实测确认:
 *   offsetof(ColorScheme, BaseColor) = 0x308
 * 推导过程(后被实测证实): YRpp 里有个字段名叫 `unknown_314[0x1C]`, 名字自带偏移,
 * 由它反推 ShadeCount=0x310 / LightConvert=0x30C / BaseColor=0x308 / ID=0x304,
 * 而 Colors(BytePalette, 768 字节) 从 0x004 起正好到 0x304 —— 完全自洽。
 *
 * 同次探针还确认了: sizeof(ColorStruct)=3, 字段顺序 R@+0, G@+1, B@+2。
 * 所以 COLORREF(0x00BBGGRR) = (B<<16)|(G<<8)|R。 */
#define OFF_COLORSCHEME_BASECOLOR   0x308
/* 某颜色方案 -> 实际显示颜色  (thiscall: ECX=&dst, 栈=&src)  Phobos 用法见
 * MessageColumn.cpp:534 —— ((ColorStruct*(*)(ColorStruct*, ColorStruct*))0x517440)(&src,&dst) */
#define ADDR_COLORSCHEME_TO_COLOR   0x00517440u

/* 虚表偏移 (objdump 实测: 0x623AA8 处 call *0x14(%edx) 即 FillRect)
 *
 *   Surface 的虚函数顺序: 0=~Surface 1=CopyFromWhole 2=CopyFromPart 3=CopyFrom
 *                        4=FillRectEx 5=FillRect 6=Fill 7=FillRectTrans ...
 *   注意 XSurface / DSurface 的虚函数追加在后, 不影响前面这些索引。 */
#define VT_FILL_RECT                0x14   /* FillRect(RectangleStruct*, COLORREF) */
#define VT_FILL_RECT_TRANS          0x1C   /* FillRectTrans(RectangleStruct*, ColorStruct*, int opacity) */
#define VT_GET_RECT                 0x78   /* GetRect(RectangleStruct*) */

/* ============ 消息分类（由阶段 1 的实测日志确定）============
 *
 * 实测 56 条消息, ret(调用者返回地址) 与参数分布如下:
 *
 *   ret=0x00732BF5  cs=7  to=240   sp=1   21 次  快捷键提示("选取部队横越地图"/"未选取")
 *   ret=0x006DE127  cs=28 to=1259  sp=0   12 次  剧情/任务文本
 *   ret=0x00730AC8  cs=7  to=480   sp=1   12 次  路径点模式提示
 *   ret=0x004F9E01  cs=6  to=1259  sp=0    4 次  结盟消息
 *   ret=0x0055F0FA  cs=7  to=1259  sp=0    3 次  玩家聊天(本地)
 *   ret=0x0048D97E  cs=15 to=1259  sp=0    1 次  玩家聊天(远程)
 *   ret=0x004C79F4  cs=15 to=1259  sp=0    1 次  系统消息(改变游戏速度)
 *   ret=0x00430D31/6E cs=7 to=225  sp=0    2 次  通信信标提示
 *
 * ★ 最重要的规律: 第 7 个参数(sp) 并不是 YRpp 里写的 SinglePlayer, 而是 silent。
 *   所有"操作提示类"消息 (快捷键 / 路径点) 都是 sp=1,
 *   所有"真消息" (聊天 / 剧情 / 结盟 / 系统) 都是 sp=0 —— 零例外。
 *   => 用 silent 就能干净地把会淹没列表的快捷提示分流出去。
 *      (RA2 原始源码里这个参数就叫 silent; 它控制是否播放提示音。)
 */
#define RET_HOTKEY_HINT             0x00732BF5u  /* 快捷键提示 */
#define RET_WAYPOINT_HINT           0x00730AC8u  /* 路径点模式提示 */
