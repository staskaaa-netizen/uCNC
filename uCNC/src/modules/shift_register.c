/*
	Name: ic74hc595.c
	Description: This module adds the ability to control the IC74HC595 shift register controller (used for example in the MKS-DLC32 board) to µCNC.
				 Up to 56 output pins can be assigned.

	Copyright: Copyright (c) João Martins
	Author: João Martins
	Date: 01/09/2022

	µCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	µCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include "../cnc.h"
#include <stdint.h>
#include <stdbool.h>

#ifndef IC74HC595_COUNT
#define IC74HC595_COUNT 0
#endif

#ifndef IC74HC165_COUNT
#define IC74HC165_COUNT 0
#endif

#ifndef SHIFT_REGISTER_SDO
#define SHIFT_REGISTER_SDO DOUT8
#endif

#ifndef SHIFT_REGISTER_SDI
#define SHIFT_REGISTER_SDI DIN8
#endif

#ifndef SHIFT_REGISTER_CLK
#define SHIFT_REGISTER_CLK DOUT9
#endif

#ifndef IC74HC595_LATCH
#define IC74HC595_LATCH DOUT10
#endif

#ifndef IC74HC165_LOAD
#define IC74HC165_LOAD DOUT11
#endif

#ifndef SHIFT_REGISTER_DELAY_CYCLES
#define SHIFT_REGISTER_DELAY_CYCLES 0
#endif

#if (IC74HC595_COUNT >= IC74HC165_COUNT)
#define SHIFT_REGISTER_BYTES IC74HC595_COUNT
#else
#define SHIFT_REGISTER_BYTES IC74HC165_COUNT
#endif

#define shift_register_delay() mcu_delay_cycles(SHIFT_REGISTER_DELAY_CYCLES)
static DECL_MUTEX(shifter_running);

#if (IC74HC595_COUNT != 0) || (IC74HC165_COUNT != 0)
#if (IC74HC595_COUNT != 0)
volatile uint8_t __attribute__((used)) ic74hc595_io_pins[IC74HC595_COUNT];
#endif
#if (IC74HC165_COUNT != 0)
volatile uint8_t __attribute__((used)) ic74hc165_io_pins[IC74HC165_COUNT];
#endif

#ifndef SHIFT_REGISTER_CUSTOM_CALLBACK
MCU_CALLBACK void __attribute__((weak)) shift_register_io_pins(void)
{
	BIN_SEMPH_INIT(shifter_running, BIN_SEMPH_UNLOCKED);

	if(BIN_SEMPH_TRYLOCK(shifter_running))
	{
		uint8_t pins[SHIFT_REGISTER_BYTES];

#if (IC74HC165_COUNT > 0)
		memset(pins, 0, IC74HC165_COUNT);
		mcu_set_output(IC74HC165_LOAD);
#endif
#if (IC74HC595_COUNT > 0)
		ATOMIC_CODEBLOCK
		{
			memcpy(pins, (const void *)ic74hc595_io_pins, IC74HC595_COUNT);
		}
		mcu_clear_output(IC74HC595_LATCH);
#endif
		/**
		 * shift bytes
		 */
		for (uint8_t i = SHIFT_REGISTER_BYTES; i != 0;)
		{
			i--;
#if (defined(SHIFT_REGISTER_USE_HW_SPI) && defined(MCU_HAS_SPI))
			pins[i] = mcu_spi_xmit(pins[i]);
#else
			uint8_t pinbyte = pins[i];
			for (uint8_t j = 0x80; j != 0; j >>= 1)
			{
#if (SHIFT_REGISTER_DELAY_CYCLES)
				shift_register_delay();
#endif
				mcu_clear_output(SHIFT_REGISTER_CLK);
#if (IC74HC595_COUNT > 0)
				// write
				if (pinbyte & j)
				{
					mcu_set_output(SHIFT_REGISTER_SDO);
				}
				else
				{
					mcu_clear_output(SHIFT_REGISTER_SDO);
				}
#endif
// read
#if (IC74HC165_COUNT > 0)
				if (mcu_get_input(SHIFT_REGISTER_SDI))
				{
					pinbyte |= j;
				}
				else
				{
					pinbyte &= ~j;
				}
#endif
				mcu_set_output(SHIFT_REGISTER_CLK);
			}

#if (IC74HC165_COUNT > 0)
			pins[i] = pinbyte;
#endif

#if (SHIFT_REGISTER_DELAY_CYCLES)
			shift_register_delay();
#endif
			mcu_set_output(SHIFT_REGISTER_CLK);

#endif
		}

#if (IC74HC165_COUNT > 0)
		memcpy((void *)ic74hc165_io_pins, (const void *)pins, IC74HC165_COUNT);
		mcu_clear_output(IC74HC165_LOAD); // allow a new load
#endif
#if (IC74HC595_COUNT > 0)
		mcu_set_output(IC74HC595_LATCH);
#endif

		BIN_SEMPH_UNLOCK(shifter_running);
	}
}
#else
MCU_CALLBACK void __attribute__((weak)) shift_register_io_pins(void)
{
}
#endif
#endif

/* Read the complete input chain without touching the output shift registers'
   storage latch. The shared clock moves the 595 shift stages, but their outputs
   only change on LATCH, so motion outputs stay put while an input sample runs. */
bool shift_register_read_inputs(uint32_t *value)
{
#if (IC74HC165_COUNT > 0)
	uint8_t pins[IC74HC165_COUNT];
	uint32_t raw = 0;

	if (!value)
	{
		return false;
	}
	BIN_SEMPH_INIT(shifter_running, BIN_SEMPH_UNLOCKED);
	if (!BIN_SEMPH_TRYLOCK(shifter_running))
	{
		return false;
	}

	mcu_clear_output(IC74HC165_LOAD);
#if (SHIFT_REGISTER_DELAY_CYCLES)
	shift_register_delay();
#endif
	mcu_set_output(IC74HC165_LOAD);

	for (uint8_t i = IC74HC165_COUNT; i != 0;)
	{
		uint8_t pinbyte = 0;

		i--;
		for (uint8_t j = 0x80; j != 0; j >>= 1)
		{
			mcu_clear_output(SHIFT_REGISTER_CLK);
			if (mcu_get_input(SHIFT_REGISTER_SDI))
			{
				pinbyte |= j;
			}
			mcu_set_output(SHIFT_REGISTER_CLK);
		}
		pins[i] = pinbyte;
	}

	memcpy((void *)ic74hc165_io_pins, (const void *)pins, IC74HC165_COUNT);
	for (uint8_t i = 0; i < IC74HC165_COUNT; i++)
	{
		raw = (raw << 8) | pins[i];
	}
	*value = raw;
	BIN_SEMPH_UNLOCK(shifter_running);
	return true;
#else
	(void)value;
	return false;
#endif
}

#if (IC74HC595_COUNT != 0) || (IC74HC165_COUNT != 0)
DECL_MODULE(shift_register)
{
#if (IC74HC595_COUNT > 0)
	io_config_output(SHIFT_REGISTER_SDO);
	io_clear_output(SHIFT_REGISTER_SDO);
	io_config_output(SHIFT_REGISTER_CLK);
	io_set_output(SHIFT_REGISTER_CLK);
	io_config_output(IC74HC595_LATCH);
	io_set_output(IC74HC595_LATCH);
#endif
#if (IC74HC165_COUNT > 0)
	io_config_input(SHIFT_REGISTER_SDI);
	io_config_output(IC74HC165_LOAD);
	io_set_output(IC74HC165_LOAD);
#endif
	io_extended_pins_update();
}
#endif
