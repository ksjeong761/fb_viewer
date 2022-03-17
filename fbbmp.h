#ifndef _FBBMP_
#define _FBBMP_

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

#define DEVICE_PUSH_SWITCH "/dev/fpga_push_switch"
#define DEVICE_TEXT_LCD "/dev/fpga_text_lcd"
#define DEVICE_FRAME_BUFFER "/dev/fb0"

#define OUTPUT_BITMAP_FILE_NAME "output.bmp"
#define BITMAP_EXTENSION "bmp"

#define PUSH_SWITCH_BUFFER_SIZE 9       // Push Switch의 버튼 갯수는 9개다.
#define TEXT_LCD_HEIGHT 2               // Text LCD는 2개의 행을 가진다.
#define TEXT_LCD_WIDTH 16               // Text LCD는 한 줄당 16개 문자를 출력할 수 있다.
#define TEXT_LCD_BUFFER_SIZE 32         // Text LCD는 총 32개 문자를 출력할 수 있다.

#define FILE_NAME_ARRAY_SIZE 64         // 비트맵 파일 이름을 64개 까지만 저장할 것이다.

#define BITMAP_HEADER_SIZE 54           // 비트맵 헤더의 크기는 54로 고정되어 있다.
#define BITMAP_DEFAULT_BPP 24           // 비트맵 파일의 기본 BPP는 24이다.

#define BPP_16 16                       // 16 BPP (Bits Per Pixel)
#define BPP_24 24                       // 24 BPP (Bits Per Pixel)
#define BPP_32 32                       // 32 BPP (Bits Per Pixel)

#define UCHAR_MIN 0                     // unsigned char 최솟값
#define UCHAR_MAX 255                   // unsigned char 최댓값

#define BRIGHTNESS_DELTA 30             // 변화시킬 프레임 버퍼 밝기

extern unsigned char quit;              // 무한 반복문 종료를 위한 변수
extern int frameBufferBPP;              // 프레임 버퍼의 BPP를 설정하기 위한 변수
extern bool isDeviceConnected;          // 장치가 연결되어 있는지 확인하기 위한 변수

typedef struct pixel_24bit
{
    unsigned char blue;
    unsigned char green;
    unsigned char red;
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
int thresholding(
    int value,
    const int rangeMin,
    const int rangeMax);

// 픽셀의 밝기 값을 변화시킨다. 밝기 값의 범위는 0 ~ 255(unsigned char)이다.
RGBpixel changePixelBrightness(
    RGBpixel pixelBrightness,
    const int pixelBrightnessDelta);

// 24비트 RGB 픽셀을 32비트 ABGR 픽셀로 순서 변환과 함께 확장한다.
unsigned int convertRGB24toABGR32(const RGBpixel pixel);

// 24비트 RGB 픽셀을 16비트 BGR 픽셀로 순서 변환과 함께 축소한다.
unsigned short convertRGB24toBGR16(const RGBpixel pixel);

// 16비트 BGR 픽셀을 24비트 BGR 픽셀로 확장한다.
unsigned int convertBGR16toBGR24(const unsigned short pixel);

// SIGINT를 받으면 호출되는 함수이다. 무한 반복문을 종료하기 위해 사용한다.
void signalCallbackQuit(const int sig);

//입력한 경로에서 특정 확장자를 가진 파일을 수집한다.
void searchFilesInPathByExtention(
    unsigned char *pFileNamesArray[FILE_NAME_ARRAY_SIZE],
    const char *pTargetPath,
    const char *pTargetExtension);

// 프레임 버퍼 크기 구하기
int calculateFrameBufferSize(const struct fb_var_screeninfo fbvar);

// 프레임 버퍼 비우기
void clearFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar);

// 프레임 버퍼에 이미지 출력
void drawImageOnFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *pBitmapHeader,
    RGBpixel **pBitmapPixel2dArray,
    const int brightness);

// 프레임 버퍼에서 이미지를 읽어와 24BPP 비트맵 파일에 저장한다.
void captureFrameBuffer(
    unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader *pBitmapHeader);

// 비트맵 파일의 헤더와 이미지를 읽고 동적 할당하여 매개변수로 포인터를 전달한다.
void loadBitmapImage(
    const unsigned int *pfbmap,
    const struct fb_var_screeninfo fbvar, 
    BMPHeader **pReturnBitmapHeader,
    RGBpixel ***pReturnBitmapPixel2dArray,
    const char *pFileName);

#endif