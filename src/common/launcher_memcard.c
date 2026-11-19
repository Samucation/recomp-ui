// launcher_memcard.c — see launcher_memcard.h.

#include "launcher_memcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LMC_CARD_SIZE  (128 * 1024)
#define LMC_BLOCK_SIZE (8 * 1024)
#define LMC_FRAME      128

static void set_err(char* err, int cap, const char* msg) {
    if (err && cap > 0) snprintf(err, (size_t)cap, "%s", msg);
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int load_card(const char* path, uint8_t* buf) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(buf, 1, LMC_CARD_SIZE, f);
    fclose(f);
    if (n != LMC_CARD_SIZE) return 0;
    return buf[0] == 'M' && buf[1] == 'C';
}

static int save_card(const char* path, const uint8_t* buf) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const size_t n = fwrite(buf, 1, LMC_CARD_SIZE, f);
    const int ok = fclose(f) == 0 && n == LMC_CARD_SIZE;
    return ok;
}

static int backup_card(const char* path, const uint8_t* buf) {
    char bak[1100];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    return save_card(bak, buf);
}

static uint8_t* dir_frame(uint8_t* card, int idx) { return card + LMC_FRAME * (1 + idx); }

static void fix_checksum(uint8_t* frame) {
    uint8_t x = 0;
    for (int i = 0; i < LMC_FRAME - 1; ++i) x ^= frame[i];
    frame[LMC_FRAME - 1] = x;
}

// Shift-JIS (the subset save titles use: full-width ASCII + common punctuation)
// to plain ASCII. Unknown double-byte characters become '?'.
static void sjis_to_ascii(const uint8_t* s, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && s[i] && o < cap - 1;) {
        const uint8_t c = s[i];
        if (c < 0x80) { out[o++] = (char)c; ++i; continue; }
        if (c >= 0xA1 && c <= 0xDF) { out[o++] = '?'; ++i; continue; }  // half-width kana
        if (i + 1 >= n) break;
        const uint16_t w = (uint16_t)((c << 8) | s[i + 1]);
        i += 2;
        char a = '?';
        if (w >= 0x824F && w <= 0x8258) a = (char)('0' + (w - 0x824F));
        else if (w >= 0x8260 && w <= 0x8279) a = (char)('A' + (w - 0x8260));
        else if (w >= 0x8281 && w <= 0x829A) a = (char)('a' + (w - 0x8281));
        else switch (w) {
            case 0x8140: a = ' '; break;  case 0x8143: a = ','; break;
            case 0x8144: a = '.'; break;  case 0x8146: a = ':'; break;
            case 0x8147: a = ';'; break;  case 0x8148: a = '?'; break;
            case 0x8149: a = '!'; break;  case 0x814F: a = '^'; break;
            case 0x8151: a = '_'; break;  case 0x815B: a = '-'; break;
            case 0x815C: a = '-'; break;  case 0x815D: a = '-'; break;
            case 0x815E: a = '/'; break;  case 0x815F: a = '\\'; break;
            case 0x8160: a = '~'; break;  case 0x8162: a = '|'; break;
            case 0x8165: a = '\''; break; case 0x8166: a = '\''; break;
            case 0x8167: a = '"'; break;  case 0x8168: a = '"'; break;
            case 0x8169: a = '('; break;  case 0x816A: a = ')'; break;
            case 0x816D: a = '['; break;  case 0x816E: a = ']'; break;
            case 0x816F: a = '{'; break;  case 0x8170: a = '}'; break;
            case 0x817B: a = '+'; break;  case 0x817C: a = '-'; break;
            case 0x8181: a = '='; break;  case 0x8183: a = '<'; break;
            case 0x8184: a = '>'; break;  case 0x8190: a = '$'; break;
            case 0x8193: a = '%'; break;  case 0x8194: a = '#'; break;
            case 0x8195: a = '&'; break;  case 0x8196: a = '*'; break;
            case 0x8197: a = '@'; break;
            default: break;
        }
        out[o++] = a;
    }
    out[o] = 0;
    // collapse runs of spaces, trim
    int r = 0, w2 = 0, prev_sp = 1;
    for (; out[r]; ++r) {
        const int sp = out[r] == ' ';
        if (sp && prev_sp) continue;
        out[w2++] = out[r];
        prev_sp = sp;
    }
    while (w2 > 0 && out[w2 - 1] == ' ') --w2;
    out[w2] = 0;
}

static void decode_icon(const uint8_t* block, int frame, uint8_t* rgba) {
    const uint8_t* pal = block + 0x60;
    const uint8_t* px = block + LMC_FRAME * (1 + frame);
    for (int i = 0; i < 256; ++i) {
        const uint8_t b = px[i >> 1];
        const int idx = (i & 1) ? (b >> 4) : (b & 15);
        const uint16_t c = rd16(pal + idx * 2);
        uint8_t* o = rgba + i * 4;
        const int r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
        o[0] = (uint8_t)((r << 3) | (r >> 2));
        o[1] = (uint8_t)((g << 3) | (g >> 2));
        o[2] = (uint8_t)((bl << 3) | (bl >> 2));
        o[3] = c == 0 ? 0 : 255;
    }
}

static void product_from_name(const char* fn, char* prod, int pcap, char* reg, int rcap) {
    // "BASLUS-00453xxxx": 2 region letters + 10-char product code.
    prod[0] = 0; reg[0] = 0;
    if (strlen(fn) >= 12 && fn[0] == 'B') {
        snprintf(prod, (size_t)pcap, "%.10s", fn + 2);
        if (fn[1] == 'A') snprintf(reg, (size_t)rcap, "EUA");
        else if (fn[1] == 'I') snprintf(reg, (size_t)rcap, "JAPAO");
        else if (fn[1] == 'E') snprintf(reg, (size_t)rcap, "EUROPA");
    }
}

static int parse(const uint8_t* card, LmcCard* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < LMC_BLOCKS; ++i) out->block_owner[i] = -1;
    out->valid = 1;
    for (int i = 0; i < LMC_BLOCKS; ++i) {
        const uint8_t* d = card + LMC_FRAME * (1 + i);
        if ((rd32(d) & 0xFF) != 0x51) continue;
        LmcSave* s = &out->saves[out->num_saves];
        memset(s, 0, sizeof(*s));
        s->first_block = i;
        // follow the chain
        int cur = i, guard = 0;
        while (cur >= 0 && cur < LMC_BLOCKS && guard++ < LMC_BLOCKS) {
            if (out->block_owner[cur] != -1) break;
            s->chain[s->num_blocks++] = cur;
            out->block_owner[cur] = out->num_saves;
            const uint16_t nx = rd16(card + LMC_FRAME * (1 + cur) + 8);
            if (nx == 0xFFFF) break;
            cur = nx;
        }
        memcpy(s->file_name, d + 0x0A, 20);
        s->file_name[20] = 0;
        product_from_name(s->file_name, s->product, sizeof(s->product), s->region, sizeof(s->region));
        const uint8_t* blk = card + LMC_BLOCK_SIZE * (1 + i);
        if ((blk[0] == 'S' || blk[0] == 's') && (blk[1] == 'C' || blk[1] == 'c')) {
            sjis_to_ascii(blk + 4, 64, s->title, sizeof(s->title));
            const uint8_t flag = blk[2];
            s->icon_frames = flag == 0x13 ? 3 : (flag == 0x12 ? 2 : (flag == 0x11 ? 1 : 1));
            for (int f = 0; f < s->icon_frames; ++f) decode_icon(blk, f, s->icon_rgba[f]);
        }
        if (!s->title[0]) snprintf(s->title, sizeof(s->title), "%s", s->file_name);
        out->num_saves++;
    }
    for (int i = 0; i < LMC_BLOCKS; ++i) {
        const uint32_t st = rd32(card + LMC_FRAME * (1 + i)) & 0xFF;
        if (out->block_owner[i] == -1 && (st == 0xA0 || st == 0xA1 || st == 0xA2 || st == 0xA3))
            out->free_blocks++;
    }
    return 1;
}

int lmc_read(const char* path, LmcCard* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < LMC_BLOCKS; ++i) out->block_owner[i] = -1;
    if (!path || !path[0]) return 0;
    uint8_t* card = (uint8_t*)malloc(LMC_CARD_SIZE);
    if (!card) return 0;
    int ok = load_card(path, card);
    if (ok) ok = parse(card, out);
    free(card);
    return ok;
}

int lmc_delete(const char* path, int save_index, char* err, int err_cap) {
    uint8_t* card = (uint8_t*)malloc(LMC_CARD_SIZE);
    if (!card) { set_err(err, err_cap, "Sem memória."); return 0; }
    LmcCard info;
    if (!load_card(path, card) || !parse(card, &info)) {
        free(card); set_err(err, err_cap, "Não consegui ler o memory card."); return 0;
    }
    if (save_index < 0 || save_index >= info.num_saves) {
        free(card); set_err(err, err_cap, "Save não encontrado."); return 0;
    }
    if (!backup_card(path, card)) {
        free(card); set_err(err, err_cap, "Não consegui gravar o backup (.bak)."); return 0;
    }
    const LmcSave* s = &info.saves[save_index];
    for (int k = 0; k < s->num_blocks; ++k) {
        uint8_t* d = dir_frame(card, s->chain[k]);
        const uint8_t st = d[0];
        d[0] = (uint8_t)(st == 0x51 ? 0xA1 : (st == 0x52 ? 0xA2 : (st == 0x53 ? 0xA3 : 0xA1)));
        fix_checksum(d);
    }
    const int ok = save_card(path, card);
    free(card);
    if (!ok) set_err(err, err_cap, "Não consegui gravar o memory card.");
    return ok;
}

int lmc_copy(const char* src, int save_index, const char* dst, char* err, int err_cap) {
    uint8_t* a = (uint8_t*)malloc(LMC_CARD_SIZE);
    uint8_t* b = (uint8_t*)malloc(LMC_CARD_SIZE);
    int ok = 0;
    LmcCard ia, ib;
    if (!a || !b) { set_err(err, err_cap, "Sem memória."); goto done; }
    if (!load_card(src, a) || !parse(a, &ia)) { set_err(err, err_cap, "Não consegui ler o card de origem."); goto done; }
    if (!load_card(dst, b) || !parse(b, &ib)) { set_err(err, err_cap, "O card de destino não está formatado."); goto done; }
    if (save_index < 0 || save_index >= ia.num_saves) { set_err(err, err_cap, "Save não encontrado."); goto done; }
    {
        const LmcSave* s = &ia.saves[save_index];
        for (int i = 0; i < ib.num_saves; ++i)
            if (strncmp(ib.saves[i].file_name, s->file_name, 20) == 0) {
                set_err(err, err_cap, "O outro card já tem este save.");
                goto done;
            }
        int freeb[LMC_BLOCKS], nfree = 0;
        for (int i = 0; i < LMC_BLOCKS; ++i) {
            const uint8_t st = dir_frame(b, i)[0];
            if (ib.block_owner[i] == -1 && (st == 0xA0 || st == 0xA1 || st == 0xA2 || st == 0xA3))
                freeb[nfree++] = i;
        }
        if (nfree < s->num_blocks) {
            set_err(err, err_cap, "O outro card não tem blocos livres suficientes.");
            goto done;
        }
        if (!backup_card(dst, b)) { set_err(err, err_cap, "Não consegui gravar o backup (.bak)."); goto done; }
        const uint32_t size = rd32(dir_frame(a, s->first_block) + 4);
        for (int k = 0; k < s->num_blocks; ++k) {
            const int to = freeb[k];
            uint8_t* d = dir_frame(b, to);
            memset(d, 0, LMC_FRAME);
            d[0] = (uint8_t)(s->num_blocks == 1 ? 0x51 : (k == 0 ? 0x51 : (k == s->num_blocks - 1 ? 0x53 : 0x52)));
            if (k == 0) {
                wr32(d + 4, size ? size : (uint32_t)(s->num_blocks * LMC_BLOCK_SIZE));
                memcpy(d + 0x0A, s->file_name, 20);
            }
            wr16(d + 8, (uint16_t)(k + 1 < s->num_blocks ? freeb[k + 1] : 0xFFFF));
            fix_checksum(d);
            memcpy(b + LMC_BLOCK_SIZE * (1 + to), a + LMC_BLOCK_SIZE * (1 + s->chain[k]), LMC_BLOCK_SIZE);
        }
        ok = save_card(dst, b);
        if (!ok) set_err(err, err_cap, "Não consegui gravar o memory card.");
    }
done:
    free(a);
    free(b);
    return ok;
}
