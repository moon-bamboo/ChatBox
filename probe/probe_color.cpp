/*
 * probe_color.cpp - 实测 ColorScheme 的字段偏移
 *
 * 背景: ChatBox 要给"玩家聊天"按发言者颜色着色, 需要从 ColorScheme 对象里
 *       取出 BaseColor。而 BaseColor 的偏移在 YRpp 里没有现成的 static_assert,
 *       我方原先是从 `unknown_314[0x1C]` 这个"名字里带偏移"的字段反推出来的
 *       (推测 0x308), 属于推导值而非实测值 —— 本探针就是为了把它钉死。
 *
 * 编译方法见 AutoKit/probe/README.md:
 *   -I<override 目录> 必须排在 -I<YRpp> 前面
 */

#include <cstdio>
#include <cstddef>

#include "gcc_compat.h"
#include <YRPPCore.h>
#include <ColorScheme.h>
#include <GeneralStructures.h>

int main()
{
    printf("=== ColorScheme 布局实测 ===\n\n");

    printf("sizeof(ColorScheme)                    = 0x%X  (%u)\n",
           (unsigned)sizeof(ColorScheme), (unsigned)sizeof(ColorScheme));
    printf("sizeof(ColorStruct)                    = 0x%X  (%u)\n",
           (unsigned)sizeof(ColorStruct), (unsigned)sizeof(ColorStruct));
    printf("sizeof(BytePalette)                    = 0x%X  (%u)\n",
           (unsigned)sizeof(BytePalette), (unsigned)sizeof(BytePalette));
    printf("\n");

    printf("offsetof(ColorScheme, ArrayIndex)      = 0x%X\n", (unsigned)offsetof(ColorScheme, ArrayIndex));
    printf("offsetof(ColorScheme, Colors)          = 0x%X\n", (unsigned)offsetof(ColorScheme, Colors));
    printf("offsetof(ColorScheme, ID)              = 0x%X\n", (unsigned)offsetof(ColorScheme, ID));
    printf("offsetof(ColorScheme, BaseColor)       = 0x%X   <-- 我们要的\n",
           (unsigned)offsetof(ColorScheme, BaseColor));
    printf("offsetof(ColorScheme, LightConvert)    = 0x%X\n", (unsigned)offsetof(ColorScheme, LightConvert));
    printf("offsetof(ColorScheme, ShadeCount)      = 0x%X\n", (unsigned)offsetof(ColorScheme, ShadeCount));
    printf("offsetof(ColorScheme, MainShadeIndex)  = 0x%X\n", (unsigned)offsetof(ColorScheme, MainShadeIndex));
    printf("\n");

    /* ColorStruct 的字段顺序 (决定 COLORREF 怎么拼) */
    ColorStruct cs;
    cs.R = 0x11; cs.G = 0x22; cs.B = 0x33;
    {
        unsigned char* p = (unsigned char*)&cs;
        printf("ColorStruct 内存布局: [0]=0x%02X [1]=0x%02X [2]=0x%02X\n", p[0], p[1], p[2]);
        printf("  => R 在 +0, G 在 +1, B 在 +2 (若输出 11/22/33)\n");
        printf("  => COLORREF(0x00BBGGRR) = (B<<16)|(G<<8)|R\n");
    }

    return 0;
}
