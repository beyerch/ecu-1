#include <SPI.h>
#include "spi_adc.h"

/*
 *  Name spi_adc.cpp
 *  Author: Alex Pink
 *
 *  Description:
 *  For use with the Microchip MCP3304 13-bit ADC IC. The chip uses the SPI 
 *  communication bus, and the built-in Arduino SPI library is used on the 
 *  the firmware side.
 *
 *  The MCP3304 receives two command bytes:
 *
 *      X,X,X,X,(Start),(Diff/Single),(Ch[2]),(Ch[1]) 
 *
 *  and 
 *
 *      (Ch[0]),X,X,X,X,X,X,X
 *
 *  Ch[2-0] refers of which channel on the chip to sample (0 --> 7)
 *  Differential mode returns the difference of two channels (not used in our application)
 *  Single-ended mode returns the sampled value at the specified channel
 *  The start bit is always high, as it signifies the start of an SPI transmission
 *
 *  Upon receiving the last Ch.0 bit, the ADC samples and returns two bytes:
 *
 *      X,X,X,(signbit),D[11],D[10],D[9],D[8]
 *
 *  and 
 *
 *      D[7],D[6],D[5],D[4],D[3],D[2],D[1],D[0]
 *  
 *  For our usage, there can never be a negative value (we are not comparing two channels),
 *  so the sign bit is ignored.
 */

// set up SPI communication to the MCP3304 DAQ
void initSPI(void){
    SPI.begin(CHIP_SELECT_PIN);
    SPI.setClockDivider(CHIP_SELECT_PIN, SPI_CLK_DIV);  // SPI clock: 2MHz (84MHz/42)
    SPI.setDataMode(CHIP_SELECT_PIN, SPI_MODE0);             // SPI 0,0 as per MCP330x datasheet 
    SPI.setBitOrder(CHIP_SELECT_PIN, MSBFIRST);       // MSB mode, as per MCP330x datasheet
}

// return the sampled analog value of the specified channel 
// operates the MCP3304 in single-ended mode
int readADC(int readChannel){
    int byteMS; int byteLS;

    SPI.transfer(CHIP_SELECT_PIN, B00001100 | (readChannel >> 1), SPI_CONTINUE );

    byteMS = SPI.transfer(CHIP_SELECT_PIN, B00000000 | (readChannel << 7), SPI_CONTINUE );

    byteLS = SPI.transfer(CHIP_SELECT_PIN, B00000000);

    return ( ((byteMS & B00001111) * 256) + byteLS );
}
