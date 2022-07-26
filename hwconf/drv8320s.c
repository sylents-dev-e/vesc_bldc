/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "conf_general.h"
#ifdef HW_HAS_DRV8320S

#include "drv8320s.h"
#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "utils_math.h"
#include "terminal.h"
#include "commands.h"
#include <string.h>
#include <stdio.h>

// Private functions
static uint16_t spi_exchange(uint16_t x);
static void spi_transfer(uint16_t *in_buf, const uint16_t *out_buf, int length);
static void spi_begin(void);
static void spi_end(void);
static void spi_delay(void);
static void terminal_read_reg(int argc, const char **argv);
static void terminal_write_reg(int argc, const char **argv);
static void terminal_set_oc_adj(int argc, const char **argv);
static void terminal_print_faults(int argc, const char **argv);

// Private variables
static char m_fault_print_buffer[120];
static mutex_t m_spi_mutex;

void drv8320s_init(void) {
	chMtxObjectInit(&m_spi_mutex);

	// DRV8320S SPI
	palSetPadMode(DRV8320S_MISO_GPIO, DRV8320S_MISO_PIN, PAL_MODE_INPUT_PULLUP);
	palSetPadMode(DRV8320S_SCK_GPIO, DRV8320S_SCK_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	palSetPadMode(DRV8320S_CS_GPIO, DRV8320S_CS_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	palSetPadMode(DRV8320S_MOSI_GPIO, DRV8320S_MOSI_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	palSetPad(DRV8320S_MOSI_GPIO, DRV8320S_MOSI_PIN);

	chThdSleepMilliseconds(100);

	// Disable OC
	drv8320s_write_reg(5, 0x04C0);
	drv8320s_write_reg(5, 0x04C0);

	terminal_register_command_callback(
			"drv8320s_read_reg",
			"Read a register from the DRV8320S and print it.",
			"[reg]",
			terminal_read_reg);

	terminal_register_command_callback(
			"drv8320s_write_reg",
			"Write to a DRV8320S register.",
			"[reg] [hexvalue]",
			terminal_write_reg);

	terminal_register_command_callback(
			"drv8320s_set_oc_adj",
			"Set the DRV8320S OC ADJ register.",
			"[value]",
			terminal_set_oc_adj);

	terminal_register_command_callback(
			"drv8320s_print_faults",
			"Print all current DRV8320S faults.",
			0,
			terminal_print_faults);
}

/**
 * Set the threshold of the over current protection of the DRV8320S. It works by measuring
 * the voltage drop across drain-source of the MOSFETs and activates when it is higher than
 * a set value. Notice that this current limit is not very accurate.
 *
 * @param val
 * The value to use. Range [0 15]. A lower value corresponds to a lower current limit. See
 * the drv8320 datasheet for how to convert these values to currents.
 */
void drv8320s_set_oc_adj(int val) {
	// Match with the drv8301 levels
	val >>= 1;
	int reg = drv8320s_read_reg(5);
	reg &= 0xFFF0;
	reg |= (val & 0xF);
	drv8320s_write_reg(5, reg);
}

/**
 * Set the over current protection mode of the DRV8320S.
 *
 * @param mode
 * The over current protection mode.
 */
void drv8320s_set_oc_mode(drv8301_oc_mode mode) {
	// Match with the drv8301 modes
	if (mode == DRV8301_OC_LATCH_SHUTDOWN) {
		mode = DRV8301_OC_LIMIT;
	} else if (mode == DRV8301_OC_LIMIT) {
		mode = DRV8301_OC_LATCH_SHUTDOWN;
	}
	int reg = drv8320s_read_reg(5);
	reg &= 0xFF3F;
	reg |= (mode & 0x03) << 6;
	drv8320s_write_reg(5, reg);
}

/**
 * Read the fault codes of the DRV8320S.
 *
 * @return
 * The fault codes, where the bits represent the following:
 * b0: FETLC_OC
 * b1: FETHC_OC
 * b2: FETLB_OC
 * b3: FETHB_OC
 * b4: FETLA_OC
 * b5: FETHA_OC
 * b6: OTW
 * b7: OTSD
 * b8: PVDD_UV
 * b9: GVDD_UV
 * b10: FAULT
 * b11: GVDD_OV
 *
 */
unsigned long drv8320s_read_faults(void) {
	unsigned long r0 = drv8320s_read_reg(0);
	int r1 = drv8320s_read_reg(1);
	return r0 | (r1 << 16);
}

/**
 * Reset all latched faults.
 */
void drv8320s_reset_faults(void) {
	int reg = drv8320s_read_reg(2);
	reg |= 1;
	drv8320s_write_reg(2, reg);
}

char* drv8320s_faults_to_string(unsigned long faults) {
	if (faults == 0) {
		strcpy(m_fault_print_buffer, "No DRV8320S faults");
	} else {
		strcpy(m_fault_print_buffer, "|");

		if (faults & DRV8320S_FAULT_FET_LC_OC) {
			strcat(m_fault_print_buffer, " FETLC_OC |");
		}

		if (faults & DRV8320S_FAULT_FET_HC_OC) {
			strcat(m_fault_print_buffer, " FETHC_OC |");
		}

		if (faults & DRV8320S_FAULT_FET_LB_OC) {
			strcat(m_fault_print_buffer, " FETLB_OC |");
		}

		if (faults & DRV8320S_FAULT_FET_HB_OC) {
			strcat(m_fault_print_buffer, " FETHB_OC |");
		}

		if (faults & DRV8320S_FAULT_FET_LA_OC) {
			strcat(m_fault_print_buffer, " FETLA_OC |");
		}

		if (faults & DRV8320S_FAULT_FET_HA_OC) {
			strcat(m_fault_print_buffer, " FETHA_OC |");
		}

		if (faults & DRV8320S_FAULT_OTSD) {
			strcat(m_fault_print_buffer, " OTSD |");
		}

		if (faults & DRV8320S_FAULT_UVLO) {
			strcat(m_fault_print_buffer, " UVLO |");
		}

		if (faults & DRV8320S_FAULT_GDF) {
			strcat(m_fault_print_buffer, " GDF |");
		}

		if (faults & DRV8320S_FAULT_VDS_OCP) {
			strcat(m_fault_print_buffer, " VDS OCP |");
		}

		if (faults & DRV8320S_FAULT_FAULT) {
			strcat(m_fault_print_buffer, " FAULT |");
		}

		if (faults & DRV8320S_FAULT_VGS_LC) {
			strcat(m_fault_print_buffer, " FETLC VGS |");
		}

		if (faults & DRV8320S_FAULT_VGS_HC) {
			strcat(m_fault_print_buffer, " FETHC VGS |");
		}

		if (faults & DRV8320S_FAULT_VGS_LB) {
			strcat(m_fault_print_buffer, " FETLB VGS |");
		}

		if (faults & DRV8320S_FAULT_VGS_HB) {
			strcat(m_fault_print_buffer, " FETHB VGS |");
		}

		if (faults & DRV8320S_FAULT_VGS_LA) {
			strcat(m_fault_print_buffer, " FETLA VGS |");
		}

		if (faults & DRV8320S_FAULT_VGS_HA) {
			strcat(m_fault_print_buffer, " FETHA VGS |");
		}

		if (faults & DRV8320S_FAULT_CPUV) {
			strcat(m_fault_print_buffer, " CPU V |");
		}

		if (faults & DRV8320S_FAULT_OTW) {
			strcat(m_fault_print_buffer, " OTW |");
		}

		if (faults & DRV8320S_FAULT_SC_OC) {
			strcat(m_fault_print_buffer, " AMP C OC |");
		}

		if (faults & DRV8320S_FAULT_SB_OC) {
			strcat(m_fault_print_buffer, " AMP B OC |");
		}

		if (faults & DRV8320S_FAULT_SA_OC) {
			strcat(m_fault_print_buffer, " AMP A OC |");
		}
	}

	return m_fault_print_buffer;
}

unsigned int drv8320s_read_reg(int reg) {
	uint16_t out = 0;
	out |= (1 << 15);
	out |= (reg & 0x0F) << 11;
	out |= 0x807F;

	chMtxLock(&m_spi_mutex);

	if (reg != 0) {
		spi_begin();
		spi_exchange(out);
		spi_end();
	}

	spi_begin();
	uint16_t res = spi_exchange(out);
	spi_end();

	chMtxUnlock(&m_spi_mutex);

	return res;
}

void drv8320s_write_reg(int reg, int data) {
	uint16_t out = 0;
	out |= (reg & 0x0F) << 11;
	out |= data & 0x7FF;

	chMtxLock(&m_spi_mutex);
	spi_begin();
	spi_exchange(out);
	spi_end();
	chMtxUnlock(&m_spi_mutex);
}

// Software SPI
static uint16_t spi_exchange(uint16_t x) {
	uint16_t rx;
	spi_transfer(&rx, &x, 1);
	return rx;
}

static void spi_transfer(uint16_t *in_buf, const uint16_t *out_buf, int length) {
	for (int i = 0;i < length;i++) {
		uint16_t send = out_buf ? out_buf[i] : 0xFFFF;
		uint16_t receive = 0;

		for (int bit = 0;bit < 16;bit++) {
			palWritePad(DRV8320S_MOSI_GPIO, DRV8320S_MOSI_PIN, send >> 15);
			send <<= 1;

			palSetPad(DRV8320S_SCK_GPIO, DRV8320S_SCK_PIN);
			spi_delay();

			palClearPad(DRV8320S_SCK_GPIO, DRV8320S_SCK_PIN);

			int r1, r2, r3;
			r1 = palReadPad(DRV8320S_MISO_GPIO, DRV8320S_MISO_PIN);
			__NOP();
			r2 = palReadPad(DRV8320S_MISO_GPIO, DRV8320S_MISO_PIN);
			__NOP();
			r3 = palReadPad(DRV8320S_MISO_GPIO, DRV8320S_MISO_PIN);

			receive <<= 1;
			if (utils_middle_of_3_int(r1, r2, r3)) {
				receive |= 1;
			}

			spi_delay();
		}

		if (in_buf) {
			in_buf[i] = receive;
		}
	}
}

static void spi_begin(void) {
	palClearPad(DRV8320S_CS_GPIO, DRV8320S_CS_PIN);
}

static void spi_end(void) {
	palSetPad(DRV8320S_CS_GPIO, DRV8320S_CS_PIN);
}

static void spi_delay(void) {
	for (volatile int i = 0;i < 10;i++) {
		__NOP();
	}
}

static void terminal_read_reg(int argc, const char **argv) {
	if (argc == 2) {
		int reg = -1;
		sscanf(argv[1], "%d", &reg);

		if (reg >= 0) {
			unsigned int res = drv8320s_read_reg(reg);
			char bl[9];
			char bh[9];

			utils_byte_to_binary((res >> 8) & 0xFF, bh);
			utils_byte_to_binary(res & 0xFF, bl);

			commands_printf("Reg 0x%02x: %s %s (0x%04x)\n", reg, bh, bl, res);
		} else {
			commands_printf("Invalid argument(s).\n");
		}
	} else {
		commands_printf("This command requires one argument.\n");
	}
}

static void terminal_write_reg(int argc, const char **argv) {
	if (argc == 3) {
		int reg = -1;
		int val = -1;
		sscanf(argv[1], "%d", &reg);
		sscanf(argv[2], "%x", &val);

		if (reg >= 0 && val >= 0) {
			drv8320s_write_reg(reg, val);
			unsigned int res = drv8320s_read_reg(reg);
			char bl[9];
			char bh[9];

			utils_byte_to_binary((res >> 8) & 0xFF, bh);
			utils_byte_to_binary(res & 0xFF, bl);

			commands_printf("New reg value 0x%02x: %s %s (0x%04x)\n", reg, bh, bl, res);
		} else {
			commands_printf("Invalid argument(s).\n");
		}
	} else {
		commands_printf("This command requires two arguments.\n");
	}
}

static void terminal_set_oc_adj(int argc, const char **argv) {
	if (argc == 2) {
		int val = -1;
		sscanf(argv[1], "%d", &val);

		if (val >= 0 && val < 32) {
			drv8320s_set_oc_adj(val);
			unsigned int res = drv8320s_read_reg(5);
			char bl[9];
			char bh[9];

			utils_byte_to_binary((res >> 8) & 0xFF, bh);
			utils_byte_to_binary(res & 0xFF, bl);

			commands_printf("New reg value 0x%02x: %s %s (0x%04x)\n", 2, bh, bl, res);
		} else {
			commands_printf("Invalid argument(s).\n");
		}
	} else {
		commands_printf("This command requires one argument.\n");
	}
}

static void terminal_print_faults(int argc, const char **argv) {
	(void)argc;
	(void)argv;
	commands_printf(drv8320s_faults_to_string(drv8320s_read_faults()));
}

#endif
