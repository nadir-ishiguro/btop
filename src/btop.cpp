/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <csignal>
#include <pthread.h>
#include <thread>
#include <future>
#include <bitset>
#include <numeric>
#include <ranges>
#include <unistd.h>
#include <cmath>
#include <iostream>
#include <exception>

#include <btop_shared.hpp>
#include <btop_tools.hpp>
#include <btop_config.hpp>
#include <btop_input.hpp>
#include <btop_theme.hpp>
#include <btop_draw.hpp>
#include <btop_menu.hpp>

#if defined(__linux__)
	#define LINUX
#elif defined(__unix__) or not defined(__APPLE__) and defined(__MACH__)
	#include <sys/param.h>
	#if defined(BSD)
		#error BSD support not yet implemented!
	#endif
#elif defined(__APPLE__) and defined(__MACH__)
	#include <TargetConditionals.h>
	#if TARGET_OS_MAC == 1
		#define OSX
		#error OSX support not yet implemented!
    #endif
#else
	#error Platform not supported!
#endif

using std::string, std::string_view, std::vector, std::array, std::atomic, std::endl, std::cout, std::min;
using std::flush, std::endl, std::string_literals::operator""s, std::to_string, std::future, std::async, std::bitset, std::future_status;
namespace fs = std::filesystem;
namespace rng = std::ranges;
using namespace Tools;

namespace Global {
	const vector<array<string, 2>> Banner_src = {
		{"#E62525", "██████╗ ████████╗ ██████╗ ██████╗"},
		{"#CD2121", "██╔══██╗╚══██╔══╝██╔═══██╗██╔══██╗   ██╗    ██╗"},
		{"#B31D1D", "██████╔╝   ██║   ██║   ██║██████╔╝ ██████╗██████╗"},
		{"#9A1919", "██╔══██╗   ██║   ██║   ██║██╔═══╝  ╚═██╔═╝╚═██╔═╝"},
		{"#801414", "██████╔╝   ██║   ╚██████╔╝██║        ╚═╝    ╚═╝"},
		{"#000000", "╚═════╝    ╚═╝    ╚═════╝ ╚═╝"},
	};
	const string Version = "0.0.30";

	int coreCount;
	string banner;
	size_t banner_width = 0;
	string overlay;
	string clock;

	fs::path self_path;

	string exit_error_msg;
	atomic<bool> thread_exception (false);

	bool debuginit = false;
	bool debug = false;
	bool utf_force = false;

	uint64_t start_time;

	atomic<bool> resized (false);
	atomic<int> resizing (0);
	atomic<bool> quitting (false);

	bool arg_tty = false;
	bool arg_low_color = false;
}


//* A simple argument parser
void argumentParser(const int& argc, char **argv) {
	for(int i = 1; i < argc; i++) {
		string argument = argv[i];
		if (argument == "-h" or argument == "--help") {
			cout 	<< "usage: btop [-h] [-v] [-/+t] [--debug]\n\n"
					<< "optional arguments:\n"
					<< "  -h, --help            show this help message and exit\n"
					<< "  -v, --version         show version info and exit\n"
					<< "  -lc, --low-color      disable truecolor, converts 24-bit colors to 256-color\n"
					<< "  -t, --tty_on          force (ON) tty mode, max 16 colors and tty friendly graph symbols\n"
					<< "  +t, --tty_off         force (OFF) tty mode\n"
					<< "  --utf-foce			force start even if no UTF-8 locale was detected"
					<< "  --debug               start with loglevel set to DEBUG, overriding value set in config\n"
					<< endl;
			exit(0);
		}
		if (argument == "-v" or argument == "--version") {
			cout << "btop version: " << Global::Version << endl;
			exit(0);
		}
		else if (argument == "-lc" or argument == "--low-color") {
			Global::arg_low_color = true;
		}
		else if (argument == "-t" or argument == "--tty_on") {
			Config::set("tty_mode", true);
			Global::arg_tty = true;
		}
		else if (argument == "+t" or argument == "--tty_off") {
			Config::set("tty_mode", false);
			Global::arg_tty = true;
		}
		else if (argument == "--utf-force")
			Global::utf_force = true;
		else if (argument == "--debug")
			Global::debug = true;
		else {
			cout << " Unknown argument: " << argument << "\n" <<
			" Use -h or --help for help." <<  endl;
			exit(1);
		}
	}
}

//* Handler for SIGWINCH and general resizing events, does nothing if terminal hasn't been resized unless force=true
void term_resize(bool force) {
	if (auto refreshed = Term::refresh(); refreshed or force) {
		if (force and refreshed) force = false;
	}
	else return;

	auto rez_state = ++Global::resizing;
	if (rez_state > 1) return;
	Global::resized = true;
	Runner::stop();
	while (not force) {
		sleep_ms(100);
		if (rez_state != Global::resizing) rez_state = --Global::resizing;
		else if (not Term::refresh()) break;
	}

	Input::interrupt = true;
	Global::resizing = 0;
}

//* Exit handler; stops threads, restores terminal and saves config changes
void clean_quit(int sig) {
	if (Global::quitting) return;
	Global::quitting = true;
	Runner::stop();

	if (not Global::exit_error_msg.empty()) {
		sig = 1;
		Logger::error(Global::exit_error_msg);
		std::cerr << "ERROR: " << Global::exit_error_msg << endl;
	}
	Config::write();
	Input::clear();
	Logger::info("Quitting! Runtime: " + sec_to_dhms(time_s() - Global::start_time));

	//? Wait for any remaining Tools::atomic_lock destructors to finish for max 1000ms
	for (int i = 0; Tools::active_locks > 0 and i < 100; i++) {
		sleep_ms(10);
	}

	if (Term::initialized) {
		Term::restore();
	}

	//? Assume error if still not cleaned up and call quick_exit to avoid a segfault from Tools::atomic_lock destructor
	if (Tools::active_locks > 0) {
		quick_exit((sig != -1 ? sig : 0));
	}

	if (sig != -1) exit(sig);
}

//* Handler for SIGTSTP; stops threads, restores terminal and sends SIGSTOP
void _sleep() {
	Runner::stop();
	Term::restore();
	std::raise(SIGSTOP);
}

//* Handler for SIGCONT; re-initialize terminal and force a resize event
void _resume() {
	Term::init();
	term_resize(true);
}

void _exit_handler() {
	clean_quit(-1);
}

void _signal_handler(const int sig) {
	switch (sig) {
		case SIGINT:
			clean_quit(0);
			break;
		case SIGTSTP:
			_sleep();
			break;
		case SIGCONT:
			_resume();
			break;
		case SIGWINCH:
			term_resize();
			break;
	}
}

//* Generate the btop++ banner
void banner_gen() {
	Global::banner.clear();
	Global::banner_width = 0;
	string b_color, bg, fg, oc, letter;
	auto& lowcolor = Config::getB("lowcolor");
	auto& tty_mode = Config::getB("tty_mode");
	for (size_t z = 0; const auto& line : Global::Banner_src) {
		if (const auto w = ulen(line[1]); w > Global::banner_width) Global::banner_width = w;
		if (tty_mode) {
			fg = (z > 2) ? "\x1b[31m" : "\x1b[91m";
			bg = (z > 2) ? "\x1b[90m" : "\x1b[37m";
		}
		else {
			fg = Theme::hex_to_color(line[0], lowcolor);
			int bg_i = 120 - z * 12;
			bg = Theme::dec_to_color(bg_i, bg_i, bg_i, lowcolor);
		}
		for (size_t i = 0; i < line[1].size(); i += 3) {
			if (line[1][i] == ' ') {
				letter = ' ';
				i -= 2;
			}
			else
				letter = line[1].substr(i, 3);

			// if (tty_mode and letter != "█" and letter != " ") letter = "░";
			b_color = (letter == "█") ? fg : bg;
			if (b_color != oc) Global::banner += b_color;
			Global::banner += letter;
			oc = b_color;
		}
		if (++z < Global::Banner_src.size()) Global::banner += Mv::l(ulen(line[1])) + Mv::d(1);
	}
	Global::banner += Mv::r(18 - Global::Version.size())
			+ (tty_mode ? "\x1b[0;40;37m" : Theme::dec_to_color(0,0,0, lowcolor, "bg") + Theme::dec_to_color(150, 150, 150, lowcolor))
			+ Fx::i + "v" + Global::Version + Fx::ui;
}

//* Manages secondary thread for collection and drawing of boxes
namespace Runner {
	atomic<bool> active (false);
	atomic<bool> stopping (false);
	atomic<bool> waiting (false);

	string output;
	sigset_t mask;
	pthread_mutex_t mtx;

	const unordered_flat_map<string, uint_fast8_t> box_bits = {
		{"proc",	0b0000'0001},
		{"mem",		0b0000'0100},
		{"net",		0b0001'0000},
		{"cpu",		0b0100'0000},
	};

	enum bit_pos {
		proc_present, proc_running,
		mem_present, mem_running,
		net_present, net_running,
		cpu_present, cpu_running
	};

	const uint_fast8_t proc_done 	= 0b0000'0011;
	const uint_fast8_t mem_done 	= 0b0000'1100;
	const uint_fast8_t net_done 	= 0b0011'0000;
	const uint_fast8_t cpu_done 	= 0b1100'0000;

	struct runner_conf {
		vector<string> boxes;
		bool no_update = false;
		bool force_redraw = false;
		string overlay = "";
		string clock = "";
	};

	struct runner_conf current_conf;

	//? ------------------------------- Secondary thread: async launcher and drawing ----------------------------------
	void * _runner(void * confptr) {
		struct runner_conf *conf;
		conf = (struct runner_conf *) confptr;

		//? Block all signals in this thread to avoid deadlock from any signal handlers trying to stop this thread
		pthread_sigmask(SIG_BLOCK, &mask, NULL);

		//? pthread_mutex_lock to make sure this thread is a single instance thread
		thread_lock pt_lck(mtx);
		if (pt_lck.status != 0) {
			Global::exit_error_msg = "Exception in runner thread -> pthread_mutex_lock error id: " + to_string(pt_lck.status);
			Global::thread_exception = true;
			Input::interrupt = true;
			stopping = true;
		}

		if (active or stopping or Global::resized) {
			pthread_exit(NULL);
		}

		//? Secondary atomic lock used for signaling status to main thread
		atomic_lock lck(active);

		//! DEBUG stats
		auto timestamp = time_micros();

		output.clear();

		//? Setup bitmask for selected boxes instead of parsing strings in the loop
		bitset<8> box_mask;
		for (const auto& box : conf->boxes) {
			box_mask |= box_bits.at(box);
		}

		future<Cpu::cpu_info> cpu;
		future<Mem::mem_info> mem;
		future<Net::net_info> net;
		future<vector<Proc::proc_info>> proc;

		//* Start collection functions for all boxes in async threads and draw in this thread when finished
		//? Starting order below based on mean time to finish
		while (box_mask.count() > 0) {
			if (stopping) break;
			try {
				//* PROC
				if (box_mask.test(proc_present)) {
					if (not box_mask.test(proc_running)) {
						proc = async(Proc::collect, conf->no_update);
						box_mask.set(proc_running);
					}
					else if (not proc.valid())
						throw std::runtime_error("Proc::collect() future not valid.");

					else if (proc.wait_for(ZeroSec) == future_status::ready) {
						try {
							output += Proc::draw(proc.get(), conf->force_redraw, conf->no_update);
						}
						catch (const std::exception& e) {
							throw std::runtime_error("Proc:: -> " + (string)e.what());
						}
						box_mask ^= proc_done;
					}
				}
				//* MEM
				if (box_mask.test(mem_present)) {
					if (not box_mask.test(mem_running)) {
						mem = async(Mem::collect, conf->no_update);
						box_mask.set(mem_running);
					}
					else if (not mem.valid())
						throw std::runtime_error("Mem::collect() future not valid.");

					else if (mem.wait_for(ZeroSec) == future_status::ready) {
						try {
							output += Mem::draw(mem.get(), conf->force_redraw, conf->no_update);
						}
						catch (const std::exception& e) {
							throw std::runtime_error("Mem:: -> " + (string)e.what());
						}
						box_mask ^= mem_done;
					}
				}
				//* NET
				if (box_mask.test(net_present)) {
					if (not box_mask.test(net_running)) {
						net = async(Net::collect, conf->no_update);
						box_mask.set(net_running);
					}
					else if (not net.valid())
						throw std::runtime_error("Net::collect() future not valid.");

					else if (net.wait_for(ZeroSec) == future_status::ready) {
						try {
							output += Net::draw(net.get(), conf->force_redraw, conf->no_update);
						}
						catch (const std::exception& e) {
							throw std::runtime_error("Net:: -> " + (string)e.what());
						}
						box_mask ^= net_done;
					}
				}
				//* CPU
				if (box_mask.test(cpu_present)) {
					if (not box_mask.test(cpu_running)) {
						cpu = async(Cpu::collect, conf->no_update);
						box_mask.set(cpu_running);
					}
					else if (not cpu.valid())
						throw std::runtime_error("Cpu::collect() future not valid.");

					else if (cpu.wait_for(ZeroSec) == future_status::ready) {
						try {
							output += Cpu::draw(cpu.get(), conf->force_redraw, conf->no_update);
						}
						catch (const std::exception& e) {
							throw std::runtime_error("Cpu:: -> " + (string)e.what());
						}
						box_mask ^= cpu_done;
					}
				}
			}
			catch (const std::exception& e) {
				Global::exit_error_msg = "Exception in runner thread -> " + (string)e.what();
				Global::thread_exception = true;
				Input::interrupt = true;
				stopping = true;
				break;
			}
		}

		if (stopping) {
			pthread_exit(NULL);
		}

		//? If overlay isn't empty, print output without color and then print overlay on top
		cout << Term::sync_start << (conf->overlay.empty()
				? output + conf->clock
				: Theme::c("inactive_fg") + Fx::uncolor(output + conf->clock) + conf->overlay)
			 << Term::sync_end << flush;

		//! DEBUG stats -->
		cout << Fx::reset << Mv::to(1, 20) << "Runner took: " << rjust(to_string(time_micros() - timestamp), 5) << " μs.  " << flush;

		pthread_exit(NULL);
	}
	//? ------------------------------------------ Secondary thread end -----------------------------------------------

	//* Runs collect and draw in a secondary thread, unlocks and locks config to update cached values, box="all": all boxes
	void run(const string& box, const bool no_update, const bool force_redraw) {
		atomic_lock lck(waiting);
		atomic_wait(active);
		if (stopping or Global::resized) return;

		if (box == "overlay") {
			cout << Term::sync_start << Global::overlay << Term::sync_end;
		}
		else if (box == "clock") {
			if (not Global::clock.empty())
				cout << Term::sync_start << Global::clock << Term::sync_end;
		}
		else if (box.empty() and Config::current_boxes.empty()) {
			cout << Term::sync_start << Term::clear + Mv::to(10, 10) << "No boxes shown!" << Term::sync_end;
		}
		else {
			Config::unlock();
			Config::lock();

			current_conf = { (box == "all" ? Config::current_boxes : vector{box}), no_update, force_redraw, Global::overlay, Global::clock};

			pthread_t runner_id;
			if (pthread_create(&runner_id, NULL, &_runner, (void *) &current_conf) != 0)
				throw std::runtime_error("Failed to create _runner thread!");

			if (pthread_detach(runner_id) != 0)
				throw std::runtime_error("Failed to detach _runner thread!");

			for (int i = 0; not active and i < 10; i++) sleep_ms(1);
		}
	}

	//* Stops any secondary thread running
	void stop() {
		stopping = true;
		int ret = pthread_mutex_trylock(&mtx);
		if (ret == EOWNERDEAD or ret == ENOTRECOVERABLE) {
			if (active) active = false;
			Global::exit_error_msg = "Runner thread died unexpectedly!";
			if (not Global::quitting) exit(1);
		}
		else if (ret == EBUSY)
			atomic_wait(active);
		else if (ret == 0)
			pthread_mutex_unlock(&mtx);
		sleep_ms(1);
		stopping = false;
	}

}


//* --------------------------------------------- Main starts here! ---------------------------------------------------
int main(int argc, char **argv) {

	//? ------------------------------------------------ INIT ---------------------------------------------------------

	Global::start_time = time_s();

	//? Call argument parser if launched with arguments
	if (argc > 1) argumentParser(argc, argv);

	//? Setup signal handlers for CTRL-C, CTRL-Z, resume and terminal resize
	std::atexit(_exit_handler);
	std::signal(SIGINT, _signal_handler);
	std::signal(SIGTSTP, _signal_handler);
	std::signal(SIGCONT, _signal_handler);
	std::signal(SIGWINCH, _signal_handler);
	sigemptyset(&Runner::mask);
	sigaddset(&Runner::mask, SIGINT);
	sigaddset(&Runner::mask, SIGTSTP);
	sigaddset(&Runner::mask, SIGWINCH);
	sigaddset(&Runner::mask, SIGTERM);

	//? Setup paths for config, log and user themes
	for (const auto& env : {"XDG_CONFIG_HOME", "HOME"}) {
		if (getenv(env) != NULL and access(getenv(env), W_OK) != -1) {
			Config::conf_dir = fs::path(getenv(env)) / (((string)env == "HOME") ? ".config/btop" : "btop");
			break;
		}
	}
	if (not Config::conf_dir.empty()) {
		if (std::error_code ec; not fs::is_directory(Config::conf_dir) and not fs::create_directories(Config::conf_dir, ec)) {
			cout 	<< "WARNING: Could not create or access btop config directory. Logging and config saving disabled.\n"
					<< "Make sure $XDG_CONFIG_HOME or $HOME environment variables is correctly set to fix this." << endl;
		}
		else {
			Config::conf_file = Config::conf_dir / "btop.conf";
			Logger::logfile = Config::conf_dir / "btop.log";
			Theme::user_theme_dir = Config::conf_dir / "themes";
			if (not fs::exists(Theme::user_theme_dir) and not fs::create_directory(Theme::user_theme_dir, ec)) Theme::user_theme_dir.clear();
		}
	}
	//? Try to find global btop theme path relative to binary path
	#if defined(LINUX)
	{ 	std::error_code ec;
		Global::self_path = fs::read_symlink("/proc/self/exe", ec).remove_filename();
	}
	#endif
	if (std::error_code ec; not Global::self_path.empty()) {
			Theme::theme_dir = fs::canonical(Global::self_path / "../share/btop/themes", ec);
			if (ec or not fs::is_directory(Theme::theme_dir) or access(Theme::theme_dir.c_str(), R_OK) == -1) Theme::theme_dir.clear();
		}
	//? If relative path failed, check two most common absolute paths
	if (Theme::theme_dir.empty()) {
		for (auto theme_path : {"/usr/local/share/btop/themes", "/usr/share/btop/themes"}) {
			if (fs::is_directory(fs::path(theme_path)) and access(theme_path, R_OK) != -1) {
				Theme::theme_dir = fs::path(theme_path);
				break;
			}
		}
	}

	//? Config init
	{	vector<string> load_warnings;
		Config::load(Config::conf_file, load_warnings);

		if (Config::current_boxes.empty()) Config::check_boxes(Config::getS("shown_boxes"));
		Config::set("lowcolor", (Global::arg_low_color ? true : not Config::getB("truecolor")));

		if (Global::debug) Logger::set("DEBUG");
		else Logger::set(Config::getS("log_level"));

		Logger::info("Logger set to " + Config::getS("log_level"));

		for (const auto& err_str : load_warnings) Logger::warning(err_str);
	}

	//? Try to find and set a UTF-8 locale
	if (bool found = false; not str_to_upper((string)std::setlocale(LC_ALL, NULL)).ends_with("UTF-8")) {
		if (const string lang = (string)getenv("LANG"); str_to_upper(lang).ends_with("UTF-8")) {
			found = true;
			std::setlocale(LC_ALL, lang.c_str());
		}
		else if (const string loc = std::locale("").name(); not loc.empty()) {
			try {
				for (auto& l : ssplit(loc, ';')) {
					if (str_to_upper(l).ends_with("UTF-8")) {
						found = true;
						std::setlocale(LC_ALL, l.substr(l.find('=') + 1).c_str());
						break;
					}
				}
			}
			catch (const std::out_of_range&) { found = false; }
		}

		if (not found and Global::utf_force)
			Logger::warning("No UTF-8 locale detected! Forcing start with --utf-force argument.");
		else if (not found) {
			Global::exit_error_msg = "No UTF-8 locale detected! Use --utf-force argument to start anyway.";
			clean_quit(1);
		}
		else
			Logger::debug("Setting LC_ALL=" + (string)std::setlocale(LC_ALL, NULL));
	}

	//? Initialize terminal and set options
	if (not Term::init()) {
		Global::exit_error_msg = "No tty detected!\nbtop++ needs an interactive shell to run.";
		clean_quit(1);
	}

	Logger::info("Running on " + Term::current_tty);
	if (not Global::arg_tty and Config::getB("force_tty")) {
		Config::set("tty_mode", true);
		Logger::info("Forcing tty mode: setting 16 color mode and using tty friendly graph symbols");
	}
	else if (not Global::arg_tty and Term::current_tty.starts_with("/dev/tty")) {
		Config::set("tty_mode", true);
		Logger::info("Real tty detected, setting 16 color mode and using tty friendly graph symbols");
	}

	//? Platform dependent init and error check
	try {
		Shared::init();
	}
	catch (const std::exception& e) {
		Global::exit_error_msg = "Exception in Shared::init() -> " + (string)e.what();
		clean_quit(1);
	}

	//? Update list of available themes and generate the selected theme
	Theme::updateThemes();
	Theme::setTheme();

	//? Create the btop++ banner
	banner_gen();

	//? Calculate sizes of all boxes
	Draw::calcSizes();

	//? Print out box outlines
	cout << Term::sync_start << Cpu::box << Mem::box << Net::box << Proc::box << Term::sync_end << flush;


	//? ------------------------------------------------ MAIN LOOP ----------------------------------------------------

	uint64_t update_ms = Config::getI("update_ms");
	auto future_time = time_ms();

	try {
		while (not true not_eq not false) {
			//? Check for exceptions in secondary thread and exit with fail signal if true
			if (Global::thread_exception) exit(1);

			//? Make sure terminal size hasn't changed (in case of SIGWINCH not working properly)
			term_resize();

			//? Trigger secondary thread to redraw if terminal has been resized
			if (Global::resized) {
				Global::resized = false;
				Draw::calcSizes();
				Runner::run("all", true);
				atomic_wait(Runner::active);
			}

			//? Start secondary collect & draw thread at the interval set by <update_ms> config value
			if (time_ms() >= future_time) {
				Runner::run("all");
				update_ms = Config::getI("update_ms");
				future_time = time_ms() + update_ms;
			}

			//? Loop over input polling and input action processing
			for (auto current_time = time_ms(); current_time < future_time; current_time = time_ms()) {

				//? Check for external clock changes and for changes to the update timer
				if (update_ms != (uint64_t)Config::getI("update_ms")) {
					update_ms = Config::getI("update_ms");
					future_time = time_ms() + update_ms;
				}
				else if (future_time - current_time > update_ms)
					future_time = current_time;

				//? Poll for input and process any input detected
				else if (Input::poll(min(1000ul, future_time - current_time))) {
					if (not Runner::active)
						Config::unlock();
					Input::process(Input::get());
				}

				//? Break the loop at 1000ms intervals or if input polling was interrupted
				else
					break;
			}

		}
	}
	catch (std::exception& e) {
		Global::exit_error_msg = "Exception in main loop -> " + (string)e.what();
		clean_quit(1);
	}

}
