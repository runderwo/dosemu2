/*
 * All modifications in this file to the original code are
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

#include <string.h>
#include "builtins.h"
#include "msetenv.h"

#include "blaster.h"

#include "emu.h"
#include "sound.h"

int parse_error(char *loc) {
	com_fprintf(com_stderr, "Error parsing command line at %s\n", loc);
	return 1;
}

int blaster_main(int argc, char **argv) {

	if (config.sound) {
		char blaster[255];
		boolean print_help = FALSE;

		int i;
		for (i = 1; i < argc; i++) {
			char **p = &argv[i];
			int intarg;
			if ((*p)[0] == '/') {
				char *arg = &(*p)[2];
				switch((*p)[1]) {
					case 'A': case 'a':
						if (sscanf(arg, "%x", &intarg) < 1
								|| (intarg != 0x210 && intarg != 0x220 && intarg != 0x240 &&
									  intarg != 0x260 && intarg != 0x280))
							return parse_error(*p);
						config.sb_base = intarg;
						break;
					case 'I': case 'i':
						if (sscanf(arg, "%d", &intarg) < 1
								|| (intarg != 3 && intarg != 5 && intarg != 7 && intarg != 10 && intarg != 11))
							return parse_error(*p);
						config.sb_irq = intarg;
						break;
					case 'D': case 'd':
						if (sscanf(arg, "%d", &intarg) < 1
								|| (intarg != 0 && intarg != 1 && intarg != 3))
							return parse_error(*p);
						config.sb_dma = intarg;
						break;
					case 'T': case 't':
						if (sscanf(arg, "%d", &intarg) < 1
								|| (intarg != 1 && intarg != 2 && intarg != 3 && intarg != 4 && intarg != 6))
							return parse_error(*p);
						config.sb_type = intarg;
						break;
					case 'H': case 'h':
						if (sscanf(arg, "%d", &intarg) < 1
								|| (intarg != 5 && intarg != 6 && intarg != 7))
							return parse_error(*p);
						config.sb_hdma = intarg;
						break;
					case 'P': case 'p':
						if (sscanf(arg, "%x", &intarg) < 1
								|| (intarg != 0x300 && intarg != 0x330))
							return parse_error(*p);
						config.mpu401_base = intarg;
						break;
					case 'Q': case 'q':
						if (sscanf(arg, "%d", &intarg) < 1
								|| !(intarg >= 2 && intarg <= 9 && intarg != 8))
							return parse_error(*p);
						config.mpu401_irq = intarg;
						break;
					case '?':
					default:
						print_help = TRUE;
						return 0;
						break;
				}
			}
		}

		com_printf("Sound on: ");

		char *type;
		switch (config.sb_type) {
			case 1:
				type = "1.0/1.5"; break;
			case 2:
				type = "Pro"; break;
			case 3:
				type = "2.0"; break;
			case 4:
				type = "Pro 2.0"; break;
			case 5:
				type = "MCV"; break;
			case 6:
				type = "16"; break;
			default:
				type = "(unknown)"; break;
		}

		com_printf("SB %s @ (A)ddr 0x%x-0x%x, (I)RQ=%d, (D)MA8=%d", type, config.sb_base,
				config.sb_base+15, config.sb_irq, config.sb_dma);
		if (config.sb_hdma) {
			com_printf(", (H)DMA16=%d", config.sb_hdma);
		}
		com_printf(".\n");

		if (config.mpu401_base) {
			com_printf("MPU-401 (P)ort at 0x%x-0x%x",
					config.mpu401_base, config.mpu401_base+1);
			if (config.mpu401_irq) {
				com_printf(", IR(Q)=%d", config.mpu401_irq);
			}
			com_printf(".\n");
		}

		snprintf(blaster, sizeof(blaster), "A%x I%d D%d T%d", config.sb_base,
				config.sb_irq, config.sb_dma, config.sb_type);

		char tmpbuf[30];
		if (config.sb_type == 6) {
			if (config.sb_hdma) {
				snprintf(tmpbuf, sizeof(tmpbuf), " H%d",
						config.sb_hdma);
				tmpbuf[sizeof(tmpbuf)-1] = '\0';
				strncat(blaster, tmpbuf, 10);
			}
			if (config.mpu401_base) {
				snprintf(tmpbuf, sizeof(tmpbuf), " P%x",
						config.mpu401_base);
				tmpbuf[sizeof(tmpbuf)-1] = '\0';
				strncat(blaster, tmpbuf, 10);
			}
		}

		if (msetenv("BLASTER", blaster) == -1) {
			com_printf("Environment too small for BLASTER! "
			    "(needed %zu bytes)\n", strlen(blaster));
		}

		snprintf(blaster, sizeof(blaster), "SYNTH:%d MAP:%c MODE:%d",
		    config.mpu401_base ? 2 : 1, 'E', 0);

		if (msetenv("MIDI", blaster) == -1) {
			com_printf("Environment too small for MIDI! (needed %zu bytes)\n", strlen(blaster));
		}

		char *sb16inst = "D:\\SB16";
		if (msetenv("SOUND", sb16inst) == -1) {
			com_printf("Environment too small for SOUND! (needed %zu bytes)\n", strlen(sb16inst));
		}

		if (print_help)
			com_printf(
				"\nTo change sound resources, use /A, /I, /D, etc.  Example:\n"
				"BLASTER /A240 /Q3 puts SB base at 0x240 and MPU401 IRQ at 3.\n"
			);
	}
	else {
		com_printf("Sound not enabled in config!\n");
	}
	return 0;
}

