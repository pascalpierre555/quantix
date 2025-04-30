/*****************************************************************************
* | File      	:   DEV_Config.c
* | Author      :   Waveshare team
* | Function    :   Hardware underlying interface
* | Info        :
*                Used to shield the underlying layers of each master
*                and enhance portability
*----------------
* |	This version:   V2.0
* | Date        :   2018-10-30
* | Info        :
# ******************************************************************************
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "EPD_config.h"

spi_device_handle_t spi_handle;
void DEV_SPI_WriteByte(uint8_t value) {
    spi_transaction_t t = {
        .length = 8,                    // bits
        .tx_buffer = &value,
    };
    spi_device_transmit(spi_handle, &t);
}

void DEV_SPI_Write_nByte(uint8_t *value, size_t len) {
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = value,
    };
    spi_device_transmit(spi_handle, &t);
}

void DEV_GPIO_Mode(gpio_num_t gpio, uint32_t mode) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = (mode == 0) ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT,
        .pull_up_en = (mode == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
}

void DEV_GPIO_Init()
{
    // do nothing, handled by spi_bus_initialize()
}

void DEV_SPI_Init() {
    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_MOSI_PIN,
        .miso_io_num = EPD_MISO_PIN,
        .sclk_io_num = EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,    // 10MHz
        .mode = 0,                              // SPI mode 0
        .spics_io_num = EPD_CS_PIN,           // 自動控制 CS 腳
        .queue_size = 7,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
}

void DEV_SPI_SendData(UBYTE Reg)
{
	UBYTE i,j=Reg;
	DEV_GPIO_Mode(EPD_MOSI_PIN, 1);
    DEV_GPIO_Mode(EPD_SCK_PIN, 1);
	DEV_Digital_Write(EPD_CS_PIN, 0);
	for(i = 0; i<8; i++)
    {
        DEV_Digital_Write(EPD_SCK_PIN, 0);     
        if (j & 0x80)
        {
            DEV_Digital_Write(EPD_MOSI_PIN, 1);
        }
        else
        {
            DEV_Digital_Write(EPD_MOSI_PIN, 0);
        }
        
        DEV_Digital_Write(EPD_SCK_PIN, 1);
        j = j << 1;
    }
	DEV_Digital_Write(EPD_SCK_PIN, 0);
	DEV_Digital_Write(EPD_CS_PIN, 1);
}

UBYTE DEV_SPI_ReadData()
{
	UBYTE i,j=0xff;
	DEV_GPIO_Mode(EPD_MOSI_PIN, 0);
    DEV_GPIO_Mode(EPD_SCK_PIN, 1);
	DEV_Digital_Write(EPD_CS_PIN, 0);
	for(i = 0; i<8; i++)
	{
		DEV_Digital_Write(EPD_SCK_PIN, 0);
		j = j << 1;
		if (DEV_Digital_Read(EPD_MOSI_PIN))
		{
            j = j | 0x01;
		}
		else
		{
            j= j & 0xfe;
		}
		DEV_Digital_Write(EPD_SCK_PIN, 1);
	}
	DEV_Digital_Write(EPD_SCK_PIN, 0);
	DEV_Digital_Write(EPD_CS_PIN, 1);
	return j;
}

int DEV_Module_Init(void)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
	DEV_Digital_Write(EPD_PWR_PIN, 1);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    return 0;
}

void DEV_Module_Exit(void)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);

    //close 5V
	DEV_Digital_Write(EPD_PWR_PIN, 0);
    DEV_Digital_Write(EPD_RST_PIN, 0);
}

