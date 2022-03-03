#include <stdio.h>
#include <stdlib.h> 
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

#define OUTPUT_BITMAP_FILE "output.bmp"

unsigned char quit = 0;

typedef struct pixel_24bit
{
    unsigned char red;
    unsigned char green;
    unsigned char blue;
}RGBpixel;

#pragma pack(push, 1)
typedef struct bmpHeader
{
    unsigned short bfType;             // BMP 파일 매직 넘버
    unsigned int   bfSize;           // 파일 크기
    unsigned short bfReserved1;      // 사용되지 않음
    unsigned short bfReserved2;      // 사용되지 않음
    unsigned int   bfOffBits;        // 비트맵 데이터의 시작 위치

    unsigned int   biSize;           // 현재 구조체의 크기
    int            biWidth;          // 비트맵 이미지의 가로 크기
    int            biHeight;         // 비트맵 이미지의 세로 크기
    unsigned short biPlanes;         // 사용하는 색상판의 수
    unsigned short biBitCount;       // 픽셀 하나를 표현하는 비트 수
    unsigned int   biCompression;    // 압축 방식
    unsigned int   biSizeImage;      // 비트맵 이미지의 픽셀 데이터 크기
    int            biXPelsPerMeter;  // 그림의 가로 해상도(미터당 픽셀)
    int            biYPelsPerMeter;  // 그림의 세로 해상도(미터당 픽셀)
    unsigned int   biClrUsed;        // 색상 테이블에서 실제 사용되는 색상 수
    unsigned int   biClrImportant;   // 비트맵을 표현하기 위해 필요한 색상 인덱스 수
}BMPHeader;
#pragma pack(pop)

unsigned int makepixel(RGBpixel pixel, int brightness)
{
    unsigned int alpha = 0;
    int clamping = 0;
    
    clamping = pixel.red + brightness;
    if(clamping > 255) pixel.red = 255;
    else if(clamping < 0) pixel.red = 0;

    clamping = pixel.green + brightness;
    if(clamping > 255) pixel.green = 255;
    else if(clamping < 0) pixel.green = 0;

    clamping = pixel.blue + brightness;
    if(clamping > 255) pixel.blue = 255;
    else if(clamping < 0) pixel.blue = 0;

    return ((alpha << 24) | (pixel.red << 0) | (pixel.green << 8) | (pixel.blue << 16));
}

void user_signal1(int sig) 
{
    quit = 1;
}

int main(void)
{
    //파일 디스크립터
    int fdBitmapInput; //비트맵 입력 파일
    int fdBitmapOutput; //비트맵 출력 파일
    int fdPushSwitch; //푸시 스위치 드라이버
    int fdTextLcd; //Text LCD 드라이버
    int fdFrameBuffer; //프레임 버퍼 드라이버

    //비트맵 이미지 관련
    int bitmapPixelSize; //비트맵 픽셀 영역의 크기
    BMPHeader * bitmapHeader; //입력 비트맵 헤더 구조체
    BMPHeader bitmapOutputHeader; //출력 비트맵 헤더 구조체
    RGBpixel ** bitmapPixel; //RGB 각 8비트로 구성된 24비트 픽셀
    int brightness = 0;    //4, 5번 버튼으로 조절할 픽셀의 밝기

    //푸시 스위치
    int pushSwitchValue = 0; //푸시 스위치 인덱스 값, switch-case문에 들어간다.
    int pushSwitchBufferSize; //푸시 스위치 버퍼 크기, 값은 9이다.
    unsigned char pushSwitchBuffer[9] = {0,0,0,0,0,0,0,0,0}; //푸시 스위치 버퍼, 버튼이 눌리면 값이 1로 변한다.

    //Text LCD
    unsigned char textLcdString[32] = {0}; //Text LCD에 write 하게 될 버퍼이다. FirstLine, SecondLine을 합친 문자열이 저장간다.
    unsigned char textLcdFirstLine[16] = {0}; //Text LCD의 첫 번째 줄, 파일명이 저장된다.
    unsigned char textLcdSecondLine[16] = {0}; //Text LCD의 두 번째 줄, 해상도, BPP(Bits Per Pixel)가 저장된다.

    //프레임 버퍼
    struct fb_var_screeninfo fbvar; //프레임 버퍼의 가변 정보를 저장한다.
    unsigned int *pfbmap; //mmap으로 매핑시킬 주소 공간을 가리킬 포인터

    //디렉토리
    DIR *dp;
    struct dirent *dent;
    char * ext;

    unsigned char *fileList[10] = {NULL}; //readdir()로 디렉토리의 .bmp파일 목록을 저장한다.
    int fileIndex = 0; //1, 2번 버튼으로 다루게 될 fileList 배열의 인덱스이다.
    int fileTempIndex = 0; //6번 버튼으로 프레임 버퍼를 캡처할 경우 fileIndex가 초기화 되므로 값을 임시로 저장하기 위한 변수이다. 

    //시간 측정
    clock_t timeStart;// timeEnd-timeStart로 시간을 측정한다.
    clock_t timeEnd;// timeEnd-timeStart로 시간을 측정한다.
    
    int checkRead; //read() 호출 시 에러가 발생하는 지 확인하기 위한 변수
    int checkFirstRun; //이미지가 열리지 않은 상태로 3~6번 버튼이 눌리는 상황을 방지하기 위한 변수
    
    int i, j; //반복문 제어 변수
    
    //------------------PUSH SWITCH------------------
    fdPushSwitch = open(DEVICE_PUSH_SWITCH, O_RDWR); //푸시 스위치 드라이버 열기
    if (fdPushSwitch < 0)
    {
        perror("PUSH SWITCH OPEN");
        exit(1);
    }
    (void)signal(SIGINT, user_signal1); //반복문을 나오기 위한 시그널 등록
    //-------------------TEXT LCD--------------------
    fdTextLcd = open(DEVICE_TEXT_LCD, O_WRONLY); //Text LCD 드라이버 열기
    if (fdTextLcd < 0)
    {
        perror("TEXT LCD OPEN");
        exit(1);
    }
    for(i=0; i<32; i++)
    {
        textLcdString[i] = ' ';
    }
    write(fdTextLcd, textLcdString, 32); //Text LCD 값을 ' '로 초기화
    
    //------------------FRAME_BUFFER-----------------
    fdFrameBuffer = open(DEVICE_FRAME_BUFFER, O_RDWR);
    if(fdFrameBuffer < 0)
    {
        perror("FRAME BUFFER OPEN");
        exit(1);
    }

    if(ioctl(fdFrameBuffer, FBIOGET_VSCREENINFO, &fbvar) < 0)
    {
        perror("FRAME BUFFER IOCTL GET");
        exit(1);
    }
    fbvar.bits_per_pixel = 32;
    
    if(ioctl(fdFrameBuffer, FBIOPUT_VSCREENINFO, &fbvar) < 0)
    {
        perror("FRAME BUFFER IOCTL PUT");
        exit(1);
    }
    if(fbvar.bits_per_pixel != 32)
    {
        fprintf(stderr, "BPP IS NOT 32, CURRENT BPP IS \n");
        printf("%d", fbvar.bits_per_pixel);
        exit(1);
    }

    pfbmap = (unsigned int *)mmap(
            0,
            fbvar.xres * fbvar.yres * 32/8,
            PROT_READ|PROT_WRITE,
            MAP_SHARED,
            fdFrameBuffer,
            0);
    if(pfbmap == (unsigned int *)-1)
    {
        perror("FRAME BUFFER MMAP");
        exit(1);
    }
    memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32/8);

    //--------------------DIRECTORY---------------------
    if( (dp = opendir(".")) == NULL )
    {
        perror("DIRECTORY OPEN");
        exit(1);
    }

    while ((dent = readdir(dp))) 
    {
        if( !strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..") )
            continue;

        ext = strrchr(dent->d_name, '.');
        if(ext == NULL){}
        else{
            if( !strcmp(ext+1, "bmp") )
            {
                fileList[fileIndex++] = dent->d_name;
                printf("index%d : %s\n", fileIndex-1, fileList[fileIndex-1]);
            }
        }
    }
    fileIndex = -1;

    //-------------------BUTTON FUNCTION---------------------------
    pushSwitchBufferSize = sizeof(pushSwitchBuffer);
    printf("Press <ctrl+c> to quit. \n");
    while(!quit)
    {
        usleep(1000000);
        printf("looping.. select numbers (1~9)\n");
        read(fdPushSwitch, &pushSwitchBuffer, pushSwitchBufferSize);
        for(i=0; i < 9; i++)
        {
            if(pushSwitchBuffer[i] == 1) pushSwitchValue = i + 1;
        }        
        //scanf("%d", &pushSwitchValue); //FOR DEBUG

        switch(pushSwitchValue)
        {
            case 1: //OPEN NEXT FILE
                printf("case 1:\n");
                checkFirstRun++;        
                printf("index:%d, %s\n", fileIndex, textLcdString); //FOR DEBUG
                timeStart = clock();

                //-------------------BITMAP-----------------------
                if((++fileIndex) > 9)
                    fileIndex = 9;
                
                if(fileList[fileIndex] == NULL)
                {
                    printf("NEXT FILE NOT EXISTS, INDEX:%d\n", fileIndex);     
                    break;
                }
                memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32/8);
                fdBitmapInput = open(fileList[fileIndex], O_RDWR);
                if(fdBitmapInput < 0)
                {
                    perror("INPUT IMAGE OPEN");
                    exit(1);
                }    

                bitmapHeader = (BMPHeader*)malloc(54);
                checkRead = read(fdBitmapInput, bitmapHeader, 54);
                if(checkRead < 0)
                {
                    perror("BITMAP HEADER READ ERROR");
                    exit(1);
                }

                bitmapPixelSize = bitmapHeader-> bfSize - 54;        
                bitmapPixel = (RGBpixel**)malloc(bitmapPixelSize);
                for(i=0; i < bitmapHeader->biHeight; i++)
                {
                    bitmapPixel[i] = (RGBpixel*)malloc(sizeof(RGBpixel) * bitmapHeader->biWidth);
                }
                for(i = bitmapHeader->biHeight - 1; i >= 0; i--){
                    for(j = 0; j < bitmapHeader->biWidth; j++){
                            checkRead = read(fdBitmapInput, &bitmapPixel[i][j], sizeof(RGBpixel));
                        if(checkRead < 0)
                        {
                            perror("BITMAP PIXEL READ ERROR");
                            exit(1);
                        }
                    }
                }

                //----------------TEXT_LCD------------------
                lseek(fdTextLcd, 0, SEEK_SET);
                for(i=0; i<16; i++)
                {
                    textLcdFirstLine[i] = fileList[fileIndex][i];
                }
                sprintf(textLcdSecondLine, "%d*%d BPP:%d", bitmapHeader->biWidth, bitmapHeader->biHeight, bitmapHeader->biBitCount);
                for(i=0; i<16; i++)
                {
                fileTempIndex = fileIndex;
                    if(i < (strlen(textLcdFirstLine)))
                        textLcdString[i] = textLcdFirstLine[i];
                    else textLcdString[i] = ' ';
                }
                for(i=16; i<32; i++)
                {
                    if((i-16) < (strlen(textLcdSecondLine)))
                        textLcdString[i] = textLcdSecondLine[i-16];
                    else textLcdString[i] = ' ';
                }
                write(fdTextLcd, textLcdString, 32);

                //-----------------------DRAW IMAGE---------------------------
                for(i = 0; i < MIN(bitmapHeader->biHeight, fbvar.yres); i++)
                {
                    for(j = 0; j < MIN(bitmapHeader->biWidth, fbvar.xres); j++)
                    {
                        *(pfbmap + i * fbvar.xres + j) = makepixel(bitmapPixel[i][j], 0);
                    }
                }
                timeEnd = clock();
                printf("TIME : %f\n", ((double)(timeEnd - timeStart)/1000000));
                break;

            case 2: //OPEN PREVIOUS FILE
                printf("case 2:\n");
                checkFirstRun++;            
                printf("index:%d, %s\n", fileIndex, textLcdString); //FOR DEBUG
                                timeStart = clock();
                //-------------------BITMAP-----------------------
                if((--fileIndex) < 0)
                    fileIndex = 0;
                
                if(fileList[fileIndex] == NULL)
                {
                    printf("PREVIOUS FILE NOT EXISTS, INDEX:%d\n", fileIndex);     
                    break;
                }
                memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32/8);
                fdBitmapInput = open(fileList[fileIndex], O_RDWR);
                if(fdBitmapInput < 0)
                {
                    perror("INPUT IMAGE OPEN");
                    exit(1);
                }    

                bitmapHeader = (BMPHeader*)malloc(54);
                checkRead = read(fdBitmapInput, bitmapHeader, 54);
                if(checkRead < 0)
                {
                    perror("BITMAP HEADER READ ERROR");
                    exit(1);
                }

                bitmapPixelSize = bitmapHeader-> bfSize - 54;        
                bitmapPixel = (RGBpixel**)malloc(bitmapPixelSize);
                for(i=0; i < bitmapHeader->biHeight; i++)
                {
                    bitmapPixel[i] = (RGBpixel*)malloc(sizeof(RGBpixel) * bitmapHeader->biWidth);
                }
                for(i = bitmapHeader->biHeight - 1; i >= 0; i--){
                    for(j = 0; j < bitmapHeader->biWidth; j++){
                            checkRead = read(fdBitmapInput, &bitmapPixel[i][j], sizeof(RGBpixel));
                        if(checkRead < 0)
                        {
                            perror("BITMAP PIXEL READ ERROR");
                            exit(1);
                        }
                    }
                }
    
                //----------------TEXT_LCD------------------
                lseek(fdTextLcd, 0, SEEK_SET);
                for(i=0; i<16; i++)
                {
                    textLcdFirstLine[i] = fileList[fileIndex][i];
                }
                sprintf(textLcdSecondLine, "%d*%d BPP:%d", bitmapHeader->biWidth, bitmapHeader->biHeight, bitmapHeader->biBitCount);
                for(i=0; i<16; i++)
                {
                    if(i < (strlen(textLcdFirstLine)))
                        textLcdString[i] = textLcdFirstLine[i];
                    else textLcdString[i] = ' ';
                }
                for(i=16; i<32; i++)
                {
                    if((i-16) < (strlen(textLcdSecondLine)))
                        textLcdString[i] = textLcdSecondLine[i-16];
                    else textLcdString[i] = ' ';
                }
                write(fdTextLcd, textLcdString, 32);

                //-----------------------DRAW IMAGE---------------------------
                for(i = 0; i < MIN(bitmapHeader->biHeight, fbvar.yres); i++)
                {
                    for(j = 0; j < MIN(bitmapHeader->biWidth, fbvar.xres); j++)
                    {
                        *(pfbmap + i * fbvar.xres + j) = makepixel(bitmapPixel[i][j], 0);
                    }
                }
                timeEnd = clock();
                printf("TIME : %f\n", ((double)(timeEnd - timeStart)/1000000));
                break;

            case 3: //CLEAR FRAME BUFFER
                printf("case 3:\n");
                memset(pfbmap, 0, fbvar.xres * fbvar.yres * 32/8);
                break;

            case 4: //INCREASE BRIGHTNESS
                printf("case 4:\n");
                if(checkFirstRun == 0){
                    printf("IMAGE IS NOT OPENED, PUSH SWITCH 1 OR 2\n");
                    break;
                }
                brightness += 30;
                if(brightness > 256) brightness = 256;
                
                for(i = 0; i < MIN(bitmapHeader->biHeight, fbvar.yres); i++)
                {
                    for(j = 0; j < MIN(bitmapHeader->biWidth, fbvar.xres); j++)
                    {
                        *(pfbmap + i * fbvar.xres + j) = makepixel(bitmapPixel[i][j], brightness);
                    }
                }
                break;

            case 5: //DECREASE BRIGHTNESS
                printf("case 5:\n");
                if(checkFirstRun == 0){
                    printf("IMAGE IS NOT OPENED, PUSH SWITCH 1 OR 2\n");
                    break;
                }
                brightness -= 30;
                if(brightness < -256) brightness = -256;
                
                for(i = 0; i < MIN(bitmapHeader->biHeight, fbvar.yres); i++)
                {
                    for(j = 0; j < MIN(bitmapHeader->biWidth, fbvar.xres); j++)
                    {
                        *(pfbmap + i * fbvar.xres + j) = makepixel(bitmapPixel[i][j], brightness);
                    }
                }
                break;

            case 6: //FRAME BUFFER CAPTURE
                printf("case 6:\n");
                timeStart = clock();
                if(checkFirstRun == 0){
                    printf("IMAGE IS NOT OPENED, PUSH SWITCH 1 OR 2\n");
                    break;
                }
                bitmapOutputHeader = *bitmapHeader;                                
                bitmapOutputHeader.bfSize = MIN(bitmapHeader->biWidth, fbvar.xres) * MIN(bitmapHeader->biHeight, fbvar.yres) * 3 + 54;           // 파일 크기
                bitmapOutputHeader.biSizeImage = MIN(bitmapHeader->biWidth, fbvar.xres) * MIN(bitmapHeader->biHeight, fbvar.yres) * 3;      // 비트맵 이미지의 픽셀 데이터 크기
                bitmapOutputHeader.biWidth = MIN(bitmapHeader->biWidth, fbvar.xres);          // 비트맵 이미지의 가로 크기
                bitmapOutputHeader.biHeight = MIN(bitmapHeader->biHeight, fbvar.yres);         // 비트맵 이미지의 세로 크기
                //bitmapOutputHeader.biXPelsPerMeter;  // 그림의 가로 해상도(미터당 픽셀)
                //bitmapOutputHeader.biYPelsPerMeter;  // 그림의 세로 해상도(미터당 픽셀)

                if(access(OUTPUT_BITMAP_FILE, F_OK) >= 0) //BITMAP OUTPUT FILE DELETE
                    unlink(OUTPUT_BITMAP_FILE);

                fdBitmapOutput = open(OUTPUT_BITMAP_FILE, O_CREAT | O_WRONLY, 0777); //BITMAP OUTPUT FILE OPEN
                if(fdBitmapOutput < 0)
                {
                    perror("OUTPUT IMAGE OPEN");
                    exit(1);
                }
                if(write(fdBitmapOutput, &bitmapOutputHeader, 54) < 0){
                    perror("WRITE BITMAP HEADER");
                    exit(1);
                }
                for(i = MIN(bitmapHeader->biHeight, fbvar.yres) - 1; i >= 0; i--)
                {
                    for(j = 0; j < MIN(bitmapHeader->biWidth, fbvar.xres); j++)
                    {
                        if(write(fdBitmapOutput, pfbmap + i * fbvar.xres + j, 24/8) < 0){
                            perror("WRITE BITMAP PIXEL");
                            exit(1);
                        } 
                    }
                }
                fileTempIndex = fileIndex;
                fileIndex = 0;
                rewinddir(dp);
                while ((dent = readdir(dp))) 
                {
                    if( !strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..") )
                        continue;

                    ext = strrchr(dent->d_name, '.');
                    if(ext == NULL) {}
                    else{
                        if( !strcmp(ext + 1, "bmp") )
                        {
                            fileList[fileIndex++] = dent->d_name;
                            printf("index%d : %s\n", fileIndex-1, fileList[fileIndex-1]);
                        }
                    }
                }
                fileIndex = fileTempIndex;
                timeEnd = clock();
                printf("TIME : %f\n", ((double)(timeEnd - timeStart)/1000000));
                break;
                
            default:
                break;
        }
        pushSwitchValue = 0;
    }   
    munmap(pfbmap, fbvar.xres*fbvar.yres*32/8);
    closedir(dp);
    close(fdTextLcd);
    close(fdPushSwitch);
    close(fdFrameBuffer);
    close(fdBitmapInput);
    close(fdBitmapOutput);

    return 0;
}

