#include "fbbmp.h"

int main(int argc, char* argv[])
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

    // 매개변수가 없을 시 32BPP, 실제 장치와 관계 없이 콘솔에서만 동작한다.
    if (argc >= 2)
    {
        // 1번 매개변수를 통해 16BPP 모드로 전환한다. 하드웨어가 지원하지 않을 경우 사용할 수 없다.
        if (atoi(argv[1]) == BPP_16)
        {
            frameBufferBPP = atoi(argv[1]);
        }
        else
        {
            printf("INVALID ARGUMENT 1 - ex) ./fbbmp 16");
            exit(1);
        }
    }

    if (argc >= 3)
    {
        // 2번 매개변수를 통해 실제 장치가 연결되었을 때와 동일하게 동작하도록 한다.
        if (!strcmp(argv[2], "device"))
        {
            isDeviceConnected = true;
        }
        else
        {
            printf("INVALID ARGUMENT 2 - ex) ./fbbmp 32 device");
            exit(1);
        }
    }

    if (isDeviceConnected)
    {
        // Push Switch 드라이버 열기
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
        // Text LCD를 ' '로 초기화
        write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE); 
    }


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
        fbvar.xres * fbvar.yres * frameBufferBPP / 8, //프레임 버퍼 해상도
        PROT_READ|PROT_WRITE,                         //매핑된 파일에 읽기, 쓰기를 허용
        MAP_SHARED,                                   //다른 프로세스와 매핑을 공유하는 옵션
        fdFrameBuffer,                                //프레임 버퍼 파일 디스크립터(/dev/fb0)
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

        if (isDeviceConnected)
        {
            // Push Switch 입력을 받고 인덱스에 1을 더해준다. (0~8 -> 1~9)
            read(fdPushSwitch, &pushSwitchBuffer, PUSH_SWITCH_BUFFER_SIZE);
            for(pushSwitchIndex=0; pushSwitchIndex < PUSH_SWITCH_BUFFER_SIZE; pushSwitchIndex++)
            {
                if (pushSwitchBuffer[pushSwitchIndex] == 1)
                {
                    pushSwitchValue = pushSwitchIndex + 1;
                }
            }
        }
        else
        {
            // 장치가 없으면 콘솔에서 숫자로 입력받는다.
            scanf("%d", &pushSwitchValue);
        }

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
                memcpy(textLCDBuffer[0], fileList[fileIndex], TEXT_LCD_BUFFER_SIZE);
                sprintf(textLCDBuffer[1], "%d*%d BPP:%d", bitmapHeader->biWidth, bitmapHeader->biHeight, bitmapHeader->biBitCount);
                printf("Text LCD : [%s] / [%s]\n", textLCDBuffer[0], textLCDBuffer[1]);
                
                if (isDeviceConnected)
                {
                    lseek(fdTextLcd, 0, SEEK_SET);
                    write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE);
                }

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
                memcpy(textLCDBuffer[0], fileList[fileIndex], TEXT_LCD_BUFFER_SIZE);
                sprintf(textLCDBuffer[1], "%d*%d BPP:%d", bitmapHeader->biWidth, bitmapHeader->biHeight, bitmapHeader->biBitCount);
                printf("Text LCD : [%s] / [%s]\n", textLCDBuffer[0], textLCDBuffer[1]);
                
                if (isDeviceConnected)
                {
                    lseek(fdTextLcd, 0, SEEK_SET);
                    write(fdTextLcd, textLCDBuffer, TEXT_LCD_BUFFER_SIZE);
                }
                
                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 3:
                printf("case 3 : CLEAR FRAME BUFFER\n");

                clearFrameBuffer(pfbmap, fbvar);
                free(bitmapHeader);
                free(bitmapPixel);
                isImageLoaded = false;
                break;

            case 4:
                printf("case 4 : INCREASE BRIGHTNESS\n");
                
                // 읽어온 이미지가 있어야 동작 가능하다.
                if (!isImageLoaded)
                {
                    printf("IMAGE IS NOT LOADED\n");
                    break;
                }

                // 밝기 조절
                brightness = thresholding(brightness + 30, -255, 255);

                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 5:
                printf("case 5: DECREASE BRIGHTNESS\n");

                // 읽어온 이미지가 있어야 동작 가능하다.
                if (!isImageLoaded)
                {
                    printf("IMAGE IS NOT LOADED\n");
                    break;
                }

                // 밝기 조절
                brightness = thresholding(brightness - 30, -255, 255);
                
                // 프레임 버퍼에 이미지 출력
                drawImageOnFrameBuffer(pfbmap, fbvar, bitmapHeader, bitmapPixel, brightness);
                break;

            case 6:
                printf("case 6 : CAPTURE FRAME BUFFER\n");
                
                // 읽어온 이미지가 있어야 동작 가능하다.
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

        // 시간 재기
        timeEnd = clock();
        if (pushSwitchValue == 1 || pushSwitchValue == 2 || pushSwitchValue == 6)
        {
            printf("TIME : %f\n", ((double)(timeEnd - timeStart)/1000000));
        }

        pushSwitchValue = 0;
    }

    munmap(pfbmap, fbvar.xres * fbvar.yres * frameBufferBPP / 8);
    close(fdFrameBuffer);

    if (isDeviceConnected)
    {
        close(fdTextLcd);
        close(fdPushSwitch);
    }

    return 0;
}
