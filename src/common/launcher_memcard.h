// launcher_memcard.h — PS1 memory card (.mcd/.mcr, 128 KiB raw) reader for the
// launcher's memory-card manager: per-save title, product code, block chain and
// the animated 16x16 icon (decoded to RGBA). Also delete/copy of one save, with
// a <card>.bak backup written before any change.
//
// Format (public, the same one the PS1 BIOS uses): block 0 = header frame "MC"
// + 15 directory frames (state, size, next block, file name, XOR checksum);
// blocks 1..15 = save data, the first block of a save starting with the "SC"
// title frame (icon flag, block count, Shift-JIS title, 16-color palette) and
// up to 3 icon frames (16x16, 4bpp).

#ifndef LAUNCHER_MEMCARD_H
#define LAUNCHER_MEMCARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LMC_BLOCKS 15

typedef struct {
    int      first_block;          // 0..14 (directory index)
    int      num_blocks;           // blocks in the chain
    int      chain[LMC_BLOCKS];    // directory indexes in order
    char     file_name[24];        // e.g. "BASLUS-00453..."
    char     product[16];          // e.g. "SLUS-00453" (from the file name)
    char     region[8];            // "EUA" / "JAPAO" / "EUROPA" / ""
    char     title[96];            // title converted to ASCII/UTF-8 (best effort)
    int      icon_frames;          // 1..3 (0 = no icon)
    uint8_t  icon_rgba[3][16 * 16 * 4];
} LmcSave;

typedef struct {
    int      valid;                // 1 = a real formatted card was read
    int      num_saves;
    LmcSave  saves[LMC_BLOCKS];
    int      block_owner[LMC_BLOCKS];   // save index using each block, -1 free
    int      free_blocks;
} LmcCard;

// Read and decode a card file. Returns 1 when the file is a formatted card.
int lmc_read(const char* path, LmcCard* out);

// Delete save `save_index` of the card at `path` (marks its blocks as deleted,
// exactly like the BIOS). Writes `<path>.bak` first. Returns 1 on success;
// on failure `err` (may be NULL) gets a short pt-BR reason.
int lmc_delete(const char* path, int save_index, char* err, int err_cap);

// Copy save `save_index` from card `src` to card `dst`. Fails (returns 0) when
// `dst` lacks free blocks or already has a save with the same file name.
// Writes `<dst>.bak` first.
int lmc_copy(const char* src, int save_index, const char* dst, char* err, int err_cap);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_MEMCARD_H
