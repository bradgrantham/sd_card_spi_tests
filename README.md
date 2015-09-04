# Read and write an SD card in SPI mode with Bus Pirate

This program uses libbuspirate (http://sourceforge.net/projects/libbuspirate/)
to operate a Bus Pirate (http://dangerousprototypes.com/docs/Bus_Pirate) in SPI mode to connect with an SD memory card in SPI mode and read and write blocks.

Notably, either libbuspirate or Bus Pirate's binary mode wouldn't receive any bulk SPI operations larger than 6 bytes.  So I broke them up in a utility function. 

It's *really* slow.  I don't know if that's the SPI clock or if it's the BusPirate protocol or a combination of all of the above...

Some references:

* http://elm-chan.org/docs/mmc/mmc_e.html
* http://wiki.seabright.co.nz/wiki/SdCardProtocol.html
* https://hackaday.io/project/3686-read-from-sdhc-card-using-bus-pirate
* http://www.microchip.com/forums/m452739.aspx?print=true (search for "IT WORKED!")
