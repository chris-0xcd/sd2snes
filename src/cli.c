/* tapplay - TAP file playback for sd2iec hardware
   Copyright (C) 2009  Ingo Korb <ingo@akana.de>

   Inspiration and low-level SD/MMC access based on code from MMC2IEC
     by Lars Pontoppidan et al., see sdcard.c|h and config.h.

   FAT filesystem access based on code from ChaN and Jim Brain, see ff.c|h.

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


   cli.c: The command line interface

*/

#include <arm/NXP/LPC17xx/LPC17xx.h>
#include <arm/bits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "config.h"
#include "diskio.h"
#include "ff.h"
#include "timer.h"
#include "uart.h"
#include "fileops.h"
#include "memory.h"
#include "snes.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "cic.h"
#include "xmodem.h"
#include "rtc.h"

#include "cli.h"

#define MAX_LINE 250

/* Variables */
static char cmdbuffer[MAX_LINE+1];
static char *curchar;

/* Word lists */
static char command_words[] =
  "cd\0reset\0dir\0ls\0test\0resume\0loadrom\0loadraw\0saveraw\0put\0d4\0vmode\0mapper\0settime\0time\0";
enum { CMD_CD = 0, CMD_RESET, CMD_DIR, CMD_LS, CMD_TEST, CMD_RESUME, CMD_LOADROM, CMD_LOADRAW, CMD_SAVERAW, CMD_PUT, CMD_D4, CMD_VMODE, CMD_MAPPER, CMD_SETTIME, CMD_TIME };

/* ------------------------------------------------------------------------- */
/*   Parse functions                                                         */
/* ------------------------------------------------------------------------- */

/* Skip spaces at curchar */
static uint8_t skip_spaces(void) {
  uint8_t res = (*curchar == ' ' || *curchar == 0);

  while (*curchar == ' ')
    curchar++;

  return res;
}

/* Parse the string in curchar for an integer with bounds [lower,upper] */
static int32_t parse_unsigned(uint32_t lower, uint32_t upper) {
  char *end;
  uint32_t result;

  if (strlen(curchar) == 1 && *curchar == '?') {
    printf("Number between %ld[0x%lx] and %ld[0x%lx] expected\n",lower,lower,upper,upper);
    return -2;
  }

  result = strtoul(curchar, &end, 10);
  if ((*end != ' ' && *end != 0) || errno != 0) {
    printf("Invalid numeric argument\n");
    return -1;
  }

  curchar = end;
  skip_spaces();

  if (result < lower || result > upper) {
    printf("Numeric argument out of range (%ld..%ld)\n",lower,upper);
    return -1;
  }

  return result;
}
/* Parse the string starting with curchar for a word in wordlist */
static int8_t parse_wordlist(char *wordlist) {
  uint8_t i, matched;
  char *cur, *ptr;
  char c;

  i = 0;
  ptr = wordlist;

  // Command list on "?"
  if (strlen(curchar) == 1 && *curchar == '?') {
    printf("Commands available: \n ");
    while (1) {
      c = *ptr++;
      if (c == 0) {
        if (*ptr == 0) {
          printf("\n");
          return -2;
        } else {
          printf("\n ");
        }
      } else
        uart_putc(c);
    }
  }

  while (1) {
    cur = curchar;
    matched = 1;
    c = *ptr;
    do {
      // If current word list character is \0: No match found
      if (c == 0) {
        printf("Unknown word: %s\n",curchar);
        return -1;
      }

      if (tolower(c) != tolower(*cur)) {
        // Check for end-of-word
        if (cur != curchar && (*cur == ' ' || *cur == 0)) {
          // Partial match found, return that
          break;
        } else {
          matched = 0;
          break;
        }
      }
      ptr++;
      cur++;
      c = *ptr;
    } while (c != 0);

    if (matched) {
      char *tmp = curchar;

      curchar = cur;
      // Return match only if whitespace or end-of-string follows
      // (avoids mismatching partial words)
      if (skip_spaces()) {
        return i;
      } else {
        printf("Unknown word: %s\n(use ? for help)\n",tmp);
        return -1;
      }
    } else {
      // Try next word in list
      i++;
      while (*ptr++ != 0) ;
    }
  }
}

/* Read a line from serial, uses cmdbuffer as storage */
static char *getline(char *prompt) {
  int i=0;
  char c;

  printf("\n%s",prompt);
  memset(cmdbuffer,0,sizeof(cmdbuffer));

  while (1) {
    c = uart_getc();
    if (c == 13)
      break;

    if (c == 27 || c == 3) {
      printf("\\\n%s",prompt);
      i = 0;
      memset(cmdbuffer,0,sizeof(cmdbuffer));
      continue;
    }

    if (c == 127 || c == 8) {
      if (i > 0) {
        i--;
        uart_putc(8);   // backspace
        uart_putc(' '); // erase character
        uart_putc(8);   // backspace
      } else
        continue;
    } else {
      if (i < sizeof(cmdbuffer)-1) {
        cmdbuffer[i++] = c;
        uart_putc(c);
      }
    }
  }
  cmdbuffer[i] = 0;
  return cmdbuffer;
}


/* ------------------------------------------------------------------------- */
/*   Command functions                                                       */
/* ------------------------------------------------------------------------- */

/* Reset */
static void cmd_reset(void) {
  /* force watchdog reset */
  LPC_WDT->WDTC = 256; // minimal timeout
  LPC_WDT->WDCLKSEL = BV(31); // internal RC, lock register
  LPC_WDT->WDMOD = BV(0) | BV(1); // enable watchdog and reset-by-watchdog
  LPC_WDT->WDFEED = 0xaa;
  LPC_WDT->WDFEED = 0x55; // initial feed to really enable WDT
}

/* Show the contents of the current directory */
static void cmd_show_directory(void) {
  FRESULT res;
  DIR dh;
  FILINFO finfo;
  uint8_t *name;

  f_getcwd((TCHAR*)file_lfn, 255);

  res = f_opendir(&dh, (TCHAR*)file_lfn);
  if (res != FR_OK) {
    printf("f_opendir failed, result %d\n",res);
    return;
  }

  finfo.lfname = (TCHAR*)file_lfn;
  finfo.lfsize = 255;

  do {
    /* Read the next entry */
    res = f_readdir(&dh, &finfo);
    if (res != FR_OK) {
      printf("f_readdir failed, result %d\n",res);
      return;
    }

    /* Abort if none was found */
    if (!finfo.fname[0])
      break;

    /* Skip volume labels */
    if (finfo.fattrib & AM_VOL)
      continue;

    /* Select between LFN and 8.3 name */
    if (finfo.lfname[0])
      name = (uint8_t*)finfo.lfname;
    else {
      name = (uint8_t*)finfo.fname;
      strlwr((char *)name);
    }

#if 0
    if (finfo.fattrib & AM_DIR) {
      printf("DIR ");
    } else {
      printf("    ");
    }
#endif

    printf("%s",name);

    /* Directory indicator (Unix-style) */
    if (finfo.fattrib & AM_DIR)
      uart_putc('/');

    printf("\n");
  } while (finfo.fname[0]);
}


static void cmd_loadrom(void) {
  uint32_t address = 0;
  snes_reset(1);
  set_mcu_ovr(1);
  load_rom((uint8_t*)curchar, address);
  set_mcu_ovr(0);
  snes_reset(0);
}

static void cmd_saveraw(void) {
  uint32_t address = parse_unsigned(0,16777216);
  uint32_t length = parse_unsigned(0,16777216);
  set_mcu_ovr(0);
  save_sram((uint8_t*)curchar, length, address);
}

static void cmd_d4(void) {
  int32_t hz;

  if(get_cic_state() != CIC_PAIR) {
    printf("not in pair mode\n");
  } else {
    hz = parse_unsigned(50,60);
    if(hz==50) {
      cic_d4(CIC_PAL);
    } else {
      cic_d4(CIC_NTSC);
    }
    printf("ok\n");
  }
}

static void cmd_vmode(void) {
  int32_t hz;
  if(get_cic_state() != CIC_PAIR) {
    printf("not in pair mode\n");
  } else {
    hz = parse_unsigned(50,60);
    if(hz==50) {
      cic_videomode(CIC_PAL);
    } else {
      cic_videomode(CIC_NTSC);
    }
    printf("ok\n");
  }
}

#if 0
  uint8_t *buf = malloc(256);

  if (buf == NULL) {
    printf("buffer allocation failed!\n");
    return;
  }

  for (int i=0;i<256;i++) {
    buf[i] = i2c_read_register(0xa0, i);
  }

  uart_trace(buf, 0, 256);
  free(buf);
#endif

void cmd_put(void) {
  if(*curchar != 0) {
    file_open((uint8_t*)curchar, FA_CREATE_ALWAYS | FA_WRITE);
    if(file_res) {
      printf("FAIL: error opening file %s\n", curchar);
    } else {
      printf("OK, start xmodem transfer now.\n");
      xmodem_rxfile(&file_handle);
    }
    file_close();
  } else {
    printf("Usage: put <filename>\n");
  }
}

void cmd_mapper(void) {
  int32_t mapper;
  mapper = parse_unsigned(0,7);
  set_mapper((uint8_t)mapper & 0x7);
  printf("mapper set to %ld\n", mapper);
}

void cmd_settime(void) {
  struct tm time;
  if(strlen(curchar) != 4+2+2 + 2+2+2) {
    printf("invalid time format (need YYYYMMDDhhmmss)\n");
  } else {
    time.tm_sec = atoi(curchar+4+2+2+2+2);
    curchar[4+2+2+2+2] = 0;
    time.tm_min = atoi(curchar+4+2+2+2);
    curchar[4+2+2+2] = 0;
    time.tm_hour = atoi(curchar+4+2+2);
    curchar[4+2+2] = 0;
    time.tm_mday = atoi(curchar+4+2);
    curchar[4+2] = 0;
    time.tm_mon = atoi(curchar+4);
    curchar[4] = 0;
    time.tm_year = atoi(curchar) - 1900;
    set_rtc(&time);
  }
}

void cmd_time(void) {
  struct tm time;
  read_rtc(&time);
  printf("%04d-%02d-%02d %02d:%02d:%02d\n", time.tm_year+1900, time.tm_mon,
    time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
}

/* ------------------------------------------------------------------------- */
/*   CLI interface functions                                                 */
/* ------------------------------------------------------------------------- */

void cli_init(void) {
}

void cli_entrycheck() {
  if(uart_gotc() && uart_getc() == 27) {
    printf("*** BREAK\n");
    cli_loop();
  }
}

void cli_loop(void) {
  while (1) {
    curchar = getline(">");
    printf("\n");

    /* Process medium changes before executing the command */
    if (disk_state != DISK_OK && disk_state != DISK_REMOVED) {
      FRESULT res;

      printf("Medium changed... ");
      res = f_mount(0,&fatfs);
      if (res != FR_OK) {
        printf("Failed to mount new medium, result %d\n",res);
      } else {
        printf("Ok\n");
      }

    }

    /* Remove whitespace */
    while (*curchar == ' ') curchar++;
    while (strlen(curchar) > 0 && curchar[strlen(curchar)-1] == ' ')
      curchar[strlen(curchar)-1] = 0;

    /* Ignore empty lines */
    if (strlen(curchar) == 0)
      continue;

    /* Parse command */
    int8_t command = parse_wordlist(command_words);
    if (command < 0)
      continue;


    FRESULT res;
    switch (command) {
    case CMD_CD:
#if _FS_RPATH
      if (strlen(curchar) == 0) {
        f_getcwd((TCHAR*)file_lfn, 255);
        printf("%s\n",file_lfn);
        break;
      }

      res = f_chdir((const TCHAR *)curchar);
      if (res != FR_OK) {
        printf("chdir %s failed with result %d\n",curchar,res);
      } else {
        printf("Ok.\n");
      }
#else
      printf("cd not supported.\n");
      res;
#endif
    break;
    case CMD_RESET:
      cmd_reset();
      break;

    case CMD_DIR:
    case CMD_LS:
      cmd_show_directory();
      break;

    case CMD_RESUME:
      return;
      break;

    case CMD_LOADROM:
      cmd_loadrom();
      break;

    case CMD_SAVERAW:
      cmd_saveraw();
      break;

    case CMD_D4:
      cmd_d4();
      break;

    case CMD_VMODE:
      cmd_vmode();
      break;

    case CMD_PUT:
      cmd_put();
      break;

    case CMD_MAPPER:
      cmd_mapper();
      break;

    case CMD_SETTIME:
      cmd_settime();
      break;

    case CMD_TIME:
      cmd_time();
      break;
    }
  }
}