# Read and write an SD card in SPI mode with Bus Pirate

This program demonstrates a way to use libbuspirate (http://sourceforge.net/projects/libbuspirate/)
to operate a Bus Pirate (http://dangerousprototypes.com/docs/Bus_Pirate) in SPI mode to connect with an SD memory card in SPI SD mode and read and write blocks.

Notably, either libbuspirate or Bus Pirate's binary mode wouldn't receive any bulk SPI operations larger than 6 bytes.  So I broke them up in a utility function. 

It's *really* slow.  I think it's the serial port, as changing the SPI clock doesn't change anything.  There looks to be a way to load the FTDI serial driver with baud rates higher than 115200, but libbuspirate opens the serial port as 115200 baud, so without a library change this sample won't go any faster.

I used this setup, basically wiring the Bus Pirate directly to the SD Card.  I used the pullup mode of the Bus Pirate.  The SD card is actually a microSD adapter (so I didn't solder directly to a real SD card) and I've tested only against a SanDisk 4GB MicroSDHC.

## This is a potentially destructive sample!  Only test it on SD cards you don't mind destroying.

![Testing Setup](https://raw.githubusercontent.com/bradgrantham/sd_card_spi_tests/master/buspirate-sdcard.jpg)

Some references:

* http://elm-chan.org/docs/mmc/mmc_e.html
* http://wiki.seabright.co.nz/wiki/SdCardProtocol.html
* https://hackaday.io/project/3686-read-from-sdhc-card-using-bus-pirate
* http://www.microchip.com/forums/m452739.aspx?print=true (search for "IT WORKED!")
