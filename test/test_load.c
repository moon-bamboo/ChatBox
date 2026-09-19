/*
 * test_load.c - ChatBox.dll 加载验证（32 位测试程序）
 *
 * 32 位 DLL 无法用 64 位 PowerShell 的 LoadLibrary 验证
 * （会返回 err=193 ERROR_BAD_EXE_FORMAT），所以单独编个 32 位程序。
 *
 * 验证内容：
 *   1. DLL 能否被 LoadLibrary 加载（DllMain 不崩）
 *   2. 导出函数是否都在
 *   3. SyringeHandshake 能否正常调用并返回 S_OK
 *      —— 顺带触发 BuildPaths + LoadConfig + LogOpen，
 *         可检查 <本程序目录>\MsgLog\*.log 是否生成
 *
 * 注意：阶段 1 的 ChatBox 没有帧钩子，唯一的钩子是
 *       ChatBox_AddMessageHook（由 .inj 声明到 0x5D3BA0）。
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

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

typedef long (__cdecl *handshake_fn)(void*);

int main(void)
{
    HMODULE h;
    FARPROC fn;
    char buf[256];
    SyringeHandshakeInfo info;
    long hr;
    int fail = 0;

    printf("== ChatBox.dll load test ==\n\n");

    h = LoadLibraryA("ChatBox.dll");
    if (!h) { printf("[FAIL] LoadLibrary, GetLastError=%lu\n", GetLastError()); return 1; }
    printf("[PASS] LoadLibrary OK, base = %p\n", (void*)h);

    fn = GetProcAddress(h, "ChatBox_AddMessageHook");
    printf("%s ChatBox_AddMessageHook = %p\n", fn ? "[PASS]" : "[FAIL]", (void*)fn);
    if (!fn) fail++;

    fn = GetProcAddress(h, "SyringeHandshake");
    printf("%s SyringeHandshake       = %p\n", fn ? "[PASS]" : "[FAIL]", (void*)fn);
    if (!fn) { fail++; }
    else {
        memset(&info, 0, sizeof(info));
        memset(buf, 0, sizeof(buf));
        info.cbSize = sizeof(info);
        info.cchMessage = sizeof(buf);
        info.Message = buf;

        hr = ((handshake_fn)fn)(&info);
        printf("%s SyringeHandshake -> hr = 0x%08lX (期望 0)\n", hr == 0 ? "[PASS]" : "[FAIL]", hr);
        printf("       message = \"%s\"\n", buf);
        if (hr != 0) fail++;
    }

    FreeLibrary(h);
    printf("\nfailures: %d\n", fail);
    return fail;
}
