#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h> 
#include <unistd.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <dirent.h>
#include <string.h>

#include "fbbmp.h"

// 값이 지정한 범위를 벗어나지 않도록 한다.
unsigned char thresholding(
    int value,
    const int rangeMin,
    const int rangeMax)
{
    if (value < rangeMin) value = rangeMin;
    if (value > rangeMax) value = rangeMax;

    return value;
}

// 픽셀의 밝기 값을 변화시킨다. 밝기 값의 범위는 0 ~ 255(unsigned char)이다.
RGBpixel changePixelBrightness(
    RGBpixel pixelBrightness,
    const int pixelBrightnessDelta)
{
    pixelBrightness.red = thresholding(pixelBrightness.red + pixelBrightnessDelta, UCHAR_MIN, UCHAR_MAX);
    pixelBrightness.green = thresholding(pixelBrightness.green + pixelBrightnessDelta, UCHAR_MIN, UCHAR_MAX);
    pixelBrightness.blue = thresholding(pixelBrightness.blue + pixelBrightnessDelta, UCHAR_MIN, UCHAR_MAX);

    return pixelBrightness;
}

// 24비트 RGB 픽셀을 32비트 ABGR 픽셀로 순서 변환과 함께 확장한다.
unsigned int convertRGB24toABGR32(const RGBpixel pixel)
{
    const unsigned int alpha = 0;   // 투명도
    
    return ((alpha << 24) | (pixel.blue << 0) | (pixel.green << 8) | (pixel.red << 16));
}

// 24비트 RGB 픽셀을 16비트 BGR 픽셀로 순서 변환과 함께 축소한다.
unsigned short convertRGB24toBGR16(const RGBpixel pixel)
{
	return ((pixel.blue << 11) | (pixel.green << 6) | (pixel.red << 0));
}

// 16비트 BGR 픽셀을 24비트 BGR 픽셀로 확장한다.
unsigned int convertBGR16toBGR24(const unsigned short pixel)
{
    // BGR 값을 추출하기 위한 마스크
    const unsigned short maskBlue = 65535-2047;
    const unsigned short maskGreen = 2047-31;
    const unsigned short maskRed = 31;

    // BGR 값 추출
    unsigned short blue = pixel & maskBlue;    // 11111 000000 00000
    unsigned short green = pixel & maskGreen;  // 00000 111111 00000
    unsigned short red = pixel & maskRed;      // 00000 000000 11111

    // 공백 제거
    blue = blue >> 11;                         // 00000 000000 11111
    green = green >> 6;                        // 00000 000000 11111
    red = red >> 0;                            // 00000 000000 11111

    // 24비트에 맞춰 확장 준비
    blue = blue << 3;                          // 00000 000111 11000
    green = green << 3;                        // 00000 000111 11000
    red = red << 3;                            // 00000 000111 11000

    return ((blue << 16) | (green << 8) | (red << 0));
}

// SIGINT를 받으면 호출되는 함수이다. 무한 반복문을 종료하기 위해 사용한다.
void signalCallbackQuit(const int sig) 
{
    quit = true;
    exit(1);
}

// 입력한 경로에서 특정 확장자를 가진 파일을 수집한다.
void searchFilesInPathByExtention(
    unsigned char *pFileNameArray[FILE_NAME_ARRAY_SIZE],
    const char *pTargetPath,
    const char *pTargetExtension)
{
    // 디렉토리 열기
    DIR *pDIR = opendir(pTargetPath);
    if (!pDIR)
    {
        perror("Failed to open directory.");
        exit(1);
    }

    // 디렉토리 탐색
    int fileNameIndex = 0;
    struct dirent *pDirent;
    while (pDirent = readdir(pDIR)) 
    {
        // 현재 탐색중인 파일명 얻기
        char *pCurrentFileName = pDirent->d_name;

        // 현재 디렉토리, 부모 디렉토리는 무시
        if (!strcmp(pCurrentFileName, ".")) continue;
        if (!strcmp(pCurrentFileName, "..")) continue;

        // 찾고 있던 확장자인지 확인
        const char *pCurrentExtension = strrchr(pCurrentFileName, '.');
        if (!pCurrentExtension) continue;
        if (strcmp(pCurrentExtension + 1, pTargetExtension)) continue;

        //파일 목록 수집
        pFileNameArray[fileNameIndex++] = pCurrentFileName;
    }

    closedir(pDIR);
}

// 프레임 버퍼 크기 구하기
int calculateFrameBufferSize(const struct fb_var_screeninfo fbvar)
{
    // 가로 * 세로 * BPP에 따른 바이트 크기 (32BPP : 4Bytes / 16BPP : 2Bytes)
    return fbvar.xres_virtual * fbvar.yres_virtual * (frameBufferBPP / 8);
}

// 프레임 버퍼 비우기
void clearFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar)
{
    memset(pfbmap, 0, calculateFrameBufferSize(fbvar));
}

// 프레임 버퍼에 이미지 출력
void drawImageOnFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *pBitmapHeader,
    RGBpixel **pBitmapPixel2dArray,
    const int brightness)
{
    // 이미지 크기가 화면 밖을 벗어나는 경우에 대비해
    // 화면과 이미지 중 더 작은 너비, 높이 값을 사용한다.
    const int minHeight = pBitmapHeader->biHeight;
    const int minWidth = pBitmapHeader->biWidth;

    // 프레임 버퍼를 탐색한다.
    for (int rowIndex = 0; rowIndex < minHeight; rowIndex++)
    {
        for (int columnIndex = 0; columnIndex < minWidth; columnIndex++)
        {
            // 밝기 조절 기능에 의해 변경된 밝기 값을 반영한다.
            RGBpixel pixelRGB = changePixelBrightness(pBitmapPixel2dArray[rowIndex][columnIndex], brightness);

            // 현재 탐색중인 프레임 버퍼 위치 : 프레임 버퍼 너비 * 행 번호 + 열 번호
            int offset = fbvar.xres_virtual * rowIndex + columnIndex;

            // int *pFrameBufferIndex = pfbmap + ((fbvar.xres_virtual) * rowIndex) + columnIndex;
            
            // // 24BPP인 기존 비트맵 이미지를 설정한 BPP에 맞게 프레임 버퍼에 출력한다.
            // *pFrameBufferIndex = (frameBufferBPP == BPP_16) ? convertRGB24toBGR16(pixelRGB) : convertRGB24toABGR32(pixelRGB);
        
            *(pfbmap + offset) = convertRGB24toABGR32(pixelRGB);
        }
    }
}

// 프레임 버퍼에서 이미지를 읽어와 24BPP 비트맵 파일에 저장한다.
void captureFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *pBitmapHeader)
{
    // 기존에 캡처된 파일이 있다면 지운다.
    const int checkFileExistence = access(OUTPUT_BITMAP_FILE_NAME, F_OK);
    if (checkFileExistence >= 0)
    {
        unlink(OUTPUT_BITMAP_FILE_NAME);
    }

    // 캡처된 이미지를 저장하기 위해 파일을 만들어 연다.
    int fdBitmapOutput = open(OUTPUT_BITMAP_FILE_NAME, O_CREAT | O_WRONLY, 0777);
    if (fdBitmapOutput < 0)
    {
        perror("Failed to create output image file.");
        exit(1);
    }

    // 비트맵 헤더 복사
    BMPHeader *pBitmapOutputHeader = (BMPHeader*)malloc(BITMAP_HEADER_SIZE);
    memcpy(pBitmapOutputHeader, pBitmapHeader, BITMAP_HEADER_SIZE);

    // 이미지 크기가 화면 밖을 벗어나는 경우
    // 화면 크기에 맞게 이미지를 자르기 위해 비트맵 헤더를 수정한다.
    const int minHeight = fbvar.yres_virtual;
    const int minWidth = fbvar.xres_virtual;

    pBitmapOutputHeader->bfSize = minWidth * minHeight * (24 / 8) + BITMAP_HEADER_SIZE;  // 파일 크기
    pBitmapOutputHeader->biSizeImage = minWidth * minHeight * (24 / 8);                  // 비트맵 이미지의 픽셀 데이터 크기
    pBitmapOutputHeader->biWidth = minWidth;                                             // 비트맵 이미지의 가로 크기
    pBitmapOutputHeader->biHeight = minHeight;                                           // 비트맵 이미지의 세로 크기
    pBitmapOutputHeader->biBitCount = BITMAP_DEFAULT_BPP;                                // BPP(Bits Per Pixel)

    // 비트맵 헤더를 파일에 쓴다.
    int writeBytes = write(fdBitmapOutput, pBitmapOutputHeader, sizeof(BMPHeader));
    if (writeBytes < 0)
    {
        perror("Failed to write bitmap header.");
        exit(1);
    }

    // 프레임 버퍼 탐색
    for (int rowIndex = minHeight - 1; rowIndex >= 0; rowIndex--)
    {
        for (int columnIndex = 0; columnIndex < minWidth; columnIndex++)
        {
            // 현재 탐색중인 프레임 버퍼 위치 : 프레임 버퍼 너비 * 행 번호 + 열 번호
            unsigned int *pFrameBufferIndex = pfbmap + (fbvar.xres_virtual * rowIndex) + columnIndex;

            // 픽셀 값을 읽어온다. 
            unsigned int fixelRGBValue = *pFrameBufferIndex;

            // 16BPP 프레임 버퍼를 캡처할 경우 24BPP 이미지로 픽셀을 확장한다.
            if (frameBufferBPP == BPP_16)
            {
                fixelRGBValue = convertBGR16toBGR24(fixelRGBValue);
            }

            // 읽어온 픽셀 값을 24BPP 비트맵 파일에 저장한다.
            writeBytes = write(fdBitmapOutput, &fixelRGBValue, BITMAP_DEFAULT_BPP / 8);
            if (writeBytes < 0)
            {
                perror("Failed to write bitmap pixel.");
                exit(1);
            }
        }
    }

    close(fdBitmapOutput);
}

// 비트맵 파일의 헤더와 이미지를 읽고 동적 할당하여 매개변수로 포인터를 전달한다.
void loadBitmapImage(
    const unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader **pReturnBitmapHeader,
    RGBpixel ***pReturnBitmapPixel2dArray,
    const char *pFileName)
{
    // 이미지 파일 열기
    const int fdBitmapInput = open(pFileName, O_RDWR);
    if (fdBitmapInput < 0)
    {
        perror("Failed to open image file.");
        exit(1);
    }

    // 비트맵 헤더를 저장할 공간을 동적 할당 (54 Bytes)
    BMPHeader *pBitmapHeader = (BMPHeader*)malloc(BITMAP_HEADER_SIZE);

    // 비트맵 헤더 읽기
    int readBytes = read(fdBitmapInput, pBitmapHeader, BITMAP_HEADER_SIZE);
    if (readBytes < 0)
    {
        perror("Failed to read bitmap header.");
        exit(1);
    }
    
    // 비트맵 이미지를 저장할 공간을 동적 할당 (헤더 54 Bytes를 제외한 나머지 크기)
    RGBpixel **pBitmapPixel2dArray = (RGBpixel**)malloc(pBitmapHeader->biWidth * pBitmapHeader->biHeight);

    // 비트맵 이미지는 위아래가 뒤집어져 있으므로 역순으로 읽는다.
    for (int rowIndex = pBitmapHeader->biHeight-1; rowIndex >= 0; rowIndex--)
    {
        // 배열을 사용하기 위해 각 행마다 다시 동적 할당한다. (가로 픽셀 수 * 픽셀 바이트 크기)
        pBitmapPixel2dArray[rowIndex] = (RGBpixel*)malloc(sizeof(RGBpixel) * pBitmapHeader->biWidth);

        for (int columnIndex = 0; columnIndex < pBitmapHeader->biWidth; columnIndex++)
        {
            // 비트맵 픽셀 크기만큼 이미지를 읽는다.
            readBytes = read(fdBitmapInput, &pBitmapPixel2dArray[rowIndex][columnIndex], sizeof(RGBpixel));
            if (readBytes < 0)
            {
                perror("Failed to read bitmap pixel.");
                exit(1);
            }
        }
        
        // 비트맵 너비가 4의 배수가 아닐 경우 4의 배수를 채우기 위해 패딩 바이트가 들어간다.
        int paddingBytes = pBitmapHeader->biWidth % 4;
        while (paddingBytes > 0)
        {
            // 패딩 바이트를 버린다.
            unsigned char trashcan = ' ';
            readBytes = read(fdBitmapInput, &trashcan, 1);
            paddingBytes--;
        }
    }

    // 동적 할당한 주소를 넘겨준다.
    *pReturnBitmapHeader = pBitmapHeader;
    *pReturnBitmapPixel2dArray = pBitmapPixel2dArray;

    close(fdBitmapInput);
}