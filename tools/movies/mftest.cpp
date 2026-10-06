#include <windows.h>
#include <stdio.h>
extern "C" void F4SilenceVoices() {}
void PlayMovieMF(const char *filename, HWND game);
int main(int argc, char **argv)
{
    HWND w = CreateWindowA("STATIC", "g", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 960, 540, 0, 0, 0, 0);
    DWORD t = GetTickCount();
    PlayMovieMF(argv[1], w);
    printf("played %lu ms\n", GetTickCount() - t);
    return 0;
}
