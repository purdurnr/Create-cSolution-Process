/*
 * Copyright (c) 2013 - 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "board.h"
#include "fsl_iocon.h"
#include <stdbool.h>
#include "NAFE13388.h"
#include <math.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define SPI_MOSI_PORT                       (3)
#define SPI_MOSI_PIN                        (21)
#define SPI_MISO_PORT                       (3)
#define SPI_MISO_PIN                        (22)
#define SPI_CLK_PORT                        (3)
#define SPI_CLK_PIN                         (20)
#define NAFE_CS_PORT                        (3)
#define NAFE_CS_PIN                         (30)
#define NAFE_DATA_RDY_PORT                  (1)
#define NAFE_DATA_RDY_PIN                   (18)
#define NAFE_DATA_SYNC_PORT                 (0)
#define NAFE_DATA_SYNC_PIN                  (24)


#define SPI_CLK_FRQ                         (8*1000*1000)
#define SPI_INSTACE                         (SPI9)
#define NAFE_ADDR                           (0)


#define ADC_READING_TO_VOLTAGE(reading)     (reading * 10.F / 16777216)
#define ADC_RATE_SELECT                     (7)

 /*******************************************************************************
 * Variables
 ******************************************************************************/
static nafe_t       instance;
static uint8_t      s_adcBuf[64 * 1024]; 
static RINGBUFF_T   s_adcRingBuf;
 /*******************************************************************************
 * Code
 ******************************************************************************/
 
static void delay(uint32_t ms)
{
    volatile int i,j;
    for(i=0; i<ms; i++)
    {
        for(j=0; j<10000; j++)
        {
            __NOP();
        }
    }
}

static void _cli_wait_quit(void)
{
    uint8_t ch;
    PRINTF("Press Z to return\r\n");
    while (1)
    {
        ch = GETCHAR();
        if(ch == 'Z' || ch == 'z')
        {
            return;
        }
    }
}

static uint8_t _cli_num_sel(uint8_t start, uint8_t end)
{
    uint8_t ch;
    while (1)
    {
        ch = GETCHAR();
        if ((ch < start + '0') || (ch > end + '0'))
        {
            continue;
        }
        else
        {
            break;
        }
    }
    return ch - '0';
}

static uint32_t tic(void)
{
    SysTick_Config(0x00FFFFFF);
    SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
    return SysTick->VAL;
}

static uint32_t toc(uint32_t tic_ticks)
{
    return tic_ticks - SysTick->VAL;
}

static void _sel_ch_config0(uint32_t *hv_inp, uint32_t *hv_inn, uint32_t *pga_gain)
{
    PRINTF("Select HV_INP(0-8)\r\n"
        "\t0. internal GND\r\n"
        "\t1. AI1P\r\n"
        "\t2. AI2P\r\n"
        "\t3. AI3P\r\n"
        "\t4. AI4P\r\n"
        "\t5. REFCAL_H\r\n"
        "\t6. REFCAL_L\r\n"
        "\t7. AICOM\r\n"
        "\t8. VEXC\r\n");
    *hv_inp = _cli_num_sel(0, 8);
    
    PRINTF("Select HV_INN(0-8)\r\n"
        "\t0. internal GND\r\n"
        "\t1. AI1N\r\n"
        "\t2. AI2N\r\n"
        "\t3. AI3N\r\n"
        "\t4. AI4N\r\n"
        "\t5. REFCAL_H\r\n"
        "\t6. REFCAL_L\r\n"
        "\t7. AICOM\r\n"
        "\t8. VEXC\r\n");
    *hv_inn = _cli_num_sel(0, 8);
    
    PRINTF("Select PGA_GAIN(0-5)\r\n"
        "\t0. 0.2x\r\n"
        "\t1. 0.4x\r\n"
        "\t2. 0.8x\r\n"
        "\t3. 1.0x\r\n"
        "\t4. 2.0x\r\n"
        "\t5. 4.0x\r\n"
        "\t6. 8.0x\r\n"
        "\t7. 16.0x\r\n");
    *pga_gain = _cli_num_sel(0, 7);
}


static void SCSR_demo(void)
{
    uint8_t ch, adc_sync, ch_delay;
    uint32_t adc_chl, hv_inp, hv_inn, pga_gain, i;
    int32_t adc_res;
    uint8_t buf[3];
    uint16_t tmp;
    float voltage;
    
    PRINTF("Select ADC Channel:(0-9)\r\n");
    adc_chl = _cli_num_sel(0, 15);
    
    _sel_ch_config0(&hv_inp, &hv_inn, &pga_gain);
    NAFE13388_CmdSetCurrentPointer(&instance, adc_chl);
    NAFE13388_WriteReg16B(&instance, CH_CONFIG1, ADC_RATE_SELECT<<3);
    NAFE13388_WriteReg16B(&instance, CH_CONFIG0, (hv_inp<<12) | (hv_inn<<8) | (1<<4) | (pga_gain<<5));

    ch_delay = 0x1A;
    tmp = NAFE13388_ReadReg16B(&instance, CH_CONFIG2);
    tmp &= ~(((uint8_t)pow(2,5)-1)<<10);
    tmp |= ch_delay<<10;
    NAFE13388_WriteReg16B(&instance, CH_CONFIG2, tmp);

    /* config ADC_SYNC bit */
    PRINTF("Select trigger by ADC_SYNC(0-1)\r\n"
        "\t0. the conversion start is triggered by this SPI command at the last SPI clock falling edge\r\n"
        "\t1. the conversion start is triggered by SYNC rising edge\r\n");
    adc_sync = _cli_num_sel(0, 1);
    tmp = NAFE13388_ReadReg16B(&instance, SYS_CONFIG0);
    tmp &= ~(1<<5);
    tmp |= adc_sync<<5;
    NAFE13388_WriteReg16B(&instance, SYS_CONFIG0, tmp);
    
    PRINTF("Press ENTER to trigger conversion.\r\n");
    PRINTF("Press Z to return to main menu\r\n");
    
    while(1)
    {
        ch = GETCHAR();

        startScsr(&instance);
        
        if(adc_sync)
        {
            /* create a low frq pulse to trigger adc conversion */
            for (i=0;i<50;i++){
            	__NOP();
            }
            GPIO_PinWrite(GPIO, NAFE_DATA_SYNC_PORT, NAFE_DATA_SYNC_PIN, 1);
            for (i=0;i<50;i++){
            	__NOP();
            }
            GPIO_PinWrite(GPIO, NAFE_DATA_SYNC_PORT, NAFE_DATA_SYNC_PIN, 0);
        }
        

        while(GPIO_PinRead(GPIO, instance.gpio_rdy_port, instance.gpio_rdy_pin)==0); /* wait for RDY pin to high */
        NAFE13388_SPIRead(&instance, buf, 3);
        
        /* convert 24 bit result into int32_t format */
        adc_res = (buf[0]<<24) + (buf[1]<<16) + (buf[2]<<8);
        adc_res /= 256;

        voltage = ADC_READING_TO_VOLTAGE(adc_res);
        PRINTF("ADC[%d]:%fV\r\n", adc_chl, voltage);
        
        if(ch == 'Z' || ch == 'z')
        {
            return;
        }
    }
}


static void MCMR_demo(void)
{
    uint8_t ch,ch_delay;
    uint32_t len, i, chlmask, adc_chl, hv_inp, hv_inn, pga_gain;//, num_of_loop;
    uint8_t buf[3];
    float voltage;
    uint8_t drdyb_pin_seq;
    uint16_t tmp;
    
    PRINTF("In this demo, two channels are selected for MCMR\r\n");
    
    chlmask = 0x0000;
    for(i=0; i<2; i++)
    {
        PRINTF("Select ADC sample channel[%d]:(0-9)\r\n", i);
        adc_chl = _cli_num_sel(0, 15);
        chlmask |= (1<<adc_chl);
        _sel_ch_config0(&hv_inp, &hv_inn, &pga_gain);
        NAFE13388_CmdSetCurrentPointer(&instance, adc_chl);

        NAFE13388_WriteReg16B(&instance, CH_CONFIG0, (hv_inp<<12) | (hv_inn<<8) | (1<<4) | (pga_gain<<5));

        NAFE13388_WriteReg16B(&instance, CH_CONFIG1, ADC_RATE_SELECT<<3);

        ch_delay = 0x1A;
        tmp = NAFE13388_ReadReg16B(&instance, CH_CONFIG2);
        tmp &= ~(((uint8_t)pow(2,5)-1)<<10);
        tmp |= ch_delay<<10;
        NAFE13388_WriteReg16B(&instance, CH_CONFIG2, tmp);

    }
    /* Enable chl */
    NAFE13388_WriteReg16B(&instance, CH_CONFIG4, chlmask);
    PRINTF("Channel enable mask:0x%X\r\n", chlmask);
    
    /* config DRDYB_PIN_SEQ bit */
    PRINTF("Select trigger by DRDYB_PIN_SEQ:(0-1)\r\n"
        "\t0. produce falling on every channel conversion done\r\n"
        "\t1. produce falling edge only when the sequencer is done with the last enabled channel conversion\r\n");
    drdyb_pin_seq = _cli_num_sel(0, 1);
    tmp = NAFE13388_ReadReg16B(&instance, SYS_CONFIG0);
    tmp &= ~(1<<4);
    tmp |= drdyb_pin_seq<<4;
    NAFE13388_WriteReg16B(&instance, SYS_CONFIG0, tmp);
    
    PRINTF("Press ENTER to trigger conversion.\r\n");
    PRINTF("Press Z to return\r\n");

    while(1)
    {
        ch = GETCHAR();

        startMcmr(&instance, chlmask, drdyb_pin_seq);

        while(NAFE13388_GetReadingsRemaining(&instance) != 0) {};
        len = NAFE13388_GetRBCount(&instance);
                
        PRINTF("ADC[%d]:%d samples collected\r\n", adc_chl, len / ADC_READ_SIZE);
        for(i=0; i<len / ADC_READ_SIZE; i++)
        {
            NAFE13388_ReadFromRB(&instance, buf, 3);
            voltage = ((buf[0]<<24) + (buf[1]<<16) + (buf[2]<<8)) / 256;
            voltage = ADC_READING_TO_VOLTAGE(voltage);

            PRINTF("%fV\r\n", voltage);
        }
        
        if(ch == 'Z' || ch == 'z')
        {
            return;
        }
    }

}


static void SCCR_demo(void)
{
    uint32_t adc_chl, hv_inp, hv_inn, pga_gain, num_of_loop, len, i, ticks, ch_delay;
    uint8_t buf[3];
    uint8_t adc_sync;
    uint16_t tmp;
    float voltage;
    
    PRINTF("Select ADC Channel:(0-9)\r\n");
    adc_chl = _cli_num_sel(0, 15);
    
    _sel_ch_config0(&hv_inp, &hv_inn, &pga_gain);
    NAFE13388_CmdSetCurrentPointer(&instance, adc_chl);
    NAFE13388_WriteReg16B(&instance, CH_CONFIG1, ADC_RATE_SELECT<<3);
    NAFE13388_WriteReg16B(&instance, CH_CONFIG0, (hv_inp<<12) | (hv_inn<<8) | (1<<4) | (pga_gain<<5));
    
    ch_delay = 0x1A;
    tmp = NAFE13388_ReadReg16B(&instance, CH_CONFIG2);
    tmp &= ~(((uint8_t)pow(2,5)-1)<<10);
    tmp |= ch_delay<<10;
    NAFE13388_WriteReg16B(&instance, CH_CONFIG2, tmp);

    PRINTF("Select number of loop:(1-3)\r\n"
        "\t1. 64\r\n"
        "\t2. 128\r\n"
        "\t3. 192\r\n");
    num_of_loop = _cli_num_sel(1, 3)*64;

    /* config ADC_SYNC bit */
    PRINTF("Select trigger by ADC_SYNC(0-1)\r\n"
        "\t0. the conversion start is triggered by this SPI command at the last SPI clock falling edge\r\n"
        "\t1. the conversion start is triggered by SYNC rising edge\r\n");
    adc_sync = _cli_num_sel(0, 1);
    tmp = NAFE13388_ReadReg16B(&instance, SYS_CONFIG0);
    tmp &= ~(1<<5);
    tmp |= adc_sync<<5;
    NAFE13388_WriteReg16B(&instance, SYS_CONFIG0, adc_sync<<5);
    
    /* start transfer and waiit for complete */
    startSccr(&instance, num_of_loop);
    
    if(adc_sync)
    {
        /* create a low frq pulse to trigger adc conversion */
        for (i=0;i<50;i++){
        	__NOP();
        }
        GPIO_PinWrite(GPIO, NAFE_DATA_SYNC_PORT, NAFE_DATA_SYNC_PIN, 1);
        for (i=0;i<50;i++){
        	__NOP();
        }
        GPIO_PinWrite(GPIO, NAFE_DATA_SYNC_PORT, NAFE_DATA_SYNC_PIN, 0);
    }
    
    ticks = tic();
    while(NAFE13388_GetReadingsRemaining(&instance) != 0) {};
        
    ticks = toc(ticks);
    len = NAFE13388_GetRBCount(&instance);
        
    /* convert sample to voltage */
    for(i=0; i<len / ADC_READ_SIZE; i++)
    {
        NAFE13388_ReadFromRB(&instance, buf, 3);
        voltage = ((buf[0]<<24) + (buf[1]<<16) + (buf[2]<<8)) / 256;
        voltage = ADC_READING_TO_VOLTAGE(voltage);
        PRINTF("%fV\r\n", voltage);
    }
    PRINTF("%d samples collected, time elapsed:%dus\r\n", len / ADC_READ_SIZE, (ticks) / (SystemCoreClock / 1000000));
    NAFE13388_CmdAbort(&instance);
    
    _cli_wait_quit();
}

static void MCCR_demo(void)
{
    uint32_t adc_chl, hv_inp, hv_inn, pga_gain, num_of_loop, chlmask, len, i, ticks, ch_delay;
    uint8_t buf[3],VIEX_VI,VIEX_POL,VIEX_MAG,VEXC_EN,VIEX_AIP_EN,VIEX_AIN_EN;
    float voltage;
    uint16_t tmp;
    uint8_t drdyb_pin_seq;
    
    PRINTF("In this demo, 2 channels are selected for MCCR\r\n");
    
    chlmask = 0x0000;
    for(i=0; i<2; i++)
    {
        PRINTF("Select ADC sample channel[%d]:(0-9)\r\n", i);
        adc_chl = _cli_num_sel(0, 15);
        NAFE13388_CmdSetCurrentPointer(&instance, adc_chl);
        chlmask |= (1<<adc_chl);

        _sel_ch_config0(&hv_inp, &hv_inn, &pga_gain);
        NAFE13388_WriteReg16B(&instance, CH_CONFIG0, (hv_inp<<12) | (hv_inn<<8) | (1<<4) | (pga_gain<<5));

        NAFE13388_WriteReg16B(&instance, CH_CONFIG1, ADC_RATE_SELECT<<3);

        ch_delay = 0x1A;
        tmp = NAFE13388_ReadReg16B(&instance, CH_CONFIG2);
        tmp &= ~(((uint8_t)pow(2,5)-1)<<10);
        tmp |= ch_delay<<10;
        NAFE13388_WriteReg16B(&instance, CH_CONFIG2, tmp);
    }

    /* Enable chl */
    NAFE13388_WriteReg16B(&instance, CH_CONFIG4, chlmask);
    PRINTF("Channel enable mask:0x%X\r\n", chlmask);
    
    PRINTF("Select number of loop:(1-3)\r\n"
        "\t1. 64\r\n"
        "\t2. 128\r\n"
        "\t3. 192\r\n");
    num_of_loop = _cli_num_sel(1, 3)*64;
    
    /* config DRDYB_PIN_SEQ bit */
    PRINTF("Select trigger by DRDYB_PIN_SEQ:(0-1)\r\n"
        "\t0. produce falling on every channel conversion done\r\n"
        "\t1. produce falling edge only when the sequencer is done with the last enabled channel conversion\r\n");
    drdyb_pin_seq = _cli_num_sel(0, 1);
    tmp = NAFE13388_ReadReg16B(&instance, SYS_CONFIG0);
    tmp &= ~(1<<4);
    tmp |= drdyb_pin_seq<<4;
    NAFE13388_WriteReg16B(&instance, SYS_CONFIG0, tmp);
    
    ticks = tic();
    
    /* start transfer and waiit for complete */
    startMccr(&instance, num_of_loop, chlmask, drdyb_pin_seq);

    while(NAFE13388_GetReadingsRemaining(&instance) != 0) {};
    ticks = toc(ticks);
    len = NAFE13388_GetRBCount(&instance);
          
    for(i=0; i<len / ADC_READ_SIZE; i++)
    {
        NAFE13388_ReadFromRB(&instance, buf, 3);
        voltage = ((buf[0]<<24) + (buf[1]<<16) + (buf[2]<<8)) / 256;
        voltage = ADC_READING_TO_VOLTAGE(voltage);
        PRINTF("ADC[%d]%fV\r\n", i%2, voltage);
    }
    PRINTF("%d samples collected, time elapsed:%dus\r\n", len / ADC_READ_SIZE, (ticks) / (SystemCoreClock / 1000000));
    NAFE13388_CmdAbort(&instance);
    _cli_wait_quit();
}

static void GEFRAN_Test(void)
{
	//Geeral support variables
	const uint8_t colorStringBlu[] = "\033[0;34m";
	const uint8_t colorStringPurple[] = "\033[0;35m";
    uint8_t ch, adc_sync;
    uint32_t adc_chl, hv_inp, hv_inn, pga_gain,status_bit,status_sticky, ch_delay;
    int32_t adc_res;
    uint8_t buf[6];
    uint16_t tmp;
    int j=0;
    float voltage;
    float PGA2FLOAT[]={0.2,0.4,0.8,1,2,4,8,16};

    adc_chl = 0;
    hv_inp=5;
    hv_inn=0;
    pga_gain=0;
    adc_sync=0;    //SPI EN going high
    status_bit=1;
    status_sticky=0; //i.e. sticky

    tmp = NAFE13388_ReadReg16B(&instance, SYS_CONFIG0);
    tmp &= ~(1<<5);
    tmp |= adc_sync<<5;
    tmp |= status_bit<<6; //Enable status bit
    tmp |= status_sticky<<13; //Enable status bit
    NAFE13388_WriteReg16B(&instance, SYS_CONFIG0, tmp); //Configuring SYS_CONFIG0 for my setup

    NAFE13388_CmdSetCurrentPointer(&instance, adc_chl); //Working only with channel0
    NAFE13388_WriteReg16B(&instance, CH_CONFIG1, ADC_RATE_SELECT<<3); //Select rate

    ch_delay = 0x1A;
    tmp = NAFE13388_ReadReg16B(&instance, CH_CONFIG2);
    tmp &= ~(((uint8_t)pow(2,5)-1)<<10);
    tmp |= ch_delay<<10;
    NAFE13388_WriteReg16B(&instance, CH_CONFIG2, tmp);

    NAFE13388_WriteReg24B(&instance, CHCONFIGOVTHRS0, 0x7fffff); //Selecting thresholds
    NAFE13388_WriteReg24B(&instance, CHCONFIGUDRTHRS0,0x800000 );


    while(1)
    {

        if (j&0x08){//every 8 conversion put the ADC in overload
        	pga_gain=6;
        	PRINTF(colorStringBlu);
        } else {
        	pga_gain=0;
        	PRINTF(colorStringPurple);
        }

        NAFE13388_WriteReg16B(&instance, CH_CONFIG0, (hv_inp<<12) | (hv_inn<<8) | (1<<4) | (pga_gain<<5)); //Set correct PGA gain again

        startScsr(&instance); //Conversion start
        while(GPIO_PinRead(GPIO, instance.gpio_rdy_port, instance.gpio_rdy_pin)==0);

        NAFE13388_SPIRead(&instance, buf, 3+status_bit); //read the result + the status bit

        /* convert 24 bit result into int32_t format */
        adc_res = (buf[1]<<24) + (buf[2]<<16) + (buf[3]<<8);
        adc_res /= 256;

        voltage = ADC_READING_TO_VOLTAGE(adc_res)/PGA2FLOAT[pga_gain];
        PRINTF("[%d]\tADC:%.3fV\t", j, voltage);
        PRINTF("STATUS:0x%x",buf[0]);

        if (buf[0]&0x08){ //if the PGA_OVERLOAD bit is set then reset it with CMD_CLEARDATA
        	NAFE13388_SendCmd(&instance, CMD_CLEARDATA, CMD_WR, NULL, 0);
        	PRINTF("\033[0;31m");
        	PRINTF(" -------> CMD_CLEARDATA\r\n");
        	PRINTF("\033[0m");
        	}
        else{
        	PRINTF("\r\n");
        	}

        if(j>40){
        	PRINTF("\033[0m");
            return;
        }
        j++;
    }

}

static void GEFRAN_Test2(void)
{
	//Geeral support variables
	const uint8_t colorStringBlu[] = "\033[0;34m";
	const uint8_t colorStringPurple[] = "\033[0;35m";
    uint8_t ch, adc_sync;
    uint32_t adc_chl, hv_inp, hv_inn, pga_gain,status_bit,status_sticky, ch_delay,global_alarm_sticky;
    int32_t adc_res;
    uint8_t buf[6];
    uint16_t tmp,ch_status0,global_alarm_interrupt;
    int j=0;
    float voltage;
    float PGA2FLOAT[]={0.2,0.4,0.8,1,2,4,8,16};

    adc_chl = 0;
    hv_inp=5;
    hv_inn=0;
    pga_gain=6;
    adc_sync=0;    //SPI EN going high
    status_bit=1;
    status_sticky=0; //LAtched
    //status_sticky=1; //Live
    global_alarm_sticky=0; //Live
    //global_alarm_sticky=1; //LAtched

    tmp = NAFE13388_ReadReg16B(&instance, SYS_CONFIG0);
    tmp &= ~(1<<5);
    tmp |= global_alarm_sticky<<3; //Enable status bit
    tmp |= adc_sync<<5;
    tmp |= status_bit<<6; //Enable status bit
    tmp |= status_sticky<<13; //Enable status bit
    NAFE13388_WriteReg16B(&instance, SYS_CONFIG0, tmp); //Configuring SYS_CONFIG0 for my setup

    NAFE13388_WriteReg16B(&instance, GLOBALALAMENABLE, 0xFFFF);

    NAFE13388_CmdSetCurrentPointer(&instance, adc_chl); //Working only with channel0
    NAFE13388_WriteReg16B(&instance, CH_CONFIG1, ADC_RATE_SELECT<<3); //Select rate

    ch_delay = 0x1A;
    tmp = NAFE13388_ReadReg16B(&instance, CH_CONFIG2);
    tmp &= ~(((uint8_t)pow(2,5)-1)<<10);
    tmp |= ch_delay<<10;
    NAFE13388_WriteReg16B(&instance, CH_CONFIG2, tmp);

    NAFE13388_WriteReg24B(&instance, CHCONFIGOVTHRS0, 0x7fffff); //Selecting thresholds
    NAFE13388_WriteReg24B(&instance, CHCONFIGUDRTHRS0,0x800000 );
    NAFE13388_WriteReg16B(&instance, 0x37, 0x7FFF);

    NAFE13388_SendCmd(&instance, CMD_CLEARALARM, CMD_WR, NULL, 0);
    NAFE13388_SendCmd(&instance, CMD_CLEARDATA, CMD_WR, NULL, 0);

    while(1)
    {

        if (j&0x08){//every 8 conversion put the ADC in overload
        	hv_inp=5;
        	PRINTF("\033[0m");
        } else {
        	hv_inp=6;
        	PRINTF(colorStringPurple);
        }

        NAFE13388_WriteReg16B(&instance, CH_CONFIG0, (hv_inp<<12) | (hv_inn<<8) | (1<<4) | (pga_gain<<5)); //Set correct PGA gain again

        startScsr(&instance); //Conversion start
        while(GPIO_PinRead(GPIO, instance.gpio_rdy_port, instance.gpio_rdy_pin)==0);

        NAFE13388_SPIRead(&instance, buf, 3+status_bit); //read the result + the status bit

        ch_status0 = NAFE13388_ReadReg16B(&instance,CH_STATUS0);
        global_alarm_interrupt = NAFE13388_ReadReg16B(&instance,GLOBALALARMINTERRUPT);

        /* convert 24 bit result into int32_t format */
        adc_res = (buf[1]<<24) + (buf[2]<<16) + (buf[3]<<8);
        adc_res /= 256;

        voltage = ADC_READING_TO_VOLTAGE(adc_res)/PGA2FLOAT[pga_gain];
        PRINTF("[%d]\tADC:%.3fV\t", j, voltage);
        PRINTF("STATUS:0x%x\t",buf[0]);
        PRINTF("CHSTATUS0:0x%x\t",ch_status0);
        PRINTF("GLOBALALARM:0x%x",global_alarm_interrupt);

/*      // Block to uncomment in case of GAI
        if (global_alarm_interrupt&0x08 && global_alarm_sticky==1){ //if the PGA_OVERLOAD bit is set then reset it with CMD_CLEARDATA
        	NAFE13388_SendCmd(&instance, CMD_CLEARALARM, CMD_WR, NULL, 0);
        	PRINTF("\033[0;31m");
        	PRINTF(" -------> CMD_CLEARALARM\r\n");
        	PRINTF("\033[0m");
        	}
        else{
        	PRINTF("\r\n");
        	}
*/
        // Block to uncomment in case of Status Byte
        if (buf[0]&0x08 && status_sticky==0){ //if the PGA_OVERLOAD bit is set then reset it with CMD_CLEARDATA
        	NAFE13388_SendCmd(&instance, CMD_CLEARDATA, CMD_WR, NULL, 0);
        	PRINTF("\033[0;31m");
        	PRINTF(" -------> CMD_CLEARDATA\r\n");
        	PRINTF("\033[0m");
        	}
        else{
        	PRINTF("\r\n");
        	}


        if(j>40){
        	PRINTF("\033[0m");
            return;
        }
        j++;
    }

}

static void pinmux_init(void)
{
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio2);
    CLOCK_EnableClock(kCLOCK_Gpio3);
    
    IOCON_PinMuxSet(IOCON, SPI_MOSI_PORT, SPI_MOSI_PIN, IOCON_FUNC1 | IOCON_DIGITAL_EN | IOCON_INPFILT_OFF); /* MOSI */
    IOCON_PinMuxSet(IOCON, SPI_MISO_PORT, SPI_MISO_PIN, IOCON_FUNC1 | IOCON_DIGITAL_EN | IOCON_INPFILT_OFF); /* MISO */
    IOCON_PinMuxSet(IOCON, SPI_CLK_PORT, SPI_CLK_PIN, IOCON_FUNC1 | IOCON_DIGITAL_EN | IOCON_INPFILT_OFF); /* SCK */
    
    /* Set READY pin to be input */
    GPIO->DIRCLR[NAFE_DATA_RDY_PORT] = 1 << NAFE_DATA_RDY_PIN;
    IOCON_PinMuxSet(IOCON, NAFE_DATA_RDY_PORT, NAFE_DATA_RDY_PIN, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN | IOCON_INPFILT_OFF);
    
    /* Set SYNC to be output */
    GPIO->DIRSET[NAFE_DATA_SYNC_PORT] = 1 << NAFE_DATA_SYNC_PIN;
    IOCON_PinMuxSet(IOCON, NAFE_DATA_SYNC_PORT, NAFE_DATA_SYNC_PIN, IOCON_FUNC0  | IOCON_DIGITAL_EN | IOCON_INPFILT_OFF);
    GPIO_PinWrite(GPIO, NAFE_DATA_SYNC_PORT, NAFE_DATA_SYNC_PIN, 0);
    
    /* set CS pin to be output */
    GPIO->DIRSET[NAFE_CS_PORT] = 1 << NAFE_CS_PIN; /* SSEL */
    IOCON_PinMuxSet(IOCON, NAFE_CS_PORT, NAFE_CS_PIN, IOCON_FUNC0  | IOCON_DIGITAL_EN | IOCON_INPFILT_OFF);
    GPIO_PinWrite(GPIO, NAFE_CS_PORT, NAFE_CS_PIN, 1);
}


int main(void)
{
    char ch;
    
    /* Init board hardware. */
    /* attach 12 MHz clock to FLEXCOMM0 (debug console) */
    CLOCK_AttachClk(BOARD_DEBUG_UART_CLK_ATTACH);

    DMA_Init(DMA0);
    INPUTMUX_Init(INPUTMUX);
    BOARD_InitPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    CLOCK_AttachClk(kFRO_HF_to_FLEXCOMM9);
    RESET_PeripheralReset(kFC9_RST_SHIFT_RSTn);
    
    /* Setup DMA */
    DMA_EnableChannel(DMA0, kDmaRequestFlexcomm9Tx);
    DMA_EnableChannel(DMA0, kDmaRequestFlexcomm9Rx);
    DMA_SetChannelPriority(DMA0, kDmaRequestFlexcomm9Tx, kDMA_ChannelPriority3);
    DMA_SetChannelPriority(DMA0, kDmaRequestFlexcomm9Rx, kDMA_ChannelPriority2);
    
    pinmux_init();
    
    PINT_Init(PINT);
    PINT_PinInterruptConfig(PINT, kPINT_PinInt0, kPINT_PinIntEnableLowLevel, NULL);
    INPUTMUX_AttachSignal(INPUTMUX, kPINT_PinInt0, kINPUTMUX_GpioPort1Pin18ToPintsel);

    /* Initialize ADC ringbuffer */
    RingBuffer_Init(&s_adcRingBuf, &s_adcBuf, 1, sizeof(s_adcBuf));
    
    /* setup all resource for NAFE driver */
    instance.spi_clk_frq = SPI_CLK_FRQ;
    instance.SPIx = SPI_INSTACE;
    instance.addr = NAFE_ADDR;
    instance.gpio_cs_port = NAFE_CS_PORT;
    instance.gpio_cs_pin = NAFE_CS_PIN;
    instance.gpio_rdy_port = NAFE_DATA_RDY_PORT;
    instance.gpio_rdy_pin = NAFE_DATA_RDY_PIN;
    instance.spi_dma_tx_chl = kDmaRequestFlexcomm9Tx;
    instance.spi_dma_rx_chl = kDmaRequestFlexcomm9Rx;
    instance.pAdcRingBuf = &s_adcRingBuf;

    NAFE13388_Init(&instance, CLOCK_GetFroHfFreq());
    while(1)
    {

        NAFE13388_CmdReset(&instance);
        
        PRINTF("\r\n------------------------------\r\n");
        PRINTF("NAFE13388 Demo for LPC device.\r\n");
        PRINTF("\r\n------------------------------\r\n");
        PRINTF(
            "Select an mode\r\n"
            "\t1. Single-Channel Single Reading (SCSR)\r\n"
            "\t2. Single-Channel Continuous Reading (SCCR)\r\n"
            "\t3. Multi-Channel Multi Reading (MCMR)\r\n"
            "\t4. Multi-Channel Continuous Reading (MCCR)\r\n"
        	"\t5. Gefran test\r\n"
        	"\t6. Gefran test2\r\n");
        ch = _cli_num_sel(1, 6);
        
        switch(ch)
        {
            case 1:
                PRINTF("SCSR\r\n");
                SCSR_demo();
                break;
            case 2:
                PRINTF("SCCR\r\n");
                SCCR_demo();
                break;
            case 3:
                PRINTF("MCMR\r\n");
                MCMR_demo();
                break;
            case 4:
                PRINTF("MCCR\r\n");
                MCCR_demo();
                break;
            case 5:
				PRINTF("GEFRAN_Test\r\n");
				GEFRAN_Test();
				break;
            case 6:
				PRINTF("GEFRAN_Test2\r\n");
				GEFRAN_Test2();
				break;
        }
    
    }

}



