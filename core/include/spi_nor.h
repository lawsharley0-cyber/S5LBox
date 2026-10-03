/*
 * NEON — a serial NOR flash on an SPI bus: the part under the iPhone 3GS's
 * /arm-io/spi0/nor-flash.
 *
 * What drives it. iOS 6's AppleARMSPIFlashController (AppleARMPlatform,
 * 10B500) is the only driver, and it uses exactly these commands, read from
 * its code:
 *
 *   0x9F  read ID: sends 9F FF FF FF and builds the JEDEC ID from the three
 *         answers (0x804b80d0); _identifyNORDevice (0x804b7be8) accepts only
 *         the parts in its table and names any other ID as unknown
 *   0x05  read status: 05 FF, the second answer is the status (0x804b8050);
 *         bit 0 is polled as busy after every program and erase (0x804b7fe4)
 *   0x06 / 0x04  write enable / disable, one octet each (0x804b8024)
 *   0x01  write status: write enable, then 01 <value> (0x804b8084); a write
 *         saves the status, clears the protection bits with 01 00 and puts
 *         the saved value back afterwards (0x804b7a62-0x804b7af8)
 *   0x03  read: 03 A2 A1 A0 and then up to 4096 data octets in one transfer,
 *         full duplex (0x804b798e)
 *   0x02  page program: write enable, 02 A2 A1 A0 and up to one page of
 *         data (0x804b7e5c), for the parts with page programming
 *   0x20  4 KiB erase: write enable, 20 A2 A1 A0 (0x804b7df8)
 *
 * (Two SST parts in its table program by word instead, command 0xAD; this
 * model does not present one.)
 *
 * THE PART. The 3GS's flash is 1 MiB -- the tree's regions under nor-flash
 * end exactly at 0x100000 -- and which vendor's 8-Mbit part a given phone
 * carries is not known here. This model presents ST's M25PE80 (JEDEC
 * 20 80 14), a page-program part whose entry in the driver's table gives
 * 256-octet pages and 256 erase blocks of 4 KiB, 1 MiB (0x804b7d18-0x804b7d68).
 * The table has two other 1 MiB parts (Atmel 1F 45 02 and SST BF 25 8E); this
 * is a choice among the parts the driver supports, not a claim about any
 * phone.
 *
 * What a command is. Like any SPI flash, a command is framed by chip select:
 * it starts when select asserts and takes effect when select releases, which
 * is when a program or erase commits and a write enable latches. On the 3GS
 * the select is a GPIO pin, not the controller, so the board calls
 * spi_nor_select() from the pin's register.
 *
 * What is not modelled: time (a program or erase is complete by the next
 * status read, so busy never reads 1), the block protection the status bits
 * describe (stored and read back, not enforced: the driver clears them
 * before every write anyway), and every command the driver never sends.
 * An unknown command is ignored for the rest of its selection and counted.
 *
 * The array is the caller's: its contents are the device, and they persist
 * across select, reset and machine reboots exactly as long as the caller
 * keeps them.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef NEON_SPI_NOR_H
#define NEON_SPI_NOR_H

#include "soc.h"

#include <stdbool.h>
#include <stdint.h>

#define SPI_NOR_M25PE80_ID   UINT32_C(0x208014)
#define SPI_NOR_PAGE         256u
#define SPI_NOR_ERASE_4K     4096u

#define SPI_NOR_CMD_WRSR     0x01u
#define SPI_NOR_CMD_PP       0x02u
#define SPI_NOR_CMD_READ     0x03u
#define SPI_NOR_CMD_WRDI     0x04u
#define SPI_NOR_CMD_RDSR     0x05u
#define SPI_NOR_CMD_WREN     0x06u
#define SPI_NOR_CMD_SSE      0x20u
#define SPI_NOR_CMD_RDID     0x9fu

#define SPI_NOR_SR_WIP       0x01u  /* busy: never set here, see above      */
#define SPI_NOR_SR_WEL       0x02u  /* write enable latch                   */
#define SPI_NOR_SR_WRITABLE  0x9cu  /* SRWD and BP2..BP0: what 01 can write */

typedef struct {
    uint8_t  *mem;              /* the array, the caller's                  */
    uint32_t  size;             /* a power of two; addresses wrap within it */
    uint32_t  jedec_id;

    bool      selected;
    uint8_t   status;           /* SRWD, BP2..0 and WEL                     */
    uint8_t   cmd;              /* the command octet of this selection      */
    uint32_t  pos;              /* octets clocked in this selection         */
    uint32_t  addr;
    bool      wren_pending, wrdi_pending, ignoring;
    uint8_t   wrsr_value;
    bool      wrsr_pending;
    uint8_t   page[SPI_NOR_PAGE];   /* page program's buffer               */
    uint32_t  page_fill;            /* octets clocked into it, uncapped    */

    /* What the guest did, for reports and tests. */
    uint64_t  selections, commands, reads, programs, erases, status_writes;
    uint64_t  read_octets, programmed_octets;
    uint64_t  refused;          /* a program, erase or status write with
                                   the write enable latch clear             */
    uint64_t  unknown;          /* commands this model does not know       */
    uint8_t   last_unknown;
} spi_nor_t;

/* Wire a device to an array of `size` octets (a power of two, at least one
 * erase block). False, and nothing changed, if the arguments are unusable. */
bool    spi_nor_init(spi_nor_t *nor, uint8_t *mem, uint32_t size, uint32_t jedec_id);
/* Power-on: deselected, status clear, the array untouched. */
void    spi_nor_reset(spi_nor_t *nor);
/* The chip select, active: true starts a command, false ends it (and commits
 * a program, an erase, a status write or a write enable/disable). */
void    spi_nor_select(spi_nor_t *nor, bool active);
/* One full-duplex octet; the s5l_spi_slave_t transfer. A deselected device
 * does not drive the line, which reads 0xFF. */
uint8_t spi_nor_transfer(void *nor, uint8_t out);
/* Fill `slave` so an SPI controller drives this device. */
void    spi_nor_bind(spi_nor_t *nor, s5l_spi_slave_t *slave);

#endif /* NEON_SPI_NOR_H */
