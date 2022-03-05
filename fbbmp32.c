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

// 값이 지정한 범위를 벗어나지 않도록 한다.
unsigned char thresholding(int value, const int rangeMin, const int rangeMax)
{
    if (value < rangeMin) value = rangeMin;
    if (value > rangeMax) value = rangeMax;

    return value;
}

// 픽셀의 밝기 값을 변화시킨다. 밝기 값의 범위는 0 ~ 255(unsigned char)이다.
RGBpixel changePixelBrightness(RGBpixel pixelBrightness, const int pixelBrightnessDelta)
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
    
    return ((0 << 24) | (pixel.blue << 16) | (pixel.green << 8) | (pixel.red << 0));
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


int main(void)
{
    // 파일 디스크립터
    int fdPushSwitch;                           // Push Switch 드라이버
    int fdTextLcd;                              // Text LCD 드라이버
    int fdFrameBuffer;                          // 프레임 버퍼 드라이버 파일 디스크립터

    int pushSwitchValue;                        // Push Switch 인덱스 값, switch-case문에 들어간다.
    int pushSwitchIndex;                        // 지속적으로 푸시 스위치를 스캔해서 눌린 버튼을 찾는다.

    // 장치별 버퍼
    unsigned char pushSwitchBuffer[PUSH_SWITCH_BUFFER_SIZE] = {0};  // 버튼이 눌리면 값이 1로 변한다.
    unsigned char textLCDBuffer[2][TEXT_LCD_LINE_SIZE] = {0};       // 파일명, 해상도 및 BPP(Bits Per Pixel)를 표시한다.

    // 비트맵 이미지 관련
    BMPHeader *bitmapHeader;                    // 입력 비트맵 헤더 구조체
    RGBpixel **bitmapPixel;                     // RGB 각 8비트로 구성된 24비트 픽셀
    int brightness;	                            // 4, 5번 버튼으로 조절할 픽셀의 밝기
    bool isImageLoaded;                         // 이미지가 열리지 않은 상태로 3~6번 버튼이 눌리는 상황을 방지하기 위한 변수

    // 프레임 버퍼
    unsigned int *pfbmap;                       // mmap으로 매핑시킬 주소 공간을 가리킬 포인터
    struct fb_var_screeninfo fbvar;             // 프레임 버퍼의 가변 정보를 저장한다.

    // 파일 목록
    unsigned char *fileList[FILE_NAME_ARRAY_SIZE] = {NULL};     // readdir()로 디렉토리의 .bmp파일 목록을 저장한다.
    int fileIndex = -1;                                         // 1, 2번 버튼으로 다루게 될 fileList 배열의 인덱스이다.

    // 시간 재기
    clock_t timeStart, timeEnd;                 // timeEnd-timeStart로 시간을 측정한다.


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
    write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE); 

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

    while (!quit)
    {
        printf("1 : Next image\n");
        printf("2 : Previous image\n");
        printf("3 : Clear frame buffer\n");
        printf("4 : Increase brightness\n");
        printf("5 : Decrease brightness\n");
        printf("6 : Capture frame buffer\n");
        printf("Ctrl + c : quit\n");

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
            case 1:
                printf("case 1 : OPEN NEXT FILE\n");
                
                clearFrameBuffer(pfbmap, fbvar);
                brightness = 0;

                // 다음 파일이 있는지 체크
                fileIndex = thresholding(fileIndex + 1, 0, FILE_NAME_ARRAY_SIZE - 1);
                if (fileList[fileIndex] == NULL)
                {
                    printf("NEXT FILE NOT EXISTS, INDEX:%d\n", fileIndex); 
                    fileIndex--;    
                    break;
                }

                // 다음 파일이 있다면 이미지 읽어오기
                loadBitmapImage(pfbmap, fbvar, &bitmapHeader, &bitmapPixel, fileList[fileIndex]);
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

                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 2:
                printf("case 2 : OPEN PREVIOUS FILE\n");
                
                clearFrameBuffer(pfbmap, fbvar);
                brightness = 0;

                // 이전 파일이 있는지 체크
                fileIndex = thresholding(fileIndex - 1, 0, FILE_NAME_ARRAY_SIZE - 1);
                if (fileList[fileIndex] == NULL)
                {
                    printf("PREVIOUS FILE NOT EXISTS, INDEX:%d\n", fileIndex);    
                    fileIndex++;
                    break;
                }
                
                // 이전 파일이 있다면 이미지 읽어오기
                loadBitmapImage(pfbmap, fbvar, &bitmapHeader, &bitmapPixel, fileList[fileIndex]);
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
                printf("case 3 : CLEAR FRAME BUFFER\n");

                clearFrameBuffer(pfbmap, fbvar);
                free(bitmapHeader);
                free(bitmapPixel);
                isImageLoaded = false;
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
                
                // 프레임 버퍼 캡처
                captureFrameBuffer(pfbmap,fbvar, bitmapHeader);

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

    munmap(pfbmap, fbvar.xres * fbvar.yres * frameBufferBPP / 8);
    close(fdTextLcd);
    close(fdPushSwitch);
    close(fdFrameBuffer);

    return 0;
}
