#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <ctype.h>
#include <buspirate.h>
#include <spi.h>

/* destructive test */

unsigned char crc7_add_byte(unsigned char data, unsigned char previous_crc)
{
    unsigned int crc = previous_crc;

    for (int b = 0; b < 8; b++) {
        unsigned int bit = crc & 0x40;

        if ((data & 0x80UL) != 0)
        {
            bit ^= 0x40;
        }

        data <<= 1;
        crc <<= 1;

        if (bit != 0)
        {
            crc ^= 0x09;
        }
    }

    return crc;
}

unsigned char crc7_generate_bytes(unsigned char *b, int count)
{
    unsigned char crc = 0;

    for(int i = 0; i < count; i++)
        crc = crc7_add_byte(b[i], crc);

    return crc;
}

bool debug = false;

// cribbed somewhat from http://elm-chan.org/docs/mmc/mmc_e.html
enum sdcard_command {
    CMD0 = 0,    // init; go to idle state
    CMD8 = 8,    // send interface condition
    CMD17 = 17,  // read single block
    CMD24 = 24,  // write single block
    CMD55 = 55,  // prefix command for application command
    ACMD41 = 41, // application command to send operating condition
};
const unsigned int sdcard_response_IDLE = 0x01;
const unsigned int sdcard_response_SUCCESS = 0x00;
const unsigned int sdcard_token_17_18_24 = 0xFE;

const int CS_ENABLE = 0;
const int CS_DISABLE = 1;

//
// It appears there's a bug somewhere in the _bulk chain that fails with
// writes >= 7 bytes.  So just send bulk transfers in parts.  I was going to
// have to do that anyway for read and write block, so no biggie.
//
int brad_bp_bin_spi_bulk(BP* bp, unsigned char *buffer, unsigned int nlen)
{
    while(nlen > 0) {
        unsigned char count = std::min(nlen, 6U);
        if(debug) {
            printf("send %d now:", count);
            for(int i = 0; i < count; i++) {
                printf(" %02X", buffer[i]);
            }
            printf("\n");
        }
        if (bp_bin_spi_bulk(bp, buffer, count) != BP_SUCCESS) {
            fprintf(stderr, "brad_bp_bin_spi_bulk: failed to bp_bin_spi_bulk\n");
            return BP_FAILURE;
        }
        buffer += count;
        nlen -= count;
    }
    return BP_SUCCESS;
}

// response length must include initial R1, so 1 for CMD0
bool bp_spi_sdcard_command(BP* bp, sdcard_command command, unsigned int parameter, unsigned char *response, int response_length)
{
    unsigned char command_buffer[6];
    command_buffer[0] = 0x40 | command;

    if(bp_bin_spi_cs(bp, CS_ENABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to set CS\n");
        return false;
    }

    command_buffer[1] = (parameter >> 24) & 0xff;
    command_buffer[2] = (parameter >> 16) & 0xff;
    command_buffer[3] = (parameter >> 8) & 0xff;
    command_buffer[4] = (parameter >> 0) & 0xff;
    command_buffer[5] = ((crc7_generate_bytes(command_buffer, 5) & 0x7f) << 1) | 0x01;

    if(debug) printf("command constructed: %02X %02X %02X %02X %02X %02X\n",
        command_buffer[0], command_buffer[1], command_buffer[2],
        command_buffer[3], command_buffer[4], command_buffer[5]);

    if (brad_bp_bin_spi_bulk(bp, command_buffer, sizeof(command_buffer)) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to send command\n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }
    if(debug) printf("returned in buffer: %02X %02X %02X %02X %02X %02X\n",
        command_buffer[0], command_buffer[1], command_buffer[2],
        command_buffer[3], command_buffer[4], command_buffer[5]);

    memset(response, 0xff, response_length);
    time_t time_was = time(NULL);
    do {
        time_t time_now = time(NULL);
        if(time_now - time_was > 1) {
            fprintf(stderr, "bp_spi_sdcard_command: timed out waiting on response\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
        if (brad_bp_bin_spi_bulk(bp, response, 1) != BP_SUCCESS) {
            fprintf(stderr, "bp_spi_sdcard_command: failed to read response byte 0\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
        if(debug) printf("response 0x%02X\n", response[0]);
    } while(response[0] & 0x80);

    if(response_length > 1) {
        if (brad_bp_bin_spi_bulk(bp, response + 1, response_length - 1) != BP_SUCCESS) {
            fprintf(stderr, "bp_spi_sdcard_command: failed to read response bytes 1..n\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
    }

    if(bp_bin_spi_cs(bp, CS_DISABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_command: failed to clear CS\n");
        return false;
    }

    return true;
}

bool bp_spi_sdcard_init(BP* bp)
{
    /* CS false, 80 clk pulses (read 10 bytes) */
    if(bp_bin_spi_cs(bp, CS_DISABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to clear CS\n");
        return false;
    }

    unsigned char buffer[10];
    memset(buffer, 0xff, sizeof(buffer));

    if (brad_bp_bin_spi_bulk(bp, buffer, sizeof(buffer)) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to read bulk bytes to initialize SD to SPI\n");
        return false;
    }

    unsigned char response[8];

    /* interface init */
    if(!bp_spi_sdcard_command(bp, CMD0, 0, response, 8))
        return false;
    if(response[0] != sdcard_response_IDLE) {
        fprintf(stderr, "bp_spi_sdcard_init: failed to enter IDLE mode, response was 0x%02X\n", response[0]);
        return false;
    }

    /* interface condition */
    if(!bp_spi_sdcard_command(bp, CMD8, 0x000001AA, response, 8))
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
        if(time_now - time_was > 2) {
            fprintf(stderr, "bp_spi_sdcard_init: timed out waiting on transition to ACMD41\n");
            return false;
        }
        /* get operating condition */
        if(!bp_spi_sdcard_command(bp, CMD55, 0x00000000, response, 8))
            return false;
        if(response[0] != sdcard_response_IDLE) {
            fprintf(stderr, "bp_spi_sdcard_init: not in IDLE mode for CMD55, response was 0x%02X\n", response[0]);
            return false;
        }
        if(!bp_spi_sdcard_command(bp, ACMD41, 0x40000000, response, 8))
            return false;
    } while(response[0] != sdcard_response_SUCCESS);
    if(debug) printf("returned from ACMD41: %02X %02X %02X %02X %02X %02X %02X %02X\n",
        response[0], response[1], response[2], response[3],
        response[4], response[5], response[6], response[7]);

    return true;
}

const unsigned int block_size = 512;

bool bp_spi_sdcard_readblock(BP *bp, unsigned int blocknum, unsigned char *block)
{
    unsigned char response[8];

    response[0] = 0xff;
    if(!bp_spi_sdcard_command(bp, CMD17, blocknum, response, 1))
        return false;
    if(response[0] != sdcard_response_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_readblock: failed to respond with SUCCESS, response was 0x%02X\n", response[0]);
        return false;
    }
    if(bp_bin_spi_cs(bp, CS_ENABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_readblock: failed to enable CS\n");
        return false;
    }

    response[0] = 0xff;
    time_t time_was = time(NULL);
    do {
        time_t time_now = time(NULL);
        if(time_now - time_was > 1) {
            fprintf(stderr, "bp_spi_sdcard_readblock: timed out waiting on response\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
        if (brad_bp_bin_spi_bulk(bp, response, 1) != BP_SUCCESS) {
            fprintf(stderr, "bp_spi_sdcard_readblock: failed to read response byte 0\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
        if(debug) printf("readblock response 0x%02X\n", response[0]);
    } while(response[0] != sdcard_token_17_18_24);

    if (brad_bp_bin_spi_bulk(bp, block, block_size) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_readblock: failed to read block\n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }
    memset(response, 0xff, 2);
    if (brad_bp_bin_spi_bulk(bp, response, 2) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_readblock: failed to read CRC\n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }
    if(debug) printf("CRC is 0x%02X%02X\n", response[0], response[1]);
    // discard CRC

    memset(response, 0xff, sizeof(response));
    if (brad_bp_bin_spi_bulk(bp, response, sizeof(response)) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_readblock: failed to get trailing results \n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }
    if(debug) printf("trailing read: %02X %02X %02X %02X %02X %02X %02X %02X\n",
        response[0], response[1], response[2], response[3],
        response[4], response[5], response[6], response[7]);


    if(bp_bin_spi_cs(bp, CS_DISABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_readblock: failed to clear CS\n");
        return false;
    }

    return true;
}

bool bp_spi_sdcard_writeblock(BP *bp, unsigned int blocknum, unsigned char block[512])
{
    unsigned char response[8];
    unsigned char blockcopy[block_size];

    memset(response, 0xff, sizeof(response));
    if(!bp_spi_sdcard_command(bp, CMD24, blocknum, response, 1))
        return false;
    if(response[0] != sdcard_response_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to respond with SUCCESS, response was 0x%02X\n", response[0]);
        return false;
    }
    if(bp_bin_spi_cs(bp, CS_ENABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to enable CS\n");
        return false;
    }

    response[0] = sdcard_token_17_18_24;
    if (brad_bp_bin_spi_bulk(bp, response, 1) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to write one byte preceeding write\n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }

    memcpy(blockcopy, block, block_size);
    if (brad_bp_bin_spi_bulk(bp, blockcopy, block_size) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to write block data\n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }

    // junk CRC
    memset(response, 0xff, sizeof(response));
    if (brad_bp_bin_spi_bulk(bp, response, 2) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to write block CRC\n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }

    time_t time_was = time(NULL);
    do {
        time_t time_now = time(NULL);
        if(time_now - time_was > 1) {
            fprintf(stderr, "bp_spi_sdcard_writeblock: timed out waiting on response\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
        if (brad_bp_bin_spi_bulk(bp, response, 1) != BP_SUCCESS) {
            fprintf(stderr, "bp_spi_sdcard_writeblock: failed to read response byte 0\n");
            bp_bin_spi_cs(bp, CS_DISABLE);
            return false;
        }
        if(debug) printf("writeblock response 0x%02X\n", response[0]);
    } while(response[0] == 0);

    memset(response, 0xff, sizeof(response));
    if (brad_bp_bin_spi_bulk(bp, response, sizeof(response)) != BP_SUCCESS) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to get trailing results \n");
        bp_bin_spi_cs(bp, CS_DISABLE);
        return false;
    }
    if(debug) printf("trailing write: %02X %02X %02X %02X %02X %02X %02X %02X\n",
        response[0], response[1], response[2], response[3],
        response[4], response[5], response[6], response[7]);

    if(bp_bin_spi_cs(bp, CS_DISABLE) < 0) {
        fprintf(stderr, "bp_spi_sdcard_writeblock: failed to clear CS\n");
        return false;
    }

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
    bool successful = true;
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
    if (bp_bin_init(bp, &version) != BP_SUCCESS) {
	fprintf(stderr, "Could not enter binary mode\n");
        bp_close(bp);
        exit(EXIT_FAILURE);
    }
    printf("Binary mode version: %u\n", version);

    if (bp_bin_mode_spi(bp, &version) != BP_SUCCESS) {
        fprintf(stderr, "failed to set SPI mode\n");
        bp_bin_reset(bp, &version);
        bp_close(bp);
        exit(EXIT_FAILURE);
    }
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

    if(!bp_spi_sdcard_init(bp)) {
        fprintf(stderr, "failed to start access to SD card as SPI\n");
        bp_bin_reset(bp, &version);
        bp_close(bp);
        exit(EXIT_FAILURE);
    }
    printf("SD Card interface is initialized for SPI\n");

    unsigned char originalblock[512];
    unsigned char junkblock[512];
    unsigned char blockread[512];

    bp_spi_sdcard_readblock(bp, 0, originalblock);
    printf("original block:\n");
    dump_buffer_hex(4, originalblock, 512);

    for(int i = 0; i < 512; i++)
        junkblock[i] = i % 256;

    bp_spi_sdcard_writeblock(bp, 0, junkblock);
    printf("Wrote junk block\n");

    bp_spi_sdcard_readblock(bp, 0, blockread);
    if(memcmp(blockread, junkblock, sizeof(junkblock)) != 0) {
        printf("whoops, error verifying write of junk to block 0\n");
        printf("original junk:\n");
        dump_buffer_hex(4, junkblock, 512);
        printf("junk I got back:\n");
        dump_buffer_hex(4, blockread, 512);
        successful = false;
    } else {
        printf("Verified junk block was written\n");
    }

    bp_spi_sdcard_writeblock(bp, 0, originalblock);
    printf("Wrote original block\n");

    bp_spi_sdcard_readblock(bp, 0, blockread);
    if(memcmp(blockread, originalblock, sizeof(originalblock)) != 0) {
        printf("whoops, error verifying write of original to block 0\n");
        printf("block I got back:\n");
        dump_buffer_hex(4, blockread, 512);
        successful = false;
    } else {
        printf("Verified original block was written\n");
    }
    if(successful) {
        printf("Success!\n");
    } else {
        printf("There were problems.\n");
    }

    bp_bin_reset(bp, &version);
    bp_close(bp);
}
