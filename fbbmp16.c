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
	unsigned short bfType;        	 // BMP 파일 매직 넘버
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

unsigned short makepixel(RGBpixel pixel, int brightness)
{
	int clamping = 0;
	
	clamping = pixel.red + brightness;
	if(clamping > 255) pixel.red = 255;
	else if(clamping < 0) pixel.red = 0;
    pixel.red = pixel.red >> 3;

	clamping = pixel.green + brightness;
	if(clamping > 255) pixel.green = 255;
	else if(clamping < 0) pixel.green = 0;
    pixel.green = pixel.green >> 3;

	clamping = pixel.blue + brightness;
	if(clamping > 255) pixel.blue = 255;
	else if(clamping < 0) pixel.blue = 0;
    pixel.blue = pixel.blue >> 3;

	return ((pixel.blue << 11) | (pixel.green << 6) | (pixel.red << 0));
}

void user_signal1(int sig) 
{
	quit = 1;
}

int main(void)
{
	int fdBitmapInput;
	int fdBitmapOutput;
	int fdPushSwitch;
	int fdTextLcd;
	int fdFrameBuffer;

	int bitmapPixelSize;
	BMPHeader * bitmapHeader;
	BMPHeader bitmapOutputHeader;
	RGBpixel ** bitmapPixel;
	int brightness = 0;	

	int pushSwitchValue = 0;
	int pushSwitchBufferSize;
	unsigned char pushSwitchBuffer[9] = {0,0,0,0,0,0,0,0,0};

	unsigned char textLcdString[32] = {0};
	unsigned char textLcdFirstLine[16] = {0};
	unsigned char textLcdSecondLine[16] = {0};

	struct fb_var_screeninfo fbvar;
	unsigned short *pfbmap;

	DIR *dp;
	struct dirent *dent;
	char * ext;

	unsigned char *fileList[10] = {NULL};
	int fileIndex = 0;
	int fileTempIndex = 0;

    unsigned short mask1 = 65535-2047;
	unsigned short mask2 = 2047-31;
	unsigned short mask3 = 31;
	unsigned short maskedBlue = 65535-2047;
	unsigned short maskedGreen = 2047-31;
	unsigned short maskedRed = 31;
    unsigned short temp = 0;
	unsigned int temp2 = 0;
    unsigned char tempRed = 0;
    unsigned char tempGreen = 0;
    unsigned char tempBlue = 0;

    clock_t timeStart;
    clock_t timeEnd;
    double timeResult;

	int i, j, checkRead, checkFirstRun;
	
	//------------------PUSH SWITCH------------------
	fdPushSwitch = open(DEVICE_PUSH_SWITCH, O_RDWR);
	if (fdPushSwitch < 0)
	{
		perror("PUSH SWITCH OPEN");
		exit(1);
	}
	(void)signal(SIGINT, user_signal1);
	//-------------------TEXT LCD--------------------
	fdTextLcd = open(DEVICE_TEXT_LCD, O_WRONLY);
	if (fdTextLcd < 0)
	{
		perror("TEXT LCD OPEN");
		exit(1);
	}
	for(i=0; i<32; i++)
	{
		textLcdString[i] = ' ';
	}
	write(fdTextLcd, textLcdString, 32);
	
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
    fbvar.bits_per_pixel = 16;
    
    if(ioctl(fdFrameBuffer, FBIOPUT_VSCREENINFO, &fbvar) < 0)
    {
        perror("FRAME BUFFER IOCTL PUT");
        exit(1);
    }

	if(fbvar.bits_per_pixel != 16)
	{
		fprintf(stderr, "BPP IS NOT 16, CURRENT BPP IS \n");
		printf("%d", fbvar.bits_per_pixel);
		exit(1);
	}

	pfbmap = (unsigned short *)mmap(
            0,
            fbvar.xres * fbvar.yres * 16/8,
	        PROT_READ|PROT_WRITE,
            MAP_SHARED,
            fdFrameBuffer,
            0);
	if(pfbmap == (unsigned short *)-1)
	{
		perror("FRAME BUFFER MMAP");
		exit(1);
	}
	memset(pfbmap, 0, fbvar.xres * fbvar.yres * 16/8);

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
				memset(pfbmap, 0, fbvar.xres * fbvar.yres * 16/8);
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
				memset(pfbmap, 0, fbvar.xres * fbvar.yres * 16/8);
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
				memset(pfbmap, 0, fbvar.xres * fbvar.yres * 16/8);
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
						temp = *(pfbmap + i * fbvar.xres + j);
						maskedBlue = temp & mask1;
						maskedGreen = temp & mask2;
						maskedRed = temp & mask3;
						tempBlue = maskedBlue >> 11;
						tempBlue = tempBlue << 3;
						tempGreen = maskedGreen >> 6;
						tempGreen = tempGreen << 3;
						tempRed = maskedRed >> 0;
						tempRed = tempRed << 3;
						temp2 = ((tempRed << 0) | (tempGreen << 8) | (tempBlue << 16));
						if(write(fdBitmapOutput, &temp2, 24/8) < 0){
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
	munmap(pfbmap, fbvar.xres*fbvar.yres*16/8);
	closedir(dp);
	close(fdTextLcd);
	close(fdPushSwitch);
	close(fdFrameBuffer);
	close(fdBitmapInput);
	close(fdBitmapOutput);

	return 0;
}

