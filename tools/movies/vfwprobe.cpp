// vfwprobe: can this machine decode an AVI's video stream through classic VFW, the way the game does
// (src/movie/avimovie.cpp: ICDecompressOpen on the stream's FourCC, output RGB555)?
//   vfwprobe.exe file.avi [frame]
#include <windows.h>
#include <vfw.h>
#include <stdio.h>
#pragma comment(lib, "vfw32.lib")

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;

    AVIFileInit();
    PAVISTREAM vs = NULL;
    HRESULT hr = AVIStreamOpenFromFileA(&vs, argv[1], streamtypeVIDEO, 0, OF_READ, NULL);

    if (hr != 0 || !vs)
    {
        printf("%s: cannot open video stream (0x%08lx)\n", argv[1], (unsigned long)hr);
        return 1;
    }

    AVISTREAMINFOA si;
    AVIStreamInfoA(vs, &si, sizeof(si));
    char fcc[5] = {(char)(si.fccHandler & 255), (char)((si.fccHandler >> 8) & 255),
                   (char)((si.fccHandler >> 16) & 255), (char)((si.fccHandler >> 24) & 255), 0};
    BITMAPINFOHEADER want;
    ZeroMemory(&want, sizeof(want));
    want.biSize = sizeof(want);
    want.biWidth = si.rcFrame.right - si.rcFrame.left;
    want.biHeight = si.rcFrame.bottom - si.rcFrame.top;
    want.biPlanes = 1;
    want.biBitCount = 16;
    want.biCompression = BI_RGB;
    PGETFRAME gf = AVIStreamGetFrameOpen(vs, &want);

    if (!gf)
    {
        printf("%s: handler '%s' %ldx%ld -> NO DECODER (AVIStreamGetFrameOpen failed)\n", argv[1], fcc,
               (long)want.biWidth, (long)want.biHeight);
        return 1;
    }

    long frame = argc > 2 ? atoi(argv[2]) : 30;
    LPBITMAPINFOHEADER bi = (LPBITMAPINFOHEADER)AVIStreamGetFrame(gf, frame);

    if (!bi)
    {
        printf("%s: handler '%s' -> decoder opened but frame %ld failed\n", argv[1], fcc, frame);
        return 1;
    }

    unsigned char *px = (unsigned char *)bi + bi->biSize;
    long sum = 0, n = bi->biSizeImage < 4096 ? bi->biSizeImage : 4096;

    for (long i = 0; i < n; i++)
        sum += px[i];

    printf("%s: handler '%s' %ldx%ld %d-bit, frames %ld -> DECODES (frame %ld ok, sample sum %ld)\n", argv[1], fcc,
           (long)bi->biWidth, (long)bi->biHeight, bi->biBitCount, (long)si.dwLength, frame, sum);
    AVIStreamGetFrameClose(gf);
    AVIStreamRelease(vs);
    AVIFileExit();
    return 0;
}
