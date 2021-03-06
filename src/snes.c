/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   snes.c: SNES hardware control and monitoring
*/

#include <arm/NXP/LPC17xx/LPC17xx.h>
#include "bits.h"
#include "config.h"
#include "uart.h"
#include "snes.h"
#include "memory.h"
#include "fileops.h"
#include "ff.h"
#include "led.h"
#include "smc.h"
#include "timer.h"
#include "cli.h"
#include "fpga.h"
#include "fpga_spi.h"

uint32_t saveram_crc, saveram_crc_old;
extern snes_romprops_t romprops;
extern int snes_boot_configured;

volatile int reset_changed;
volatile int reset_pressed;

void prepare_reset() {
  snes_reset(1);
  delay_ms(SNES_RESET_PULSELEN_MS);
  if(romprops.ramsize_bytes && fpga_test() == FPGA_TEST_TOKEN) {
    writeled(1);
    save_sram(file_lfn, romprops.ramsize_bytes, SRAM_SAVE_ADDR);
    writeled(0);
  }
  rdyled(1);
  readled(1);
  writeled(1);
  snes_reset(0);
  while(get_snes_reset());
  snes_reset(1);
  fpga_dspx_reset(1);
  delay_ms(200);
}

void snes_init() {
  /* put reset level on reset pin */
  BITBAND(SNES_RESET_REG->FIOCLR, SNES_RESET_BIT) = 1;
  /* reset the SNES */
  snes_reset(1);
}

void snes_reset_pulse() {
  snes_reset(1);
  delay_ms(SNES_RESET_PULSELEN_MS);
  snes_reset(0);
}

/*
 * sets the SNES reset state.
 *
 *  state: put SNES in reset state when 1, release when 0
 */
void snes_reset(int state) {
  BITBAND(SNES_RESET_REG->FIODIR, SNES_RESET_BIT) = state;
}

/*
 * gets the SNES reset state.
 *
 * returns: 1 when reset, 0 when not reset
 */
uint8_t get_snes_reset() {
  return !BITBAND(SNES_RESET_REG->FIOPIN, SNES_RESET_BIT);
}

uint8_t get_snes_reset_state(void) {

  static tick_t rising_ticks;
  tick_t rising_ticks_tmp = getticks();

  static uint8_t resbutton=0, resbutton_prev=0;
  static uint8_t pushes=0, reset_short_flag=0;

  uint8_t release_flag = 0;

  uint8_t result=SNES_RESET_NONE;

  snes_reset(0); // release sd2snes-reset (just in case it was a reset detected before)
  delay_ms(SNES_RESET_PULSELEN_MS);
  resbutton = get_snes_reset();

  if(rising_ticks_tmp > rising_ticks + 22) {/* reset push counter after 230ms (time for igr's double reset) */
    pushes = 0;
    reset_short_flag = 0;
  }

  if(resbutton && !resbutton_prev) { /* push, reset tick-timer */
    pushes++;
    rising_ticks = getticks();
  } else if(!resbutton && resbutton_prev) { /* button released */
    result = SNES_RESET_SHORT;
    reset_short_flag = 1;
    release_flag = 1;
  }

  if(pushes == 2) {/* we had a second push  -> initiate long reset */
    result = SNES_RESET_LONG;
    reset_short_flag = 0;
  }

  if(resbutton && rising_ticks_tmp > rising_ticks + 99) /* a (normal) long reset detected */
    result = SNES_RESET_LONG;

  if(reset_short_flag) {
    snes_reset(1);
    if(release_flag)
      delay_ms(170);
    else
      delay_ms(SNES_RESET_PULSELEN_MS);
  }

  resbutton_prev = resbutton;
  return result;
}

/*
 * SD2SNES game loop.
 * monitors SRAM changes and other things
 */
uint32_t diffcount = 0, samecount = 0, didnotsave = 0, save_failed = 0, last_save_failed = 0;
uint8_t sram_valid = 0;
uint8_t snes_main_loop() {
  if(!romprops.ramsize_bytes)return snes_get_mcu_cmd();
  saveram_crc = calc_sram_crc(SRAM_SAVE_ADDR, romprops.ramsize_bytes);
  sram_valid = sram_reliable();
  if(crc_valid && sram_valid) {
    if(save_failed) didnotsave++;
    if(saveram_crc != saveram_crc_old) {
      if(samecount) {
        diffcount=1;
      } else {
        diffcount++;
        didnotsave++;
      }
      samecount=0;
    }
    if(saveram_crc == saveram_crc_old) {
      samecount++;
    }
    if(diffcount>=1 && samecount==5) {
      printf("SaveRAM CRC: 0x%04lx; saving %s\n", saveram_crc, file_lfn);
      writeled(1);
      save_sram(file_lfn, romprops.ramsize_bytes, SRAM_SAVE_ADDR);
      last_save_failed = save_failed;
      save_failed = file_res ? 1 : 0;
      didnotsave = save_failed ? 25 : 0;
      writeled(0);
    }
    if(didnotsave>50) {
      printf("periodic save (sram contents keep changing or previous save failed)\n");
      diffcount=0;
      writeled(1);
      save_sram(file_lfn, romprops.ramsize_bytes, SRAM_SAVE_ADDR);
      last_save_failed = save_failed;
      save_failed = file_res ? 1 : 0;
      didnotsave = save_failed ? 25 : 0;
      writeled(!last_save_failed);
    }
    saveram_crc_old = saveram_crc;
  }
  printf("crc=%lx crc_valid=%d sram_valid=%d diffcount=%ld samecount=%ld, didnotsave=%ld\n", saveram_crc, crc_valid, sram_valid, diffcount, samecount, didnotsave);

  return snes_get_mcu_cmd();
}

/*
 * SD2SNES menu loop.
 * monitors menu selection. return when selection was made.
 */
uint8_t menu_main_loop() {
  uint8_t cmd = 0;
  snes_set_mcu_cmd(0);
  while(!cmd) {
    if(!get_snes_reset()) {
      while(!sram_reliable())printf("hurr\n");
      cmd = snes_get_mcu_cmd();
    }
    if(get_snes_reset()) {
      cmd = 0;
    }
    sleep_ms(20);
    cli_entrycheck();
  }
  return cmd;
}

void get_selected_name(uint8_t* fn) {
  uint32_t cwdaddr;
  uint32_t fdaddr;
  cwdaddr = snes_get_mcu_param();
  fdaddr = snescmd_readlong(0x08);
  printf("cwd addr=%lx  fdaddr=%lx\n", cwdaddr, fdaddr);
  uint16_t count = sram_readstrn(fn, cwdaddr, 256);
  if(count && fn[count-1] != '/') {
    fn[count] = '/';
    count++;
  }
  sram_readstrn(fn+count, fdaddr+6+SRAM_MENU_ADDR, 256-count);
}

void snes_bootprint(void* msg) {
  if(!snes_boot_configured) {
    fpga_rompgm();
    sram_writebyte(0, SRAM_CMD_ADDR);
    load_bootrle(SRAM_MENU_ADDR);
    set_saveram_mask(0x1fff);
    set_rom_mask(0x3fffff);
    set_mapper(0x7);
    snes_reset(0);
    snes_boot_configured = 1;
    sleep_ms(20);
  }
  sram_writeblock(msg, SRAM_CMD_ADDR, 33);
}

void snes_menu_errmsg(int err, void* msg) {
  sram_writeblock(msg, SRAM_CMD_ADDR+1, 64);
  sram_writebyte(err, SRAM_CMD_ADDR);
}

uint8_t snes_get_last_game_index() {
  return sram_readbyte(SRAM_PARAM_ADDR);
}

uint8_t snes_get_mcu_cmd() {
  fpga_set_snescmd_addr(SNESCMD_MCU_CMD);
  return fpga_read_snescmd();
}

void snes_set_mcu_cmd(uint8_t cmd) {
  fpga_set_snescmd_addr(SNESCMD_MCU_CMD);
  fpga_write_snescmd(cmd);
}

uint8_t snes_get_snes_cmd() {
  fpga_set_snescmd_addr(SNESCMD_SNES_CMD);
  return fpga_read_snescmd();
}

void snes_set_snes_cmd(uint8_t cmd) {
  fpga_set_snescmd_addr(SNESCMD_SNES_CMD);
  fpga_write_snescmd(cmd);
}

uint32_t snes_get_mcu_param() {
  fpga_set_snescmd_addr(SNESCMD_MCU_PARAM);
  return (fpga_read_snescmd()
         | ((uint32_t)fpga_read_snescmd() << 8)
         | ((uint32_t)fpga_read_snescmd() << 16)
         | ((uint32_t)fpga_read_snescmd() << 24));
}

void snescmd_writeshort(uint16_t val, uint16_t addr) {
  fpga_set_snescmd_addr(addr);
  fpga_write_snescmd(val & 0xff);
  fpga_write_snescmd(val >> 8);
}

void snescmd_writebyte(uint8_t val, uint16_t addr) {
  fpga_set_snescmd_addr(addr);
  fpga_write_snescmd(val);
}

uint8_t snescmd_readbyte(uint16_t addr) {
  fpga_set_snescmd_addr(addr);
  return fpga_read_snescmd();
}

uint16_t snescmd_readshort(uint16_t addr) {
  uint16_t data = 0;
  fpga_set_snescmd_addr(addr);
  data = fpga_read_snescmd();
  data |= (uint16_t)fpga_read_snescmd() << 8;
  return data;
}

uint32_t snescmd_readlong(uint16_t addr) {
  uint32_t data = 0;
  fpga_set_snescmd_addr(addr);
  data = fpga_read_snescmd();
  data |= (uint32_t)fpga_read_snescmd() << 8;
  data |= (uint32_t)fpga_read_snescmd() << 16;
  data |= (uint32_t)fpga_read_snescmd() << 24;
  return data;
}

void snes_get_filepath(uint8_t *buffer, uint16_t length) {
  uint32_t path_address = snescmd_readlong(SNESCMD_MCU_PARAM);
  sram_readstrn(buffer, path_address, length-1);
printf("%s\n", buffer);
}

void snescmd_writeblock(void *buf, uint16_t addr, uint16_t size) {
  fpga_set_snescmd_addr(addr);
  while(size--) {
    fpga_write_snescmd(*(uint8_t*)buf++);
  }
}

uint64_t snescmd_gettime(void) {
  fpga_set_snescmd_addr(SNESCMD_MCU_PARAM);
  uint8_t data;
  uint64_t result = 0LL;
  /* 1st nibble is the century - 10 (binary)
     4th nibble is the month (binary)
     all other fields are BCD */
  for(int i=0; i<12; i++) {
    data = fpga_read_snescmd();
    data &= 0xf;
    switch(i) {
      case 0:
        result = (result << 4) | ((data / 10) + 1);
        result = (result << 4) | (data % 10);
        break;
      case 3:
        result = (result << 4) | ((data / 10));
        result = (result << 4) | (data % 10);
        break;
      default:
        result = (result << 4) | data;
    }
  }
  return result & 0x00ffffffffffffffLL;
}

void snescmd_prepare_nmihook() {
  uint16_t bram_src = sram_readshort(SRAM_MENU_ADDR + MENU_ADDR_BRAM_SRC);
  uint8_t bram[512];
  sram_readblock(bram, SRAM_MENU_ADDR + bram_src, 512);
  snescmd_writeblock(bram, SNESCMD_HOOKS, 12);
  snescmd_writeblock(bram+12, 0x4, 224);
  snescmd_writeshort(SNES_BUTTON_LRET, SNESCMD_NMI_RESET);
  snescmd_writeshort(SNES_BUTTON_LREX, SNESCMD_NMI_RESET_TO_MENU);
  snescmd_writeshort(SNES_BUTTON_LRSA, SNESCMD_NMI_ENABLE_CHEATS);
  snescmd_writeshort(SNES_BUTTON_LRSB, SNESCMD_NMI_DISABLE_CHEATS);
  snescmd_writeshort(SNES_BUTTON_LRSY, SNESCMD_NMI_KILL_NMIHOOK);
  snescmd_writeshort(SNES_BUTTON_LRSX, SNESCMD_NMI_TMP_KILL_NMIHOOK);
  snescmd_writebyte(0x01, SNESCMD_NMI_RUNMASK);
}
