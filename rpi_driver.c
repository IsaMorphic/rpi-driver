// Test of an R-2R DAC on the Raspberry Pi using SMI
// For detailed description, see https://iosoft.blog
//
// Copyright (c) 2020 Jeremy P Bentham
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// v0.01 JPB 16/7/20  Derived from rpi_smi_test v0.20
//                    Tidied up for github

#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include "rpi_dma_utils.h"

#define DISP_ZEROS      0

#define SMI_A0_PIN      5
#define SMI_SOE_PIN     6
#define SMI_SWE_PIN     7

#define DAC_D0_PIN      8
#define DAC_NPINS       8

#define NSAMPLES        795
#define NBUFFERS        525
#define NFRAMES         3

#define SMI_BASE    (PHYS_REG_BASE + 0x600000)
#define SMI_CS      0x00    // Control & status
#define SMI_L       0x04    // Transfer length
#define SMI_A       0x08    // Address
#define SMI_D       0x0c    // Data
#define SMI_DSR0    0x10    // Read settings device 0
#define SMI_DSW0    0x14    // Write settings device 0
#define SMI_DSR1    0x18    // Read settings device 1
#define SMI_DSW1    0x1c    // Write settings device 1
#define SMI_DSR2    0x20    // Read settings device 2
#define SMI_DSW2    0x24    // Write settings device 2
#define SMI_DSR3    0x28    // Read settings device 3
#define SMI_DSW3    0x2c    // Write settings device 3
#define SMI_DMC     0x30    // DMA control
#define SMI_DCS     0x34    // Direct control/status
#define SMI_DCA     0x38    // Direct address
#define SMI_DCD     0x3c    // Direct data
#define SMI_FD      0x40    // FIFO debug
#define SMI_REGLEN  (SMI_FD * 4)

#define SMI_DEV     0
#define SMI_8_BITS  0
#define SMI_16_BITS 1
#define SMI_18_BITS 2
#define SMI_9_BITS  3

#define DMA_SMI_DREQ 4

char *smi_regstrs[] = {
    "CS","LEN","A","D","DSR0","DSW0","DSR1","DSW1",
    "DSR2","DSW2","DSR3","DSW3","DMC","DCS","DCA","DCD",""
};

#define STRS(x)     STRS_(x) ","
#define STRS_(...)  #__VA_ARGS__
#define REG_DEF(name, fields) typedef union {struct {volatile uint32_t fields;}; volatile uint32_t value;} name

#define SMI_CS_FIELDS \
    enable:1, done:1, active:1, start:1, clear:1, write:1, _x1:2,\
    teen:1, intd:1, intt:1, intr:1, pvmode:1, seterr:1, pxldat:1, edreq:1,\
    _x2:8, _x3:1, aferr:1, txw:1, rxr:1, txd:1, rxd:1, txe:1, rxf:1
REG_DEF(SMI_CS_REG, SMI_CS_FIELDS);
#define SMI_L_FIELDS \
    len:32
REG_DEF(SMI_L_REG, SMI_L_FIELDS);
#define SMI_A_FIELDS \
    addr:6, _x1:2, dev:2
REG_DEF(SMI_A_REG, SMI_A_FIELDS);
#define SMI_D_FIELDS \
    data:32
REG_DEF(SMI_D_REG, SMI_D_FIELDS);
#define SMI_DMC_FIELDS \
    reqw:6, reqr:6, panicw:6, panicr:6, dmap:1, _x1:3, dmaen:1
REG_DEF(SMI_DMC_REG, SMI_DMC_FIELDS);
#define SMI_DSR_FIELDS \
    rstrobe:7, rdreq:1, rpace:7, rpaceall:1, rhold:6, fsetup:1, mode68:1, rsetup:6, rwidth:2
REG_DEF(SMI_DSR_REG, SMI_DSR_FIELDS);
#define SMI_DSW_FIELDS \
    wstrobe:7, wdreq:1, wpace:7, wpaceall:1, whold:6, wswap:1, wformat:1, wsetup:6, wwidth:2
REG_DEF(SMI_DSW_REG, SMI_DSW_FIELDS);
#define SMI_DCS_FIELDS \
    enable:1, start:1, done:1, write:1
REG_DEF(SMI_DCS_REG, SMI_DCS_FIELDS);
#define SMI_DCA_FIELDS \
    addr:6, _x1:2, dev:2
REG_DEF(SMI_DCA_REG, SMI_DCA_FIELDS);
#define SMI_DCD_FIELDS \
    data:32
REG_DEF(SMI_DCD_REG, SMI_DCD_FIELDS);
#define SMI_FLVL_FIELDS \
    fcnt:6, _x1:2, flvl:6
REG_DEF(SMI_FLVL_REG, SMI_FLVL_FIELDS);

char *smi_cs_regstrs = STRS(SMI_CS_FIELDS);

#define CLK_SMI_CTL     0xb0
#define CLK_SMI_DIV     0xb4

extern MEM_MAP gpio_regs, dma_regs;
MEM_MAP clk_regs, smi_regs;

MEM_MAP vc_mem[NBUFFERS];

volatile SMI_CS_REG  *smi_cs;
volatile SMI_L_REG   *smi_l;
volatile SMI_A_REG   *smi_a;
volatile SMI_D_REG   *smi_d;
volatile SMI_DMC_REG *smi_dmc;
volatile SMI_DSR_REG *smi_dsr;
volatile SMI_DSW_REG *smi_dsw;
volatile SMI_DCS_REG *smi_dcs;
volatile SMI_DCA_REG *smi_dca;
volatile SMI_DCD_REG *smi_dcd;

#define TX_SAMPLE_SIZE  2       // Number of raw bytes per sample
#define VC_MEM_SIZE(ns) (PAGE_SIZE + ((ns)+4)*TX_SAMPLE_SIZE)

uint8_t sample_buff[NSAMPLES * NBUFFERS];
size_t buff_next(FILE *file_ptr);

void dac_init(void);
void dac_start(void);
void dac_next(void);

void map_devices(void);
void fail(char *s);
void terminate(int sig);
void init_smi(int width, int ns, int setup, int hold, int strobe);
void disp_smi(void);
void disp_reg_fields(char *regstrs, char *name, uint32_t val);


void* func(void* arg) 
{ 
    // detach the current thread 
    // from the calling thread 
    pthread_detach(pthread_self()); 
  
    buff_next(stdin);
  
    // exit the current thread 
    pthread_exit(NULL); 
} 

int main(int argc, char *argv[])
{
    int frame_num, parity_flag;
    struct timespec deadline;
    
    pthread_t ptid; 

    signal(SIGINT, terminate);

    map_devices();
    init_smi(0, 6, 5, 10, 5);

    gpio_mode(SMI_SOE_PIN, GPIO_ALT1);
    gpio_mode(SMI_SWE_PIN, GPIO_ALT1);

    smi_cs->clear = 1;

    FILE* file_ptr;
    size_t read_count;
    file_ptr = stdin;

    dac_init();

    read_count = buff_next(file_ptr);
    dac_next();
    
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    do
    {
        dac_start();

        for(frame_num = 0; frame_num < NFRAMES; frame_num++)
        {
            deadline.tv_nsec += (NSAMPLES - parity_flag) * NBUFFERS * 78;
            if(deadline.tv_nsec >= 1000000000) 
            {  
                deadline.tv_nsec -= 1000000000;
                deadline.tv_sec++;
            }
  
            // Creating a new thread 
            pthread_create(&ptid, NULL, &func, NULL); 
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
            pthread_join(ptid, NULL);

            parity_flag = !parity_flag;
        }
    } while(!feof(file_ptr));

    terminate(0);
    return(0);
}

void dac_init(void)
{
    int i;

    smi_cs->clear = smi_cs->seterr = smi_cs->aferr = 1;
    smi_dcs->enable = 1;

    for(i = 0; i < DAC_NPINS; i++)
        gpio_mode(DAC_D0_PIN + i, GPIO_ALT1);

    for(i = 0; i < NBUFFERS; i++)
        map_uncached_mem(&vc_mem[i], VC_MEM_SIZE(NSAMPLES));

    smi_dsr->rwidth = SMI_8_BITS;
    smi_l->len = NSAMPLES * NBUFFERS * (NFRAMES + 1) * TX_SAMPLE_SIZE;
    smi_dmc->dmaen = 1;
    smi_cs->clear = 1;
    smi_cs->write = 1;
    smi_cs->enable = 1;

    enable_dma(DMA_CHAN_A);
    for(int i = 0; i < NBUFFERS; i++)
    {
        MEM_MAP *mp = &vc_mem[i];
        DMA_CB *cbs = mp->virt;
        uint8_t *txdata = (uint8_t *)(cbs+1);

        cbs[0].ti = DMA_DEST_DREQ | (DMA_SMI_DREQ << 16) | DMA_CB_SRCE_INC;
        cbs[0].tfr_len = NSAMPLES * TX_SAMPLE_SIZE;
        cbs[0].srce_ad = MEM_BUS_ADDR(mp, txdata);
        cbs[0].dest_ad = REG_BUS_ADDR(smi_regs, SMI_D);
        cbs[0].next_cb = i == NBUFFERS - 1 ? MEM_BUS_ADDR((&vc_mem[0]), (&vc_mem[0])->virt) : MEM_BUS_ADDR((&vc_mem[i + 1]), (&vc_mem[i + 1])->virt);
    }
}

size_t buff_next(FILE* file_ptr)
{
    size_t read_count = 0;
    while ((read_count += fread(sample_buff + read_count, sizeof(uint8_t), NSAMPLES * NBUFFERS - read_count, file_ptr)) < NSAMPLES * NBUFFERS) 
    {
        if(feof(file_ptr)) return read_count;
    }

    return read_count;
}

void dac_next(void)
{
    for(int i = 0; i < NBUFFERS; i++)
    {
        MEM_MAP *mp = &vc_mem[i];
        DMA_CB *cbs = mp->virt;
        uint16_t *txdata = (uint16_t *)(cbs+1);

        for(int j = 0; j < NSAMPLES; j++)
        {
            txdata[j] = (uint16_t)sample_buff[i * NSAMPLES + j];
        }
    }
}

void dac_start(void)
{
    start_dma(&vc_mem[0], DMA_CHAN_A, (DMA_CB*)(&vc_mem[0])->virt, 0);
    smi_cs->start = 1;
}

// Map GPIO, DMA and SMI registers into virtual mem (user space)
// If any of these fail, program will be terminated
void map_devices(void)
{
    map_periph(&gpio_regs, (void *)GPIO_BASE, PAGE_SIZE);
    map_periph(&dma_regs, (void *)DMA_BASE, PAGE_SIZE);
    map_periph(&clk_regs, (void *)CLK_BASE, PAGE_SIZE);
    map_periph(&smi_regs, (void *)SMI_BASE, PAGE_SIZE);
    memset(smi_regs.virt, 0, SMI_REGLEN);
}

// Catastrophic failure in initial setup
void fail(char *s)
{
    printf(s);
    terminate(0);
}

// Free memory segments and exit
void terminate(int sig)
{
    int i;

    printf("Closing\n");
    disp_reg_fields(smi_cs_regstrs, "CS", *REG32(smi_regs, SMI_CS));
    for (i=0; i<DAC_NPINS; i++)
        gpio_mode(DAC_D0_PIN+i, GPIO_IN);
    if (smi_regs.virt)
        *REG32(smi_regs, SMI_CS) = 0;
    stop_dma(DMA_CHAN_A);
    for(i=0; i<NBUFFERS; i++)
        unmap_periph_mem(&vc_mem[i]);
    unmap_periph_mem(&smi_regs);
    unmap_periph_mem(&dma_regs);
    unmap_periph_mem(&gpio_regs);

    pthread_exit(NULL); 
    exit(0);
}

// Initialise SMI, given data width, time step, and setup/hold/strobe counts
// Step value is in nanoseconds: even numbers, 2 to 30
void init_smi(int width, int ns, int setup, int strobe, int hold)
{
    int divi = ns / 2;

    smi_cs  = (SMI_CS_REG *) REG32(smi_regs, SMI_CS);
    smi_l   = (SMI_L_REG *)  REG32(smi_regs, SMI_L);
    smi_a   = (SMI_A_REG *)  REG32(smi_regs, SMI_A);
    smi_d   = (SMI_D_REG *)  REG32(smi_regs, SMI_D);
    smi_dmc = (SMI_DMC_REG *)REG32(smi_regs, SMI_DMC);
    smi_dsr = (SMI_DSR_REG *)REG32(smi_regs, SMI_DSR0);
    smi_dsw = (SMI_DSW_REG *)REG32(smi_regs, SMI_DSW0);
    smi_dcs = (SMI_DCS_REG *)REG32(smi_regs, SMI_DCS);
    smi_dca = (SMI_DCA_REG *)REG32(smi_regs, SMI_DCA);
    smi_dcd = (SMI_DCD_REG *)REG32(smi_regs, SMI_DCD);
    smi_cs->value = smi_l->value = smi_a->value = 0;
    smi_dsr->value = smi_dsw->value = smi_dcs->value = smi_dca->value = 0;
    if (*REG32(clk_regs, CLK_SMI_DIV) != divi << 12)
    {
        *REG32(clk_regs, CLK_SMI_CTL) = CLK_PASSWD | (1 << 5);
        usleep(10);
        while (*REG32(clk_regs, CLK_SMI_CTL) & (1 << 7)) ;
        usleep(10);
        *REG32(clk_regs, CLK_SMI_DIV) = CLK_PASSWD | (divi << 12);
        usleep(10);
        *REG32(clk_regs, CLK_SMI_CTL) = CLK_PASSWD | 6 | (1 << 4);
        usleep(10);
        while ((*REG32(clk_regs, CLK_SMI_CTL) & (1 << 7)) == 0) ;
        usleep(100);
    }

    if (smi_cs->seterr)
        smi_cs->seterr = 1;

    smi_dsr->rsetup = smi_dsw->wsetup = setup;
    smi_dsr->rstrobe = smi_dsw->wstrobe = strobe;
    smi_dsr->rhold = smi_dsw->whold = hold;
    smi_dmc->panicr = smi_dmc->panicw = 8;
    smi_dmc->reqr = smi_dmc->reqw = 2;
    smi_dsr->rwidth = smi_dsw->wwidth = width;
}

// Display SMI registers
void disp_smi(void)
{
    volatile uint32_t *p=REG32(smi_regs, SMI_CS);
    int i=0;

    while (smi_regstrs[i][0])
    {
        printf("%4s=%08X ", smi_regstrs[i++], *p++);
        if (i%8==0 || smi_regstrs[i][0]==0)
            printf("\n");
    }
}

// Display bit values in register
void disp_reg_fields(char *regstrs, char *name, uint32_t val)
{
    char *p=regstrs, *q, *r=regstrs;
    uint32_t nbits, v;

    printf("%s %08X", name, val);
    while ((q = strchr(p, ':')) != 0)
    {
        p = q + 1;
        nbits = 0;
        while (*p>='0' && *p<='9')
            nbits = nbits * 10 + *p++ - '0';
        v = val & ((1 << nbits) - 1);
        val >>= nbits;
        if ((v || DISP_ZEROS) && *r!='_')
            printf(" %.*s=%X", q-r, r, v);
        while (*p==',' || *p==' ')
            p = r = p + 1;
    }
    printf("\n");
}

// EOF
