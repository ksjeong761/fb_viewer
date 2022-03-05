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
    pixelBrightness.red = thresholding(pixelBrightness.red + pixelBrightnessDelta, 0, 255);
    pixelBrightness.green = thresholding(pixelBrightness.green + pixelBrightnessDelta, 0, 255);
    pixelBrightness.blue = thresholding(pixelBrightness.blue + pixelBrightnessDelta, 0, 255);

    return pixelBrightness;
}

// 24비트 RGB 픽셀을 32비트 ABGR 픽셀로 순서 변환과 함께 확장한다.
unsigned int convertRGB24toABGR32(const RGBpixel pixel)
{
    const unsigned int alpha = 0;   // 투명도
    
    return ((alpha << 24) | (pixel.blue << 16) | (pixel.green << 8) | (pixel.red << 0));
}

// 24비트 RGB 픽셀을 16비트 BGR 픽셀로 순서 변환과 함께 축소한다.
unsigned short convertRGB24toBGR16(RGBpixel pixel)
{
	return ((pixel.blue << 11) | (pixel.green << 6) | (pixel.red << 0));
}

// 16비트 BGR 픽셀을 24비트 BGR 픽셀로 확장한다.
unsigned int convertBGR16toBGR24(unsigned short pixel)
{
    // BGR 값을 추출하기 위한 마스크
    unsigned short maskBlue = 65535-2047;
    unsigned short maskGreen = 2047-31;
    unsigned short maskRed = 31;

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

//입력한 경로에서 특정 확장자를 가진 파일을 수집한다.
void searchFilesInPathByExtention(
    unsigned char *fileNamesReference[FILE_NAME_ARRAY_SIZE],
    const char *targetPath,
    const char *targetExtension)
{
    DIR *dp;                    //디렉토리를 열고 읽기 위한 포인터
    struct dirent *dent;        //디렉토리 정보를 저장할 포인터

    char *currentFileName;      //현재 탐색중인 파일명을 나타낸다.
    char *currentExtension;     //현재 탐색중인 파일의 확장자를 나타낸다.
    int currentFileIndex = 0;   //파일 탐색용 인덱스
    
    //디렉토리 열기
    if ((dp = opendir(targetPath)) == NULL)
    {
        perror("DIRECTORY OPEN");
        exit(1);
    }

    //디렉토리 탐색
    while((dent = readdir(dp))) 
    {
        currentFileName = dent->d_name;

        if (!strcmp(currentFileName, ".")) continue;
        if (!strcmp(currentFileName, "..")) continue;

        //확장자 체크
        currentExtension = strrchr(currentFileName, '.');

        if (currentExtension == NULL) continue;
        if (strcmp(currentExtension + 1, targetExtension)) continue;

        //파일 목록 수집
        fileNamesReference[currentFileIndex++] = currentFileName;
    }

    closedir(dp);
}

// 프레임 버퍼 비우기
void clearFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar)
{
    memset(pfbmap, 0, fbvar.xres * fbvar.yres * frameBufferBPP / 8);
}

// 프레임 버퍼에 이미지 출력
void drawImageOnFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *bitmapHeader,
    RGBpixel **bitmapPixel,
    const int brightness)
{
    int rowIndex, columnIndex;              // 반복문 제어 변수
    RGBpixel pixelRGB;                      // 현재 탐색중인 프레임 버퍼에 출력할 비트맵 픽셀 값
    unsigned int *frameBufferFixelPointer;  // 현재 탐색중인 프레임 버퍼 위치

    // 이미지 크기가 화면 밖을 벗어나는 경우에 대비해
    // 화면과 이미지 중 더 작은 너비, 높이 값을 사용한다.
    const int minHeight = MIN(bitmapHeader->biHeight, fbvar.yres);
    const int minWidth = MIN(bitmapHeader->biWidth, fbvar.xres);

    // 프레임 버퍼를 탐색한다.
    for(rowIndex = 0; rowIndex < minHeight; rowIndex++)
    {
        for(columnIndex = 0; columnIndex < minWidth; columnIndex++)
        {
            // 밝기 조절 기능에 의해 변경된 밝기 값을 반영한다.
            pixelRGB = changePixelBrightness(bitmapPixel[rowIndex][columnIndex], brightness);

            // 24BPP인 기존 비트맵 이미지를 설정한 BPP에 맞게 프레임 버퍼에 출력한다.
            frameBufferFixelPointer = pfbmap + rowIndex * fbvar.xres + columnIndex;
            *frameBufferFixelPointer = (frameBufferBPP == BPP_16)
                ? convertRGB24toBGR16(pixelRGB)
                : convertRGB24toABGR32(pixelRGB);
        }
    }
}

// 프레임 버퍼에서 이미지를 읽어와 24BPP 비트맵 파일에 저장한다.
void captureFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *bitmapHeader)
{
    int fdBitmapOutput;                     // 비트맵 출력 파일 디스크립터
    BMPHeader *bitmapOutputHeader;          // 출력 비트맵 헤더 구조체
    int rowIndex, columnIndex;              // 반복문 제어 변수
    unsigned int *frameBufferFixelPointer;  // 현재 탐색중인 프레임 버퍼 위치
    unsigned int fixelRGBValue;             // 현재 탐색중인 프레임 버퍼의 픽셀 값

    // 이미지 크기가 화면 밖을 벗어나는 경우에 대비해
    // 화면과 이미지 중 더 작은 너비, 높이 값을 사용한다.
    const int minHeight = MIN(bitmapHeader->biHeight, fbvar.yres);
    const int minWidth = MIN(bitmapHeader->biWidth, fbvar.xres);

    // 기존에 캡처된 파일이 있다면 지운다.
    if (access(OUTPUT_BITMAP_FILE_NAME, F_OK) >= 0)
    {
        unlink(OUTPUT_BITMAP_FILE_NAME);
    }

    // 캡처된 이미지를 저장하기 위해 파일을 만들어 연다.
    fdBitmapOutput = open(OUTPUT_BITMAP_FILE_NAME, O_CREAT | O_WRONLY, 0777);
    if (fdBitmapOutput < 0)
    {
        perror("OUTPUT IMAGE OPEN");
        exit(1);
    }

    // 비트맵 헤더 복사
    memcpy(&bitmapOutputHeader, &bitmapHeader, sizeof(BMPHeader));

    // 비트맵 헤더 값 수정
    bitmapOutputHeader->bfSize = minWidth * minHeight * 3 + BITMAP_HEADER_SIZE;   // 파일 크기
    bitmapOutputHeader->biSizeImage = minWidth * minHeight * 3;                   // 비트맵 이미지의 픽셀 데이터 크기
    bitmapOutputHeader->biWidth = minWidth;                                       // 비트맵 이미지의 가로 크기
    bitmapOutputHeader->biHeight = minHeight;                                     // 비트맵 이미지의 세로 크기
    bitmapOutputHeader->biBitCount = BITMAP_DEFAULT_BPP;                          // BPP(Bits Per Pixel)

    // 비트맵 헤더를 파일에 쓴다.
    if (write(fdBitmapOutput, bitmapOutputHeader, sizeof(BMPHeader)) < 0)
    {
        perror("WRITE BITMAP HEADER");
        exit(1);
    }

    // 프레임 버퍼 탐색
    for(rowIndex = minHeight - 1; rowIndex >= 0; rowIndex--)
    {
        for(columnIndex = 0; columnIndex < minWidth; columnIndex++)
        {
            // 현재 탐색중인 프레임 버퍼 위치
            frameBufferFixelPointer = pfbmap + rowIndex * fbvar.xres + columnIndex;

            // 프레임 버퍼의 픽셀 값을 읽어온다.
            fixelRGBValue = *frameBufferFixelPointer;

            // 16BPP 프레임 버퍼를 캡처할 경우 24BPP 이미지로 픽셀을 확장한다.
            if(frameBufferBPP == BPP_16)
            {
                fixelRGBValue = convertBGR16toBGR24(fixelRGBValue);
            }

            // 읽어온 픽셀 값을 24BPP 비트맵 파일에 저장한다.
            if (write(fdBitmapOutput, &fixelRGBValue, BITMAP_DEFAULT_BPP/8) < 0)
            {
                perror("WRITE BITMAP PIXEL");
                exit(1);
            }
        }
    }

    close(fdBitmapOutput);
}

// 비트맵 파일의 헤더와 이미지를 읽고 동적할당하여 매개변수로 전달한다.
void loadBitmapImage(
    const unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader **bitmapHeaderReference,
    RGBpixel ***bitmapPixelReference,
    const char *fileName)
{
    int rowIndex, columnIndex;  // 반복문 순회용 변수
    int readBytes;              // read()로 값을 성공적으로 읽어왔는지 확인하기 위한 변수
    int fdBitmapInput;          // 비트맵 이미지 파일 디스크립터
    int bitmapPixelAreaSize;    // 비트맵 헤더를 제외한 이미지 영역 크기

    BMPHeader *bitmapHeader;    // 비트맵 헤더 구조체
    RGBpixel **bitmapPixel;     // RGB 각 8비트로 구성된 24비트 픽셀

    //이미지 파일 열기
    fdBitmapInput = open(fileName, O_RDWR);
    if (fdBitmapInput < 0)
    {
        perror("INPUT IMAGE OPEN");
        exit(1);
    }

    // 비트맵 헤더 정보를 저장할 공간을 동적할당
    bitmapHeader = (BMPHeader*)malloc(BITMAP_HEADER_SIZE);

    // 비트맵 헤더 읽기
    readBytes = read(fdBitmapInput, bitmapHeader, BITMAP_HEADER_SIZE);
    if (readBytes < 0)
    {
        perror("BITMAP HEADER READ ERROR");
        exit(1);
    }
    
    //비트맵 이미지를 저장할 공간을 동적할당
    bitmapPixelAreaSize = bitmapHeader-> bfSize - BITMAP_HEADER_SIZE;  
    bitmapPixel = (RGBpixel**)malloc(bitmapPixelAreaSize);

    //비트맵 이미지 읽기
    for(rowIndex = bitmapHeader->biHeight - 1; rowIndex >= 0; rowIndex--)
    {
        bitmapPixel[rowIndex] = (RGBpixel*)malloc(sizeof(RGBpixel) * bitmapHeader->biWidth);

        for(columnIndex = 0; columnIndex < bitmapHeader->biWidth; columnIndex++)
        {
            readBytes = read(fdBitmapInput, &bitmapPixel[rowIndex][columnIndex], sizeof(RGBpixel));
            if (readBytes < 0)
            {
                perror("BITMAP PIXEL READ ERROR");
                exit(1);
            }
        }
    }

    //동적 할당한 주소를 넘겨준다
    *bitmapHeaderReference = bitmapHeader;
    *bitmapPixelReference = bitmapPixel;

    close(fdBitmapInput);
}