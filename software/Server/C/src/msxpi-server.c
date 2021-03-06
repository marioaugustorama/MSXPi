/*
 ;|===========================================================================|
 ;|                                                                           |
 ;| MSXPi Interface                                                           |
 ;|                                                                           |
 ;| Version : 0.8.1                                                           |
 ;|                                                                           |
 ;| Copyright (c) 2015-2016 Ronivon Candido Costa (ronivon@outlook.com)       |
 ;|                                                                           |
 ;| All rights reserved                                                       |
 ;|                                                                           |
 ;| Redistribution and use in source and compiled forms, with or without      |
 ;| modification, are permitted under GPL license.                            |
 ;|                                                                           |
 ;|===========================================================================|
 ;|                                                                           |
 ;| This file is part of MSXPi Interface project.                             |
 ;|                                                                           |
 ;| MSX PI Interface is free software: you can redistribute it and/or modify  |
 ;| it under the terms of the GNU General Public License as published by      |
 ;| the Free Software Foundation, either version 3 of the License, or         |
 ;| (at your option) any later version.                                       |
 ;|                                                                           |
 ;| MSX PI Interface is distributed in the hope that it will be useful,       |
 ;| but WITHOUT ANY WARRANTY; without even the implied warranty of            |
 ;| MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             |
 ;| GNU General Public License for more details.                              |
 ;|                                                                           |
 ;| You should have received a copy of the GNU General Public License         |
 ;| along with MSX PI Interface.  If not, see <http://www.gnu.org/licenses/>. |
 ;|===========================================================================|
 ; 
 ; File history :
 ; 0.8.1  : MSX-DOS working properly.
 ; 0.8    : Rewritten with new protocol-v2
 ;          New functions, new main loop, new framework for better reuse
 ;          This version now includes MSX-DOS 1.03 driver
 ; 0.7    : Commands CD and MORE working for http, ftp, nfs, win, local files.
 ; 0.6d   : Added http suport to LOAD and FILES commands
 ; 0.6c   : Initial version commited to git
 ;
 
 PI pinout x GPIO:
 http://abyz.co.uk/rpi/pigpio/index.html
 
 
 Library required by this program and how to install:
 http://abyz.co.uk/rpi/pigpio/download.html
 
 Steps:
 sudo apt-get install libcurl4-nss-dev
 wget abyz.co.uk/rpi/pigpio/pigpio.tar
 tar xf pigpio.tar
 cd PIGPIO
 make -j4
 sudo make install
 
 To compile and run this program:
 cc -Wall -pthread -o msxpi-server msxpi-server.c -lpigpio -lrt -lcurl
 
 */

#include <stdio.h>
#include <pigpio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <fcntl.h>

#define version     "0.8.1"
#define V07SUPPORT
#define DISKIMGPATH "/home/pi/msxpi/disks"
#define HOMEPATH "/home/pi/msxpi"

/* GPIO pin numbers used in this program */

#define cs    21
#define sclk  20
#define mosi  16
#define miso  12
#define rdy   25

#define SPI_SCLK_LOW_TIME 0
#define SPI_SCLK_HIGH_TIME 0
#define HIGH 1
#define LOW 0
#define command 1
#define binary  2

#define GLOBALRETRIES      5

#define SPI_INT_TIME            3000
#define PIWAITTIMEOUTOTHER      120      // seconds
#define PIWAITTIMEOUTBIOS       60      // seconds
#define SYNCTIMEOUT             5
#define BYTETRANSFTIMEOUT       5
#define SYNCTRANSFTIMEOUT       3

#define RC_SUCCESS              0xE0
#define RC_INVALIDCOMMAND       0xE1
#define RC_CRCERROR             0xE2
#define RC_TIMEOUT              0xE3
#define RC_INVALIDDATASIZE      0xE4
#define RC_OUTOFSYNC            0xE5
#define RC_FILENOTFOUND         0xE6
#define RC_FAILED               0xE7
#define RC_INFORESPONSE         0xE8
#define RC_UNDEFINED            0xEF

#define st_init                 0       // waiting loop, waiting for a command
#define st_cmd                  1       // transfering data for a command
#define st_recvdata             2
#define st_senddata             4
#define st_synch                5       // running a command received from MSX
#define st_runcmd               6
#define st_shutdown             99

// commands
#define CMDREAD         0x00
#define LOADROM         0x01
#define LOADCLIENT      0x02

// from 0x03 to 0xF reserver
// 0xAA - 0xAF : Control code
#define STARTTRANSFER   0xA0
#define SENDNEXT        0xA1
#define ENDTRANSFER     0xA2
#define READY           0xAA
#define ABORT           0xAD
#define WAIT            0xAE

#define WIFICFG         0x1A
#define CMDDIR          0x1D
#define CMDPIFSM        0x33
#define CMDGETSTAT      0x55
#define SHUTDOWN        0x66
#define DATATRANSF      0x77
#define CMDSETPARM      0x7A
#define CMDSETPATH      0x7B
#define CMDPWD          0x7C
#define CMDMORE         0x7D
#define CMDPATHERR1     0x7E
#define UNKERR          0x98
#define FNOTFOUND       0x99
#define PI_READY        0xAA
#define NOT_READY       0xAF
#define RUNPICMD        0xCC
#define CMDSETVAR       0xD1
#define NXTDEV_INFO     0xE0
#define NXTDEV_STATUS   0xE1
#define NXTDEV_RSECT    0xE2
#define NXTDEV_WSECT    0xE3
#define NXTLUN_INFO     0xE4
#define CMDERROR        0xEE
#define FNOTFOUD        0xEF
#define CMDLDFILE       0xF1
#define CMDSVFILE       0xF5
#define CMDRESET        0xFF
#define RAW     0
#define LDR     1
#define CLT     2
#define BIN     3
#define ROM     4
#define FSLOCAL     1
#define FSUSB1      2
#define FSUSB2      3
#define FSNFS       4
#define FSWIN       5
#define FSHTTP      6
#define FSHTTPS     7
#define FSFTP       8
#define FSFTPS      9

typedef struct {
    unsigned char rc;
             int  datasize;
} transferStruct;

typedef struct {
    unsigned char appstate;
    unsigned char pibyte;
    unsigned char msxbyte;
    unsigned char datasize;
    unsigned char data[32768];
    unsigned char bytecounter;
    unsigned char crc;
    unsigned char rc;
             char stdout[255];
             char stderr[255];
} MSXData;

typedef struct {
    unsigned char deviceNumber;
    unsigned char mediaDescriptor;
    unsigned char logicUnitNumber;
    unsigned char sectors;
    int           initialSector;
} DOS_SectorStruct;

struct DiskImgInfo {
    int rc;
    char dskname[65];
    unsigned char *data;
    unsigned char deviceNumber;
    double size;
};

struct psettype {
    char var[16];
    char value[129];
};

struct curlMemStruct {
    char *memory;
    size_t size;
};
typedef struct curlMemStruct MemoryStruct;

unsigned char appstate = st_init;
unsigned char msxbyte;
unsigned char msxbyterdy;
unsigned char pibyte;

//Tools for waiting for a new command
pthread_mutex_t newComMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t newComCond  = PTHREAD_COND_INITIALIZER;

void delay(unsigned int secs) {
    unsigned int retTime = time(0) + secs;   // Get finishing time.
    while (time(0) < retTime);               // Loop until it arrives.
}

char** str_split(char* a_str, const char a_delim) {
    char** result    = 0;
    size_t count     = 0;
    char* tmp        = a_str;
    char* last_comma = 0;
    char delim[2];
    delim[0] = a_delim;
    delim[1] = 0;
    
    /* Count how many elements will be extracted. */
    while (*tmp)
    {
        if (a_delim == *tmp)
        {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }
    
    // fix for bug when delimiter is "/"
    if (a_delim==0x2f)
        count--;
    
    /* Add space for trailing token. */
    count += last_comma < (a_str + strlen(a_str) - 1);
    
    /* Add space for terminating null string so caller
     knows where the list of returned strings ends. */
    count++;
    
    result = malloc(sizeof(char*) * count);
    
    if (result)
    {
        size_t idx  = 0;
        char* token = strtok(a_str, delim);
        
        while (token)
        {
            assert(idx < count);
            *(result + idx++) = strdup(token);
            token = strtok(0, delim);
            //printf("token,idx,count = %s,%i,%i\n",token,idx,count);
        }
        
        assert(idx == count - 1);
        *(result + idx) = 0;
    }
    
    return result;
}

void init_spi_bitbang(void) {
    gpioSetMode(cs, PI_INPUT);
    gpioSetMode(sclk, PI_OUTPUT);
    gpioSetMode(mosi, PI_INPUT);
    gpioSetMode(miso, PI_OUTPUT);
    gpioSetMode(rdy, PI_OUTPUT);
    
    gpioSetPullUpDown(cs, PI_PUD_UP);
    gpioSetPullUpDown(mosi, PI_PUD_DOWN);
    
}

void write_MISO(unsigned char bit) {
    gpioWrite(miso, bit);
}

void tick_sclk(void) {
    gpioWrite(sclk,HIGH);
    gpioDelay(SPI_SCLK_HIGH_TIME);
    gpioWrite(sclk,LOW);
    gpioDelay(SPI_SCLK_LOW_TIME);
}

// This is where the SPI protocol is implemented.
// This function will transfer a byte (send and receive) to the MSX Interface.
// It receives a byte as input, return a byte as output.
// It is full-duplex, sends a bit, read a bit in each of the 8 cycles in the loop.
// It is tightely linked to the register-shift implementation in the CPLD,
// If something changes there, it must have changes here so the protocol will match.

unsigned char SPI_MASTER_transfer_byte(unsigned char byte_out) {
    unsigned char byte_in = 0;
    unsigned char bit;
    unsigned rdbit;
    
    tick_sclk();
    
    for (bit = 0x80; bit; bit >>= 1) {
        
        write_MISO((byte_out & bit) ? HIGH : LOW);
        gpioWrite(sclk,HIGH);
        gpioDelay(SPI_SCLK_HIGH_TIME);
        
        rdbit = gpioRead(mosi);
        if (rdbit == HIGH)
            byte_in |= bit;
        
        gpioWrite(sclk,LOW);
        gpioDelay(SPI_SCLK_LOW_TIME);
        
    }
    
    tick_sclk();
    return byte_in;
    
}

// This is the function set in the interrupt for the CS signal.
// When CS signal is asserted (by the MSX Interface) to start a transfer,
// RDY signal is asserted LOW (Busy).
// RDY should stay LOW until the current byte is processed by the statre machine.

void func_st_cmd(int gpio, int level, uint32_t tick) {
    if (level == 0) {
        gpioWrite(rdy,LOW);
        /*if (pibyte == SENDNEXT) {
         SPI_MASTER_transfer_byte(SENDNEXT);
         gpioWrite(rdy,HIGH);
         } else*/
        msxbyte = SPI_MASTER_transfer_byte(pibyte);
        
        pthread_mutex_lock(&newComMutex); //Lock to update status
        msxbyterdy = 1;
        pthread_cond_signal(&newComCond); //Signal waiting process
        pthread_mutex_unlock(&newComMutex); //Release.
        
        //printf("Sent %x, Received %x\n",pibyte,msxbyte);
        
    }
}

int piexchangebyte(unsigned char mypibyte) {
    time_t start_t, end_t;
    double diff_t;
    time(&start_t);
    int rc = 0;
    msxbyterdy = 0;
    pibyte = mypibyte;
    gpioWrite(rdy,HIGH);
    pthread_mutex_lock(&newComMutex);
    while (msxbyterdy == 0 && rc==0) {
        pthread_cond_wait(&newComCond, &newComMutex);
        time(&end_t);
        diff_t = difftime(end_t, start_t);
        if (diff_t > BYTETRANSFTIMEOUT) rc = -1;
    }
    pthread_mutex_unlock(&newComMutex);
    if (msxbyterdy==0) return -1; else return msxbyte;
}

/* senddatablock
 ---------------
 21/03/2017
 
 Send a block of data to MSX. Read the data from a pointer passed to the function.
 Do not retry if it fails (this should be implemented somewhere else).
 Will inform the block size to MSX (two bytes) so it knows the size of transfer.
 
 Logic sequence is:
 1. read MSX status (expect SENDNEXT)
 2. send lsb for block size
 3. send msb for block size
 4. read (lsb+256*msb) bytes from buffer and send to MSX
 5. exchange crc with msx
 6. end function and return status
 
 Return code will contain the result of the oepration.
 */
transferStruct senddatablock(unsigned char *buffer, int datasize, bool sendsize) {
    
    transferStruct dataInfo;
    
    int bytecounter = 0;
    unsigned char mymsxbyte,mypibyte;
    unsigned char crc = 0;
    
    //printf("senddatablock: starting\n");
    mymsxbyte = piexchangebyte(SENDNEXT);
    
    if (mymsxbyte != SENDNEXT) {
        //printf("senddatablock:Out of sync with MSX, waiting SENDNEXT, received %x\n",mymsxbyte);
        dataInfo.rc = RC_OUTOFSYNC;
    } else {
        // send block size if requested by caller.
        if (sendsize)
            piexchangebyte(datasize % 256); piexchangebyte(datasize / 256);
        
        //printf("senddatablock:blocksize = %i\n",datasize);
        
        while(datasize>bytecounter && mymsxbyte>=0) {
            //printf("senddatablock:waiting MSX request byte\n");
            
            mypibyte = *(buffer + bytecounter);
            
            mymsxbyte = piexchangebyte(mypibyte);
            
            if (mymsxbyte>=0) {
                //printf("senddatablock:%i Sent %x %c Received:%x\n",bytecounter,mypibyte,mypibyte,mymsxbyte);
                crc ^= mypibyte;
                bytecounter++;
            } else {
                //printf("senddatablock:Error during transfer\n");
                break;
            }
        }
        
        if(mymsxbyte>=0) {
            //printf("senddatablock:Sending CRC: %x\n",crc);
            
            mymsxbyte = piexchangebyte(crc);
            
            //printf("senddatablock:Received MSX CRC: %x\n",mymsxbyte);
            if (mymsxbyte == crc) {
                //printf("mymsxbyte:CRC verified\n");
                dataInfo.rc = RC_SUCCESS;
            } else {
                dataInfo.rc = RC_CRCERROR;
                printf("senddatablock:CRC ERROR CRC: %x different than MSX CRC: %x\n",crc,dataInfo.rc);
            }
            
        } else {
            dataInfo.rc = RC_TIMEOUT;
        }
    }
    
    //printf("senddatablock:exiting with rc = %x\n",dataInfo.rc);
    return dataInfo;
}

/* recvdatablock
 ---------------
 Read a block of data from MSX and stores in the pointer passed to the function.
 Do not retry if it fails (this should be implemented somewhere else).
 Will read the block size from MSX (two bytes) to know size of transfer.
 
 Logic sequence is:
 1. read MSX status (expect SENDNEXT)
 2. read lsb for block size
 3. read msb for block size
 4. read (lsb+256*msb) bytes from MSX and store in buffer
 5. exchange crc with msx
 6. end function and return status
 
 Return code will contain the result of the oepration.
 */

transferStruct recvdatablock(unsigned char *buffer) {
    transferStruct dataInfo;
    
    int bytecounter = 0;
    unsigned char mymsxbyte;
    unsigned char crc = 0;
    
    //printf("recvdatablock:starting\n");
    mymsxbyte = piexchangebyte(SENDNEXT);
    if (mymsxbyte != SENDNEXT) {
        printf("recvdatablock:Out of sync with MSX, waiting SENDNEXT, received %x\n",mymsxbyte);
        dataInfo.rc = RC_OUTOFSYNC;
    } else {
        // read block size
        dataInfo.datasize = (unsigned char)piexchangebyte(SENDNEXT)+(256 * (unsigned char)piexchangebyte(SENDNEXT));
        //printf("recvdatablock:blocksize = %i\n",dataInfo.datasize);
        
        while(dataInfo.datasize>bytecounter && mymsxbyte>=0) {
            //printf("recvdatablock:waiting byte from MSX\n");
            
            mymsxbyte = piexchangebyte(SENDNEXT);
            
            if (mymsxbyte>=0) {
                //printf("recvdatablock:Received byte:%x\n",mymsxbyte);
                *(buffer + bytecounter) = mymsxbyte;
                crc ^= mymsxbyte;
                bytecounter++;
            } else {
                //printf("recvdatablock:Error during transfer\n");
                break;
            }
        }
        
        if(mymsxbyte>=0) {
            //printf("recvdatablock:Sending CRC: %x\n",crc);
            
            mymsxbyte = piexchangebyte(crc);
            
            //printf("recvdatablock:Received MSX CRC: %x\n",mymsxbyte);
            if (mymsxbyte == crc) {
                //printf("recvdatablock:CRC verified\n");
                dataInfo.rc = RC_SUCCESS;
            } else {
                dataInfo.rc = RC_CRCERROR;
                //printf("recvdatablock:CRC ERROR CRC: %x different than MSX CRC: %x\n",crc,dataInfo.rc);
            }
            
        } else {
            dataInfo.rc = RC_TIMEOUT;
        }
    }
    
    //printf("recvdatablock:exiting with rc = %x\n",dataInfo.rc);
    return dataInfo;
}

int secsenddata(unsigned char *buf, int filesize) {
    
    int rc;
    int blockindex,numsectors,initsector,mymsxbyte,blocksize,retries;
    transferStruct dataInfo;
    
    mymsxbyte = piexchangebyte(SENDNEXT);
    //printf("secsenddata:Sent SENDNEXT, received:%i\n",mymsxbyte);
        
    piexchangebyte(filesize % 256); piexchangebyte(filesize / 256);
    
    // now send 512 bytes at a time.
    blockindex = 0;
    if (filesize>512) blocksize = 512; else blocksize = filesize;
    while(blockindex<filesize) {
        retries=0;
        rc = RC_UNDEFINED;
        while(retries<GLOBALRETRIES && rc != RC_SUCCESS) {
            rc = RC_UNDEFINED;
            //printf("secsenddata:inner:index = %i retries:%i filesize:%i  blocksize:%i\n",blockindex,retries,filesize,blocksize);
            dataInfo = senddatablock(buf+blockindex,blocksize,true);
            rc = dataInfo.rc;
            retries++;
        }
            
        // Transfer interrupted due to CRC error
        if(retries>GLOBALRETRIES) break;
        
        blockindex += 512;
        
        if (filesize-blockindex>512) blocksize = 512; else blocksize = filesize-blockindex;
        //printf("secsenddata:outer:index = %i retries:%i filesize:%i  blocksize:%i  rc:%x\n",blockindex,retries,filesize,blocksize,rc);
        
    }
    
    //printf("secsenddata:Exiting loop with rc %x\n",rc);
    
    if(retries>GLOBALRETRIES) {
        //printf("secsenddata:Transfer interrupted due to CRC error\n");
        rc = RC_CRCERROR;
    } else {
        rc = dataInfo.rc;
        //printf("secsenddata:Exiting with rc %x\n",rc);
    }
    
    return rc;

}

int secrecvdata(unsigned char *buf) {
    
    int rc;
    int blockindex,numsectors,initsector,mymsxbyte,blocksize,retries;
    transferStruct dataInfo;
    int filesize;
    unsigned char bytem,bytel;
    
    mymsxbyte = piexchangebyte(SENDNEXT);
    printf("secrecvdata:Sent SENDNEXT, received:%x\n",mymsxbyte);
    
    // Send totalfile size to transfer
    bytel = piexchangebyte(SENDNEXT);
    bytem = piexchangebyte(SENDNEXT);
    
    filesize = bytel + (bytem * 256);
    printf("secrecvdata:bytem:%x bytel:%x filesize:%i\n",bytem,bytel,filesize);
    
    // now read 512 bytes at a time.
    blockindex = 0;
    if (filesize>512) blocksize = 512; else blocksize = filesize;
    while(blockindex<filesize) {
        retries=0;
        rc = RC_UNDEFINED;
        while(retries<GLOBALRETRIES && rc != RC_SUCCESS) {
            rc = RC_UNDEFINED;
            dataInfo = recvdatablock(buf+blockindex);
            printf("secrecvdata:inner:index = %i retries:%i filesize:%i  blocksize:%i\n",blockindex,retries,filesize,dataInfo.datasize);
            rc = dataInfo.rc;
            retries++;
        }
        
        // Transfer interrupted due to CRC error
        if(retries>GLOBALRETRIES) break;
        
        blockindex += 512;
        
        if (filesize-blockindex>512) blocksize = 512; else blocksize = filesize-blockindex;
        printf("secrecvdata:outer:index = %i retries:%i filesize:%i  blocksize:%i  rc:%x\n",blockindex,retries,filesize,blocksize,rc);
        
    }
    
    printf("secrecvdata:Exiting loop with rc %x\n",rc);
    
    if(retries>GLOBALRETRIES) {
        printf("secrecvdata:Transfer interrupted due to CRC error\n");
        rc = RC_CRCERROR;
    } else {
        rc = dataInfo.rc;
    }
    
    printf("secrecvdata:Exiting with rc %x\n",rc);
    return rc;
    
}

int sync_transf(unsigned char mypibyte) {
    time_t start_t, end_t;
    double diff_t;
    time(&start_t);
    int rc = 0;
    msxbyterdy = 0;
    pibyte = mypibyte;
    gpioWrite(rdy,HIGH);
    pthread_mutex_lock(&newComMutex);
    while (msxbyterdy == 0 && rc==0) {
        pthread_cond_wait(&newComCond, &newComMutex);
        time(&end_t);
        diff_t = difftime(end_t, start_t);
        if (diff_t > SYNCTRANSFTIMEOUT) rc = -1;
    }
    pthread_mutex_unlock(&newComMutex);
    if (msxbyterdy==0) rc = -1; else rc = msxbyte;
    return rc;
}


int sync_client() {
    int rc = -1;
    
    printf("sync_client:Syncing\n");
    while(rc<0) {
        rc = piexchangebyte(READY);
        
#ifdef V07SUPPORT
        
        if (rc == LOADCLIENT) {
            LOADCLIENT_V07PROTOCOL();
            rc = READY;
        }
        
#endif
        
    }
    
    return rc;
}

int more(unsigned char *msxcommand) {
    int rc;
    FILE *fp;
    int filesize;
    unsigned char *buf;
    unsigned char *fname;
    transferStruct dataInfo;
    
    printf("more:starting %s\n",msxcommand);
    
    filesize = 22;
    buf = (unsigned char *)malloc(sizeof(unsigned char) * filesize);
    strcpy(buf,"Pi:Error opening file");
    
    if (strlen(msxcommand)>5) {
        fname = (unsigned char *)malloc((sizeof(unsigned char) * strlen(msxcommand)) - 5);
        strcpy(fname,msxcommand+5);
        
        fp = fopen(fname,"rb");
        if(fp) {
            printf("more:file name to show is %s\n",fname);
            fseek(fp, 0L, SEEK_END);
            filesize = ftell(fp);        // file has 4 zeros at the end, we only need one
            rewind(fp);
            
            buf = (unsigned char *)malloc((sizeof(unsigned char) * filesize) + 1);
            fread(buf,filesize,1,fp);
            fclose(fp);
            
            *(buf + filesize) = 0;
        }
        
        free(fname);
            
    }
    
    printf("more:file size is %i\n",filesize);
    dataInfo = senddatablock(buf,filesize+1,true);
    free(buf);
    rc = dataInfo.rc;
    printf("more:exiting rc = %x\n",rc);
    return rc;
    
}

int runpicmd(unsigned char *msxcommand) {
    int rc;
    FILE *fp;
    int filesize;
    unsigned char *buf;
    unsigned char *fname;
    
    printf("runpicmd:starting command >%s<+\n",msxcommand);

    fname = (unsigned char *)malloc(sizeof(unsigned char) * 256);
    sprintf(fname,"%s>/tmp/msxpi_out.txt 2>&1",msxcommand);

    printf("runpicmd:prepared output in command >%s<\n",fname);
    
    if(fp = popen(fname, "r")) {
        fclose(fp);
        filesize = 24;
        buf = (unsigned char *)malloc(sizeof(unsigned char) * 256 );
        strcpy(buf,"more /tmp/msxpi_out.txt");
        printf("more:Success running command %s\n",fname);
        rc = RC_SUCCESS;
    } else {
        printf("more:Error running command %s\n",fname);
        filesize = 22;
        buf = (unsigned char *)malloc(sizeof(unsigned char) * 256 );
        strcpy(buf,"Pi:Error running command");
        rc = RC_FILENOTFOUND;
    }
    
    printf("runpicmd:call more to send output\n");
    if (rc==RC_SUCCESS) more(buf); else senddatablock(buf,strlen(buf)+1,true);
    free(buf);
    free(fname);
    
    printf("runpicmd:exiting rc = %x\n",rc);
    return rc;
    
}

int loadrom(unsigned char *msxcommand) {
    int rc;
    FILE *fp;
    int filesize,index,blocksize,retries;
    unsigned char *buf;
    unsigned char *stdout;
    unsigned char mymsxbyte;
    transferStruct dataInfo;
    char** tokens;
    
    printf("load:starting %s\n",msxcommand);
    
    tokens = str_split(msxcommand,' ');
    printf("load:parsed command is %s %s\n",*(tokens),*(tokens + 1));
    
    stdout = (unsigned char *)malloc(sizeof(unsigned char) * 65);
    
    dataInfo.rc = RC_UNDEFINED;
    
    fp = fopen(*(tokens + 1),"rb");
    if (fp) {
        fseek(fp, 0L, SEEK_END);
        filesize = ftell(fp);
        rewind(fp);
        buf = (unsigned char *)malloc(sizeof(unsigned char) * filesize);
        fread(buf,filesize,1,fp);
        fclose(fp);
        
        if ((*(buf)!='A') || (*(buf+1)!='B')) {
            printf("loadrom:Not a .rom program. Aborting\n");
            rc = RC_UNDEFINED;
            strcpy(stdout,"Pi:Not a .rom file");
            piexchangebyte(ABORT);
        } else {
            
            piexchangebyte(STARTTRANSFER);

            // send to msx the total size of file
            //printf("load:sending file size %i\n",filesize);
            piexchangebyte(filesize % 256); piexchangebyte(filesize / 256);
            
            //printf("load:calling senddatablock\n");
            
            // now send 512 bytes at a time.
            index = 0;
            
            if (filesize>512) blocksize = 512; else blocksize = filesize;
            while(blocksize) {
                retries=0;
                dataInfo.rc = RC_UNDEFINED;
                while(retries<GLOBALRETRIES && dataInfo.rc != RC_SUCCESS) {
                    dataInfo.rc = RC_UNDEFINED;
                    //printf("load:index = %i %04x blocksize = %i retries:%i rc:%x\n",index,index+0x4000,blocksize,retries,dataInfo.rc);
                    dataInfo = senddatablock(buf+index,blocksize,true);
                    retries++;
                    rc = dataInfo.rc;
                }
                
                // Transfer interrupted due to CRC error
                if(retries>GLOBALRETRIES) break;
                
                index += blocksize;
                if (filesize - index > 512) blocksize = 512; else blocksize = filesize - index;
            }
            
            if(retries>=GLOBALRETRIES) {
                //printf("load:Transfer interrupted due to CRC error\n");
                rc = RC_CRCERROR;
                strcpy(stdout,"Pi:CRC Error");
                mymsxbyte = piexchangebyte(ABORT);
            } else {
                //printf("load:done\n");
                
                strcpy(stdout,"Pi:Ok");
                mymsxbyte = piexchangebyte(ENDTRANSFER);
                printf("load:Sent ENDTRANSFER, Received %x\n",mymsxbyte);
            }
        }
        
        free(buf);
        
    } else {
        //printf("load:error opening file\n");
        rc = RC_FILENOTFOUND;
        mymsxbyte = piexchangebyte(ABORT);
        strcpy(stdout,"Pi:Error opening file");
    }
    
    printf("load:sending stdout %s\n",stdout);
    dataInfo = senddatablock(stdout,strlen(stdout)+1,true);
    
    free(tokens);
    free(stdout);
    
    printf("load:exiting rc = %x\n",rc);
    return rc;
    
}

int loadbin(unsigned char *msxcommand) {
    int rc;
    FILE *fp;
    int filesize,index,blocksize,retries;
    unsigned char *buf;
    unsigned char *stdout;
    unsigned char mymsxbyte;
    char** tokens;
    transferStruct dataInfo;

    //printf("loadbin:starting %s\n",msxcommand);
    
    tokens = str_split(msxcommand,' ');
    printf("loadbin:parsed command is %s %s\n",*(tokens),*(tokens + 1));
    
    rc = RC_UNDEFINED;
    
    fp = fopen(*(tokens + 1),"rb");
    if (fp) {
        fseek(fp, 0L, SEEK_END);
        filesize = ftell(fp);
        rewind(fp);
        buf = (unsigned char *)malloc(sizeof(unsigned char) * filesize);
        fread(buf,filesize,1,fp);
        fclose(fp);
        
        index = 0;
        
        if (*(buf)!=0xFE) {
            printf("loadbin:Not a .bin program. Aborting\n");
            rc = RC_UNDEFINED;
            stdout = (unsigned char *)malloc(sizeof(unsigned char) * 19);
            strcpy(stdout,"Pi:Not a .bin file");
            piexchangebyte(ABORT);
        } else {
            
            piexchangebyte(STARTTRANSFER);
            
            // send to msx the total size of file
            printf("load:sending file size %i\n",filesize - 7);
            piexchangebyte((filesize - 7) % 256); piexchangebyte((filesize - 7) / 256);
            
            // send file header: 0xFE
            piexchangebyte(*(buf+index));index++;
            // program start address
            piexchangebyte(*(buf+index));index++;
            piexchangebyte(*(buf+index));index++;
            
            // program end address
            piexchangebyte(*(buf+index));index++;
            piexchangebyte(*(buf+index));index++;
            
            // program exec address
            piexchangebyte(*(buf+index));index++;
            piexchangebyte(*(buf+index));index++;
            
            printf("loadbin:Start address = %02x%02x Exec address = %02x%02x\n",*(buf+2),*(buf+1),*(buf+4),*(buf+3));
            
            printf("loadbin:calling senddatablock\n");
            
            // now send 512 bytes at a time.
            
            if (filesize>512) blocksize = 512; else blocksize = filesize;
            while(blocksize) {
                retries=0;
                dataInfo.rc = RC_UNDEFINED;
                while(retries<GLOBALRETRIES && dataInfo.rc != RC_SUCCESS) {
                    dataInfo.rc = RC_UNDEFINED;
                    dataInfo = senddatablock(buf+index,blocksize,true);
                    printf("loadbin:index = %i blocksize = %i retries:%i rc:%x\n",index,blocksize,retries,dataInfo.rc);
                    retries++;
                    rc = dataInfo.rc;
                }
                
                // Transfer interrupted due to CRC error
                if(retries>GLOBALRETRIES) break;
                
                index += blocksize;
                if (filesize - index > 512) blocksize = 512; else blocksize = filesize - index;
            }
            
            printf("loadbin:(exited) index = %i blocksize = %i retries:%i rc:%x\n",index,blocksize,retries,dataInfo.rc);
            mymsxbyte = piexchangebyte(ENDTRANSFER);
            
            if(retries>=GLOBALRETRIES || rc != RC_SUCCESS) {
                printf("loadbin:Error during data transfer:%x\n",rc);
                rc = RC_CRCERROR;
                stdout = (unsigned char *)malloc(sizeof(unsigned char) * 13);
                
                strcpy(stdout,"Pi:CRC Error");
            } else {
                printf("load:done\n");
                stdout = (unsigned char *)malloc(sizeof(unsigned char) * 15);
                strcpy(stdout,"Pi:File loaded");
            }
            
        mymsxbyte = piexchangebyte(ENDTRANSFER);
            
        }
        
        free(buf);
        
    } else {
        printf("loadbin:error opening file\n");
        rc = RC_FILENOTFOUND;
        mymsxbyte = piexchangebyte(ABORT);
        stdout = (unsigned char *)malloc(sizeof(unsigned char) * 22);
        strcpy(stdout,"Pi:Error opening file");
    }
    
    //printf("load:sending stdout %s\n",stdout);
    dataInfo = senddatablock(stdout,strlen(stdout)+1,true);
    
    free(tokens);
    free(stdout);
    
    printf("loadbin:exiting rc = %x\n",rc);
    return rc;
    
}

int msxdos_secinfo(DOS_SectorStruct *sectorInfo) {
    unsigned char byte_lsb, byte_msb;
    int mymsxbyte=0;
    
    int rc = 1;
    
    //printf("msxdos_secinfo: syncing with MSX\n");
    //while(rc) {
     // mymsxbyte = piexchangebyte(SENDNEXT);
     //   if (mymsxbyte==SENDNEXT || mymsxbyte<0) rc=false;
    //}
    
  mymsxbyte = piexchangebyte(SENDNEXT);

    if (mymsxbyte == SENDNEXT) {
	//printf("msxdos_secinfo: received SENDNEXT\n");
        sectorInfo->deviceNumber = piexchangebyte(SENDNEXT);
        sectorInfo->sectors = piexchangebyte(SENDNEXT);
        sectorInfo->logicUnitNumber = piexchangebyte(SENDNEXT);
        byte_lsb = piexchangebyte(SENDNEXT);
        byte_msb = piexchangebyte(SENDNEXT);
        
        sectorInfo->initialSector = byte_lsb + 256 * byte_msb;
        
        //printf("msxdos_secinfo:deviceNumber=%x logicUnitNumber=%x #sectors=%x sectorInfo->initialSector=%i\n",sectorInfo->deviceNumber,sectorInfo->logicUnitNumber,sectorInfo->sectors,sectorInfo->initialSector);
        
        if (sectorInfo->deviceNumber == -1 || sectorInfo->sectors == -1 || sectorInfo->logicUnitNumber == -1 || byte_lsb == -1 || byte_msb == -1)
            rc = RC_FAILED;
        else
            rc = RC_SUCCESS;
    } else {
        //printf("msxdos_secinfo:sync_transf error\n");
        rc = RC_OUTOFSYNC;
    }
    
    //printf("msxdos_secinfo:exiting rc = %x\n",rc);
    return rc;
    
}


int msxdos_readsector(unsigned char *currentdrive,DOS_SectorStruct *sectorInfo) {
    
    int rc,numsectors,initsector;
    
    numsectors = sectorInfo->sectors;
    initsector = sectorInfo->initialSector;
    
    // now tansfer sectors to MSX, 512 bytes at a time and perform sync betwen blocks
    //printf("msxdos_readsector:calling secsenddata\n");
    rc = secsenddata(currentdrive+(initsector*512),numsectors*512);
 
    //printf("msxdos_readsector:exiting rc = %x\n",rc);
           
    return rc;
        
}

int msxdos_writesector(unsigned char *currentdrive,DOS_SectorStruct *sectorInfo) {
    
        int rc,sectorcount,numsectors,initsector,index;
    
        numsectors = sectorInfo->sectors;
        initsector = sectorInfo->initialSector;
            
        index = 0;
        sectorcount = numsectors;
        // Read data from MSX
        while(sectorcount) {
            rc = secrecvdata(currentdrive+index+(initsector*512));
            if (rc!=RC_SUCCESS) break;
            index += 512;
            sectorcount--;
        }
            
        if (rc==RC_SUCCESS) {
            printf("msxdos_writesector:Success transfering data sector\n");
        } else {
            printf("msxdos_writesector:Error transfering data sector\n");
        }
    
        printf("msxdos_writesector:exiting rc = %x\n",rc);
        
        return rc;
}

int pnewdisk(unsigned char * msxcommand, char *dsktemplate) {
    char** tokens;
    struct stat diskstat;
    char *buf;
    char *cpycmd;
    int rc;
    FILE *fp;
    
    printf("pnewdisk:starting %s\n",msxcommand);
    
    tokens = str_split(msxcommand,' ');
    
    buf = (unsigned char *)malloc(sizeof(unsigned char) * 40);
    
    rc = RC_FAILED;
    
    if (*(tokens + 1) != NULL) {
    
        cpycmd = (unsigned char *)malloc(sizeof(unsigned char) * 140);
        
        printf("pnewdisk:Creating new dsk file: %s\n",*(tokens + 1));
        
        
        sprintf(cpycmd,"cp %s %s",dsktemplate,*(tokens + 1));
        
        if(fp = popen(cpycmd, "r")) {
            fclose(fp);
            
            // check if file was created
            if( access( *(tokens + 1), F_OK ) != -1 ) {
                sprintf(cpycmd,"chown pi.pi %s",*(tokens + 1));
                fp = popen(cpycmd, "r");
                fclose(fp);
                strcpy(buf,"Pi:Ok");
                rc = RC_SUCCESS;
            } else
                strcpy(buf,"Pi:Error verifying disk");
        } else
            strcpy(buf,"Pi:Error creating disk");
        
        free(cpycmd);
        
    } else
        strcpy(buf,"Pi:Error\nSyntax: pnewdisk <file>");
    
    senddatablock(buf,strlen(buf)+1,true);
    
    free(tokens);
    free(buf);
    printf("pnewdisk:Exiting with rc=%x\n",rc);
    return rc;
    
}

int msxdos_format(struct DiskImgInfo *driveInfo) {
    FILE *fp;
    int rc;
    
    char *cpycmd;
    
    cpycmd = (unsigned char *)malloc(sizeof(unsigned char) * 140);

    sprintf(cpycmd,"mkfs -t msdos -F 12 %s",driveInfo->dskname);
    
    printf("msxdos_format:Formating drive: %s\n",cpycmd);
    
    // run mkfs command using popen()"
    if(fp = popen(cpycmd, "r")) {
        fclose(fp);
        piexchangebyte(ENDTRANSFER);
        rc = RC_SUCCESS;
    } else {
        piexchangebyte(ABORT);
        rc = RC_FAILED;
    }
    
    free(cpycmd);
    printf("msxdos_format:Exiting with rc=%x\n",rc);
    return rc;
}

int * msxdos_inihrd(struct DiskImgInfo *driveInfo) {

    int rc,fp;
    struct   stat diskstat;

    printf("msxdos_inihrd:Disk disk image name:%s\n",driveInfo->dskname);

    driveInfo->rc = RC_FAILED;

    if( access( driveInfo->dskname, F_OK ) != -1 ) {
	printf("msxdos_inihrd:Mounting disk image 1:%s\n",driveInfo->dskname);
        fp = open(driveInfo->dskname,O_RDWR);

        if (stat(driveInfo->dskname, &diskstat) == 0) {
            driveInfo->size = diskstat.st_size;
            if ((driveInfo->data = mmap((caddr_t)0, driveInfo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fp, 0))  == (caddr_t) -1) {
                    printf("msxdos_inihrd:Disk image failed to mount\n");
    	    } else {
        	 printf("msxdos_inihrd:Disk mapped in ram with size %i Bytes\n",driveInfo->size);
                 driveInfo->rc = RC_SUCCESS;
            }
         } else {
             printf("msxdos_inihrd:Error getting disk image size\n");
         }
    } else {
        printf("msxdos_inihrd:Disk image not found\n");
    }
    
    return driveInfo->rc;
}

/* LOADCLIENT_V07PROTOCOL
 ------------------------
 21/03/2017
 
 This function implemente the protocol for the load in ROM v0.7
 it will allow using the existing ROM in the EPROM without chantes to load the Client.
 */

int LOADCLIENT_V07PROTOCOL(void) {
    FILE *fp;
    unsigned char *buf;
    int counter = 7;
    int rc;
    
    printf("LOADCLIENT_V07PROTOCOL:Sending msxpi-client.bin using protocol v0.7\n");
    fp = fopen("/home/pi/msxpi/msxpi-client.bin","rb");
    fseek(fp, 0L, SEEK_END);
    int filesize = ftell(fp) - 7;
    rewind(fp);
    buf = (unsigned char *)malloc(sizeof(unsigned char) * filesize);
    fread(buf,filesize,1,fp);
    fclose(fp);
    
    piexchangebyte(filesize % 256);
    piexchangebyte(filesize / 256);
    piexchangebyte(*(buf+5));
    piexchangebyte(*(buf+6));
    
    printf("Filesize = %i\n",filesize);
    printf("Exec address =%02x%02x\n",*(buf+6),*(buf+5));
    while(filesize>counter) {
        //printf("cunter:%i  byte:%x\n",counter,*(buf+counter));
        rc = piexchangebyte(*(buf+counter));
        counter++;
    }
    printf("LOADCLIENT_V07PROTOCOL:terminated\n");
    return rc;
}

struct DiskImgInfo psetdisk(unsigned char * msxcommand) {
    struct DiskImgInfo diskimgdata;
    char** tokens;
    struct   stat diskstat;
    char *buf;
    
    printf("psetdisk:starting %s\n",msxcommand);
    
    tokens = str_split(msxcommand,' ');
    buf = (unsigned char *)malloc(sizeof(unsigned char) * 64);
    
    if ((*(tokens + 2) != NULL) && (*(tokens + 1) != NULL)) {
        
        diskimgdata.rc = RC_SUCCESS;
        
        if(strcmp(*(tokens + 1),"0")==0)
            diskimgdata.deviceNumber = 0;
        else if (strcmp(*(tokens + 1),"1")==0)
            diskimgdata.deviceNumber = 1;
        else
            diskimgdata.rc = RC_FAILED;
        
        if (diskimgdata.rc != RC_FAILED) {
            strcpy(diskimgdata.dskname,*(tokens + 2));
            printf("psetdisk:Disk image is:%s\n",diskimgdata.dskname);
            
            if( access( diskimgdata.dskname, F_OK ) != -1 ) {
                printf("psetdisk:Found disk image\n");
                strcpy(buf,"Pi:OK");
            } else {
                printf("psetdisk:Disk image not found.\n");
                diskimgdata.rc = RC_FAILED;
                strcpy(buf,"Pi:Error\nDisk image not found");
            }
        } else {
            printf("psetdisk:Invalid device\n");
            diskimgdata.rc = RC_FAILED;
            strcpy(buf,"Pi:Error\nInvalid device\nDevice must be 0 or 1");
        }
    } else {
        printf("psetdisk:Invalid parameters\n");
        diskimgdata.rc = RC_FAILED;
        strcpy(buf,"Pi:Error\nSyntax: psetdisk <0|1> <file>");
    }
    
    senddatablock(buf,strlen(buf)+1,true);
    free(tokens);
    free(buf);

    return diskimgdata;
}

int pset(struct psettype *psetvar, unsigned char *msxcommand) {
    int rc;
    unsigned char *buf;
    char** tokens;
    char *stdout;
    int n;
    bool found = false;
    
    printf("pset:starting %s\n",msxcommand);
    
    tokens = str_split(msxcommand,' ');
    stdout = (unsigned char *)malloc(sizeof(unsigned char) * 64);
    strcpy(stdout,"Pi:Ok");

    rc = RC_FAILED;
    
    
    if ((*(tokens + 1) == NULL)) {
        printf("pset:missing parameters\n");
        strcpy(stdout,"Pi:Error\nSyntax: pset display | <variable> <value>");
    } else if ((*(tokens + 2) == NULL)) {
        
        //DISPLAY is requested?
        if ((strncmp(*(tokens + 1),"display",1)==0) ||
            (strncmp(*(tokens + 1),"DISPLAY",1)==0)) {
            
            printf("pcd:generating output for DISPLAY\n");
            buf = (unsigned char *)malloc(sizeof(unsigned char) * (10*16 + 10*128) + 1);
            strcpy(buf,"\n");
            for(n=0;n<10;n++) {
                strcat(buf,psetvar[n].var);
                strcat(buf,"=");
                strcat(buf,psetvar[n].value);
                strcat(buf,"\n");
            }
            
            rc = RC_INFORESPONSE;
        } else {
            printf("pset:missing parameters\n");
            strcpy(stdout,"Pi:Error\nSyntax: pset <variable> <value>");
        }
    } else {
    
        printf("pset:setting %s to %s\n",*(tokens+1),*(tokens+2));
        
        for(n=0;n<10;n++) {
            printf("psetvar[%i]=%s\n",n,psetvar[n].var);
            if (strcmp(psetvar[n].var,*(tokens +1))==0) {
                strcpy(psetvar[n].value,*(tokens +2));
                found = true;
                break;
            }
        }
        
        if (!found) {
            for(n=0;n<10;n++) {
                printf("psetvar[%i]=%s\n",n,psetvar[n].var);
                if (strcmp(psetvar[n].var,"free")==0) {
                    strcpy(psetvar[n].var,*(tokens +1));
                    strcpy(psetvar[n].value,*(tokens +2));
                    break;
                }
            }
        }
        if (n==10) {
            rc = RC_FAILED;
            printf("pset:All slots are taken\n");
            strcpy(stdout,"Pi:Error\nAll slots are taken");
        }
    }
    
    if (rc == RC_INFORESPONSE) {
        printf("pset:sending Display output\n");
        senddatablock(buf,strlen(buf)+1,true);
        free(buf);
        rc = RC_SUCCESS;
    } else {
        printf("pset:sending stdout %s\n",stdout);
        senddatablock(stdout,strlen(stdout)+1,true);
    }
    
    free(stdout);
    free(tokens);
    return rc;
}

int pwifi(char * msxcommand, char *wifissid, char *wifipass) {
    int rc;
    char** tokens;
    char *stdout;
    char *buf;
    int i;
    FILE *fp;
    
    rc = RC_FAILED;
    stdout = (unsigned char *)malloc(sizeof(unsigned char) * 128);
    tokens = str_split(msxcommand,' ');
    
    if ((*(tokens + 1) == NULL)) {
        printf("pset:missing parameters\n");
        strcpy(stdout,"Pi:Error\nSyntax: pwifi display | set");
        senddatablock(stdout,strlen(stdout)+1,true);
    } else if ((strncmp(*(tokens + 1),"DISPLAY",1)==0) ||
               (strncmp(*(tokens + 1),"display",1)==0)) {
            
        rc = runpicmd("ifconfig wlan0 | grep inet >/tmp/msxpi.tmp");
            
    } else if ((strncmp(*(tokens + 1),"SET",1)==0) ||
               (strncmp(*(tokens + 1),"set",1)==0)) {
    
        buf = (unsigned char *)malloc(sizeof(unsigned char) * 256);
        fp = fopen("/etc/wpa_supplicant/wpa_supplicant.conf", "w+");
        
        strcpy(buf,"ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n");
        strcat(buf,"update_config=1\n");
        strcat(buf,"network={\n");
        strcat(buf,"\tssid=\"");
        strcat(buf,wifissid);
        strcat(buf,"\"\n");
        strcat(buf,"\tpsk=\"");
        strcat(buf,wifipass);
        strcat(buf,"\"\n");
        strcat(buf,"}\n");
        
        fprintf(fp,buf);
        fclose(fp);
        free(buf);
        
        rc = runpicmd("ifdown wlan0 && ifup wlan0");
    } else {
        printf("pset:Invalid parameters\n");
        strcpy(stdout,"Pi:Error\nSyntax: pwifi display | set");
        senddatablock(stdout,strlen(stdout)+1,true);
    }
    
    free(tokens);
    free(stdout);
        
    printf("pwifi:Exiting with rc=%x\n",rc);
    return rc;
    
}

int pcd(struct psettype *psetvar,char * msxcommand) {
    int rc;
    struct stat diskstat;
    char** tokens;
    char *stdout;
    char *buf;
    int i;
    
    printf("pcd:aprsign command:%s\n",msxcommand);
    tokens = str_split(msxcommand,' ');
    
    rc = RC_SUCCESS;
    stdout = (unsigned char *)malloc(sizeof(unsigned char) * 70);

    // Deals with absolute local filesystem PATHs
    //if cd has no parameter (want to go home)
    if (*(tokens + 1)==NULL) {
        printf("pcd:going local home\n");
        strcpy(psetvar[0].value,HOMEPATH);
        sprintf(stdout,"Pi:%s",HOMEPATH);

    //DISPLAY is requested?
    } else if ((strncmp(*(tokens + 1),"display",1)==0) ||
               (strncmp(*(tokens + 1),"DISPLAY",1)==0)) {
        
        printf("pcd:generating output for DISPLAY\n");
        buf = (unsigned char *)malloc(sizeof(unsigned char) * (10*16 + 10*128) + 1);
        strcpy(buf,"\n");
        for(i=0;i<10;i++) {
            strcat(buf,psetvar[0].var);
            strcat(buf,"=");
            strcat(buf,psetvar[0].value);
            strcat(buf,"\n");
        }
                   
        rc = RC_INFORESPONSE;
               
    //error if path is too long (> 128)
    } else if (strlen(*(tokens + 1))>128) {
        printf("pcd:path is too long\n");
        strcpy(stdout,"Pi:Error: Path is too long");
        rc = RC_FAILED;
        
    
    //if start with "/<ANYTHING>"
    } else if (strncmp(*(tokens + 1),"/",1)==0) {
        printf("pcd:going local root /\n");
        if( access( *(tokens + 1), F_OK ) != -1 ) {
            strcpy(psetvar[0].value,*(tokens + 1));
            sprintf(stdout,"Pi:OK\n%s",*(tokens + 1));
        } else {
            strcpy(stdout,"Pi:Error: Path does not exist");
            rc = RC_FAILED;
        }

    // Deals with absolute remote filesystems / URLs
    /*
    else if start with "http"
        test PATH
        if OK
            pset(PATH)
            cd PATH*/
    } else if ((strncmp(*(tokens + 1),"http:",5)==0) ||
                (strncmp(*(tokens + 1),"ftp:",4)==0) ||
                (strncmp(*(tokens + 1),"smb:",4)==0) ||
                (strncmp(*(tokens + 1),"nfs:",4)==0)) {
        
        printf("pcd:absolute remote path / URL\n");
        strcpy(psetvar[0].value,*(tokens + 1));

    // is resulting path too long?
    } else if ((strlen(psetvar[0].value)+strlen(*(tokens + 1))+2) >128) {
            printf("pcd:Resulting path is too long\n");
            strcpy(stdout,"Pi:Error: Resulting path is too long");
            rc = RC_FAILED;

    // is relative path
    // is current PATH a remote PATH / URL?
    } else if ((strncmp(psetvar[0].value,"http:",5)==0)||
               (strncmp(psetvar[0].value,"ftp:",4)==0) ||
               (strncmp(psetvar[0].value,"smb:",4)==0) ||
               (strncmp(psetvar[0].value,"nfs:",4)==0)) {

        printf("pcd:append to relative remote path / URL\n");
        strcat(psetvar[0].value,"/");
        strcat(psetvar[0].value,*(tokens + 1));
        
    /* else is local
        test PATH/<given path>
        if OK
            pset(PATH)
            cd PATH
     */
    } else {
        printf("pcd:append to relative path\n");
        char *newpath = (unsigned char *)malloc(sizeof(unsigned char) * (strlen(psetvar[0].value)+strlen(*(tokens + 1)+2)));
        strcpy(newpath,psetvar[0].value);
        strcat(newpath,"/");
        strcat(newpath,*(tokens + 1));
            
        if( access( newpath, F_OK ) != -1 ) {
            strcpy(psetvar[0].value,newpath);
        } else {
            strcpy(stdout,"Pi:Error: Path does not exist");
            rc = RC_FAILED;
        }
          
        free(newpath);
    }
    
    
    if (rc == RC_INFORESPONSE) {
        printf("pcd:sending Display output\n");
        senddatablock(buf,strlen(buf)+1,true);
        free(buf);
    } else {
        if (rc == RC_SUCCESS ) {
            sprintf(stdout,"Pi:OK\n%s",psetvar[0].value);
        }
        printf("pcd:sending stdout\n");
        senddatablock(stdout,strlen(stdout)+1,true);
    }
        
    free(tokens);
    free(stdout);
    printf("pcd:Exiting with rc=%x\n",rc);
    return rc;
    
}


static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    
    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if(mem->memory == NULL) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }
    
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

int httpdownload(unsigned char *theurl,MemoryStruct *chunk) {
    
    int rc = RC_FAILED;
    CURL *curl_handle;
    CURLcode res;
    long http_code = 0;
    
    printf("httpdownload:Starting\n");
    curl_global_init(CURL_GLOBAL_ALL);
    
    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, theurl);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    
    res = curl_easy_perform(curl_handle);
    curl_easy_getinfo (curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
    printf("httpdownload:httpdownload error code:%lu\n",http_code);

    if(http_code==200)
        rc = RC_SUCCESS;
    else
        rc = http_code;
    
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    
    printf("httpdownload:Exiting with rc=%i\n",rc);

    return rc;
}

/*
int pget2(unsigned char *msxcommand) {
    int rc;
    FILE *fp;
    unsigned char *buf;
    unsigned char *url;
    char **tokens;
    
    printf("pget:starting command %s\n",msxcommand);
    
    tokens = str_split(msxcommand,' ');
    buf = (unsigned char *)malloc(sizeof(unsigned char) * 130 );
    
    if (*(tokens + 1)==NULL) {
        printf("pget:Error missing parameters\n");
        strcpy(buf,"Pi:Error\nSyntax: pget </r> </s> source target\n/r run the program (default)\n/s save to disk\nNote: options not implemented yet");
        rc = RC_FILENOTFOUND;
    } else {
        url = (unsigned char *)malloc(sizeof(unsigned char) * 256);
        sprintf(url,"wget -o /tmp/pget.log -O /tmp/pget.file -T 60 %s",*(tokens + 1));
        
        printf("pget:prepared output in command >%s\n",url);

        if(fp = popen(url, "r")) {
            fclose(fp);
            strcpy(buf,"Pi:File downloaded");
            printf("pget:File downloaded\n");
            rc = RC_SUCCESS;
        } else {
            printf("pget:Error running command\n");
            strcpy(buf,"Pi:Error running command");
            rc = RC_FILENOTFOUND;
        }
    }
    printf("pget:sending response\n");
    piexchangebyte(rc);
    senddatablock(buf,strlen(buf)+1,true);
    free(buf);
    free(url);
    free(tokens);
    
    printf("pget:exiting rc = %x\n",rc);
    return rc;
    
}
 */

int uploaddata(unsigned char *data, size_t totalsize, int index, int retries) {
    int rc,crc,bytecounter,blocksize;
    unsigned char mypibyte,mymsxbyte;

    printf("uploaddata: %s\n",data);
    
    if (piexchangebyte(STARTTRANSFER) != STARTTRANSFER)
        return RC_OUTOFSYNC;

    // read blocksize, MAXIMUM 65535 KB
    blocksize = piexchangebyte(SENDNEXT) + 256 * piexchangebyte(SENDNEXT);
    
    printf("uploaddata:Received block size %i\n",blocksize);
     printf("uploaddata:totalsize is %i\n",totalsize);
    
    //Now verify if has finished transfering data
    if (index * blocksize >= totalsize) {
        piexchangebyte(ENDTRANSFER);
        return ENDTRANSFER;
    }
    
    piexchangebyte(SENDNEXT);
    
    // send back to msx the block size, or the actual file size if blocksize > totalsize
    if (totalsize < blocksize)
        blocksize = totalsize;

    piexchangebyte(blocksize % 256); piexchangebyte(blocksize / 256);
    
    printf("uploaddata:sending final block size %i\n",blocksize);
        
    crc = 0;
    bytecounter = 0;
        
    while(bytecounter<blocksize) {
        mypibyte = *(data + index + bytecounter);
        piexchangebyte(mypibyte);
        crc ^= mypibyte;
        bytecounter++;
    }

    
    // exchange crc
    mymsxbyte=piexchangebyte(crc);
    if (mymsxbyte == crc)
        rc = RC_SUCCESS;
    else
        rc = RC_CRCERROR;

    printf("uploaddata:local crc: %x / remote crc:%x\n",crc,mymsxbyte);
    
    printf("uploaddata:exiting rc = %x\n",rc);
    
    return rc;
        
}

int pget(unsigned char * msxcommand) {
    int rc;
    char** tokens;
    char *stdout;
    unsigned char *buf;
    unsigned char mymsxbyte;
    int *bufsize;
    unsigned char *theurl;
    int i,n;
    FILE *fp;
    int runoption,saveoption,source,target,fidpos,index;
    int filesize,blocksize,retries;
    transferStruct dataInfo;

    MemoryStruct chunk;
    MemoryStruct *chunkptr = &chunk;
    
    stdout = malloc(sizeof(char) * 256 );
    
    tokens = str_split(msxcommand,' ');
    
    fidpos = 1; // token position for source file name
    runoption=0;
    saveoption=0;
    
    /*
    for (n=1;n<=2;n++) {
        if ((*(tokens + n) == NULL)) break;
        if ((strncmp(*(tokens + n),"/R",2)==0) ||
            (strncmp(*(tokens + n),"/r",2)==0)) {
            runoption=1;
            fidpos++;
        } else if ((strncmp(*(tokens + n),"/S",2)==0) ||
                   (strncmp(*(tokens + n),"/s",2)==0)) {
            saveoption=1;
            fidpos++;
        }
    }
    
    // send filename to save:
    // pget /r /s http://xyz.com/file.com myfile.com
    //            ^fidpos                 ^fidpos + 1
    //
    
    // send command line parameters back to MSX
    //piexchangebyte(runoption);
    //piexchangebyte(saveoption);
    
    */
     
    saveoption = 1;
    // verify if all required parameters are present
    printf("fidpos %i\n",fidpos);
    if ((*(tokens + fidpos) == NULL) ||
        (saveoption && (*(tokens + fidpos + 1)) == NULL)) {
        printf("pget:missing parameters\n");
        piexchangebyte(RC_FAILED);
        //strcpy(stdout,"Pi:Error\nSyntax: pget </r> </s> source target\n/r run the program (default)\n/s save to disk\n");
        strcpy(stdout,"Pi:Error\nSyntax: pget <source url> <target file>\n");
        printf("%i\n",strlen(stdout));
        senddatablock(stdout,strlen(stdout)+1,true);
        free(stdout);
        free(tokens);
        return RC_FAILED;
    }
    
    piexchangebyte(SENDNEXT);

    // send file name to save
    printf("pget:Sending filename: %s\n",*(tokens + fidpos + 1));
    //if (saveoption)
    senddatablock(*(tokens + fidpos + 1),strlen(*(tokens + fidpos + 1))+1,true);
    
    theurl = (unsigned char *)malloc(sizeof(unsigned char) * strlen(msxcommand)+1);
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    printf("pget:calling getfile\n");
    strcpy(theurl,*(tokens + fidpos));
        
    rc = httpdownload(*(tokens + fidpos),chunkptr);

    printf("Buffer size:%i\n",chunk.size);
    printf("Buffer data:%s\n",chunk.memory);
        
    printf("pget:returned from getfile ");
    if (rc != RC_SUCCESS) {
        printf("with FAILURE\n");
        piexchangebyte(RC_FAILED);
        sprintf(stdout,"Pi:Error with httpcode: %i",rc);
        senddatablock(stdout,strlen(stdout)+1,true);
        free(chunk.memory);
        free(tokens);
        free(stdout);
        return rc;
    }
    
    printf("with success\n");
    piexchangebyte(SENDNEXT);
    
    
    index = 0;
    rc=RC_SUCCESS;
    while (rc==RC_SUCCESS) {
        printf("pget:read block %i\n",index);
        rc = uploaddata(chunk.memory, chunk.size, index, retries);
        index++;
    }

    printf("pget:Exited uploaddata with rc %x\n",rc);
    if(rc==ENDTRANSFER) {
        strcpy(stdout,"Pi:Ok");
        senddatablock(stdout,strlen(stdout)+1,true);
        rc = RC_SUCCESS;
    } else {
        strcpy(stdout,"Pi:Error transfering file");
        senddatablock(stdout,strlen(stdout)+1,true);
        rc = RC_FAILED;
    }
    
    free(chunk.memory);
    free(tokens);
    free(stdout);
    
    printf("pget:Exiting with rc=%x\n",rc);
    return rc;
    
}

int pdir(unsigned char * msxcommand) {
    memcpy(msxcommand,"ls  ",4);
    return runpicmd(msxcommand);
}

int main(int argc, char *argv[]){
    
    int startaddress,endaddress,execaddress;

    struct psettype psetvar[10];
    strcpy(psetvar[0].var,"PATH");strcpy(psetvar[0].value,"/home/pi/msxpi");
    strcpy(psetvar[1].var,"DRIVE0");strcpy(psetvar[1].value,"disks/msxpiboot.dsk");
    strcpy(psetvar[2].var,"DRIVE1");strcpy(psetvar[2].value,"disks/msxpitools.dsk");
    strcpy(psetvar[3].var,"WIFISSID");strcpy(psetvar[3].value,"my wifi");
    strcpy(psetvar[4].var,"WIFIPWD");strcpy(psetvar[4].value,"secret");
    strcpy(psetvar[5].var,"DSKTMPL");strcpy(psetvar[5].value,"msxpi_720KB_template.dsk");
    strcpy(psetvar[6].var,"free");strcpy(psetvar[6].value,"");
    strcpy(psetvar[7].var,"free");strcpy(psetvar[7].value,"");
    strcpy(psetvar[8].var,"free");strcpy(psetvar[8].value,"");
    strcpy(psetvar[9].var,"free");strcpy(psetvar[9].value,"");
    
    // numdrives is hardocde here to assure MSX will always have only 2 drives allocated
    // more than 2 drives causes some MSX to hang
    unsigned char numdrives = 2;
    unsigned char msxcommand[255];

    
    unsigned char appstate = st_init;
    
    unsigned char mymsxbyte;
    unsigned char mymsxbyte2;

    int rc;
    
    //time_t start_t, end_t;
    
    transferStruct dataInfo;
    struct DiskImgInfo drive0,drive1,currentdrive;
    DOS_SectorStruct sectorInfo;
    
    char buf[255];
    
    
    if (gpioInitialise() < 0)
    {
        fprintf(stderr, "pigpio initialisation failed\n");
        return 1;
    }
    
    init_spi_bitbang();
    gpioWrite(rdy,LOW);
    
    printf("GPIO Initialized\n");
    printf("Starting MSXPi Server v%s\n",version);

    //drive0.dskname = (unsigned char *)malloc(sizeof(unsigned char) * 256);
    strcpy(drive0.dskname,psetvar[1].value);
    drive0.deviceNumber = 0;
    msxdos_inihrd(&drive0);

    //drive1.dskname = (unsigned char *)malloc(sizeof(unsigned char) * 256);
    strcpy(drive1.dskname,psetvar[2].value);
    drive1.deviceNumber = 1;
    msxdos_inihrd(&drive1);

    //time(&start_t);
    
    gpioSetISRFunc(cs, FALLING_EDGE, SPI_INT_TIME, func_st_cmd);
    while(appstate != st_shutdown){
        
        switch (appstate) {
            case st_init:
                printf("Entered init state. Syncying with MSX...\n");
                appstate = st_cmd;
                /*if(sync_client()==READY) {
                    printf("ok, synced. Listening for commands now.\n");
                    appstate = st_cmd;
                } else {
                    printf("OPS...not synced. Will continue trying.\n");
                    appstate = st_init;
                }*/
                break;
                
            case st_cmd:
                
                printf("st_recvcmd: waiting command\n");
                dataInfo = recvdatablock(msxcommand);
                
                if(dataInfo.rc==RC_SUCCESS) {
                    //printf("st_recvcmd: received command: ");
                    *(msxcommand + dataInfo.datasize) = '\0';
                    printf("%s\n",msxcommand);
                    appstate = st_runcmd;
                } else {
                    printf("st_recvcmd: error receiving data\n");
                    appstate = st_cmd;
                }
                break;
                
            case st_runcmd:
                printf("st_run_cmd: running command ");
                
                if(strcmp(msxcommand,"SCT")==0) {
                    printf("DOS_SECINFO\n");

                    if(msxdos_secinfo(&sectorInfo)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strcmp(msxcommand,"RDS")==0) || (strcmp(msxcommand,"WRS")==0)) {
                    
                    if (sectorInfo.deviceNumber==0)
                        if(strcmp(msxcommand,"RDS")==0) {
                            printf("READ SECTOR\n");
                            // This function could be implemented in this single line,
                            // but I am usign a function instead for learning purposes.
                            //rc = secsenddata(currentdrive+(sectorInfo.initialSector*512),sectorInfo.sectors*512);
                            rc = msxdos_readsector(drive0.data,&sectorInfo);
                        } else {
                            printf("WRITE SECTOR\n");
                            rc = msxdos_writesector(drive0.data,&sectorInfo);
                        }
                    else if (sectorInfo.deviceNumber==1)
                        if(strcmp(msxcommand,"RDS")==0) {
                            printf("READ SECTOR\n");
                            // This function could be implemented in this single line,
                            // but I am usign a function instead for learning purposes.
                            //rc = secsenddata(currentdrive+(sectorInfo.initialSector*512),sectorInfo.sectors*512);
                            rc = msxdos_readsector(drive1.data,&sectorInfo);
                        } else {
                            printf("WRITE SECTOR\n");
                            rc = msxdos_writesector(drive1.data,&sectorInfo);
                        }
                    else {
                          printf("Error. Invalid device number.\n");
                          piexchangebyte(ABORT);
                          break;
                    }
                    
                    if (rc!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                        
                    appstate = st_cmd;
                    break;

               } else if(strcmp(msxcommand,"INIHRD")==0) {
                    printf("DOS_INIHRD\n");
                    appstate = st_cmd;
                    break;
                
               } else if(strcmp(msxcommand,"DRIVES")==0) {
                    printf("DOS_DRIVES\n");

                   printf("Returning number of drives:%i\n",numdrives);
                   piexchangebyte(numdrives);
                    
                    appstate = st_cmd;
                    break;
   
                } else if((strncmp(msxcommand,"more",4)==0) ||
                          (strncmp(msxcommand,"MORE",4)==0)) {
                    
                    printf("MORE\n");
                    
                    if (more(msxcommand)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"ploadrom",8)==0) ||
                          (strncmp(msxcommand,"PLOADROM",8)==0)) {
                    
                    printf("PLOADROM\n");
                    rc = loadrom(msxcommand);
                    
                    appstate = st_cmd;
                    
                    if (rc!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!");
                    
                    break;

                } else if((strncmp(msxcommand,"ploadbin",8)==0) ||
                          (strncmp(msxcommand,"loadbin",7)==0) ||
                          (strncmp(msxcommand,"PLOADBIN",8)==0)) {
                    
                    printf("PLOADBIN\n");
                    rc = loadbin(msxcommand);
                    
                    appstate = st_cmd;
                    
                    if (rc!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!");
                    
                    break;
                    
                } else if(strcmp(msxcommand,"FMT")==0) {
                    printf("FMT\n");
                    
                    // Read Choice, but not used by the driver
                    mymsxbyte = piexchangebyte(SENDNEXT);
                    printf("st_run_cmd:Choice is %x\n",mymsxbyte);
                   
                    // Read drive number
                    mymsxbyte2 = piexchangebyte(SENDNEXT);
                    printf("st_run_cmd:drive number is %x\n",mymsxbyte2);

                    if (mymsxbyte2 == 0) {
                        rc = msxdos_format(&drive0);
                    } else {
                        rc = msxdos_format(&drive1);
                    }
                    
                    if (rc!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;

                } else if((strncmp(msxcommand,"SYN",3)==0) ||
                          (strncmp(msxcommand,"chkpiconn",9)==0) ||
                          (strncmp(msxcommand,"CHKPICONN",9)==0)) {
                        
                        printf("chkpiconn\n");
                        //strcpy(buf,"MSXPi Server is running");
                        
                        //dataInfo = senddatablock(buf,strlen(buf)+1,true);
                        piexchangebyte(READY);
                    
                        //if(dataInfo.rc != RC_SUCCESS)
                        //    printf("!!!!! Error !!!!!\n");
                        
                        appstate = st_cmd;
                        break;
                    
                } else if((strncmp(msxcommand,"#",1)==0) ||
                          (strncmp(msxcommand,"RUN",3)==0)) {
                    printf("RUNPICMD\n");
                    
                    memcpy(msxcommand,"    ",4);
                    if (runpicmd(msxcommand)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"PDIR",4)==0) ||
                          (strncmp(msxcommand,"pdir",4)==0)) {
                    
                    printf("PDIR\n");
                    
                    if (pdir(msxcommand)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"PGET",4)==0) ||
                          (strncmp(msxcommand,"pget",4)==0)) {
                    
                    printf("PGET\n");
                    
                    if (pget(msxcommand)!=RC_SUCCESS)
                    printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"PSETDISK",8)==0) ||
                          (strncmp(msxcommand,"psetdisk",8)==0)) {
                    
                    printf("PSETDISK\n");
                    
                    currentdrive = psetdisk(msxcommand);
                    if (currentdrive.rc!=RC_FAILED) {
                        if (currentdrive.deviceNumber == 0) {
                            munmap(drive0.data,drive0.size);
                            strcpy(&drive0.dskname,&currentdrive.dskname);
                            printf("calling INIHRD with %s\n",drive0.dskname);
                            msxdos_inihrd(&drive0);
                            strcpy(psetvar[1].value,drive0.dskname);
                        } else {
                            munmap(drive1.data,drive1.size);
                            strcpy(&drive1.dskname,&currentdrive.dskname);
                            printf("calling INIHRD with %s\n",drive1.dskname);
                            msxdos_inihrd(&drive1);
                            strcpy(psetvar[2].value,drive1.dskname);
                        }
                    }
                    
                    if (currentdrive.rc==RC_FAILED)
                        printf("!!!!! Error !!!!!\n");
                 
                    appstate = st_cmd;
                    break;

                } else if((strncmp(msxcommand,"PSET",3)==0)  ||
                          (strncmp(msxcommand,"pset",3)==0)) {
                    
                    printf("PSET\n");
                    
                    if (pset(&psetvar,msxcommand)!=RC_SUCCESS)
                    printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"PCD",3)==0)  ||
                          (strncmp(msxcommand,"pcd",3)==0)) {
                    
                    printf("PCD\n");
                    
                    if (pcd(&psetvar,msxcommand)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"PNEWDISK",8)==0)  ||
                          (strncmp(msxcommand,"pnewdisk",8)==0)) {
                    
                    printf("PNEWDISK\n");
                    
                    if (pnewdisk(msxcommand,psetvar[5].value)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else if((strncmp(msxcommand,"PWIFI",5)==0)  ||
                          (strncmp(msxcommand,"pwifi",6)==0)) {
                    
                    printf("PWIFI\n");
                    
                    if (pwifi(msxcommand,psetvar[3].value,psetvar[4].value)!=RC_SUCCESS)
                        printf("!!!!! Error !!!!!\n");
                    
                    appstate = st_cmd;
                    break;
                    
                } else {
                    printf("st_run_cmd:Command %s - Not Implemented!\n",msxcommand);
                    sprintf(buf,"Pi:Command %s not implemented on server\n",msxcommand);
                    senddatablock(buf,strlen(buf)+1,true);
                
                    appstate = st_cmd;
                    break;
                }
    
        }
    }
    
    //create_disk
    /* Stop DMA, release resources */
    printf("Terminating GPIO\n");
    // fprintf(flog,"Terminating GPIO\n");
    gpioWrite(rdy,LOW);
    
    //system("/sbin/shutdown now &");
    //system("/usr/sbin/killall msxpi-server &");

                   
    return 0;
}
