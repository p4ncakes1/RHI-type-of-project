#include "fatal_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif

void fatal_error(const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fprintf(stderr, "FATAL ERROR: %s\n", buffer);
#ifdef _WIN32
    MessageBoxA(NULL, buffer, "Fatal Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    ExitProcess(1);
#elif __APPLE__
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "osascript -e 'display alert \"Fatal Error\" message \"%s\"'", buffer);
    system(cmd);
    exit(1);
#else
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "zenity --error --title=\"Fatal Error\" --text=\"%s\" 2>/dev/null", buffer);
    system(cmd);
    exit(1);
#endif
}