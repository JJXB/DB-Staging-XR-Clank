// SPDX-FileCopyrightText:  Aaron Giles
// SPDX-License-Identifier: BSD-3-Clause
//
// DOSBox-native shell around the TMS320C3x execution core ported from MAME.
// Instruction semantics and floating-point conversion routines are Aaron
// Giles' BSD-3-Clause work (see su2000_tms320c3x_ops.ipp); this file replaces
// MAME's device framework with a small reset/run/memory-callback interface.

// license:BSD-3-Clause
// copyright-holders:Aaron Giles

#include "su2000_tms320c3x.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

typedef uint32_t offs_t;

//**************************************************************************
//  CONSTANTS
//**************************************************************************

// indexes into the register file
enum
{
	TMR_R0 = 0,
	TMR_R1,
	TMR_R2,
	TMR_R3,
	TMR_R4,
	TMR_R5,
	TMR_R6,
	TMR_R7,
	TMR_AR0,
	TMR_AR1,
	TMR_AR2,
	TMR_AR3,
	TMR_AR4,
	TMR_AR5,
	TMR_AR6,
	TMR_AR7,
	TMR_DP,
	TMR_IR0,
	TMR_IR1,
	TMR_BK,
	TMR_SP,
	TMR_ST,
	TMR_IE,
	TMR_IF,
	TMR_IOF,
	TMR_RS,
	TMR_RE,
	TMR_RC,
	TMR_R8,     // 3204x only
	TMR_R9,     // 3204x only
	TMR_R10,    // 3204x only
	TMR_R11,    // 3204x only
	TMR_TEMP1,  // used by the interpreter
	TMR_TEMP2,  // used by the interpreter
	TMR_TEMP3   // used by the interpreter
};

// flags
const int CFLAG     = 0x0001;
const int VFLAG     = 0x0002;
const int ZFLAG     = 0x0004;
const int NFLAG     = 0x0008;
const int UFFLAG    = 0x0010;
const int LVFLAG    = 0x0020;
const int LUFFLAG   = 0x0040;
const int OVMFLAG   = 0x0080;
const int RMFLAG    = 0x0100;
//const int CFFLAG    = 0x0400;
//const int CEFLAG    = 0x0800;
//const int CCFLAG    = 0x1000;
const int GIEFLAG   = 0x2000;

//**************************************************************************
//  MACROS
//**************************************************************************

#define IREG(rnum)  (m_r[rnum].i32[0])

static void tms320_logerror(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void tms320_fatal(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	abort();
}

#define logerror(...)  tms320_logerror(__VA_ARGS__)
#define fatalerror(...) tms320_fatal(__VA_ARGS__)

// The core stores words natively; only the floating-point packing needs the
// host byte order. These helpers match MAME's BYTE_XOR_BE on little-endian
// hosts (dosbox-staging's supported desktop targets).
static inline uint32_t BYTE_XOR_BE(uint32_t a)
{
	return a ^ 1u;
}

//**************************************************************************
//  CONSTRUCTION
//**************************************************************************

tms320c3x_device::tms320c3x_device()
{
	m_int_ram.resize(0x800, 0);
}

void tms320c3x_device::SetMemoryHandlers(void* ctx, ReadHandler read,
                                         WriteHandler write)
{
	m_read_ctx  = ctx;
	m_write_ctx = ctx;
	m_read      = read;
	m_write     = write;
}

void tms320c3x_device::SetInternalRom(const uint32_t* words, uint32_t count)
{
	m_internal_rom.assign(words, words + count);
}

uint32_t tms320c3x_device::GetRegister(int index) const
{
	return (index >= 0 && index < 36) ? m_r[index].i32[0] : 0;
}

void tms320c3x_device::SetRegister(int index, uint32_t value)
{
	if (index >= 0 && index < 36)
		m_r[index].i32[0] = value;
}

//**************************************************************************
//  FLOATING POINT CONVERSIONS
//**************************************************************************

float tms320c3x_device::tmsreg::as_float() const
{
	int_double id;

	// map 0 to 0
	if (mantissa() == 0 && exponent() == -128)
		return 0;

	// handle positive numbers
	else if (mantissa() >= 0)
	{
		int exp = (exponent() + 127) << 23;
		id.i[0] = exp + (mantissa() >> 8);
	}

	// handle negative numbers
	else
	{
		int exp = (exponent() + 127) << 23;
		int32_t man = -mantissa();
		id.i[0] = 0x80000000 + exp + ((man >> 8) & 0x00ffffff);
	}

	return id.f[0];
}

double tms320c3x_device::tmsreg::as_double() const
{
	int_double id;

	// map 0 to 0
	if (mantissa() == 0 && exponent() == -128)
		return 0;

	// handle positive numbers
	else if (mantissa() >= 0)
	{
		int exp = (exponent() + 1023) << 20;
		id.i[BYTE_XOR_BE(0)] = exp + (mantissa() >> 11);
		id.i[BYTE_XOR_BE(1)] = (mantissa() << 21) & 0xffe00000;
	}

	// handle negative numbers
	else
	{
		int exp = (exponent() + 1023) << 20;
		int32_t man = -mantissa();
		id.i[BYTE_XOR_BE(0)] = 0x80000000 + exp + ((man >> 11) & 0x001fffff);
		id.i[BYTE_XOR_BE(1)] = (man << 21) & 0xffe00000;
	}

	return id.d;
}

void tms320c3x_device::tmsreg::from_double(double val)
{
	// extract mantissa and exponent from the IEEE input
	int_double id;
	id.d = val;
	int32_t mantissa = ((id.i[BYTE_XOR_BE(0)] & 0x000fffff) << 11) |
	                   ((id.i[BYTE_XOR_BE(1)] & 0xffe00000) >> 21);
	int32_t exponent = ((id.i[BYTE_XOR_BE(0)] & 0x7ff00000) >> 20) - 1023;

	// if we're too small, map to 0
	if (exponent < -128)
	{
		set_mantissa(0);
		set_exponent(-128);
	}

	// if we're too large, map to the maximum value
	else if (exponent > 127)
	{
		if ((int32_t)id.i[BYTE_XOR_BE(0)] >= 0)
			set_mantissa(0x7fffffff);
		else
			set_mantissa(0x80000001);
		set_exponent(127);
	}

	// if we're positive, map directly
	else if ((int32_t)id.i[BYTE_XOR_BE(0)] >= 0)
	{
		set_mantissa(mantissa);
		set_exponent(exponent);
	}

	// if we're negative with a non-zero mantissa, remove the leading sign bit
	else if (mantissa != 0)
	{
		set_mantissa(0x80000000 | -mantissa);
		set_exponent(exponent);
	}

	// if we're negative with a zero mantissa, normalize
	else
	{
		set_mantissa(0x80000000);
		set_exponent(exponent - 1);
	}
}

float tms320c3x_device::fp_to_float(uint32_t floatdata)
{
	tmsreg gen(floatdata << 8, (int32_t)floatdata >> 24);
	return gen.as_float();
}

double tms320c3x_device::fp_to_double(uint32_t floatdata)
{
	tmsreg gen(floatdata << 8, (int32_t)floatdata >> 24);
	return gen.as_double();
}

uint32_t tms320c3x_device::float_to_fp(float fval)
{
	tmsreg gen(fval);
	return (gen.exponent() << 24) | ((uint32_t)gen.mantissa() >> 8);
}

uint32_t tms320c3x_device::double_to_fp(double dval)
{
	tmsreg gen(dval);
	return (gen.exponent() << 24) | ((uint32_t)gen.mantissa() >> 8);
}

//**************************************************************************
//  MEMORY
//**************************************************************************

uint32_t tms320c3x_device::ROPCODE(uint32_t pc)
{
	return RMEM(pc);
}

uint32_t tms320c3x_device::RMEM(uint32_t addr)
{
	// internal 2K-word RAM block
	if (addr >= 0x809800u && addr <= 0x809fffu)
		return m_int_ram[addr - 0x809800u];

	// primary bus control register
	if (addr == 0x808064u)
		return m_primary_bus_control;

	// internal boot-loader ROM while MCBL is selected
	if (m_mcbl_mode && addr < m_internal_rom.size())
		return m_internal_rom[addr];

	if (m_read)
		return m_read(m_read_ctx, addr);

	return 0;
}

void tms320c3x_device::WMEM(uint32_t addr, uint32_t data)
{
	// internal 2K-word RAM block
	if (addr >= 0x809800u && addr <= 0x809fffu)
	{
		m_int_ram[addr - 0x809800u] = data;
		return;
	}

	// primary bus control register
	if (addr == 0x808064u)
	{
		primary_bus_control_w(0, data, 0xffffffffu);
		return;
	}

	if (m_write)
		m_write(m_write_ctx, addr, data);
}

void tms320c3x_device::primary_bus_control_w(uint32_t, uint32_t data,
                                             uint32_t mem_mask)
{
	// change in internal hold state
	if ((m_primary_bus_control ^ data) & HIZ)
	{
		if (m_primary_bus_control & HOLDST)
			m_primary_bus_control &= ~HOLDST;
		else
			m_primary_bus_control |= HOLDST;
	}

	// enable of external hold with hold pending
	if ((m_primary_bus_control & NOHOLD) && !(data & NOHOLD) && m_hold_state)
		m_primary_bus_control |= HOLDST;

	// update register
	m_primary_bus_control = (m_primary_bus_control & ~(mem_mask | WMASK)) |
	                        (data & mem_mask & WMASK);
}

//**************************************************************************
//  EXECUTION
//**************************************************************************

void tms320c3x_device::check_irqs()
{
	// determine if we have any live interrupts
	uint16_t validints = IREG(TMR_IF) & IREG(TMR_IE) & 0x0fff;
	if (validints == 0 || (IREG(TMR_ST) & GIEFLAG) == 0)
		return;

	// find the lowest signalled value
	int whichtrap = 0;
	for (int i = 0; i < 12; i++)
		if (validints & (1 << i))
		{
			whichtrap = i + 1;
			break;
		}

	// no longer idling if we get here
	m_is_idling = false;
	if (!m_delayed)
	{
		uint16_t intmask = 1 << (whichtrap - 1);

		// bit in IF is cleared when interrupt is taken
		IREG(TMR_IF) &= ~intmask;
		trap(whichtrap);

		// after auto-clearing the interrupt bit, we need to re-trigger
		// level-sensitive interrupts
		if (m_chip_type == CHIP_TYPE_TMS320C31 ||
		    (IREG(TMR_ST) & 0x4000) == 0)
			IREG(TMR_IF) |= m_irq_state & 0x0f;
	}
	else
		m_irq_pending = true;
}

void tms320c3x_device::SetIrq(int inputnum, bool asserted)
{
	if (inputnum == TMS320C3X_MCBL)
	{
		m_mcbl_mode = asserted;
		return;
	}

	if (inputnum == TMS320C3X_HOLD)
	{
		m_hold_state = asserted;

		if (m_hold_state)
		{
			if (!(m_primary_bus_control & NOHOLD))
				m_primary_bus_control |= HOLDST;
		}
		else if ((m_primary_bus_control & HOLDST) &&
		         !(m_primary_bus_control & HIZ))
			m_primary_bus_control &= ~HOLDST;
		return;
	}

	// update the external state
	uint16_t intmask = 1 << inputnum;
	if (asserted)
	{
		m_irq_state |= intmask;
		IREG(TMR_IF) |= intmask;
	}
	else
		m_irq_state &= ~intmask;

	// external interrupts are level-sensitive on the '31
	if (m_chip_type != CHIP_TYPE_TMS320C32 || (IREG(TMR_ST) & 0x4000) == 0)
		IREG(TMR_IF) |= m_irq_state & 0x0f;
}

void tms320c3x_device::Reset()
{
	m_pc = RMEM(0);

	// reset some registers
	IREG(TMR_IE) = 0;
	IREG(TMR_IF) = 0;
	IREG(TMR_ST) = 0;
	IREG(TMR_IOF) = 0;

	// update IF with the external interrupt state (required for boot loader)
	IREG(TMR_IF) |= m_irq_state & 0x0f;

	// reset peripheral registers
	m_primary_bus_control = 0x000010f8;

	// reset internal stuff
	m_delayed = m_irq_pending = m_is_idling = m_is_lopower = false;
}

void tms320c3x_device::Run(int cycles)
{
	m_icount = cycles;

	// check IRQs up front
	check_irqs();

	// if we're idling, just eat the cycles
	if (m_is_idling)
	{
		m_icount = 0;
		return;
	}

	while (m_icount > 0)
	{
		if ((IREG(TMR_ST) & RMFLAG) && m_pc == IREG(TMR_RE) + 1)
		{
			if ((int32_t)--IREG(TMR_RC) >= 0)
				m_pc = IREG(TMR_RS);
			else
			{
				IREG(TMR_ST) &= ~RMFLAG;
				if (m_delayed)
				{
					m_delayed = false;
					if (m_irq_pending)
					{
						m_irq_pending = false;
						check_irqs();
					}
				}
			}
			continue;
		}

		execute_one();
	}
}

//**************************************************************************
//  CORE OPCODES
//**************************************************************************

// Small subset of MAME's util/eminline helpers used by the opcode core.
#define BIT(x, n)  (((x) >> (n)) & 1)

template <typename T>
static inline T tms_clamp(T value, T lo, T hi)
{
	return (value < lo) ? lo : ((value > hi) ? hi : value);
}

static inline int64_t mul_32x32(int32_t a, int32_t b)
{
	return int64_t(a) * int64_t(b);
}

namespace util {
static inline int32_t sext(uint32_t value, int bits)
{
	const uint32_t sign = 1u << (bits - 1);
	const uint32_t mask = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
	value &= mask;
	return static_cast<int32_t>((value ^ sign) - sign);
}
}

static inline int tms_countl_zero(uint32_t v)
{
	if (v == 0)
		return 32;
	int n = 0;
	if ((v & 0xffff0000u) == 0) { n += 16; v <<= 16; }
	if ((v & 0xff000000u) == 0) { n += 8;  v <<= 8; }
	if ((v & 0xf0000000u) == 0) { n += 4;  v <<= 4; }
	if ((v & 0xc0000000u) == 0) { n += 2;  v <<= 2; }
	if ((v & 0x80000000u) == 0) { n += 1; }
	return n;
}

static inline int tms_countl_one(uint32_t v)
{
	return tms_countl_zero(~v);
}

#include "su2000_tms320c3x_ops.ipp"
