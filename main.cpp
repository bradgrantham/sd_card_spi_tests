#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <ctype.h>
#include <buspirate.h>
#include <spi.h>

/* destructive test */

/*
    unsigned char data[2]= { 0x55, 0xAA };
    if (bp_bin_spi_cs(bp, 1) < 0)
      return -1;
    if (bp_bin_spi_bulk(bp, data, sizeof(data)) != BP_SUCCESS)
      return -1;
    if (bp_bin_spi_cs(bp, 0) < 0)
      return -1;
*/

unsigned char crc7_add_byte(unsigned char b, unsigned char previous_crc)
{
    unsigned int crc = previous_crc;

    for (int b = 0; b < 8; b++) {
        unsigned int bit = crc & 0x40;

        if ((b & 0x80UL) != 0) {
            bit ^= 0x40;
        }

        b <<= 1;
        crc <<= 1;

        if (bit != 0) {
            crc ^= 0x09;
        }
    }
    return crc;
}

unsigned char crc7_generate_bytes(unsigned char *b, int count)
{
    unsigned char crc = 0;

    for(int i = 0; i < count; i++)
        crc = crc7_add_byte(b[0], crc);

    return crc;
}

// cribbed somewhat from http://elm-chan.org/docs/mmc/mmc_e.html
enum sdcard_command {
    CMD0 = 0,    // init; go to idle state
    CMD8 = 8,    // send interface condition
    CMD17 = 17,  // read single block
    CMD24 = 24,  // write single block
    CMD55 = 55,  // prefix command for application command
    ACMD41 = 41, // application command to send operating condition
};
const int sdcard_response_IDLE = 0x01;
const int sdcard_response_READY = 0x00;

// response length must include initial R1, so 1 for CMD0
bool bp_spi_sdcard_command(BP* bp, sdcard_command command, unsigned int parameter, unsigned char *response, int response_length)
{
    unsigned char command_buffer[6];
    command_buffer[0] = 0x40 | command;

    if(bp_bin_spi_cs(bp, 1) < 0) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to set CS\n");
        return false;
    }

    command_buffer[1] = (parameter >> 24) & 0xff;
    command_buffer[2] = (parameter >> 16) & 0xff;
    command_buffer[3] = (parameter >> 8) & 0xff;
    command_buffer[4] = (parameter >> 0) & 0xff;
    command_buffer[5] = crc7_generate_bytes(command_buffer, 5);

    if (bp_bin_spi_bulk(bp, command_buffer, sizeof(command_buffer)) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to send command\n");
        bp_bin_spi_cs(bp, 0);
        return false;
    }

    memset(response, 0xff, response_length);
    time_t time_was = time(NULL);
    do {
        time_t time_now = time(NULL);
        if(time_now - time_was > 1) {
            fprintf(stderr, "bp_spi_sdcard_command: timed out waiting on response\n");
            bp_bin_spi_cs(bp, 0);
            return false;
        }
        if (bp_bin_spi_bulk(bp, response, 1) != BP_SUCCESS) {
            fprintf(stderr, "bp_spi_sdcard_command: failed to get to CS\n");
            bp_bin_spi_cs(bp, 0);
            return false;
        }
    } while(response[0] == 0xff);
    if (bp_bin_spi_bulk(bp, response + 1, response_length - 1) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to set CS\n");
        bp_bin_spi_cs(bp, 0);
        return false;
    }

    if(bp_bin_spi_cs(bp, 0) < 0) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to clear CS\n");
        return false;
    }

    return true;
}

bool bp_spi_sdcard_init(BP* bp)
{
    /* CS false, 80 clk pulses (read 10 bytes) */
    unsigned char buffer[10];
    memset(buffer, 0xff, sizeof(buffer));
    if(bp_bin_spi_cs(bp, 0) < 0) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to clear CS\n");
        return false;
    }
    if (bp_bin_spi_bulk(bp, buffer, sizeof(buffer)) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to read bulk bytes to initialize SD to SPI\n");
        return false;
    }

    unsigned char response[8];

    /* interface init */
    if(!bp_spi_sdcard_command(bp, CMD0, 0, response, 1))
        return false;
    if(response[0] != sdcard_response_IDLE) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to enter IDLE mode, response was 0x%02X\n", response[0]);
        return false;
    }

    /* interface condition */
    if(!bp_spi_sdcard_command(bp, CMD8, 0x000001AA, response, 5))
        return false;
    if(response[0] != sdcard_response_IDLE) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to enter IDLE mode, response was 0x%02X\n", response[0]);
        return false;
    }
    unsigned int OCR = (response[1] << 24) | (response[2] << 16) | (response[3] << 8) | (response[4] << 0);
    fprintf(stderr, "bp_spi_sdcard_init: OCR response is 0x%08X\n", OCR);

    // should get CSD, CID, print information about them

    time_t time_was = time(NULL);
    do {
        time_t time_now = time(NULL);
        if(time_now - time_was > 1) {
            fprintf(stderr, "bp_spi_sdcard_init: timed out waiting on transition to ACMD41\n");
            return false;
        }
        /* get operating condition */
        if(!bp_spi_sdcard_command(bp, CMD55, 0x00000000, response, 1))
            return false;
        if(response[0] != sdcard_response_IDLE) {
            fprintf(stderr, "bp_spi_sdcard_init: not in IDLE mode for CMD55, response was 0x%02X\n", response[0]);
            return false;
        }
        if(!bp_spi_sdcard_command(bp, ACMD41, 0x40000000, response, 1))
            return false;
    } while(response[0] != sdcard_response_READY);

    return true;
}

bool bp_spi_sdcard_readblock(BP *bp, unsigned int blocknum, unsigned char block[512])
{
    /* CMD17, address is MSB first */
    /* R1 response */
    return true;
}

bool bp_spi_sdcard_writeblock(BP *bp, unsigned int blocknum, unsigned char block[512])
{
    /* CMD24, address is MSB first */
    /* R1 response */
    return true;
}

void dump_buffer_hex(int indent, unsigned char *data, int size)
{
    int address = 0;

    while(size > 0) {
        int howmany = std::min(size, 16);

        printf("%*s0x%04X: ", indent, "", address);
        for(int i = 0; i < howmany; i++)
            printf("%02X ", data[i]);
        printf("\n");

        printf("%*s        ", indent, "");
        for(int i = 0; i < howmany; i++)
            printf(" %c ", isprint(data[i]) ? data[i] : '.');
        printf("\n");

        size -= howmany;
        data += howmany;
        address += howmany;
    }
}

void show_spi_config(unsigned char config)
{
  assert(config <= 15);
  printf("  Output level   : %s\n",
	 (config & BP_BIN_SPI_LV_3V3?"3V3":"HiZ"));
  printf("  Clock idle     : %s\n",
	 (config & BP_BIN_SPI_CLK_IDLE_HIGH?"high":"low"));
  printf("  Clock edge     : %s\n",
	 (config & BP_BIN_SPI_CLK_EDGE_HIGH?"idle->active":"active->idle"));
  printf("  Sample position: %s\n",
	 (config & BP_BIN_SPI_SMP_END?"end":"middle"));
}

int main(int argc, char **argv)
{
    if(argc < 2) {
        fprintf(stderr, "usage: %s devicename\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    BP *bp = bp_open(argv[1]);
    if (bp == NULL) {
        fprintf(stderr, "Could not open bus pirate\n");
        exit(EXIT_FAILURE);
    }
    printf("Bus Pirate opened\n");

    unsigned char version;
    if (bp_bin_mode_spi(bp, &version) != BP_SUCCESS)
        return -1;
    printf("Binary I/O SPI version: %u\n", version);

    unsigned char speed= BP_BIN_SPI_SPEED_30K;
    /* Note: SPI speed >= 2.6MHz do not work due to issues
         within the BP firmware (checked with version 5.10) */
    printf("Select SPI speed : %s\n", BP_BIN_SPI_SPEEDS[speed]);
    if (bp_bin_spi_set_speed(bp, speed) != BP_SUCCESS) {
        fprintf(stderr, "failed to set SPI speed\n");
        bp_bin_reset(bp, &version);
        bp_close(bp);
        exit(EXIT_FAILURE);
    }

    unsigned char config=
        BP_BIN_SPI_LV_HIZ |
        BP_BIN_SPI_CLK_IDLE_LOW |
        BP_BIN_SPI_CLK_EDGE_HIGH |
        BP_BIN_SPI_SMP_MIDDLE;

    printf("Binary I/O SPI config (%u):\n", config);
    show_spi_config(config);

    printf("Setting SPI config\n");
    if (bp_bin_spi_set_config(bp, config) != BP_SUCCESS) {
        fprintf(stderr, "failed to set SPI config\n");
        bp_bin_reset(bp, &version);
        bp_close(bp);
        exit(EXIT_FAILURE);
    }

    printf("Configuring power and pull-ups.\n");
    if (bp_bin_spi_set_periph(bp, BP_BIN_SPI_PERIPH_POWER |
			        BP_BIN_SPI_PERIPH_PULLUPS) < 0) {
        fprintf(stderr, "failed to set SPI power and pullups\n");
        bp_bin_reset(bp, &version);
        bp_close(bp);
        exit(EXIT_FAILURE);
    }

    bp_spi_sdcard_init(bp);

    exit(EXIT_SUCCESS);

    unsigned char originalblock[512];
    unsigned char junkblock[512];
    unsigned char blockread[512];

    bp_spi_sdcard_readblock(bp, 0, originalblock);
    printf("original block:\n");
    dump_buffer_hex(4, originalblock, 512);

    for(int i = 0; i < 512; i++)
        junkblock[i] = i % 256;

    bp_spi_sdcard_writeblock(bp, 0, junkblock);

    bp_spi_sdcard_readblock(bp, 0, blockread);
    if(memcmp(blockread, junkblock, sizeof(junkblock)) != 0) {
        printf("whoops, error verifying write of junk to block 0\n");
        printf("original junk:\n");
        dump_buffer_hex(4, junkblock, 512);
        printf("junk I got back:\n");
        dump_buffer_hex(4, blockread, 512);
    }

    bp_spi_sdcard_writeblock(bp, 0, originalblock);

    bp_spi_sdcard_readblock(bp, 0, blockread);
    if(memcmp(blockread, originalblock, sizeof(originalblock)) != 0) {
        printf("whoops, error verifying write of original to block 0\n");
        printf("block I got back:\n");
        dump_buffer_hex(4, blockread, 512);
    }

    bp_bin_reset(bp, &version);
    bp_close(bp);
}
