/*
main.c - Entry point
Copyright (C) 2021  Mr0maks <mr.maks0443@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "compiler.h"
#include "mem_ops.h"
#include "rvvm.h"
//#include "devices/ata.h"

#include <stdio.h>

#include "devices/clint.h"
#include "devices/plic.h"
#include "devices/ns16550a.h"
#include "devices/ata.h"
#include "devices/fb_window.h"
#include "devices/ps2-altera.h"
#include "devices/ps2-keyboard.h"
#include "devices/ps2-mouse.h"

#ifdef _WIN32
// For unicode fix
#include <windows.h>
#endif

#ifdef USE_NET
#include "devices/eth-oc.h"
#endif

#ifndef VERSION
#define VERSION "v0.4"
#endif

typedef struct {
    const char* bootrom;
    const char* kernel;
    const char* dtb;
    const char* image;
    size_t mem;
    uint32_t smp;
    bool rv64;
    bool sbi_align_fix;
} vm_args_t;

static size_t get_arg(const char** argv, const char** arg_name, const char** arg_val)
{
    if (argv[0][0] == '-') {
        size_t offset = (argv[0][1] == '-') ? 2 : 1;
        *arg_name = &argv[0][offset];
        for (size_t i=0; argv[0][offset + i] != 0; ++i) {
            if (argv[0][offset + i] == '=') {
                // Argument format -arg=val
                *arg_val = &argv[0][offset + i + 1];
                return 1;
            }
        }
        
        if (argv[1] == NULL || argv[1][0] == '-') {
            // Argument format -arg
            *arg_val = "";
            return 1;
        } else {
            // Argument format -arg val
            *arg_val = argv[1];
            return 2;
        }
    } else {
        *arg_name = "bootrom";
        *arg_val = argv[0];
        return 1;
    }
}

static inline size_t mem_suffix_shift(char suffix)
{
    switch (suffix) {
        case 'k': return 10;
        case 'K': return 10;
        case 'M': return 20;
        case 'G': return 30;
        default: return 0;
    }
}

static inline bool cmp_arg(const char* arg, const char* name)
{
    for (size_t i=0; arg[i] != 0 && arg[i] != '='; ++i) {
        if (arg[i] != name[i]) return false;
    }
    return true;
}

static void print_help()
{
#ifdef _WIN32
    const wchar_t* help = L"\n"
#else
    printf("\n"
#endif
           "RVVM "VERSION"\n"
           "\n"
           "  ██▀███   ██▒   █▓ ██▒   █▓ ███▄ ▄███▓\n"
           " ▓██ ▒ ██▒▓██░   █▒▓██░   █▒▓██▒▀█▀ ██▒\n"
           " ▓██ ░▄█ ▒ ▓██  █▒░ ▓██  █▒░▓██    ▓██░\n"
           " ▒██▀▀█▄    ▒██ █░░  ▒██ █░░▒██    ▒██ \n"
           " ░██▓ ▒██▒   ▒▀█░     ▒▀█░  ▒██▒   ░██▒\n"
           " ░ ▒▓ ░▒▓░   ░ ▐░     ░ ▐░  ░ ▒░   ░  ░\n"
           "   ░▒ ░ ▒░   ░ ░░     ░ ░░  ░  ░      ░\n"
           "   ░░   ░      ░░       ░░  ░      ░   \n"
           "    ░           ░        ░         ░   \n"
           "               ░        ░              \n"
           "\n"
           "Usage: rvvm [-mem 256M] [-smp 1] [-dtb ...] ... [bootrom]\n"
           "\n"
           "    -mem <amount>    Memory amount, default: 256M\n"
           "    -smp <count>     Cores count, default: 1\n"
           "    -rv64            Enable 64-bit RISC-V, 32-bit by default\n"
           "    -dtb <file>      Pass Device Tree Blob to the machine\n"
           "    -image <file>    Attach hard drive with raw image\n"
           "    -verbose         Enable verbose logging\n"
           "    -help            Show this help message\n"
           "    [bootrom]        Machine bootrom (SBI, BBL, etc)\n"
#ifdef _WIN32
           "\n";
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), help, wcslen(help), NULL, NULL);
#else
           "\n");
#endif
}

static bool parse_args(int argc, const char** argv, vm_args_t* args)
{
    const char* arg_name = "";
    const char* arg_val = "";
    
    // Default params: 1 core, 256M ram
    args->smp = 1;
    args->mem = 256 << 20;
    
    for (int i=1; i<argc;) {
        i += get_arg(argv + i, &arg_name, &arg_val);
        if (cmp_arg(arg_name, "dtb")) {
            args->dtb = arg_val;
        } else if (cmp_arg(arg_name, "image")) {
            args->image = arg_val;
        } else if (cmp_arg(arg_name, "bootrom")) {
            args->bootrom = arg_val;
        } else if (cmp_arg(arg_name, "kernel")) {
            args->kernel = arg_val;
        } else if (cmp_arg(arg_name, "mem")) {
            if (arg_name[0])
                args->mem = ((size_t)atoi(arg_val)) << mem_suffix_shift(arg_val[strlen(arg_val)-1]);
        } else if (cmp_arg(arg_name, "smp")) {
            args->smp = atoi(arg_val);
            if (args->smp > 1024) {
                rvvm_error("Invalid cores count specified: %s", arg_val);
                return false;
            }
        } else if (cmp_arg(arg_name, "rv64")) {
            args->rv64 = true;
        } else if (cmp_arg(arg_name, "verbose")) {
            rvvm_set_loglevel(LOG_INFO);
        } else if (cmp_arg(arg_name, "help")
                 || cmp_arg(arg_name, "h")
                 || cmp_arg(arg_name, "H")) {
            print_help();
            return false;
        } else {
            rvvm_error("Unknown argument \"%s\"\n", arg_name);
            return false;
        }
    }
    return true;
}

static bool load_file_to_ram(rvvm_machine_t* machine, paddr_t addr, const char* filename)
{
    FILE* file = fopen(filename, "rb");
    size_t fsize;
    uint8_t* buffer;
    
    if (file == NULL) {
        rvvm_error("Cannot open file %s", filename);
        return false;
    }
    
    fseek(file, 0, SEEK_END);
    fsize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    buffer = safe_malloc(fsize);
    
    fread(buffer, fsize, 1, file);
    
    if (!rvvm_write_ram(machine, addr, buffer, fsize)) {
        rvvm_error("File %s does not fit in RAM", filename);
        fclose(file);
        free(buffer);
        return false;
    }

    fclose(file);
    free(buffer);
    return true;
}

static int rvvm_run_with_args(vm_args_t args)
{
    rvvm_machine_t* machine = rvvm_create_machine(RVVM_DEFAULT_MEMBASE, args.mem, args.smp, args.rv64);
    if (machine == NULL) {
        rvvm_error("VM creation failed");
        return 1;
    } else if (!load_file_to_ram(machine, machine->mem.begin, args.bootrom)) {
        rvvm_error("Failed to load bootrom");
        return 1;
    }

    if (args.dtb) {
        paddr_t dtb_addr = machine->mem.begin + machine->mem.size - 0x2000;

        if (!load_file_to_ram(machine, dtb_addr, args.dtb)) {
            rvvm_error("Failed to load DTB");
            return 1;
        }

        // pass DTB address in a1 register of each hart
        vector_foreach(machine->harts, i) {
            vector_at(machine->harts, i).registers[REGISTER_X11] = dtb_addr;
        }
    }
    
    if (args.image) {
        FILE *fp = fopen(args.image, "rb+");
        if (fp == NULL) {
            rvvm_error("Unable to open image file %s", args.image);
        } else {
            ata_init(machine, 0x40000000, 0x40001000, fp, NULL);
        }
    }
    
    clint_init(machine, 0x2000000);
    ns16550a_init(machine, 0x10000000);
    
    void *plic_data = plic_init(machine, 0xC000000);

    static struct ps2_device ps2_mouse;
    ps2_mouse = ps2_mouse_create();
    altps2_init(machine, 0x20000000, plic_data, 1, &ps2_mouse);

    static struct ps2_device ps2_keyboard;
    ps2_keyboard = ps2_keyboard_create();
    altps2_init(machine, 0x20001000, plic_data, 2, &ps2_keyboard);
    
    init_fb(machine, 0x30000000, 640, 480, &ps2_mouse, &ps2_keyboard);
#ifdef USE_NET
    ethoc_init(machine, 0x21000000, plic_data, 3);
#endif
    rvvm_enable_builtin_eventloop(false);
    rvvm_start_machine(machine);
    rvvm_run_eventloop(); // Returns on machine shutdown
    
    rvvm_free_machine(machine);
    
    return 0;
}

int main(int argc, const char** argv)
{
    vm_args_t args = {0};
    rvvm_set_loglevel(LOG_WARN);
    
    // let the vm be run by simple double-click, heh
    if (!parse_args(argc, argv, &args)) return 0;
    if (args.bootrom == NULL) {
        printf("Usage: %s [-help] [-mem 256M] [-rv64] ... [bootrom]\n", argv[0]);
        return 0;
    }

    rvvm_run_with_args(args);
    return 0;
}

