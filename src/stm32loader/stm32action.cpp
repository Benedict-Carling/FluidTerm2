#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "init.h"
#include "utils.h"
#include "serial.h"
#include "stm32.h"
#include "parsers/parser.h"
#include "port.h"

#include "parsers/binary.h"
#include "parsers/hex.h"

#if defined(__WIN32__) || defined(__CYGWIN__)
#    include <windows.h>
#endif
#include "../windows/FileDialog.h"

//
// Replace the include directive at line 996
#ifdef _WIN32
#include "../windows/SerialPort.h"
#else
#include "../mac/SerialPort.h"
#endif

#ifdef __APPLE__
#include "../mac/FileDialog.h"
#endif

/* device globals */
stm32_t*               stm    = NULL;
void*                  p_st   = NULL;
parser_t*              parser = NULL;
struct port_interface* port   = NULL;

/* settings */
struct port_options port_opts = {
    .device = "auto",
    //    .baudRate     = SERIAL_BAUD_115200,
    .baudRate     = 115200,
    .serial_mode  = "8n1",
    .bus_addr     = 0,
    .rx_frame_max = STM32_MAX_RX_FRAME,
    .tx_frame_max = STM32_MAX_TX_FRAME,
};

enum actions { ACT_NONE, ACT_READ, ACT_WRITE, ACT_WRITE_UNPROTECT, ACT_READ_PROTECT, ACT_READ_UNPROTECT, ACT_ERASE_ONLY, ACT_CRC };

enum actions action       = ACT_NONE;
int          npages       = 0;
int          spage        = 0;
int          no_erase     = 0;
char         verify       = 0;
int          retry        = 10;
char         exec_flag    = 0;
uint32_t     execute      = 0;
char         init_flag    = 1;
int          use_stdinout = 0;
char         force_binary = 0;
FILE*        diag;
char         reset_flag = 0;
const char*  filename;
char*        gpio_seq      = NULL;
uint32_t     start_addr    = 0;
uint32_t     readwrite_len = 0;

static void init_options() {
    npages       = 0;
    spage        = 0;
    no_erase     = 0;
    verify       = 0;
    retry        = 10;
    exec_flag    = 0;
    execute      = 0;
    init_flag    = 1;
    use_stdinout = 0;
    force_binary = 0;
    diag;
    reset_flag    = 0;
    gpio_seq      = NULL;
    start_addr    = 0;
    readwrite_len = 0;
}

/* functions */

static const char* action2str(enum actions act) {
    switch (act) {
        case ACT_READ:
            return "memory read";
        case ACT_WRITE:
            return "memory write";
        case ACT_WRITE_UNPROTECT:
            return "write unprotect";
        case ACT_READ_PROTECT:
            return "read protect";
        case ACT_READ_UNPROTECT:
            return "read unprotect";
        case ACT_ERASE_ONLY:
            return "flash erase";
        case ACT_CRC:
            return "memory crc";
        default:
            return "";
    };
}

static void err_multi_action(enum actions newaction) {
    fprintf(stderr,
            "ERROR: Invalid options !\n"
            "\tCan't execute \"%s\" and \"%s\" at the same time.\n",
            action2str(action),
            action2str(newaction));
}

static int is_addr_in_ram(uint32_t addr) {
    return addr >= stm->dev->ram_start && addr < stm->dev->ram_end;
}

static int is_addr_in_flash(uint32_t addr) {
    return addr >= stm->dev->fl_start && addr < stm->dev->fl_end;
}

static int is_addr_in_opt_bytes(uint32_t addr) {
    /* option bytes upper range is inclusive in our device table */
    return addr >= stm->dev->opt_start && addr <= stm->dev->opt_end;
}

static int is_addr_in_sysmem(uint32_t addr) {
    return addr >= stm->dev->mem_start && addr < stm->dev->mem_end;
}

/* returns the page that contains address "addr" */
static int flash_addr_to_page_floor(uint32_t addr) {
    int       page;
    uint32_t* psize;

    if (!is_addr_in_flash(addr))
        return 0;

    page = 0;
    addr -= stm->dev->fl_start;
    psize = stm->dev->fl_ps;

    while (addr >= psize[0]) {
        addr -= psize[0];
        page++;
        if (psize[1])
            psize++;
    }

    return page;
}

/* returns the first page whose start addr is >= "addr" */
int flash_addr_to_page_ceil(uint32_t addr) {
    int       page;
    uint32_t* psize;

    if (!(addr >= stm->dev->fl_start && addr <= stm->dev->fl_end))
        return 0;

    page = 0;
    addr -= stm->dev->fl_start;
    psize = stm->dev->fl_ps;

    while (addr >= psize[0]) {
        addr -= psize[0];
        page++;
        if (psize[1])
            psize++;
    }

    return addr ? page + 1 : page;
}

/* returns the lower address of flash page "page" */
static uint32_t flash_page_to_addr(int page) {
    int      i;
    uint32_t addr, *psize;

    addr  = stm->dev->fl_start;
    psize = stm->dev->fl_ps;

    for (i = 0; i < page; i++) {
        addr += psize[0];
        if (psize[1])
            psize++;
    }

    return addr;
}

int parse_options(int argc, char* argv[]);

int stm32main(int argc, char* argv[]) {
    int          ret = 1;
    stm32_err_t  s_err;
    parser_err_t perr;

    uint8_t      buffer[256];
    uint32_t     addr, start, end;
    unsigned int len;
    int          failed = 0;
    int          first_page, num_pages;

    diag = stdout;

    if (parse_options(argc, argv) != 0)
        goto close;

    if (action == ACT_READ && use_stdinout) {
        diag = stderr;
    }

    if (action == ACT_WRITE) {
        /* first try hex */
        if (!force_binary) {
            parser = &PARSER_HEX;
            p_st   = parser->init();
            if (!p_st) {
                fprintf(stderr, "%s Parser failed to initialize\n", parser->name);
                goto close;
            }
        }

        if (force_binary || (perr = parser->open(p_st, filename, 0)) != PARSER_ERR_OK) {
            if (force_binary || perr == PARSER_ERR_INVALID_FILE) {
                if (!force_binary) {
                    parser->close(p_st);
                    p_st = NULL;
                }

                /* now try binary */
                parser = &PARSER_BINARY;
                p_st   = parser->init();
                if (!p_st) {
                    fprintf(stderr, "%s Parser failed to initialize\n", parser->name);
                    goto close;
                }
                perr = parser->open(p_st, filename, 0);
            }

            /* if still have an error, fail */
            if (perr != PARSER_ERR_OK) {
                fprintf(stderr, "%s ERROR: %s\n", parser->name, parser_errstr(perr));
                if (perr == PARSER_ERR_SYSTEM)
                    perror(filename);
                goto close;
            }
        }

        fprintf(diag, "Using Parser : %s\n", parser->name);
    } else {
        parser = &PARSER_BINARY;
        p_st   = parser->init();
        if (!p_st) {
            fprintf(stderr, "%s Parser failed to initialize\n", parser->name);
            goto close;
        }
    }

    if (port_open(&port_opts, &port) != PORT_ERR_OK) {
        fprintf(stderr, "Failed to open port: %s\n", port_opts.device);
        goto close;
    }

    //    fprintf(diag, "Interface %s: %s\n", port->name, port->get_cfg_str(port));
    if (init_flag && init_bl_entry(port, gpio_seq)) {
        ret = 1;
        fprintf(stderr, "Failed to send boot enter sequence\n");
        goto close;
    }

    port->flush(port);

    stm = stm32_init(port, init_flag);
    if (!stm)
        goto close;

    fprintf(diag, "Version      : 0x%02x\n", stm->bl_version);
    if (port->flags & PORT_GVR_ETX) {
        fprintf(diag, "Option 1     : 0x%02x\n", stm->option1);
        fprintf(diag, "Option 2     : 0x%02x\n", stm->option2);
    }
    fprintf(diag, "Device ID    : 0x%04x (%s)\n", stm->pid, stm->dev->name);
    fprintf(diag,
            "- RAM        : Up to %dKiB  (%db reserved by bootloader)\n",
            (stm->dev->ram_end - 0x20000000) / 1024,
            stm->dev->ram_start - 0x20000000);
    fprintf(diag,
            "- Flash      : Up to %dKiB (size first sector: %dx%d)\n",
            (stm->dev->fl_end - stm->dev->fl_start) / 1024,
            stm->dev->fl_pps,
            stm->dev->fl_ps[0]);
    fprintf(diag, "- Option RAM : %db\n", stm->dev->opt_end - stm->dev->opt_start + 1);
    fprintf(diag, "- System RAM : %dKiB\n", (stm->dev->mem_end - stm->dev->mem_start) / 1024);

    /*
	 * Cleanup addresses:
	 *
	 * Starting from options
	 *	start_addr, readwrite_len, spage, npages
	 * and using device memory size, compute
	 *	start, end, first_page, num_pages
	 */
    if (start_addr || readwrite_len) {
        start = start_addr;

        if (is_addr_in_flash(start))
            end = stm->dev->fl_end;
        else {
            no_erase = 1;
            if (is_addr_in_ram(start))
                end = stm->dev->ram_end;
            else if (is_addr_in_opt_bytes(start))
                end = stm->dev->opt_end + 1;
            else if (is_addr_in_sysmem(start))
                end = stm->dev->mem_end;
            else {
                /* Unknown territory */
                if (readwrite_len)
                    end = start + readwrite_len;
                else
                    end = start + sizeof(uint32_t);
            }
        }

        if (readwrite_len && (end > start + readwrite_len))
            end = start + readwrite_len;

        first_page = flash_addr_to_page_floor(start);
        if (!first_page && end == stm->dev->fl_end)
            num_pages = STM32_MASS_ERASE;
        else
            num_pages = flash_addr_to_page_ceil(end) - first_page;
    } else if (!spage && !npages) {
        start      = stm->dev->fl_start;
        end        = stm->dev->fl_end;
        first_page = 0;
        num_pages  = STM32_MASS_ERASE;
    } else {
        first_page = spage;
        start      = flash_page_to_addr(first_page);
        if (start > stm->dev->fl_end) {
            fprintf(stderr, "Address range exceeds flash size.\n");
            goto close;
        }

        if (npages) {
            num_pages = npages;
            end       = flash_page_to_addr(first_page + num_pages);
            if (end > stm->dev->fl_end)
                end = stm->dev->fl_end;
        } else {
            end       = stm->dev->fl_end;
            num_pages = flash_addr_to_page_ceil(end) - first_page;
        }

        if (!first_page && end == stm->dev->fl_end)
            num_pages = STM32_MASS_ERASE;
    }

    if (action == ACT_READ) {
        unsigned int max_len = port_opts.rx_frame_max;

        perr = parser->open(p_st, filename, 1);
        if (perr != PARSER_ERR_OK) {
            fprintf(stderr, "%s ERROR: %s\n", parser->name, parser_errstr(perr));
            if (perr == PARSER_ERR_SYSTEM)
                perror(filename);
            goto close;
        }

        fflush(diag);
        addr = start;
        while (addr < end) {
            uint32_t left = end - addr;
            len           = max_len > left ? left : max_len;
            s_err         = stm32_read_memory(stm, addr, buffer, len);
            if (s_err != STM32_ERR_OK) {
                fprintf(stderr, "Failed to read memory at address 0x%08x, target write-protected?\n", addr);
                goto close;
            }
            if (parser->write(p_st, buffer, len) != PARSER_ERR_OK) {
                fprintf(stderr, "Failed to write data to file\n");
                goto close;
            }
            addr += len;

            fprintf(diag, "\rRead address 0x%08x (%.2f%%) ", addr, (100.0f / (float)(end - start)) * (float)(addr - start));
            fflush(diag);
        }
        fprintf(diag, "Done.\n");
        ret = 0;
        goto close;
    } else if (action == ACT_READ_PROTECT) {
        fprintf(diag, "Read-Protecting flash\n");
        /* the device automatically performs a reset after the sending the ACK */
        reset_flag = 0;
        s_err      = stm32_readprot_memory(stm);
        if (s_err != STM32_ERR_OK) {
            fprintf(stderr, "Failed to read-protect flash\n");
            goto close;
        }
        fprintf(diag, "Done.\n");
        ret = 0;
    } else if (action == ACT_READ_UNPROTECT) {
        fprintf(diag, "Read-UnProtecting flash\n");
        /* the device automatically performs a reset after the sending the ACK */
        reset_flag = 0;
        s_err      = stm32_runprot_memory(stm);
        if (s_err != STM32_ERR_OK) {
            fprintf(stderr, "Failed to read-unprotect flash\n");
            goto close;
        }
        fprintf(diag, "Done.\n");
        ret = 0;
    } else if (action == ACT_ERASE_ONLY) {
        ret = 0;
        fprintf(diag, "Erasing flash\n");

        if (num_pages != STM32_MASS_ERASE && (start != flash_page_to_addr(first_page) || end != flash_page_to_addr(first_page + num_pages))) {
            fprintf(stderr, "Specified start & length are invalid (must be page aligned)\n");
            ret = 1;
            goto close;
        }

        s_err = stm32_erase_memory(stm, first_page, num_pages);
        if (s_err != STM32_ERR_OK) {
            fprintf(stderr, "Failed to erase memory\n");
            ret = 1;
            goto close;
        }
        ret = 0;
    } else if (action == ACT_WRITE_UNPROTECT) {
        fprintf(diag, "Write-unprotecting flash\n");
        /* the device automatically performs a reset after the sending the ACK */
        reset_flag = 0;
        s_err      = stm32_wunprot_memory(stm);
        if (s_err != STM32_ERR_OK) {
            fprintf(stderr, "Failed to write-unprotect flash\n");
            goto close;
        }
        fprintf(diag, "Done.\n");
        ret = 0;
    } else if (action == ACT_WRITE) {
        fprintf(diag, "Write to memory\n");

        unsigned int offset = 0;
        unsigned int r;
        unsigned int size;
        unsigned int max_wlen, max_rlen;

        max_wlen = port_opts.tx_frame_max - 2; /* skip len and crc */
        max_wlen &= ~3;                        /* 32 bit aligned */

        max_rlen = port_opts.rx_frame_max;
        max_rlen = max_rlen < max_wlen ? max_rlen : max_wlen;

        /* Assume data from stdin is whole device */
        if (use_stdinout)
            size = end - start;
        else
            size = parser->size(p_st);

        // TODO: It is possible to write to non-page boundaries, by reading out flash
        //       from partial pages and combining with the input data
        // if ((start % stm->dev->fl_ps[i]) != 0 || (end % stm->dev->fl_ps[i]) != 0) {
        //	fprintf(stderr, "Specified start & length are invalid (must be page aligned)\n");
        //	goto close;
        // }

        // TODO: If writes are not page aligned, we should probably read out existing flash
        //       contents first, so it can be preserved and combined with new data
        if (!no_erase && num_pages) {
            fprintf(diag, "Erasing memory\n");
            s_err = stm32_erase_memory(stm, first_page, num_pages);
            if (s_err != STM32_ERR_OK) {
                fprintf(stderr, "Failed to erase memory\n");
                goto close;
            }
        }

        fflush(diag);
        addr = start;
        while (addr < end && offset < size) {
            uint32_t left = end - addr;
            len           = max_wlen > left ? left : max_wlen;
            len           = len > size - offset ? size - offset : len;

            if (parser->read(p_st, buffer, &len) != PARSER_ERR_OK)
                goto close;

            if (len == 0) {
                if (use_stdinout) {
                    break;
                } else {
                    fprintf(stderr, "Failed to read input file\n");
                    goto close;
                }
            }

        again:
            s_err = stm32_write_memory(stm, addr, buffer, len);
            if (s_err != STM32_ERR_OK) {
                fprintf(stderr, "Failed to write memory at address 0x%08x\n", addr);
                goto close;
            }

            if (verify) {
                uint8_t      compare[len];
                unsigned int offset, rlen;

                offset = 0;
                while (offset < len) {
                    rlen  = len - offset;
                    rlen  = rlen < max_rlen ? rlen : max_rlen;
                    s_err = stm32_read_memory(stm, addr + offset, compare + offset, rlen);
                    if (s_err != STM32_ERR_OK) {
                        fprintf(stderr, "Failed to read memory at address 0x%08x\n", addr + offset);
                        goto close;
                    }
                    offset += rlen;
                }

                for (r = 0; r < len; ++r)
                    if (buffer[r] != compare[r]) {
                        if (failed == retry) {
                            fprintf(stderr,
                                    "Failed to verify at address 0x%08x, expected 0x%02x and found 0x%02x\n",
                                    (uint32_t)(addr + r),
                                    buffer[r],
                                    compare[r]);
                            goto close;
                        }
                        ++failed;
                        goto again;
                    }

                failed = 0;
            }

            addr += len;
            offset += len;

            fprintf(diag, "\rWrote %saddress 0x%08x (%.2f%%) ", verify ? "and verified " : "", addr, (100.0f / size) * offset);
            fflush(diag);
        }

        fprintf(diag, "Done.\n");
        ret = 0;
        goto close;
    } else if (action == ACT_CRC) {
        uint32_t crc_val = 0;

        fprintf(diag, "CRC computation\n");

        s_err = stm32_crc_wrapper(stm, start, end - start, &crc_val);
        if (s_err != STM32_ERR_OK) {
            fprintf(stderr, "Failed to read CRC\n");
            goto close;
        }
        fprintf(diag, "CRC(0x%08x-0x%08x) = 0x%08x\n", start, end, crc_val);
        ret = 0;
        goto close;
    } else
        ret = 0;

close:
    if (stm && exec_flag && ret == 0) {
        if (execute == 0)
            execute = stm->dev->fl_start;

        fprintf(diag, "\nStarting execution at address 0x%08x... ", execute);
        fflush(diag);
        if (stm32_go(stm, execute) == STM32_ERR_OK) {
            reset_flag = 0;
            fprintf(diag, "done.\n");
        } else
            fprintf(diag, "failed.\n");
    }

    if (stm && reset_flag) {
        fprintf(diag, "\nResetting device... \n");
        fflush(diag);
        if (init_bl_exit(stm, port, gpio_seq)) {
            ret = 1;
            fprintf(diag, "Reset failed.\n");
        } else
            fprintf(diag, "Reset done.\n");
    } else if (port) {
        /* Always run exit sequence if present */
        if (gpio_seq && strchr(gpio_seq, ':'))
            ret = gpio_bl_exit(port, gpio_seq) || ret;
    }

    if (p_st)
        parser->close(p_st);
    if (stm)
        stm32_close(stm);
    if (port)
        port->close(port);

    fprintf(diag, "\n");
    return ret;
}

void show_help(char* name);
int  parse_options(int argc, char* argv[]) {
    int   c;
    char* pLen;

    init_options();
#if 0
     const char* opts = "a:b:m:r:w:e:vhn:g:jkfcChuos:S:F:i:R";
#else
    const char* opts = "p:b:m:rwe:vhn:g:jkfcChuos:S:F:R";
#endif
    optind = 1;  // Because parse_options can be called multiple times
    action = ACT_NONE;
    while (1) {
        if ((c = getopt(argc, argv, opts)) == -1) {
            break;
        }
        switch (c) {
            case 'p':
                port_opts.device = strdup(optarg);
                break;
#if 0
             case 'a':
                port_opts.bus_addr = strtoul(optarg, NULL, 0);
                break;
#endif

            case 'b':
#if 0
                port_opts.baudRate = serial_get_baud(strtoul(optarg, NULL, 0));
                if (port_opts.baudRate == SERIAL_BAUD_INVALID) {
                    serial_baud_t baudrate;
                    fprintf(stderr, "Invalid baud rate, valid options are:\n");
                    for (baudrate = SERIAL_BAUD_1200; baudrate != SERIAL_BAUD_INVALID; ++(int)baudrate)
                        fprintf(stderr, " %d\n", serial_get_baud_int(baudrate));
                    return 1;
                }
#else
                port_opts.baudRate = strtoul(optarg, NULL, 0);
#endif
                break;

            case 'm':
#if 0
                if (strlen(optarg) != 3 || serial_get_bits(optarg) == SERIAL_BITS_INVALID ||
                    serial_get_parity(optarg) == SERIAL_PARITY_INVALID || serial_get_stopbit(optarg) == SERIAL_STOPBIT_INVALID) {
                    fprintf(stderr, "Invalid serial mode\n");
                    return 1;
                }
#else
                if (strlen(optarg) != 3) {
                    fprintf(stderr, "Invalid serial mode\n");
                    return 1;
                }
#endif
                port_opts.serial_mode = optarg;
                break;

            case 'r':
            case 'w':
#if 0
                filename = optarg;
                if (filename[0] == '-' && filename[1] == '\0') {
                    use_stdinout = 1;
                    force_binary = 1;
                }
#else
                if (c == 'w') {
                    filename = getFileName("Bin or Hex\0*.bin;*.hex\0All Files\0*.*\0\0", false);
                } else {
                    filename = getFileName("Binary\0*.bin\0All Files\0*.*\0\0", true);
                }
                if (*filename == '\0') {
                    fprintf(stderr, "No file selected\n");
                    return 1;
                }
#endif
                if (action != ACT_NONE) {
                    err_multi_action((c == 'r') ? ACT_READ : ACT_WRITE);
                    return 1;
                }
                action = (c == 'r') ? ACT_READ : ACT_WRITE;
                break;
            case 'e':
                if (readwrite_len || start_addr) {
                    fprintf(stderr, "ERROR: Invalid options, can't specify start page / num pages and start address/length\n");
                    return 1;
                }
                npages = strtoul(optarg, NULL, 0);
                if (npages > STM32_MAX_PAGES || npages < 0) {
                    fprintf(stderr, "ERROR: You need to specify a page count between 0 and 255");
                    return 1;
                }
                if (!npages)
                    no_erase = 1;
                break;
            case 'u':
                if (action != ACT_NONE) {
                    err_multi_action(ACT_WRITE_UNPROTECT);
                    return 1;
                }
                action = ACT_WRITE_UNPROTECT;
                break;

            case 'j':
                if (action != ACT_NONE) {
                    err_multi_action(ACT_READ_PROTECT);
                    return 1;
                }
                action = ACT_READ_PROTECT;
                break;

            case 'k':
                if (action != ACT_NONE) {
                    err_multi_action(ACT_READ_UNPROTECT);
                    return 1;
                }
                action = ACT_READ_UNPROTECT;
                break;

            case 'o':
                if (action != ACT_NONE) {
                    err_multi_action(ACT_ERASE_ONLY);
                    return 1;
                }
                action = ACT_ERASE_ONLY;
                break;

            case 'v':
                verify = 1;
                break;

            case 'n':
                retry = strtoul(optarg, NULL, 0);
                break;

            case 'g':
                exec_flag = 1;
                execute   = strtoul(optarg, NULL, 0);
                if (execute % 4 != 0) {
                    fprintf(stderr, "ERROR: Execution address must be word-aligned\n");
                    return 1;
                }
                break;
            case 's':
                if (readwrite_len || start_addr) {
                    fprintf(stderr, "ERROR: Invalid options, can't specify start page / num pages and start address/length\n");
                    return 1;
                }
                spage = strtoul(optarg, NULL, 0);
                break;
            case 'S':
                if (spage || npages) {
                    fprintf(stderr, "ERROR: Invalid options, can't specify start page / num pages and start address/length\n");
                    return 1;
                } else {
                    start_addr = strtoul(optarg, &pLen, 0);
                    if (*pLen == ':') {
                        pLen++;
                        readwrite_len = strtoul(pLen, NULL, 0);
                        if (readwrite_len == 0) {
                            fprintf(stderr, "ERROR: Invalid options, can't specify zero length\n");
                            return 1;
                        }
                    }
                }
                break;
            case 'F':
                port_opts.rx_frame_max = strtoul(optarg, &pLen, 0);
                if (*pLen == ':') {
                    pLen++;
                    port_opts.tx_frame_max = strtoul(pLen, NULL, 0);
                }
                if (port_opts.rx_frame_max < 0 || port_opts.tx_frame_max < 0) {
                    fprintf(stderr, "ERROR: Invalid negative value for option -F\n");
                    return 1;
                }
                if (port_opts.rx_frame_max == 0)
                    port_opts.rx_frame_max = STM32_MAX_RX_FRAME;
                if (port_opts.tx_frame_max == 0)
                    port_opts.tx_frame_max = STM32_MAX_TX_FRAME;
                if (port_opts.rx_frame_max < 20 || port_opts.tx_frame_max < 6) {
                    fprintf(stderr, "ERROR: current code cannot work with small frames.\n");
                    fprintf(stderr, "min(RX) = 20, min(TX) = 6\n");
                    return 1;
                }
                if (port_opts.rx_frame_max > STM32_MAX_RX_FRAME) {
                    fprintf(stderr, "WARNING: Ignore RX length in option -F\n");
                    port_opts.rx_frame_max = STM32_MAX_RX_FRAME;
                }
                if (port_opts.tx_frame_max > STM32_MAX_TX_FRAME) {
                    fprintf(stderr, "WARNING: Ignore TX length in option -F\n");
                    port_opts.tx_frame_max = STM32_MAX_TX_FRAME;
                }
                break;
            case 'h':
                show_help(argv[0]);
                break;
            case 'f':
                force_binary = 1;
                break;

            case 'c':
                init_flag = 0;
                break;

#if 0
            case 'i':
                gpio_seq = optarg;
                break;
#endif

            case 'R':
                reset_flag = 1;
                break;

            case 'C':
                if (action != ACT_NONE) {
                    err_multi_action(ACT_CRC);
                    return 1;
                }
                action = ACT_CRC;
                break;
            case '?':
                fprintf(stderr, "Invalid switch %s\n", optarg);
                show_help(argv[0]);
                return 1;
        }
    }

#if 0
    for (c = optind; c < argc; ++c) {
        if (port_opts.device) {
            fprintf(stderr, "ERROR: Invalid parameter specified\n");
            show_help(argv[0]);
            return 1;
        }
        port_opts.device = argv[c];
    }

    if (port_opts.device == NULL) {
        fprintf(stderr, "ERROR: Device not specified\n");
        show_help(argv[0]);
        return 1;
    }
#else
    if (optind != argc) {
        fprintf(stderr, "ERROR: Invalid parameter specified\n");
        show_help(argv[0]);
        return 1;
    }
#endif

    if ((action != ACT_WRITE) && verify) {
        fprintf(stderr, "ERROR: Invalid usage, -v is only valid when writing\n");
        show_help(argv[0]);
        return 1;
    }

    return 0;
}

void show_help(char* name) {
    fprintf(stderr,
#if 0
            "Usage: %s [-bvngfhc] [-[rw] filename] [tty_device | i2c_device]\n"
            "	-a bus_address	Bus address (e.g. for I2C port)\n"
            "	-b rate		Baud rate (default 57600)\n"
            "	-m mode		Serial port mode (default 8e1)\n"
            "	-r filename	Read flash to file (or - stdout)\n"
            "	-w filename	Write flash from file (or - stdout)\n"
            "	-C		Compute CRC of flash content\n"
            "	-u		Disable the flash write-protection\n"
            "	-j		Enable the flash read-protection\n"
            "	-k		Disable the flash read-protection\n"
            "	-o		Erase only\n"
            "	-e n		Only erase n pages before writing the flash\n"
            "	-v		Verify writes\n"
            "	-n count	Retry failed writes up to count times (default 10)\n"
            "	-g address	Start execution at specified address (0 = flash start)\n"
            "	-S address[:length]	Specify start address and optionally length for\n"
            "	                   	read/write/erase operations\n"
            "	-F RX_length[:TX_length]  Specify the max length of RX and TX frame\n"
            "	-s start_page	Flash at specified page (0 = flash start)\n"
            "	-f		Force binary parser\n"
            "	-h		Show this help\n"
            "	-c		Resume the connection (don't send initial INIT)\n"
            "			*Baud rate must be kept the same as the first init*\n"
            "			This is useful if the reset fails\n"
            "	-R		Reset device at exit.\n"
            "	-i GPIO_string	GPIO sequence to enter/exit bootloader mode\n"
            "			GPIO_string=[entry_seq][:[exit_seq]]\n"
            "			sequence=[[-]signal]&|,[sequence]\n"
            "\n"
            "GPIO sequence:\n"
            "	The following signals can appear in a sequence:\n"
            "	  Integer number representing GPIO pin\n"
            "	  'dtr', 'rts' or 'brk' representing serial port signal\n"
            "	The sequence can use the following delimiters:\n"
            "	  ',' adds 100 ms delay between signals\n"
            "	  '&' adds no delay between signals\n"
            "	The following modifiers can be prepended to a signal:\n"
            "	  '-' reset signal (low) instead of setting it (high)\n"
            "\n"
            "Examples:\n"
            "	Get device information:\n"
            "		%s /dev/ttyS0\n"
            "	  or:\n"
            "		%s /dev/i2c-0\n"
            "\n"
            "	Write with verify and then start execution:\n"
            "		%s -w filename -v -g 0x0 /dev/ttyS0\n"
            "\n"
            "	Read flash to file:\n"
            "		%s -r filename /dev/ttyS0\n"
            "\n"
            "	Read 100 bytes of flash from 0x1000 to stdout:\n"
            "		%s -r - -S 0x1000:100 /dev/ttyS0\n"
            "\n"
            "	Start execution:\n"
            "		%s -g 0x0 /dev/ttyS0\n"
            "\n"
            "	GPIO sequence:\n"
            "	- entry sequence: GPIO_3=low, GPIO_2=low, 100ms delay, GPIO_2=high\n"
            "	- exit sequence: GPIO_3=high, GPIO_2=low, 300ms delay, GPIO_2=high\n"
            "		%s -i '-3&-2,2:3&-2,,,2' /dev/ttyS0\n"
            "	GPIO sequence adding delay after port opening:\n"
            "	- entry sequence: delay 500ms\n"
            "	- exit sequence: rts=high, dtr=low, 300ms delay, GPIO_2=high\n"
            "		%s -R -i ',,,,,:rts&-dtr,,,2' /dev/ttyS0\n",
            name,
            name,
            name,
            name,
            name,
            name,
            name,
            name,
            name);
#else
            "Usage: [-pCujkoevngSFsfhcR] [-[rw] filename]\n"
            "	-p [auto|uartN|direct]	Select port (default auto)\n"
            "	-r filename	Read flash to file\n"
            "	-w filename	Write flash from file\n"
            "	-C		Compute CRC of flash content\n"
            "	-u		Disable the flash write-protection\n"
            "	-j		Enable the flash read-protection\n"
            "	-k		Disable the flash read-protection\n"
            "	-o		Erase only\n"
            "	-e n		Only erase n pages before writing the flash\n"
            "	-v		Verify writes\n"
            "	-n count	Retry failed writes up to count times (default 10)\n"
            "	-g address	Start execution at specified address (0 = flash start)\n"
            "	-S address[:length]	Specify start address and optionally length for\n"
            "	                   	read/write/erase operations\n"
            "	-F RX_length[:TX_length]  Specify the max length of RX and TX frame\n"
            "	-s start_page	Flash at specified page (0 = flash start)\n"
            "	-f		Force binary parser\n"
            "	-h		Show this help\n"
            "	-c		Resume the connection (don't send initial INIT)\n"
            "			*Baud rate must be kept the same as the first init*\n"
            "			This is useful if the reset fails\n"
            "	-R		Reset device at exit.\n"
            "	-b rate		Baud rate (default 115200)\n"
            "	-m mode		Serial port mode (default 8e1)\n"
            "\n"
            "Port choices (applies to all commands):\n"
            "   -p auto   (default) FluidNC automatically selects uart\n"
            "             based on the config file\n"
            "   -p uartN  Use FluidNC uartN (N=1,2,3,4)\n"
            ""
            "Examples:\n"
            "	Get device information using uart chosen by FluidNC:\n"
            "		-p auto   (or empty command)\n"
            "	Get device information via FluidNC uart2:\n"
            "		-p uart2\n"
            "	Write file to STM32 Flash:\n"
            "		[-p port] -w filename\n"
            "	Write with verify and then start execution:\n"
            "		[-p port] -w filename -v -g 0x0\n"
            "	Read STM32 Flash to file:\n"
            "		[-p port] -r filename\n"
            "	Read 100 bytes of flash from 0x08001000\n"
            "		[-p port] -r filename -S 0x1000:100\n"
            "	Start execution:\n"
            "		[-p port] -g 0x0\n");
#endif
}

#include <vector>
#include <cstring>
#include "stm32action.h"
int stm32action(SerialPort& port, std::string cmd) {
    port_opts.extra = &port;
    std::vector<char*> argv_vector;
    cmd       = "stmloader " + cmd;
    char* str = new char[cmd.size() + 1];
    std::strcpy(str, cmd.c_str());
    argv_vector.push_back(strtok(str, " "));
    char* token;
    while ((token = strtok(nullptr, " ")) != nullptr) {
        argv_vector.push_back(token);
    }
    int    argc = argv_vector.size();
    char** argv = argv_vector.data();
    int    ret  = stm32main(argc, argv);
    delete str;
    return ret;
}

#ifdef __APPLE__
// Add macOS-specific implementation of getFileName function
const char* getFileName(const char* filter, bool save) {
    static std::string fileName;
    if (save) {
        showSaveFileDialog(fileName, filter, "Select File");
    } else {
        showOpenFileDialog(fileName, filter, "Select File");
    }
    return fileName.c_str();
}
#endif
