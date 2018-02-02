/*
 * SmartMatrix Library - Refresh Code for Teensy 3.x Platform
 *
 * Copyright (c) 2015 Louis Beaudoin (Pixelmatix)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "SmartMatrix3.h"
#include "SmartMatrixMultiplexedCommon.h"
#include "DMAChannel.h"

#define INLINE __attribute__( ( always_inline ) ) inline

#define ROW_CALCULATION_ISR_PRIORITY   0xFE // 0xFF = lowest priority

// hardware-specific definitions
// prescale of 1 is F_BUS/2
#define LATCH_TIMER_PRESCALE  0x01
#define TIMER_FREQUENCY     (F_BUS/2)
#define NS_TO_TICKS(X)      (uint32_t)(TIMER_FREQUENCY * ((X) / 1000000000.0))
#define LATCH_TIMER_PULSE_WIDTH_TICKS   NS_TO_TICKS(LATCH_TIMER_PULSE_WIDTH_NS)
#define TICKS_PER_ROW   (TIMER_FREQUENCY/refresh_refreshRate/ROWS_PER_FRAME)
#define IDEAL_MSB_BLOCK_TICKS     (TICKS_PER_ROW/2)
#define MIN_BLOCK_PERIOD_NS (LATCH_TO_CLK_DELAY_NS + ((PANEL_32_PIXELDATA_TRANSFER_MAXIMUM_NS*PIXELS_PER_LATCH)/32))
#define MIN_BLOCK_PERIOD_TICKS NS_TO_TICKS(MIN_BLOCK_PERIOD_NS)

// slower refresh rates require larger timer values - get the min refresh rate from the largest MSB value that will fit in the timer (round up)
#define MIN_REFRESH_RATE    (((TIMER_FREQUENCY/65535)/16/2) + 1)

#define TIMER_REGISTERS_TO_UPDATE   2

#ifndef ADDX_UPDATE_ON_DATA_PINS
    extern DMAChannel dmaOutputAddress;
    extern DMAChannel dmaUpdateAddress;
#endif
extern DMAChannel dmaUpdateTimer;
extern DMAChannel dmaClockOutData;

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void refresh_rowShiftCompleteISR(void);
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void refresh_rowCalculationISR(void);

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
CircularBuffer SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::dmaBuffer;

// refresh_dmaBufferNumRows = the size of the buffer that DMA pulls from to refresh the display
// must be minimum 2 rows so one can be updated while the other is refreshed
// increase beyond two to give more time for the update routine to complete
// (increase this number if non-DMA interrupts are causing display problems)
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
uint8_t SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_dmaBufferNumRows;
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
uint8_t SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_refreshRate = 120;


// todo: just use a single buffer for Blocks/LUT/Data?
#ifndef ADDX_UPDATE_ON_DATA_PINS
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_addresspair SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_addressLUT[ROWS_PER_FRAME];
#endif
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_timerpair SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_timerLUT[LATCHES_PER_ROW];

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_timerpair SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_timerPairIdle;

/*
  buffer contains:
    COLOR_DEPTH/COLOR_CHANNELS_PER_PIXEL/sizeof(int32_t) * (2 words for each pair of pixels: pixel data from n, and n+matrixRowPairOffset)
      first half of the words contain a byte for each shade, going from LSB to MSB
      second half of the words have the same data, plus a high bit in each byte for the clock
    there are MATRIX_WIDTH number of these in order to refresh a row (pair of rows)
    data is organized: matrixUpdateData[row][pixels within row][color depth * 2 updates per clock]

    DMA doesn't shift out the data sequentially, for each row, DMA goes through the buffer matrixUpdateData[currentrow][]

    layout of single matrixUpdateData row:
    in drawing, [] = byte, data is arranged as uint32_t going left to right in the drawing, "clk" is low, "CLK" is high
    DMA goes down one column of the drawing per latch signal trigger, resetting back to the top + shifting 1 byte to the right for the next latch trigger
    [pixel pair  0 - clk - MSB][pixel pair  0 - clk - MSB-1]...[pixel pair  0 - clk - LSB+1][pixel pair  0 - clk - LSB]
    [pixel pair  0 - CLK - MSB][pixel pair  0 - CLK - MSB-1]...[pixel pair  0 - CLK - LSB+1][pixel pair  0 - CLK - LSB]
    [pixel pair  1 - clk - MSB][pixel pair  1 - clk - MSB-1]...[pixel pair  1 - clk - LSB+1][pixel pair  1 - clk - LSB]
    [pixel pair  1 - CLK - MSB][pixel pair  1 - CLK - MSB-1]...[pixel pair  1 - CLK - LSB+1][pixel pair  1 - CLK - LSB]
    ...
    [pixel pair 15 - clk - MSB][pixel pair 15 - clk - MSB-1]...[pixel pair 15 - clk - LSB+1][pixel pair 15 - clk - LSB]
    [pixel pair 15 - CLK - MSB][pixel pair 15 - CLK - MSB-1]...[pixel pair 15 - CLK - LSB+1][pixel pair 15 - CLK - LSB]
 */
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::rowDataStruct * SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows;

#ifndef ADDX_UPDATE_ON_DATA_PINS
#define ADDRESS_ARRAY_REGISTERS_TO_UPDATE   2

// 2x uint32_t to match size and spacing of values it is updating: GPIOx_PSOR and GPIOx_PCOR are 32-bit and adjacent to each other
typedef struct gpiopair {
    uint32_t  gpio_psor;
    uint32_t  gpio_pcor;
} gpiopair;

static gpiopair gpiosync;
#endif

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::SmartMatrix3RefreshMultiplexed(uint8_t bufferrows, rowDataStruct * rowDataBuffer) {
    refresh_dmaBufferNumRows = bufferrows;

    refresh_matrixUpdateRows = rowDataBuffer;

    refresh_timerPairIdle.timer_period = MIN_BLOCK_PERIOD_TICKS;
    refresh_timerPairIdle.timer_oe = MIN_BLOCK_PERIOD_TICKS;

}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
bool SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_isRowBufferFree(void) {
    if(cbIsFull(&dmaBuffer))
        return false;
    else
        return true;
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::rowDataStruct * SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_getNextRowBufferPtr(void) {
    return &(refresh_matrixUpdateRows[cbGetNextWrite(&dmaBuffer)]);
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_writeRowBuffer(uint8_t currentRow) {
#ifndef ADDX_UPDATE_ON_DATA_PINS
    refresh_addresspair rowAddressPair;
    rowAddressPair.bits_to_set = refresh_addressLUT[currentRow].bits_to_set;
    rowAddressPair.bits_to_clear = refresh_addressLUT[currentRow].bits_to_clear;
#endif
        
    SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::rowDataStruct * currentRowDataPtr = SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_getNextRowBufferPtr();

    for (int i = 0; i < LATCHES_PER_ROW; i++) {
#ifndef ADDX_UPDATE_ON_DATA_PINS
        // copy bits to set and clear to generate address for current block
        currentRowDataPtr->rowbits[i].addressValues.bits_to_clear = rowAddressPair.bits_to_clear;
        currentRowDataPtr->rowbits[i].addressValues.bits_to_set = rowAddressPair.bits_to_set;
#endif
        currentRowDataPtr->rowbits[i].timerValues.timer_period = refresh_timerLUT[i].timer_period;
        currentRowDataPtr->rowbits[i].timerValues.timer_oe = refresh_timerLUT[i].timer_oe;
    }

    cbWrite(&dmaBuffer);
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_recoverFromDmaUnderrun(void) {
    // stop timer
    FTM1_SC = FTM_SC_CLKS(0) | FTM_SC_PS(LATCH_TIMER_PRESCALE);

    // point DMA addresses to the next buffer
    int currentRow = cbGetNextRead(&dmaBuffer);
#ifndef ADDX_UPDATE_ON_DATA_PINS
    dmaUpdateAddress.TCD->SADDR = &(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[0].rowbits[0].addressValues);
#endif
    dmaUpdateTimer.TCD->SADDR = &(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[currentRow].rowbits[0].timerValues.timer_oe);
    dmaClockOutData.TCD->SADDR = (uint8_t*)&SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[currentRow];

    // enable channel-to-channel linking so data will be shifted out
    dmaUpdateTimer.TCD->CSR &= ~(1 << 7);  // must clear DONE flag before enabling
    dmaUpdateTimer.TCD->CSR |= (1 << 5);
    // set timer increment back to read from matrixUpdateRows
    dmaUpdateTimer.TCD->SLAST = sizeof(refresh_matrixUpdateRows[0].rowbits[0]) - (TIMER_REGISTERS_TO_UPDATE * sizeof(uint16_t));

    // start timer again - next timer period is MIN_BLOCK_PERIOD_TICKS with OE disabled, period after that will be loaded from matrixUpdateBlock
    FTM1_SC = FTM_SC_CLKS(1) | FTM_SC_PS(LATCH_TIMER_PRESCALE);
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::matrix_underrun_callback SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::matrixUnderrunCallback;

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
typename SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::matrix_calc_callback SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::matrixCalcCallback;

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::setMatrixCalculationsCallback(matrix_calc_callback f) {
    matrixCalcCallback = f;
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::setMatrixUnderrunCallback(matrix_underrun_callback f) {
    matrixUnderrunCallback = f;
}


#define MSB_BLOCK_TICKS_ADJUSTMENT_INCREMENT    10

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_calculateTimerLUT(void) {
    int i;
    uint32_t ticksUsed;
    uint16_t msbBlockTicks = IDEAL_MSB_BLOCK_TICKS + MSB_BLOCK_TICKS_ADJUSTMENT_INCREMENT;

    // start with ideal width of the MSB, and keep lowering until the width of all bits fits within TICKS_PER_ROW
    do {
        ticksUsed = 0;
        msbBlockTicks -= MSB_BLOCK_TICKS_ADJUSTMENT_INCREMENT;
        for (i = 0; i < LATCHES_PER_ROW; i++) {
            uint16_t blockTicks = (msbBlockTicks >> (LATCHES_PER_ROW - i - 1)) + LATCH_TIMER_PULSE_WIDTH_TICKS;

            if (blockTicks < MIN_BLOCK_PERIOD_TICKS)
                blockTicks = MIN_BLOCK_PERIOD_TICKS;

            ticksUsed += blockTicks;
        }
    } while (ticksUsed > TICKS_PER_ROW);

    for (i = 0; i < LATCHES_PER_ROW; i++) {
        // set period and OE values for current block - going from smallest timer values to largest
        // order needs to be smallest to largest so the last update of the row has the largest time between
        // the falling edge of the latch and the rising edge of the latch on the next row - an ISR
        // updates the row in this time

        // period is max on time for this block, plus the dead time while the latch is high
        uint16_t period = (msbBlockTicks >> (LATCHES_PER_ROW - i - 1)) + LATCH_TIMER_PULSE_WIDTH_TICKS;
        // on-time is the max on-time * dimming factor, plus the dead time while the latch is high
        uint16_t ontime = (((msbBlockTicks >> (LATCHES_PER_ROW - i - 1)) * refresh_dimmingFactor) / refresh_dimmingMaximum) + LATCH_TIMER_PULSE_WIDTH_TICKS;

        if (period < MIN_BLOCK_PERIOD_TICKS) {
            uint16_t padding = (MIN_BLOCK_PERIOD_TICKS) - period;
            period += padding;
            ontime += padding;
        }

        // add extra padding once per latch to match refreshRate exactly?  Doesn't seem to make a big difference
#if 0        
        if(!i) {
            uint16_t padding = TICKS_PER_ROW/2 - msbBlockTicks;
            period += padding;
            ontime += padding;
        }
#endif
        refresh_timerLUT[i].timer_period = period;
        refresh_timerLUT[i].timer_oe = ontime;
    }
}

// large factor = more dim, default is full brightness
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
int SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_dimmingFactor = refresh_dimmingMaximum - (100 * 255)/100;

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_setBrightness(uint8_t newBrightness) {
    refresh_dimmingFactor = refresh_dimmingMaximum - newBrightness;
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_setRefreshRate(uint8_t newRefreshRate) {
    if(newRefreshRate > MIN_REFRESH_RATE)
        refresh_refreshRate = newRefreshRate;
    else
        refresh_refreshRate = MIN_REFRESH_RATE;
    refresh_calculateTimerLUT();
}

template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_begin(void) {
    cbInit(&dmaBuffer, refresh_dmaBufferNumRows);

#ifndef ADDX_UPDATE_ON_DATA_PINS
    int i;
    // fill refresh_addressLUT
    for (i = 0; i < ROWS_PER_FRAME; i++) {

        // set all bits that are 1 in address
        refresh_addressLUT[i].bits_to_set = 0x00;
        if (i & 0x01)
            refresh_addressLUT[i].bits_to_set |= (1 << ADDX_PIN_0);
        if (i & 0x02)
            refresh_addressLUT[i].bits_to_set |= (1 << ADDX_PIN_1);
        if (i & 0x04)
            refresh_addressLUT[i].bits_to_set |= (1 << ADDX_PIN_2);
#ifdef ADDX_PIN_3
        if (i & 0x08)
            refresh_addressLUT[i].bits_to_set |= (1 << ADDX_PIN_3);
#endif

        // set all bits that are clear in address
        refresh_addressLUT[i].bits_to_clear = (~refresh_addressLUT[i].bits_to_set) & ADDX_PIN_MASK;
    }
#endif

    // fill refresh_timerLUT
    refresh_calculateTimerLUT();

    // completely fill buffer with data before enabling DMA
    matrixCalcCallback(true);

    // setup debug output
#ifdef DEBUG_PINS_ENABLED
    pinMode(DEBUG_PIN_1, OUTPUT);
    digitalWriteFast(DEBUG_PIN_1, HIGH); // oscilloscope trigger
    digitalWriteFast(DEBUG_PIN_1, LOW);
    pinMode(DEBUG_PIN_2, OUTPUT);
    digitalWriteFast(DEBUG_PIN_2, HIGH); // oscilloscope trigger
    digitalWriteFast(DEBUG_PIN_2, LOW);
    pinMode(DEBUG_PIN_3, OUTPUT);
    digitalWriteFast(DEBUG_PIN_3, HIGH); // oscilloscope trigger
    digitalWriteFast(DEBUG_PIN_3, LOW);
#endif

    // configure the 7 output pins (one pin is left as input, though it can't be used as GPIO output)
    pinMode(GPIO_PIN_CLK_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_B0_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_R0_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_R1_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_G0_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_G1_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_B1_TEENSY_PIN, OUTPUT);

#ifdef ADDX_TEENSY_PIN_0
    // configure the address pins
    pinMode(ADDX_TEENSY_PIN_0, OUTPUT);
#endif
#ifdef ADDX_TEENSY_PIN_1
    pinMode(ADDX_TEENSY_PIN_1, OUTPUT);
#endif
#ifdef ADDX_TEENSY_PIN_2
    pinMode(ADDX_TEENSY_PIN_2, OUTPUT);
#endif
#ifdef ADDX_TEENSY_PIN_3
    pinMode(ADDX_TEENSY_PIN_3, OUTPUT);
#endif

    // setup FTM1
    FTM1_SC = 0;
    FTM1_CNT = 0;
    FTM1_MOD = IDEAL_MSB_BLOCK_TICKS;

    // setup FTM1 compares:
    // latch pulse width set based on max time to update address pins
    FTM1_C0V = LATCH_TIMER_PULSE_WIDTH_TICKS;
    // output OE signal - set to max at first to disable OE
    FTM1_C1V = IDEAL_MSB_BLOCK_TICKS;

    // setup PWM outputs
    ENABLE_LATCH_PWM_OUTPUT();
    ENABLE_OE_PWM_OUTPUT();

    // setup GPIO interrupts
    ENABLE_LATCH_RISING_EDGE_GPIO_INT();
    ENABLE_LATCH_FALLING_EDGE_GPIO_INT();


    // enable clocks to the DMA controller and DMAMUX
    SIM_SCGC7 |= SIM_SCGC7_DMA;
    SIM_SCGC6 |= SIM_SCGC6_DMAMUX;

    // enable minor loop mapping so addresses can get reset after minor loops
    DMA_CR |= DMA_CR_EMLM;

    // allocate all DMA channels up front so channels can link to each other
#ifndef ADDX_UPDATE_ON_DATA_PINS
    dmaOutputAddress.begin(false);
    dmaUpdateAddress.begin(false);
#endif
    dmaUpdateTimer.begin(false);
    dmaClockOutData.begin(false);

#ifndef ADDX_UPDATE_ON_DATA_PINS
    // dmaOutputAddress - on latch rising edge, read address from fixed address temporary buffer, and output address on GPIO
    // using combo of writes to set+clear registers, to only modify the address pins and not other GPIO pins
    // address temporary buffer is refreshed before each DMA trigger (by DMA channel dmaUpdateAddress)
    // only use single major loop, never disable channel
    dmaOutputAddress.source(gpiosync.gpio_pcor);
    dmaOutputAddress.TCD->SOFF = (int)&gpiosync.gpio_psor - (int)&gpiosync.gpio_pcor;
    dmaOutputAddress.TCD->SLAST = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&ADDX_GPIO_CLEAR_REGISTER - (int)&ADDX_GPIO_SET_REGISTER));
    dmaOutputAddress.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    // Destination Minor Loop Offset Enabled - transfer appropriate number of bytes per minor loop, and put DADDR back to original value when minor loop is complete
    // Source Minor Loop Offset Enabled - source buffer is same size and offset as destination so values reset after each minor loop
    dmaOutputAddress.TCD->NBYTES_MLOFFYES = DMA_TCD_NBYTES_SMLOE | DMA_TCD_NBYTES_DMLOE |
                               ((ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&ADDX_GPIO_CLEAR_REGISTER - (int)&ADDX_GPIO_SET_REGISTER)) << 10) |
                               (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * sizeof(gpiosync.gpio_psor));
    // start on higher value of two registers, and make offset decrement to avoid negative number in NBYTES_MLOFFYES (TODO: can switch order by masking negative offset)
    dmaOutputAddress.TCD->DADDR = &ADDX_GPIO_CLEAR_REGISTER;
    // update destination address so the second update per minor loop is ADDX_GPIO_SET_REGISTER
    dmaOutputAddress.TCD->DOFF = (int)&ADDX_GPIO_SET_REGISTER - (int)&ADDX_GPIO_CLEAR_REGISTER;
    dmaOutputAddress.TCD->DLASTSGA = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&ADDX_GPIO_CLEAR_REGISTER - (int)&ADDX_GPIO_SET_REGISTER));
    // single major loop
    dmaOutputAddress.TCD->CITER_ELINKNO = 1;
    dmaOutputAddress.TCD->BITER_ELINKNO = 1;
    // link channel dmaUpdateAddress, enable major channel-to-channel linking, don't clear enable on major loop complete
    dmaOutputAddress.TCD->CSR = (dmaUpdateAddress.channel << 8) | (1 << 5);
    dmaOutputAddress.triggerAtHardwareEvent(DMAMUX_SOURCE_LATCH_RISING_EDGE);

    // dmaUpdateAddress - copy address values from current position in array to buffer to temporarily hold row values for the next timer cycle
    // only use single major loop, never disable channel
    dmaUpdateAddress.TCD->SADDR = &(refresh_matrixUpdateRows[0].rowbits[0].addressValues);
    dmaUpdateAddress.TCD->SOFF = sizeof(uint16_t);
    dmaUpdateAddress.TCD->SLAST = sizeof(refresh_matrixUpdateRows[0].rowbits[0]) - (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * sizeof(uint16_t));
    dmaUpdateAddress.TCD->ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    // 16-bit = 2 bytes transferred
    // transfer two 16-bit values, reset destination address back after each minor loop
    dmaUpdateAddress.TCD->NBYTES_MLOFFNO = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * sizeof(uint16_t));
    // start with the register that's the highest location in memory and make offset decrement to avoid negative number in NBYTES_MLOFFYES register (TODO: can switch order by masking negative offset)
    dmaUpdateAddress.TCD->DADDR = &gpiosync.gpio_pcor;
    dmaUpdateAddress.TCD->DOFF = (int)&gpiosync.gpio_psor - (int)&gpiosync.gpio_pcor;
    dmaUpdateAddress.TCD->DLASTSGA = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&gpiosync.gpio_pcor - (int)&gpiosync.gpio_psor));
    // no minor loop linking, single major loop, single minor loop, don't clear enable after major loop complete
    dmaUpdateAddress.TCD->CITER_ELINKNO = 1;
    dmaUpdateAddress.TCD->BITER_ELINKNO = 1;
    dmaUpdateAddress.TCD->CSR = 0;
#endif

#define DMA_TCD_MLOFF_MASK  (0x3FFFFC00)

    // dmaUpdateTimer - on latch falling edge, load FTM1_CV1 and FTM1_MOD with with next values from current block
    // only use single major loop, never disable channel
    // link to dmaClockOutData channel when complete
    dmaUpdateTimer.TCD->SADDR = &(refresh_matrixUpdateRows[0].rowbits[0].timerValues.timer_oe);
    dmaUpdateTimer.TCD->SOFF = sizeof(uint16_t);
    dmaUpdateTimer.TCD->SLAST = sizeof(refresh_matrixUpdateRows[0].rowbits[0]) - (TIMER_REGISTERS_TO_UPDATE * sizeof(uint16_t));
    dmaUpdateTimer.TCD->ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    // 16-bit = 2 bytes transferred
    dmaUpdateTimer.TCD->NBYTES_MLOFFNO = TIMER_REGISTERS_TO_UPDATE * sizeof(uint16_t);
    dmaUpdateTimer.TCD->DADDR = &FTM1_C1V;
    dmaUpdateTimer.TCD->DOFF = (int)&FTM1_MOD - (int)&FTM1_C1V;
    dmaUpdateTimer.TCD->DLASTSGA = TIMER_REGISTERS_TO_UPDATE * ((int)&FTM1_C1V - (int)&FTM1_MOD);
    // no minor loop linking, single major loop
    dmaUpdateTimer.TCD->CITER_ELINKNO = 1;
    dmaUpdateTimer.TCD->BITER_ELINKNO = 1;
    // link dmaClockOutData channel, enable major channel-to-channel linking, don't clear enable after major loop complete
    dmaUpdateTimer.TCD->CSR = (dmaClockOutData.channel << 8) | (1 << 5);
    dmaUpdateTimer.triggerAtHardwareEvent(DMAMUX_SOURCE_LATCH_FALLING_EDGE);

#ifdef ADDX_UPDATE_ON_DATA_PINS
    uint16_t rowBitStructBytesToShift = sizeof(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[0].rowbits[0].data) + ADDX_UPDATE_BEFORE_LATCH_BYTES;
#else
    uint16_t rowBitStructBytesToShift = sizeof(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[0].rowbits[0].data);
#endif

    // this is the number of bytes in the gap between each sequential rowBitStruct.data arrays
    uint16_t rowBitStructDataOffset = sizeof(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[0].rowbits[0]) - rowBitStructBytesToShift;

    // dmaClockOutData - repeatedly load gpio_array into GPIOD_PDOR, stop and int on major loop complete
    dmaClockOutData.TCD->SADDR = refresh_matrixUpdateRows;
    dmaClockOutData.TCD->SOFF = 1;
    // SADDR will get updated by ISR, no need to set SLAST
    dmaClockOutData.TCD->SLAST = 0;
    dmaClockOutData.TCD->ATTR = DMA_TCD_ATTR_SSIZE(0) | DMA_TCD_ATTR_DSIZE(0);
    // after each minor loop, apply no offset to source data, it's pointing to the next buffer already
    // clock out (PIXELS_PER_LATCH * DMA_UPDATES_PER_CLOCK + ADDX_UPDATE_BEFORE_LATCH_BYTES) number of bytes per loop
    dmaClockOutData.TCD->NBYTES_MLOFFYES = DMA_TCD_NBYTES_SMLOE |
                                ((rowBitStructDataOffset << 10) & DMA_TCD_MLOFF_MASK) |
                                rowBitStructBytesToShift;
    dmaClockOutData.TCD->DADDR = &GPIOD_PDOR;
    dmaClockOutData.TCD->DOFF = 0;
    dmaClockOutData.TCD->DLASTSGA = 0;
    dmaClockOutData.TCD->CITER_ELINKNO = LATCHES_PER_ROW;
    dmaClockOutData.TCD->BITER_ELINKNO = LATCHES_PER_ROW;
    // int after major loop is complete
    dmaClockOutData.TCD->CSR = DMA_TCD_CSR_INTMAJOR;
    
    // for debugging - enable bandwidth control (space out GPIO updates so they can be seen easier on a low-bandwidth logic analyzer)
    // enable for now, until DMA sharing complications (brought to light by Teensy 3.6 SDIO) can be worked out - use bandwidth control to space out our DMA access and allow SD reads to not slow down shifting to the matrix
    // also enable for now, until it can be selectively enabled for higher clock speeds (140MHz+) where the data rate is too high for the panel
    dmaClockOutData.TCD->CSR |= (0x02 << 14);

    // enable a done interrupt when all DMA operations are complete
    dmaClockOutData.attachInterrupt(refresh_rowShiftCompleteISR<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>);

    // enable additional dma interrupt used as software interrupt
    NVIC_SET_PRIORITY(IRQ_DMA_CH0 + dmaUpdateTimer.channel, ROW_CALCULATION_ISR_PRIORITY);
    dmaUpdateTimer.attachInterrupt(refresh_rowCalculationISR<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>);

#ifndef ADDX_UPDATE_ON_DATA_PINS
    dmaOutputAddress.enable();
    dmaUpdateAddress.enable();
#endif
    dmaUpdateTimer.enable();
    dmaClockOutData.enable();

    // at the end after everything is set up: enable timer from system clock, with appropriate prescale
    FTM1_SC = FTM_SC_CLKS(1) | FTM_SC_PS(LATCH_TIMER_PRESCALE);
}

// low priority ISR triggered by software interrupt on a DMA channel that doesn't need interrupts otherwise
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void refresh_rowCalculationISR(void) {
#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_2, HIGH); // oscilloscope trigger
#endif

    SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::matrixCalcCallback(false);

#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_2, LOW);
#endif
}

// DMA transfer done (meaning data was shifted and timer value for MSB on current row just got loaded)
// set DMA up for loading the next row, triggered from the next timer latch
template <int refreshDepth, int matrixWidth, int matrixHeight, unsigned char panelType, unsigned char optionFlags>
void refresh_rowShiftCompleteISR(void) {
#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_1, HIGH); // oscilloscope trigger
#endif
    // done with previous row, mark it as read
    cbRead(&SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::dmaBuffer);

    if(cbIsEmpty(&SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::dmaBuffer)) {
#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_1, LOW); // oscilloscope trigger
#endif
        // point dmaUpdateTimer to repeatedly load from values that set mod to MIN_BLOCK_PERIOD_TICKS and disable OE
        dmaUpdateTimer.TCD->SADDR = &SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_timerPairIdle;
        // set timer increment to repeat timerPairIdle
        dmaUpdateTimer.TCD->SLAST = -(TIMER_REGISTERS_TO_UPDATE*sizeof(uint16_t));
        // disable channel-to-channel linking - don't link dmaClockOutData until buffer is ready
        dmaUpdateTimer.TCD->CSR &= ~(1 << 5);

        // set flag so other ISR can enable DMA again when data is ready
        SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::matrixUnderrunCallback();

#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_1, HIGH); // oscilloscope trigger
#endif
    } else {
        // get next row to draw to display and update DMA pointers
        int currentRow = cbGetNextRead(&SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::dmaBuffer);
#ifndef ADDX_UPDATE_ON_DATA_PINS
        dmaUpdateAddress.TCD->SADDR = &(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[currentRow].rowbits[0].addressValues);
#endif
        dmaUpdateTimer.TCD->SADDR = &(SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[currentRow].rowbits[0].timerValues.timer_oe);
        dmaClockOutData.TCD->SADDR = (uint8_t*)&SmartMatrix3RefreshMultiplexed<refreshDepth, matrixWidth, matrixHeight, panelType, optionFlags>::refresh_matrixUpdateRows[currentRow];
    }

    // trigger software interrupt to call refresh_rowCalculationISR() (DMA channel interrupt used instead of actual softint)
    NVIC_SET_PENDING(IRQ_DMA_CH0 + dmaUpdateTimer.channel);

    // clear pending int
    dmaClockOutData.clearInterrupt();

#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_1, LOW); // oscilloscope trigger
#endif
}
