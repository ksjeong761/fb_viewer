#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h> 
#include <unistd.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <dirent.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>

#include "fbbmp.h"

unsigned char quit = 0;          // 무한 반복문 종료를 위한 변수
int frameBufferBPP = BPP_32;     // 프레임 버퍼의 BPP를 설정하기 위한 변수
bool isDeviceConnected = false;  // 장치가 연결되어 있는지 확인하기 위한 변수

// 매개변수가 없을 시 32BPP로, 실제 장치와 관계 없이 콘솔에서만 동작한다.
int main(int argc, char* argv[])
{
    // 1번 매개변수를 통해 16BPP 모드로 전환한다. 하드웨어가 지원하지 않을 경우 사용할 수 없다.
    if (argc >= 2)
    {
        if (atoi(argv[1]) == BPP_16 || atoi(argv[1]) == BPP_32)
        {
            frameBufferBPP = atoi(argv[1]);
        }
        else
        {
            printf("Invalid argument 1 - ex) ./fbbmp 16\n");
            exit(1);
        }
    }

    // 2번 매개변수를 통해 실제 장치가 연결되었을 때와 동일하게 동작하도록 한다.
    if (argc >= 3)
    {
        if (!strcmp(argv[2], "device"))
        {
            isDeviceConnected = true;
        }
        else
        {
            printf("Invalid argument 2 - ex) ./fbbmp 32 device\n");
            exit(1);
        }
    }

    int pushSwitchIndex = 0;    // 지속적으로 푸시 스위치를 스캔해서 눌린 버튼을 찾는다.
    unsigned char pushSwitchBuffer[PUSH_SWITCH_BUFFER_SIZE] = {0};      // 버튼이 눌리면 값이 1로 변한다.
    unsigned char textLCDBuffer[TEXT_LCD_HEIGHT][TEXT_LCD_WIDTH] = {0}; // 파일명, 해상도 및 BPP(Bits Per Pixel)를 표시한다.
    
    // 장치 드라이버가 연결되어 있다면 읽기
    int fdPushSwitch = 0;
    int fdTextLcd = 0;
    if (isDeviceConnected)
    {
        // Push Switch 드라이버 열기
        fdPushSwitch = open(DEVICE_PUSH_SWITCH, O_RDWR);
        if (fdPushSwitch < 0)
        {
            perror("Failed to open driver - Push switch");
            exit(1);
        }

        // Text LCD 드라이버 열기
        fdTextLcd = open(DEVICE_TEXT_LCD, O_WRONLY); 
        if (fdTextLcd < 0)
        {
            perror("Failed to open driver - Text LCD");
            exit(1);
        }

        // Text LCD 초기화
        write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE); 
    }

    // 무한 반복문을 종료하기 위한 시그널 등록
    (void)signal(SIGINT, signalCallbackQuit);

    // 프레임 버퍼 열기
    int fdFrameBuffer = open(DEVICE_FRAME_BUFFER, O_RDWR);
    if (fdFrameBuffer < 0)
    {
        perror("Failed to open driver - Frame buffer");
        exit(1);
    }

    // 프레임 버퍼 가변 정보 얻어오기
    struct fb_var_screeninfo fbvar;
    if (ioctl(fdFrameBuffer, FBIOGET_VSCREENINFO, &fbvar) < 0)
    {
        perror("Failed to get variable screen info of frame buffer.");
        exit(1);
    }

    // 프레임 버퍼의 BPP를 변경
    fbvar.bits_per_pixel = frameBufferBPP;
    if (ioctl(fdFrameBuffer, FBIOPUT_VSCREENINFO, &fbvar) < 0)
    {
        perror("Failed to set BPP by ioctl.");
        exit(1);
    }

    // 변경되지 않은 경우 처리
    if (fbvar.bits_per_pixel != frameBufferBPP)
    {
        perror("BPP is not changed.");
        exit(1);
    }

    // 프레임 버퍼 크기만큼 디바이스 메모리 주소와 포인터의 메모리 주소를 연결한다.
    unsigned int *pfbmap = (unsigned int *)mmap(
        0,                                  // 할당 받고자 하는 메모리 주소 (0을 지정하면 커널에 의해 임의로 할당된 주소를 받는다.)
        calculateFrameBufferSize(fbvar),    // 프레임 버퍼 크기
        PROT_READ|PROT_WRITE,               // 매핑된 파일에 읽기, 쓰기를 허용
        MAP_SHARED,                         // 다른 프로세스와 매핑을 공유하는 옵션
        fdFrameBuffer,                      // 프레임 버퍼 드라이버 파일 디스크립터 (/dev/fb0)
        0);                                 // 오프셋 (0 ~ 프레임 버퍼 크기까지 매핑)

    if (pfbmap == (unsigned int *)-1)
    {
        perror("Failed to map frame buffer to memory.");
        exit(1);
    }

    // 프레임 버퍼를 초기화한다.
    clearFrameBuffer(pfbmap, fbvar);

    // 비트맵 이미지 관련
    BMPHeader *pBitmapHeader = NULL;        // 입력 비트맵 헤더 구조체
    RGBpixel **pBitmapPixel2dArray = NULL;  // RGB 각 8비트로 구성된 24비트 픽셀
    int fileIndex = -1;                     // 1, 2번 버튼으로 다루게 될 pFileNameArray 배열의 인덱스
    int brightness = 0;                     // 4, 5번 버튼으로 조절할 픽셀의 밝기

    // 비트맵 확장자를 가진 파일 목록 수집
    unsigned char *pFileNameArray[FILE_NAME_ARRAY_SIZE] = {0};
    searchFilesInPathByExtention(pFileNameArray, ".", BITMAP_EXTENSION);

    // 프로그램 사용법을 콘솔에 출력한다.
    printUsageOnConsole();

    while (!quit)
    {
        // 장치가 연결되어 있다면 Push Switch 입력을 받고 인덱스에 1을 더해준다. (0~8 -> 1~9)
        int pushSwitchValue = 0;
        if (isDeviceConnected)
        {
            read(fdPushSwitch, &pushSwitchBuffer, PUSH_SWITCH_BUFFER_SIZE);
            for (pushSwitchIndex=0; pushSwitchIndex < PUSH_SWITCH_BUFFER_SIZE; pushSwitchIndex++)
            {
                if (pushSwitchBuffer[pushSwitchIndex] == 1)
                {
                    pushSwitchValue = pushSwitchIndex + 1;
                }
            }
        }
        // 장치가 없으면 콘솔에서 숫자로 입력받는다.
        else
        {
            scanf("%d", &pushSwitchValue);
            while ( getchar() != '\n' );
        }

        // 시간을 측정한다.
        clock_t timeStart = clock();
        
        switch (pushSwitchValue)
        {
            // 다음 이미지 열기
            case 1:
                clearFrameBuffer(pfbmap, fbvar);
                brightness = 0;

                // 다음 파일이 있는지 체크
                fileIndex = thresholding(fileIndex + 1, 0, FILE_NAME_ARRAY_SIZE - 1);
                if (!pFileNameArray[fileIndex])
                {
                    printf("There isn't next file, index:%d\n", fileIndex); 
                    fileIndex--;    
                    break;
                }

                // 다음 파일이 있다면 이미지 읽어오기
                loadBitmapImage(pfbmap, fbvar, &pBitmapHeader, &pBitmapPixel2dArray, pFileNameArray[fileIndex]);

                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, pBitmapHeader, pBitmapPixel2dArray, brightness);

                // Text LCD를 통해 헤더 정보(파일명, 해상도, BPP) 출력
                memcpy(textLCDBuffer[0], pFileNameArray[fileIndex], TEXT_LCD_BUFFER_SIZE);
                sprintf(textLCDBuffer[1], "%d*%d BPP:%d", pBitmapHeader->biWidth, pBitmapHeader->biHeight, pBitmapHeader->biBitCount);
                printf("Text LCD : [%s] / [%s]\n", textLCDBuffer[0], textLCDBuffer[1]);
                
                if (isDeviceConnected)
                {
                    lseek(fdTextLcd, 0, SEEK_SET);
                    write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE);
                }
                break;

            // 이전 이미지 열기
            case 2:
                clearFrameBuffer(pfbmap, fbvar);
                brightness = 0;

                // 이전 파일이 있는지 체크
                fileIndex = thresholding(fileIndex - 1, 0, FILE_NAME_ARRAY_SIZE - 1);
                if (!pFileNameArray[fileIndex])
                {
                    printf("There isn't previous file, index:%d\n", fileIndex);    
                    fileIndex++;
                    break;
                }
                
                // 이전 파일이 있다면 이미지 읽어오기
                loadBitmapImage(pfbmap, fbvar, &pBitmapHeader, &pBitmapPixel2dArray, pFileNameArray[fileIndex]);

                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, pBitmapHeader, pBitmapPixel2dArray, brightness);

                // Text LCD를 통해 헤더 정보(파일명, 해상도, BPP) 출력
                memcpy(textLCDBuffer[0], pFileNameArray[fileIndex], TEXT_LCD_BUFFER_SIZE);
                sprintf(textLCDBuffer[1], "%d*%d BPP:%d", pBitmapHeader->biWidth, pBitmapHeader->biHeight, pBitmapHeader->biBitCount);
                printf("Text LCD : [%s] / [%s]\n", textLCDBuffer[0], textLCDBuffer[1]);
                
                if (isDeviceConnected)
                {
                    lseek(fdTextLcd, 0, SEEK_SET);
                    write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE);
                }
                break;

            // 프레임 버퍼 비우기
            case 3:
                if (isImageLoaded(pBitmapHeader, pBitmapPixel2dArray))
                {
                    free(pBitmapHeader);
                    
                    for (int rowIndex = pBitmapHeader->biHeight-1; rowIndex >= 0; rowIndex--)
                    {
                        free(pBitmapPixel2dArray[rowIndex]);
                    }
                    free(pBitmapPixel2dArray);

                    pBitmapHeader = NULL;
                    pBitmapPixel2dArray = NULL;
                }

                clearFrameBuffer(pfbmap, fbvar);
                break;

            // 프레임 버퍼 밝기 증가
            case 4:
                // 읽어온 이미지가 있어야 동작 가능하다.
                if (!isImageLoaded(pBitmapHeader, pBitmapPixel2dArray))
                {
                    clearFrameBuffer(pfbmap, fbvar);
                    printf("There isn't any loaded image.\n");
                    break;
                }

                // 밝기 조절
                brightness = thresholding(brightness + BRIGHTNESS_DELTA, -UCHAR_MAX, UCHAR_MAX);

                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, pBitmapHeader, pBitmapPixel2dArray, brightness);

                // 밝기를 변경시킨 경우 break 대신 continue를 사용하여 콘솔 메시지 출력을 건너뛴다.
                continue;

            // 프레임 버퍼 밝기 감소
            case 5:
                // 읽어온 이미지가 있어야 동작 가능하다.
                if (!isImageLoaded(pBitmapHeader, pBitmapPixel2dArray))
                {
                    clearFrameBuffer(pfbmap, fbvar);
                    printf("There isn't any loaded image.\n");
                    break;
                }

                // 밝기 조절
                brightness = thresholding(brightness - BRIGHTNESS_DELTA, -UCHAR_MAX, UCHAR_MAX);
                
                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, pBitmapHeader, pBitmapPixel2dArray, brightness);
                
                // 밝기를 변경시킨 경우 break 대신 continue를 사용하여 콘솔 메시지 출력을 건너뛴다.
                continue;

            // 프레임 버퍼 캡처
            case 6:
                // 읽어온 이미지가 있어야 동작 가능하다.
                if (!isImageLoaded(pBitmapHeader, pBitmapPixel2dArray))
                {
                    clearFrameBuffer(pfbmap, fbvar);
                    printf("There isn't any loaded image.\n");
                    break;
                }

                // 프레임 버퍼 캡처
                captureFrameBuffer(pfbmap, fbvar, pBitmapHeader);

                // 새 파일이 추가되었으므로 파일 목록을 다시 불러온다.
                searchFilesInPathByExtention(pFileNameArray, ".", BITMAP_EXTENSION);
                break;

            default:
                system("clear");
                printf("Invalid number.\n");
                break;
        }

        // 시간을 측정한다.
        clock_t timeEnd = clock();

        // 오래 걸리는 기능을 실행했을 경우에만 시간을 출력한다.
        if (pushSwitchValue == 1 || pushSwitchValue == 2 || pushSwitchValue == 6)
        {
            printf("TIME : %f\n", ((double)(timeEnd - timeStart)/1000000));
        }

        pushSwitchValue = 0;

        // 조작법을 콘솔에 출력한다.
        printUsageOnConsole();
    }

    // 장치 드라이버 닫기
    close(fdFrameBuffer);
    if (isDeviceConnected)
    {
        close(fdTextLcd);
        close(fdPushSwitch);
    }

    // 메모리 매핑 해제
    munmap(pfbmap, calculateFrameBufferSize(fbvar));

    // 동적 할당된 메모리 해제
    if (isImageLoaded(pBitmapHeader, pBitmapPixel2dArray))
    {
        free(pBitmapHeader);

        for (int rowIndex = pBitmapHeader->biHeight-1; rowIndex >= 0; rowIndex--)
        {
            free(pBitmapPixel2dArray[rowIndex]);
        }
        free(pBitmapPixel2dArray);
    }

    for (int fileNameArrayIndex = 0; fileNameArrayIndex < FILE_NAME_ARRAY_SIZE; fileNameArrayIndex++)
    {
        free(pFileNameArray[fileNameArrayIndex]);
    }
    
    return 0;
}
