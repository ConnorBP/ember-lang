#include <cstdio>
#include <cstdint>
#include <windows.h>
static LONG WINAPI veh(EXCEPTION_POINTERS* ep) {
    fprintf(stderr, "VEH: code=0x%lx addr=%p rip=0x%llx rsp=0x%llx\n",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress,
        (unsigned long long)ep->ContextRecord->Rip,
        (unsigned long long)ep->ContextRecord->Rsp);
    if (ep->ExceptionRecord->ExceptionCode == 0xC000001D) { // STATUS_ILLEGAL_INSTRUCTION
        // Dump bytes at rip
        uint8_t* p = (uint8_t*)ep->ContextRecord->Rip;
        fprintf(stderr, "bytes at rip: ");
        for (int i = 0; i < 16; ++i) fprintf(stderr, "%02x ", p[i]);
        fprintf(stderr, "\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, (PVECTORED_EXCEPTION_HANDLER)veh);
    // exec the child
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s", argv[1]);
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    fprintf(stderr, "child exit: %lu\n", exitCode);
    return 0;
}
