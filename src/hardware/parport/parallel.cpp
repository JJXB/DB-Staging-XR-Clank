// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/parport/parallel.h"

#include <cassert>

#include "config/setup.h"
#include "dosbox.h"
#include "hardware/port.h"

// Dummy parallel ports for programs that probe for printer or stereo
// shutter-glasses hardware (e.g. via the BIOS data area or direct port
// access). A dummy port answers detection probes and accepts writes
// silently; all traffic remains visible in the port logs.

namespace {

// Standard LPT base addresses (LPT1-3).
constexpr io_port_t ParallelBases[] = {0x378, 0x278, 0x3bc};
constexpr const char* ParallelNames[] = {"LPT1", "LPT2", "LPT3"};
constexpr const char* ParallelProps[] = {"parallel1", "parallel2", "parallel3"};
constexpr int NumParallelPorts = 3;

// Printer-ready status bits: busy high, ack high, paper-out low,
// selected high, error high.
constexpr uint8_t PrinterReadyStatus = 0xdf;

struct DummyParallelPort {
	bool enabled = false;
	io_port_t base = 0;

	// Output latches (reads return the last written value, as on
	// real hardware).
	uint8_t data    = 0x00;
	uint8_t control = 0x00;

	IO_WriteHandleObject data_write     = {};
	IO_ReadHandleObject data_read       = {};
	IO_ReadHandleObject status_read     = {};
	IO_WriteHandleObject control_write  = {};
	IO_ReadHandleObject control_read    = {};
};

DummyParallelPort dummy_ports[NumParallelPorts] = {};

void install_dummy_port(const int index)
{
	auto& port      = dummy_ports[index];
	const auto base = ParallelBases[index];
	port.base       = base;

	port.data_write.Install(
	        base,
	        [index](io_port_t, io_val_t value, io_width_t) {
		        dummy_ports[index].data = static_cast<uint8_t>(value);
	        },
	        io_width_t::byte);

	port.data_read.Install(
	        base,
	        [index](io_port_t, io_width_t) -> io_val_t {
		        return dummy_ports[index].data;
	        },
	        io_width_t::byte);

	const auto status_port = static_cast<io_port_t>(base + 1u);
	port.status_read.Install(
	        status_port,
	        [](io_port_t, io_width_t) -> io_val_t {
		        return PrinterReadyStatus;
	        },
	        io_width_t::byte);

	const auto control_port = static_cast<io_port_t>(base + 2u);
	port.control_write.Install(
	        control_port,
	        [index](io_port_t, io_val_t value, io_width_t) {
		        dummy_ports[index].control = static_cast<uint8_t>(value);
	        },
	        io_width_t::byte);

	port.control_read.Install(
	        control_port,
	        [index](io_port_t, io_width_t) -> io_val_t {
		        return dummy_ports[index].control;
	        },
	        io_width_t::byte);

	port.enabled = true;

	LOG_MSG("PARALLEL: Dummy %s on ports %03xh-%03xh",
	        ParallelNames[index],
	        base,
	        control_port);
}

void uninstall_dummy_port(const int index)
{
	auto& port = dummy_ports[index];

	port.data_write.Uninstall();
	port.data_read.Uninstall();
	port.status_read.Uninstall();
	port.control_write.Uninstall();
	port.control_read.Uninstall();

	port.enabled = false;
}

} // namespace

void PARALLEL_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	using enum Property::Changeable::Value;

	auto section = conf->AddSection("parallel");

	auto pstring = section->AddString("parallel1", OnlyAtStart, "disabled");
	pstring->SetHelp(
	        "Emulate a dummy LPT1 parallel port ('disabled' by default).\n"
	        "A dummy port answers BIOS and port detection probes (for printer\n"
	        "or stereo shutter-glasses hardware) and accepts writes silently.\n"
	        "Possible values: 'disabled', 'dummy'.\n"
	        "\n"
	        "Note: If an LPT DAC audio device is configured on the same port,\n"
	        "it takes precedence.");
	pstring->SetValues({"disabled", "dummy"});

	pstring = section->AddString("parallel2", OnlyAtStart, "disabled");
	pstring->SetHelp(
	        "Emulate a dummy LPT2 parallel port ('disabled' by default).\n"
	        "See 'parallel1' for details.");
	pstring->SetValues({"disabled", "dummy"});

	pstring = section->AddString("parallel3", OnlyAtStart, "disabled");
	pstring->SetHelp(
	        "Emulate a dummy LPT3 parallel port ('disabled' by default).\n"
	        "See 'parallel1' for details.");
	pstring->SetValues({"disabled", "dummy"});
}

void PARALLEL_Init()
{
	auto section = get_section("parallel");
	assert(section);

	for (int i = 0; i < NumParallelPorts; ++i) {
		if (section->GetString(ParallelProps[i]) == "dummy") {
			install_dummy_port(i);
		}
	}
}

void PARALLEL_Destroy()
{
	for (int i = 0; i < NumParallelPorts; ++i) {
		if (dummy_ports[i].enabled) {
			uninstall_dummy_port(i);
		}
	}
}
