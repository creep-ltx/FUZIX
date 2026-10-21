#ifndef __RDB_DOT_H__
#define __RDB_DOT_H__

/* Amiga Rigid Disk Block partitions: see rdb.c */

#define RDB_FUZIX	0x46555A00UL	/* DosType FUZ\0 */

uint_fast8_t rdb_parse(uint_fast8_t letter);
uint16_t rdb_fuzix_minor(uint_fast8_t drive);
extern uint32_t rdb_dostype[MAX_BLKDEV][MAX_PARTITIONS];

#endif
