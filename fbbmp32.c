#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h> 
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

#define DEVICE_PUSH_SWITCH "/dev/fpga_push_switch"
#define DEVICE_TEXT_LCD "/dev/fpga_text_lcd"
#define DEVICE_FRAME_BUFFER "/dev/fb0"

#define OUTPUT_BITMAP_FILE_NAME "output.bmp"

#define PUSH_SWITCH_BUFFER_SIZE 9       // Push Switch의 버튼 갯수는 9개다.
#define TEXT_LCD_LINE_SIZE 16           // Text LCD는 한 줄당 16개 문자를 출력할 수 있다.
#define TEXT_LCD_BUFFER_SIZE 32         // Text LCD는 총 32개 문자를 출력할 수 있다.

#define FILE_NAME_ARRAY_SIZE 64         // 비트맵 파일 이름을 64개 까지만 저장할 것이다.

#define BITMAP_HEADER_SIZE 54           // 비트맵 헤더의 크기는 54로 고정되어 있다.
#define BITMAP_DEFAULT_BPP 24           // 비트맵 파일의 기본 BPP는 24이다.

#define BPP_16 16                       // 16 BPP (Bits Per Pixel)
#define BPP_24 24                       // 24 BPP (Bits Per Pixel)
#define BPP_32 32                       // 32 BPP (Bits Per Pixel)

unsigned char quit = 0;                 // 무한 반복문 종료를 위한 변수
int frameBufferBPP = BPP_32;            // 프레임 버퍼의 BPP를 설정하기 위한 변수
bool isDeviceConnected = false;         // 장치가 연결되어 있는지 확인하기 위한 변수

typedef struct pixel_24bit
{
    unsigned char red;
    unsigned char green;
    unsigned char blue;
} RGBpixel;

#pragma pack(push, 1)
typedef struct bmpHeader
{
    unsigned short bfType;       // BMP 파일 매직 넘버
    unsigned int bfSize;         // 파일 크기
    unsigned short bfReserved1;  // 사용되지 않음
    unsigned short bfReserved2;  // 사용되지 않음
    unsigned int bfOffBits;      // 비트맵 데이터의 시작 위치

    unsigned int biSize;         // 현재 구조체의 크기
    int biWidth;                 // 비트맵 이미지의 가로 크기
    int biHeight;                // 비트맵 이미지의 세로 크기
    unsigned short biPlanes;     // 사용하는 색상판의 수
    unsigned short biBitCount;   // 픽셀 하나를 표현하는 비트 수
    unsigned int biCompression;  // 압축 방식
    unsigned int biSizeImage;    // 비트맵 이미지의 픽셀 데이터 크기
    int biXPelsPerMeter;         // 그림의 가로 해상도(미터당 픽셀)
    int biYPelsPerMeter;         // 그림의 세로 해상도(미터당 픽셀)
    unsigned int biClrUsed;      // 색상 테이블에서 실제 사용되는 색상 수
    unsigned int biClrImportant; // 비트맵을 표현하기 위해 필요한 색상 인덱스 수
} BMPHeader;
#pragma pack(pop)

//값이 지정한 범위를 벗어나지 않도록 한다.
unsigned char thresholding(int value, const int rangeMin, const int rangeMax)
{
    if (value < rangeMin) value = rangeMin;
    if (value > rangeMax) value = rangeMax;

    return value;
}

//픽셀의 밝기 값을 변화시킨다. 밝기 값의 범위는 0 ~ 255(unsigned char)이다.
RGBpixel changePixelBrightness(RGBpixel pixelBrightness, const int pixelBrightnessDelta)
{
    pixelBrightness.red = thresholding(pixelBrightness.red + pixelBrightnessDelta, 0, 255);
    pixelBrightness.green = thresholding(pixelBrightness.green + pixelBrightnessDelta, 0, 255);
    pixelBrightness.blue = thresholding(pixelBrightness.blue + pixelBrightnessDelta, 0, 255);

    return pixelBrightness;
}

//24비트 RGB 픽셀을 32비트 ABGR 픽셀로 순서 변환과 함께 확장한다.
unsigned int convertRGB24toABGR32(const RGBpixel pixel)
{
    const unsigned int alpha = 0;   //투명도
    
    return ((0 << 24) | (pixel.blue << 16) | (pixel.green << 8) | (pixel.red << 0));
}

//24비트 RGB 픽셀을 16비트 BGR 픽셀로 순서 변환과 함께 축소한다.
unsigned short convertRGB24toBGR16(RGBpixel pixel)
{
	return ((pixel.blue << 11) | (pixel.green << 6) | (pixel.red << 0));
}

//16비트 BGR 픽셀을 24비트 BGR 픽셀로 확장한다.
unsigned int convertBGR16toBGR24(unsigned short pixel)
{
    //BGR 값을 추출하기 위한 마스크
    unsigned short maskBlue = 65535-2047;
    unsigned short maskGreen = 2047-31;
    unsigned short maskRed = 31;

    //BGR 값 추출
    unsigned short blue = pixel & maskBlue;    //11111 000000 00000
    unsigned short green = pixel & maskGreen;  //00000 111111 00000
    unsigned short red = pixel & maskRed;      //00000 000000 11111

    //공백 제거
    blue = blue >> 11;                         //00000 000000 11111
    green = green >> 6;                        //00000 000000 11111
    red = red >> 0;                            //00000 000000 11111

    //24비트에 맞춰 확장 준비
    blue = blue << 3;                          //00000 000111 11000
    green = green << 3;                        //00000 000111 11000
    red = red << 3;                            //00000 000111 11000

    return ((blue << 16) | (green << 8) | (red << 0));
}

// SIGINT를 받으면 호출되는 함수이다. 무한 반복문을 종료하기 위해 사용한다.
void signalCallbackQuit(const int sig) 
{
    quit = true;
    exit(1);
}

//프레임 버퍼에 이미지 출력
void drawImageOnFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *bitmapHeader,
    RGBpixel **bitmapPixel,
    const int brightness)
{
    int rowIndex, columnIndex;              //반복문 제어 변수
    RGBpixel pixelRGB;                      //현재 탐색중인 프레임 버퍼에 출력할 비트맵 픽셀 값
    unsigned int *frameBufferFixelPointer;  //현재 탐색중인 프레임 버퍼 위치

    //이미지 크기가 화면 밖을 벗어나는 경우에 대비해
    //화면과 이미지 중 더 작은 너비, 높이 값을 사용한다.
    const int minHeight = MIN(bitmapHeader->biHeight, fbvar.yres);
    const int minWidth = MIN(bitmapHeader->biWidth, fbvar.xres);

    //프레임 버퍼를 탐색한다.
    for(rowIndex = 0; rowIndex < minHeight; rowIndex++)
    {
        for(columnIndex = 0; columnIndex < minWidth; columnIndex++)
        {
            //밝기 조절 기능에 의해 변경된 밝기 값을 반영한다.
            pixelRGB = changePixelBrightness(bitmapPixel[rowIndex][columnIndex], brightness);

            //24BPP인 기존 비트맵 이미지를 설정한 BPP에 맞게 프레임 버퍼에 출력한다.
            frameBufferFixelPointer = pfbmap + rowIndex * fbvar.xres + columnIndex;
            *frameBufferFixelPointer = (frameBufferBPP == BPP_16)
                ? convertRGB24toBGR16(pixelRGB)
                : convertRGB24toABGR32(pixelRGB);
        }
    }
}

int main(void)
{
    // 파일 디스크립터
    int fdBitmapInput;  // 비트맵 입력 파일
    int fdBitmapOutput; // 비트맵 출력 파일
    int fdPushSwitch;   // 푸시 스위치 드라이버
    int fdTextLcd;      // Text LCD 드라이버
    int fdFrameBuffer;  // 프레임 버퍼 드라이버

    // 비트맵 이미지 관련
    int bitmapPixelSize;          // 비트맵 픽셀 영역의 크기
    BMPHeader *bitmapHeader;      // 입력 비트맵 헤더 구조체
    BMPHeader bitmapOutputHeader; // 출력 비트맵 헤더 구조체
    RGBpixel **bitmapPixel;       // RGB 각 8비트로 구성된 24비트 픽셀
    int brightness = 0;           // 4, 5번 버튼으로 조절할 픽셀의 밝기

    // 푸시 스위치
    int pushSwitchValue = 0;                   // 푸시 스위치 인덱스 값, switch-case문에 들어간다.
    int pushSwitchBufferSize;                  // 푸시 스위치 버퍼 크기, 값은 9이다.
    unsigned char pushSwitchBuffer[9] = {0};   // 푸시 스위치 버퍼, 버튼이 눌리면 값이 1로 변한다.

    // Text LCD
    unsigned char textLcdString[32] = {0};     // Text LCD에 write 하게 될 버퍼이다. FirstLine, SecondLine을 합친 문자열이 저장간다.
    unsigned char textLcdFirstLine[16] = {0};  // Text LCD의 첫 번째 줄, 파일명이 저장된다.
    unsigned char textLcdSecondLine[16] = {0}; // Text LCD의 두 번째 줄, 해상도, BPP(Bits Per Pixel)가 저장된다.

    // 프레임 버퍼
    struct fb_var_screeninfo fbvar; // 프레임 버퍼의 가변 정보를 저장한다.
    unsigned int *pfbmap;           // mmap으로 매핑시킬 주소 공간을 가리킬 포인터

    // 디렉토리
    DIR *dp;
    struct dirent *dent;
    char *ext;

    unsigned char *fileList[FILE_NAME_ARRAY_SIZE] = {NULL}; // readdir()로 디렉토리의 .bmp파일 목록을 저장한다.
    int fileIndex = -1;                    // 1, 2번 버튼으로 다루게 될 fileList 배열의 인덱스이다.
    int fileTempIndex = 0;                // 6번 버튼으로 프레임 버퍼를 캡처할 경우 fileIndex가 초기화 되므로 값을 임시로 저장하기 위한 변수이다.

    //시간 측정
    clock_t timeStart; // timeEnd-timeStart로 시간을 측정한다.
    clock_t timeEnd;   // timeEnd-timeStart로 시간을 측정한다.

    int checkRead;     // read() 호출 시 에러가 발생하는 지 확인하기 위한 변수
    bool isImageLoaded;                         //이미지가 열리지 않은 상태로 3~6번 버튼이 눌리는 상황을 방지하기 위한 변수

    int i, j; // 반복문 제어 변수

    // 푸시 스위치 드라이버 열기
    fdPushSwitch = open(DEVICE_PUSH_SWITCH, O_RDWR);
    if (fdPushSwitch < 0)
    {
        perror("PUSH SWITCH OPEN");
        exit(1);
    }

    // Text LCD 드라이버 열기
    fdTextLcd = open(DEVICE_TEXT_LCD, O_WRONLY);
    if (fdTextLcd < 0)
    {
        perror("TEXT LCD OPEN");
        exit(1);
    }

    // Text LCD 값을 ' '로 초기화
    for (i = 0; i < 32; i++)
    {
        textLcdString[i] = ' ';
    }
    write(fdTextLcd, textLcdString, 32);

    // 무한 반복문을 종료하기 위한 시그널 등록
    (void)signal(SIGINT, signalCallbackQuit);

    // 프레임 버퍼 열기
    fdFrameBuffer = open(DEVICE_FRAME_BUFFER, O_RDWR);
    if (fdFrameBuffer < 0)
    {
        perror("FRAME BUFFER OPEN");
        exit(1);
    }

    // 프레임 버퍼 가변 정보 얻어오기
    if (ioctl(fdFrameBuffer, FBIOGET_VSCREENINFO, &fbvar) < 0)
    {
        perror("FRAME BUFFER IOCTL GET");
        exit(1);
    }

    // 프레임 버퍼의 BPP를 변경
    fbvar.bits_per_pixel = frameBufferBPP;
    if (ioctl(fdFrameBuffer, FBIOPUT_VSCREENINFO, &fbvar) < 0)
    {
        perror("FRAME BUFFER IOCTL PUT");
        exit(1);
    }

    // 변경되지 않은 경우 처리
    if (fbvar.bits_per_pixel != frameBufferBPP)
    {
        perror("BPP IS NOT CHANGED");
        exit(1);
    }

    // 프레임 버퍼 해상도만큼 디바이스 메모리 주소와 포인터의 메모리 주소를 연결한다.
    pfbmap = (unsigned int *)mmap(
        0,
        fbvar.xres * fbvar.yres * frameBufferBPP/8, //프레임 버퍼 해상도
        PROT_READ|PROT_WRITE,                       //매핑된 파일에 읽기, 쓰기를 허용
        MAP_SHARED,                                 //다른 프로세스와 매핑을 공유하는 옵션
        fdFrameBuffer,                              //프레임 버퍼 파일 디스크립터(/dev/fb0)
        0);

    if (pfbmap == (unsigned int *)-1)
    {
        perror("FRAME BUFFER MMAP");
        exit(1);
    }

    // 프레임 버퍼를 초기화한다.
    clearFrameBuffer(pfbmap, fbvar);

    // 비트맵 확장자를 가진 파일 목록 수집
    searchFilesInPathByExtention(fileList, ".", "bmp");

    //-------------------BUTTON FUNCTION---------------------------
    pushSwitchBufferSize = sizeof(pushSwitchBuffer);
    printf("Press <ctrl+c> to quit. \n");
    while (!quit)
    {
        usleep(1000000);
        printf("looping.. select numbers (1~9)\n");

        // Push Switch 입력을 받고 인덱스에 1을 더해준다. (0~8 -> 1~9)
        // read(fdPushSwitch, &pushSwitchBuffer, pushSwitchBufferSize);
        // for (i = 0; i < 9; i++)
        // {
        //     if (pushSwitchBuffer[i] == 1)
        //         pushSwitchValue = i + 1;
        // }

        // 장치가 없으면 콘솔에서 숫자로 입력 받는다.
        scanf("%d", &pushSwitchValue);

        timeStart = clock();
        
        switch (pushSwitchValue)
        {
            case 1: // OPEN NEXT FILE
                printf("case 1 : OPEN NEXT FILE\n");
                printf("index:%d, %s\n", fileIndex, textLcdString); // FOR DEBUG
                
                // 다음 파일이 있는지 체크
                if ((++fileIndex) > 9)
                    fileIndex = 9;

                if (fileList[fileIndex] == NULL)
                {
                    printf("NEXT FILE NOT EXISTS, INDEX:%d\n", fileIndex);
                    break;
                }

                // 다음 파일이 있다면 이미지 읽어오기
                memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32 / 8);
                fdBitmapInput = open(fileList[fileIndex], O_RDWR);
                if (fdBitmapInput < 0)
                {
                    perror("INPUT IMAGE OPEN");
                    exit(1);
                }

                bitmapHeader = (BMPHeader *)malloc(54);
                checkRead = read(fdBitmapInput, bitmapHeader, 54);
                if (checkRead < 0)
                {
                    perror("BITMAP HEADER READ ERROR");
                    exit(1);
                }

                bitmapPixelSize = bitmapHeader->bfSize - 54;
                bitmapPixel = (RGBpixel **)malloc(bitmapPixelSize);
                for (i = 0; i < bitmapHeader->biHeight; i++)
                {
                    bitmapPixel[i] = (RGBpixel *)malloc(sizeof(RGBpixel) * bitmapHeader->biWidth);
                }

                for (i = bitmapHeader->biHeight - 1; i >= 0; i--)
                {
                    for (j = 0; j < bitmapHeader->biWidth; j++)
                    {
                        checkRead = read(fdBitmapInput, &bitmapPixel[i][j], sizeof(RGBpixel));
                        if (checkRead < 0)
                        {
                            perror("BITMAP PIXEL READ ERROR");
                            exit(1);
                        }
                    }
                }

                isImageLoaded = true;

                // Text LCD를 통해 헤더 정보(파일명, 해상도, BPP) 출력
                lseek(fdTextLcd, 0, SEEK_SET);
                for (i = 0; i < 16; i++)
                {
                    textLcdFirstLine[i] = fileList[fileIndex][i];
                }

                sprintf(textLcdSecondLine, "%d*%d BPP:%d", bitmapHeader->biWidth, bitmapHeader->biHeight, bitmapHeader->biBitCount);
                for (i = 0; i < 16; i++)
                {
                    fileTempIndex = fileIndex;
                    if (i < (strlen(textLcdFirstLine)))
                        textLcdString[i] = textLcdFirstLine[i];
                    else
                        textLcdString[i] = ' ';
                }

                for (i = 16; i < 32; i++)
                {
                    if ((i - 16) < (strlen(textLcdSecondLine)))
                        textLcdString[i] = textLcdSecondLine[i - 16];
                    else
                        textLcdString[i] = ' ';
                }

                write(fdTextLcd, textLcdString, 32);

                //프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                
                break;

            case 2: // OPEN PREVIOUS FILE
                printf("case 2 : OPEN PREVIOUS FILE\n");
                printf("index:%d, %s\n", fileIndex, textLcdString); // FOR DEBUG
                
                // 이전 파일이 있는지 체크
                if ((--fileIndex) < 0)
                    fileIndex = 0;

                if (fileList[fileIndex] == NULL)
                {
                    printf("PREVIOUS FILE NOT EXISTS, INDEX:%d\n", fileIndex);
                    break;
                }
                
                // 이전 파일이 있다면 이미지 읽어오기
                memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32 / 8);
                fdBitmapInput = open(fileList[fileIndex], O_RDWR);
                if (fdBitmapInput < 0)
                {
                    perror("INPUT IMAGE OPEN");
                    exit(1);
                }

                bitmapHeader = (BMPHeader *)malloc(54);
                checkRead = read(fdBitmapInput, bitmapHeader, 54);
                if (checkRead < 0)
                {
                    perror("BITMAP HEADER READ ERROR");
                    exit(1);
                }

                bitmapPixelSize = bitmapHeader->bfSize - 54;
                bitmapPixel = (RGBpixel **)malloc(bitmapPixelSize);
                for (i = 0; i < bitmapHeader->biHeight; i++)
                {
                    bitmapPixel[i] = (RGBpixel *)malloc(sizeof(RGBpixel) * bitmapHeader->biWidth);
                }

                for (i = bitmapHeader->biHeight - 1; i >= 0; i--)
                {
                    for (j = 0; j < bitmapHeader->biWidth; j++)
                    {
                        checkRead = read(fdBitmapInput, &bitmapPixel[i][j], sizeof(RGBpixel));
                        if (checkRead < 0)
                        {
                            perror("BITMAP PIXEL READ ERROR");
                            exit(1);
                        }
                    }
                }

                isImageLoaded = true;

                // Text LCD를 통해 헤더 정보(파일명, 해상도, BPP) 출력
                lseek(fdTextLcd, 0, SEEK_SET);
                for (i = 0; i < 16; i++)
                {
                    textLcdFirstLine[i] = fileList[fileIndex][i];
                }

                sprintf(textLcdSecondLine, "%d*%d BPP:%d", bitmapHeader->biWidth, bitmapHeader->biHeight, bitmapHeader->biBitCount);

                for (i = 0; i < 16; i++)
                {
                    if (i < (strlen(textLcdFirstLine)))
                        textLcdString[i] = textLcdFirstLine[i];
                    else
                        textLcdString[i] = ' ';
                }

                for (i = 16; i < 32; i++)
                {
                    if ((i - 16) < (strlen(textLcdSecondLine)))
                        textLcdString[i] = textLcdSecondLine[i - 16];
                    else
                        textLcdString[i] = ' ';
                }
                write(fdTextLcd, textLcdString, 32);
                
                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 3: // CLEAR FRAME BUFFER
                printf("case 3:\n");
                memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32 / 8);
                break;

            case 4:
                printf("case 4 : INCREASE BRIGHTNESS\n");
                
                if (!isImageLoaded)
                {
                    printf("IMAGE IS NOT LOADED\n");
                    break;
                }

                // 밝기 증가
                brightness = thresholding(brightness + 30, -255, 255);

                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 5:
                printf("case 5: DECREASE BRIGHTNESS\n");

                if (!isImageLoaded)
                {
                    printf("IMAGE IS NOT LOADED\n");
                    break;
                }

                // 밝기 감소
                brightness = thresholding(brightness - 30, -255, 255);
                
                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 6:
                printf("case 6 : CAPTURE FRAME BUFFER\n");
                
                if (!isImageLoaded)
                {
                    printf("IMAGE IS NOT LOADED\n");
                    break;
                }

                bitmapOutputHeader = *bitmapHeader;
                bitmapOutputHeader.bfSize = MIN(bitmapHeader->biWidth, fbvar.xres) * MIN(bitmapHeader->biHeight, fbvar.yres) * 3 + 54; // 파일 크기
                bitmapOutputHeader.biSizeImage = MIN(bitmapHeader->biWidth, fbvar.xres) * MIN(bitmapHeader->biHeight, fbvar.yres) * 3; // 비트맵 이미지의 픽셀 데이터 크기
                bitmapOutputHeader.biWidth = MIN(bitmapHeader->biWidth, fbvar.xres);                                                   // 비트맵 이미지의 가로 크기
                bitmapOutputHeader.biHeight = MIN(bitmapHeader->biHeight, fbvar.yres);                                                 // 비트맵 이미지의 세로 크기
                // bitmapOutputHeader.biXPelsPerMeter;  // 그림의 가로 해상도(미터당 픽셀)
                // bitmapOutputHeader.biYPelsPerMeter;  // 그림의 세로 해상도(미터당 픽셀)

                if (access(OUTPUT_BITMAP_FILE_NAME, F_OK) >= 0) // BITMAP OUTPUT FILE DELETE
                    unlink(OUTPUT_BITMAP_FILE_NAME);

                fdBitmapOutput = open(OUTPUT_BITMAP_FILE_NAME, O_CREAT | O_WRONLY, 0777); // BITMAP OUTPUT FILE OPEN
                if (fdBitmapOutput < 0)
                {
                    perror("OUTPUT IMAGE OPEN");
                    exit(1);
                }
                if (write(fdBitmapOutput, &bitmapOutputHeader, 54) < 0)
                {
                    perror("WRITE BITMAP HEADER");
                    exit(1);
                }
                for (i = MIN(bitmapHeader->biHeight, fbvar.yres) - 1; i >= 0; i--)
                {
                    for (j = 0; j < MIN(bitmapHeader->biWidth, fbvar.xres); j++)
                    {
                        if (write(fdBitmapOutput, pfbmap + i * fbvar.xres + j, 24 / 8) < 0)
                        {
                            perror("WRITE BITMAP PIXEL");
                            exit(1);
                        }
                    }
                }

                // 새 파일이 추가되었으므로 파일 목록을 다시 불러온다.
                searchFilesInPathByExtention(fileList, ".", "bmp");
                break;

            default:
                break;
        }

        timeEnd = clock();

        if (pushSwitchValue == 1 || pushSwitchValue == 2 || pushSwitchValue == 6)
        {
            printf("TIME : %f\n", ((double)(timeEnd - timeStart)/1000000));
        }

        pushSwitchValue = 0;
    }

    munmap(pfbmap, fbvar.xres * fbvar.yres * 32 / 8);
    close(fdTextLcd);
    close(fdPushSwitch);
    close(fdFrameBuffer);
    close(fdBitmapInput);
    close(fdBitmapOutput);

    return 0;
}
