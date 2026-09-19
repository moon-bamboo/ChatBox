/*
 * probe_house.cpp - 实测 HouseClass 里与"玩家颜色"相关的字段偏移
 *
 * 目的: ChatBox 要把【系统/剧情消息】改成用"当前玩家(我)的颜色"显示,
 *       需要一个可靠的取色途径。已知 HouseClass 有两个候选:
 *         Color             (ColorStruct) —— 可能直接就是 RGB
 *         ColorSchemeIndex  (int)         —— 颜色方案索引, 再查 ColorScheme 数组
 *       本探针把两者的偏移都钉死, 并确认 ColorStruct 的字段顺序。
 *
 * 编译: 同 AutoKit/probe/README.md, -I override 必须排在 -I YRpp 前
 */

#include <cstdio>
#include <cstddef>

#include "gcc_compat.h"
#include <YRPPCore.h>
#include <HouseClass.h>
#include <GeneralStructures.h>

int main()
{
    printf("=== HouseClass 颜色相关字段实测 ===\n\n");

    printf("sizeof(HouseClass)                        = 0x%X (%u)\n",
           (unsigned)sizeof(HouseClass), (unsigned)sizeof(HouseClass));
    printf("\n");

    printf("offsetof(HouseClass, Color)               = 0x%X\n",
           (unsigned)offsetof(HouseClass, Color));
    printf("offsetof(HouseClass, LaserColor)          = 0x%X\n",
           (unsigned)offsetof(HouseClass, LaserColor));
    printf("offsetof(HouseClass, ColorSchemeIndex)    = 0x%X   <-- 候选 A\n",
           (unsigned)offsetof(HouseClass, ColorSchemeIndex));
    printf("\n");

    /* 顺带把 AutoKit 已实测过的几个锚点打出来, 用于交叉验证本次探针可信 */
    printf("--- 交叉验证(应与 AutoKit 的实测一致) ---\n");
    printf("offsetof(HouseClass, Balance)             = 0x%X  (期望 0x30C)\n",
           (unsigned)offsetof(HouseClass, Balance));
    printf("offsetof(HouseClass, ArrayIndex)          = 0x%X  (期望 0x30)\n",
           (unsigned)offsetof(HouseClass, ArrayIndex));
    printf("offsetof(HouseClass, Buildings)           = 0x%X  (期望 0x68)\n",
           (unsigned)offsetof(HouseClass, Buildings));
    printf("\n");

    printf("sizeof(ColorStruct)                       = 0x%X\n", (unsigned)sizeof(ColorStruct));
    {
        ColorStruct c;
        unsigned char* p = (unsigned char*)&c;
        c.R = 0x11; c.G = 0x22; c.B = 0x33;
        printf("ColorStruct 内存序: [0]=0x%02X [1]=0x%02X [2]=0x%02X (期望 11/22/33)\n",
               p[0], p[1], p[2]);
    }

    return 0;
}
