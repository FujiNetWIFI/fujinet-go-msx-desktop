/*
 * The Cart & I/O, Sound and memory-map getters (see msxdebug.h): plain reads
 * of openMSX's own debuggables and info commands, so nothing here disturbs a
 * stopped or a running machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../openmsx/msx_host.hh"
#include "../src/session_internal.h"
#include "debugger_internal.h"

/* ---- a Tcl list of names ------------------------------------------------------ */

/* Splits "a {b c} d" into its elements; returns how many fitted. */
static int split_list(const char *text, char names[][64], int max)
{
    int count = 0;
    while (*text && count < max) {
        int len = 0;
        while (*text == ' ') text++;
        if (!*text) break;
        if (*text == '{') {
            text++;
            while (*text && *text != '}' && len < 63) names[count][len++] = *text++;
            while (*text && *text != '}') text++;
            if (*text == '}') text++;
        } else {
            while (*text && *text != ' ' && len < 63) names[count][len++] = *text++;
            while (*text && *text != ' ') text++;
        }
        names[count][len] = '\0';
        count++;
    }
    return count;
}

static int ends_with(const char *text, const char *tail)
{
    const size_t len = strlen(text), tail_len = strlen(tail);
    return len >= tail_len && !strcmp(text + len - tail_len, tail);
}

/* ---- finding the chips -------------------------------------------------------- */

/* What kind of chip a debuggable is, from its description: asked once per
 * name, since a name's description does not change. */
enum { KIND_NONE = 0, KIND_PSG, KIND_SCC, KIND_OPLL, KIND_Y8950, KIND_OTHER };

static struct {
    pthread_mutex_t mtx;
    char name[96][64];
    int kind[96];
    int count;
} g_kinds = { .mtx = PTHREAD_MUTEX_INITIALIZER };

static int kind_of(const char *name)
{
    int kind = KIND_OTHER, idx;
    char desc[128];

    pthread_mutex_lock(&g_kinds.mtx);
    for (idx = 0; idx < g_kinds.count; idx++)
        if (!strcmp(g_kinds.name[idx], name)) {
            kind = g_kinds.kind[idx];
            pthread_mutex_unlock(&g_kinds.mtx);
            return kind;
        }
    pthread_mutex_unlock(&g_kinds.mtx);

    if (msxdebug_rpcf(desc, sizeof(desc), "debug desc {%s}", name)) {
        if (!strcmp(desc, "PSG")) kind = KIND_PSG;
        else if (strstr(desc, "SCC registers")) kind = KIND_SCC;
        else if (!strcmp(desc, "MSX-MUSIC")) kind = KIND_OPLL;
        else if (!strcmp(desc, "MSX-AUDIO")) kind = KIND_Y8950;
    }
    pthread_mutex_lock(&g_kinds.mtx);
    if (g_kinds.count < 96) {
        snprintf(g_kinds.name[g_kinds.count], sizeof(g_kinds.name[0]), "%s", name);
        g_kinds.kind[g_kinds.count++] = kind;
    }
    pthread_mutex_unlock(&g_kinds.mtx);
    return kind;
}

typedef struct {
    char psg[64], scc[64], opll[64], y8950[64];
    int mapper;
    char romblocks[8][64];
    int roms;
} chips;

static void find_chips(chips *found)
{
    char list[4096];
    char names[64][64];
    int count, idx;

    memset(found, 0, sizeof(*found));
    if (!msxdebug_rpc("debug list", list, sizeof(list))) return;
    count = split_list(list, names, 64);
    for (idx = 0; idx < count; idx++) {
        const char *name = names[idx];
        if (!strcmp(name, "MapperIO")) {
            found->mapper = 1;
        } else if (ends_with(name, " romblocks")) {
            if (found->roms < 8) snprintf(found->romblocks[found->roms++], 64, "%s", name);
        } else if (ends_with(name, " regs") || ends_with(name, " SCC")) {
            switch (kind_of(name)) {
            case KIND_PSG:   if (!found->psg[0]) snprintf(found->psg, 64, "%s", name); break;
            case KIND_SCC:   if (!found->scc[0]) snprintf(found->scc, 64, "%s", name); break;
            case KIND_OPLL:  if (!found->opll[0]) snprintf(found->opll, 64, "%s", name); break;
            case KIND_Y8950: if (!found->y8950[0]) snprintf(found->y8950, 64, "%s", name); break;
            default: break;
            }
        }
    }
}

/* ---- Cart & I/O ----------------------------------------------------------------- */

/* Everything the Cart & I/O tab shows except the slot map's text, in ONE
 * round trip: each trip to openMSX costs milliseconds, and the older shape of
 * this (a command per slot, page and register) took most of a second. The
 * script answers a flat list; names with spaces come back braced. */
static const char k_io_script[] =
    "set res [list [machine_info config_name] [machine_info type]];"
    "foreach slt {0 1 2 3} {lappend res [machine_info issubslotted $slt]};"
    "foreach pag {0 1 2 3} {"
    " set sel [get_selected_slot $pag]; set prm [lindex $sel 0]; set sub [lindex $sel 1];"
    " if {$sub eq {X}} {set sec 0} else {set sec $sub};"
    " lappend res $prm $sub [lindex [machine_info slot $prm $sec $pag] 0]};"
    "lappend res [debug read memory 0xFFFF];"
    "foreach prt {0xA8 0xA9 0xAA 0xAB} {lappend res [debug read ioports $prt]};"
    "set dbl [debug list];"
    "lappend res [expr {[lsearch -exact $dbl MapperIO] >= 0}];"
    "foreach idx {0 1 2 3} {if {[catch {debug read MapperIO $idx} val]} {set val 0}; lappend res $val};"
    "foreach idx {0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15} {"
    " if {[catch {debug read keymatrix $idx} val]} {set val 255}; lappend res $val};"
    "foreach idx {0 1} {if {[catch {debug read joystickports $idx} val]} {set val 63}; lappend res $val};"
    "set rom {};"
    "foreach nam $dbl {if {[string match {* romblocks} $nam]} {"
    " set blk {}; foreach adr {0 8192 16384 24576 32768 40960 49152 57344} {"
    "  if {[catch {debug read $nam $adr} val]} {set val 255}; lappend blk $val};"
    " lappend rom $nam {*}$blk}};"
    "lappend res [expr {[llength $rom] / 9}] {*}$rom;"
    "set res";

static int io_collect(msxdebug *dbg, msxdebug_io *out, int with_slotmap)
{
    char reply[4096];
    char (*item)[64];
    int count, page, idx, pos, roms;

    memset(out, 0, sizeof(*out));
    for (page = 0; page < 4; page++) {
        out->page_subslot[page] = -1;
        out->subslot_reg[page] = -1;
    }
    if (!dbg || !msxsession_is_running(msxdebug_session(dbg))) return 0;
    if (!msxdebug_rpc(k_io_script, reply, sizeof(reply))) return 0;
    item = malloc(160 * sizeof(*item));
    if (!item) return 0;
    count = split_list(reply, item, 160);
    if (count < 40) {
        free(item);
        return 0;
    }
    snprintf(out->machine, sizeof(out->machine), "%s", item[0]);
    snprintf(out->type, sizeof(out->type), "%s", item[1]);
    out->valid = 1;
    for (idx = 0; idx < 4; idx++) out->expanded[idx] = atoi(item[2 + idx]) > 0;
    pos = 6;
    for (page = 0; page < 4; page++, pos += 3) {
        char *brace;
        out->page_slot[page] = atoi(item[pos]) & 3;
        out->page_subslot[page] = item[pos + 1][0] == 'X' ? -1 : atoi(item[pos + 1]);
        snprintf(out->page_device[page], sizeof(out->page_device[page]), "%s", item[pos + 2]);
        while ((brace = strpbrk(out->page_device[page], "{}")) != NULL) memmove(brace, brace + 1, strlen(brace));
    }
    if (out->expanded[out->page_slot[3]])
        out->subslot_reg[out->page_slot[3]] = ~atoi(item[pos]) & 0xFF;     /* complemented at $FFFF */
    pos++;
    for (idx = 0; idx < 4; idx++) out->ppi[idx] = (uint8_t)atoi(item[pos++]);
    out->mapper = atoi(item[pos++]) > 0;
    for (idx = 0; idx < 4; idx++) out->mapper_reg[idx] = (uint8_t)atoi(item[pos++]);
    out->keymatrix_rows = 11;
    for (idx = 0; idx < 16; idx++) out->keymatrix[idx] = (uint8_t)atoi(item[pos++]);
    out->joyport[0] = (uint8_t)atoi(item[pos++]);
    out->joyport[1] = (uint8_t)atoi(item[pos++]);
    roms = atoi(item[pos++]);
    for (idx = 0; idx < roms && pos + 9 <= count && out->rom_mappers < 6; idx++, pos += 9) {
        int any = 0, win;
        uint8_t samples[8];
        for (win = 0; win < 8; win++) {
            samples[win] = (uint8_t)atoi(item[pos + 1 + win]);
            if (samples[win] != 0xFF) any = 1;
        }
        if (!any) continue;
        snprintf(out->rom[out->rom_mappers].device, sizeof(out->rom[0].device), "%s", item[pos]);
        {
            char *tail = strstr(out->rom[out->rom_mappers].device, " romblocks");
            if (tail) *tail = '\0';
        }
        memcpy(out->rom[out->rom_mappers].block, samples, 8);
        out->rom_mappers++;
    }
    free(item);
    if (with_slotmap) msxdebug_rpc("slotmap", out->slotmap, sizeof(out->slotmap));
    return 1;
}

int msxdebug_io_get(msxdebug *dbg, msxdebug_io *out)
{
    return io_collect(dbg, out, 1);
}

/* The older text form of the same, kept for the console and old windows. */
int msxdebug_io_state(msxdebug *dbg, char *dst, int dstsz)
{
    msxdebug_io ioinfo;
    msxdebug_sound sound;
    int len = 0, page, idx;

#define PUT(...) do { if (len < dstsz) len += snprintf(dst + len, (size_t)(dstsz - len), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!msxdebug_io_get(dbg, &ioinfo)) return 0;
    PUT("Machine: %s\n\n", ioinfo.type);
    PUT("Selected slots (CPU pages):\n");
    for (page = 0; page < 4; page++) {
        if (ioinfo.page_subslot[page] < 0)
            PUT("  page %d ($%04X): slot %d\n", page, page * 0x4000, ioinfo.page_slot[page]);
        else
            PUT("  page %d ($%04X): slot %d-%d\n", page, page * 0x4000, ioinfo.page_slot[page], ioinfo.page_subslot[page]);
    }
    PUT("\nPPI $A8 (slot register): $%02X\n", ioinfo.ppi[0]);
    if (ioinfo.mapper) {
        PUT("Memory mapper ($FC-$FF):");
        for (idx = 0; idx < 4; idx++) PUT(" $%02X", ioinfo.mapper_reg[idx]);
        PUT("\n");
    }
    if (msxdebug_sound_get(dbg, &sound) && sound.psg.present) {
        const msxdebug_psg *psg = &sound.psg;
        PUT("\nPSG (AY-3-8910) registers:\n  ");
        for (idx = 0; idx < 14; idx++) PUT("R%X=%02X%s", idx, psg->reg[idx], (idx == 6 || idx == 13) ? "\n  " : " ");
        PUT("\n  tones: A %d, B %d, C %d  noise %d  mixer $%02X  volumes A %d B %d C %d\n",
            psg->period[0], psg->period[1], psg->period[2], psg->noise_period, psg->reg[7],
            psg->level[0], psg->level[1], psg->level[2]);
    }
    PUT("\nSlot map:\n%s\n", ioinfo.slotmap);
#undef PUT
    return len < dstsz ? len : dstsz - 1;
}

/* ---- the memory map ---------------------------------------------------------------- */

int msxdebug_memory_map(msxdebug *dbg, msxdebug_region *out, int max)
{
    msxdebug_io ioinfo;
    int count = 0, page;

    if (!out || max <= 0) return 0;
    if (!dbg || !io_collect(dbg, &ioinfo, 0)) {
        snprintf(out[0].name, sizeof(out[0].name), "unmapped");
        out[0].lo = 0;
        out[0].hi = 0xFFFF;
        out[0].readable = out[0].writable = 0;
        return 1;
    }
    for (page = 0; page < 4 && count < max; page++) {
        msxdebug_region *region = &out[count];
        char slot[16];
        const char *device = ioinfo.page_device[page];
        const int ram = strstr(device, "RAM") != NULL || strstr(device, "Mapper") != NULL;

        if (ioinfo.page_subslot[page] < 0) snprintf(slot, sizeof(slot), "%d", ioinfo.page_slot[page]);
        else snprintf(slot, sizeof(slot), "%d-%d", ioinfo.page_slot[page], ioinfo.page_subslot[page]);
        region->lo = (uint16_t)(page * 0x4000);
        region->hi = (uint16_t)(page * 0x4000 + 0x3FFF);
        /* an expanded slot's register hides the last byte of page 3 */
        if (page == 3 && ioinfo.expanded[ioinfo.page_slot[3] & 3]) region->hi = 0xFFFE;
        region->readable = device[0] != '\0';
        region->writable = ram;
        if (device[0]) snprintf(region->name, sizeof(region->name), "%s (slot %s)", device, slot);
        else           snprintf(region->name, sizeof(region->name), "empty (slot %s)", slot);
        /* the same device in the same slot, page after page: one region */
        if (count > 0 && !strcmp(out[count - 1].name, region->name)) {
            out[count - 1].hi = region->hi;
            continue;
        }
        count++;
    }
    if (ioinfo.expanded[ioinfo.page_slot[3] & 3] && count < max) {
        out[count].lo = 0xFFFF;
        out[count].hi = 0xFFFF;
        out[count].readable = out[count].writable = 1;
        snprintf(out[count].name, sizeof(out[count].name), "sub-slot register (slot %d)", ioinfo.page_slot[3]);
        count++;
    }
    return count;
}

/* ---- the cartridge slots in words ---------------------------------------------------- */

void msxdebug_cart_info(msxdebug *dbg, msxdebug_cart *out)
{
    char list[2048];
    char names[40][64];
    chips found;
    char res[32];
    msxsession *session;
    int count, idx, len = 0;

    memset(out, 0, sizeof(*out));
    if (!dbg) return;
    session = msxdebug_session(dbg);
    if (!msxsession_is_running(session)) return;
    msxdebug_rpc("machine_info config_name", out->machine, sizeof(out->machine));
    msxdebug_rpc("machine_info type", out->type, sizeof(out->type));
    if (msxdebug_rpc("machine_info z80_freq", res, sizeof(res))) out->z80_hz = atoi(res);
    if (msxdebug_rpc("machine_info device", list, sizeof(list))) {
        count = split_list(list, names, 40);
        for (idx = 0; idx < count; idx++) {
            if (!strcmp(names[idx], "FujiNet")) out->fujinet = 1;
            len += snprintf(out->devices + len, sizeof(out->devices) - (size_t)len, "%s%s",
                            idx ? ", " : "", names[idx]);
            if (len >= (int)sizeof(out->devices)) len = (int)sizeof(out->devices) - 1;
        }
    }
    out->fujinet_link = msxsession_fujinet_link(session) ? 1 : 0;
    out->fujinet_external = msxsession_fujinet_external(session) ? 1 : 0;
    find_chips(&found);
    out->scc = found.scc[0] != '\0';
    out->music = found.opll[0] != '\0';
    out->audio = found.y8950[0] != '\0';
    out->mapper = found.mapper;
}

int msxdebug_cart_summary(msxdebug *dbg, char *dst, int dstsz)
{
    msxdebug_cart cart;
    int len;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    msxdebug_cart_info(dbg, &cart);
    if (!cart.machine[0]) return snprintf(dst, (size_t)dstsz, "no machine running");
    len = snprintf(dst, (size_t)dstsz, "%s (%s)", cart.machine, cart.type);
    if (cart.fujinet && len < dstsz)
        len += snprintf(dst + len, (size_t)(dstsz - len), ", FujiNet cartridge (%s%s)",
                        cart.fujinet_external ? "external, " : "", cart.fujinet_link ? "link up" : "link down");
    if (cart.mapper && len < dstsz) len += snprintf(dst + len, (size_t)(dstsz - len), ", memory mapper");
    if (cart.scc && len < dstsz) len += snprintf(dst + len, (size_t)(dstsz - len), ", SCC");
    if (cart.music && len < dstsz) len += snprintf(dst + len, (size_t)(dstsz - len), ", MSX-MUSIC");
    if (cart.audio && len < dstsz) len += snprintf(dst + len, (size_t)(dstsz - len), ", MSX-AUDIO");
    return len < dstsz ? len : dstsz - 1;
}

/* ---- Sound ---------------------------------------------------------------------------- */

static int read_block(const char *name, uint8_t *dst, int count)
{
    char command[160];
    int got = 0;
    snprintf(command, sizeof(command), "debug read_block {%s} 0 %d", name, count);
    if (!msxhost_execute_sync_binary(command, dst, count, &got) || got < count) return 0;
    return 1;
}

static void decode_psg(msxdebug_psg *psg)
{
    int voice;
    for (voice = 0; voice < 3; voice++) {
        const int amplitude = psg->reg[8 + voice];
        psg->period[voice] = (psg->reg[voice * 2] | (psg->reg[voice * 2 + 1] << 8)) & 0x0FFF;
        psg->freq[voice] = psg->period[voice] ? 111860.8 / psg->period[voice] : 0.0;
        psg->tone_on[voice] = !((psg->reg[7] >> voice) & 1);
        psg->noise_on[voice] = !((psg->reg[7] >> (voice + 3)) & 1);
        psg->level[voice] = amplitude & 0x0F;
        psg->envelope[voice] = (amplitude & 0x10) != 0;
    }
    psg->noise_period = psg->reg[6] & 0x1F;
    psg->env_period = psg->reg[11] | (psg->reg[12] << 8);
    psg->env_shape = psg->reg[13] & 0x0F;
}

static void decode_scc(msxdebug_scc *scc)
{
    int chan;
    for (chan = 0; chan < 5; chan++) {
        memcpy(scc->wave[chan], scc->reg + chan * 32, 32);
        scc->period[chan] = (scc->reg[0xA0 + chan * 2] | (scc->reg[0xA1 + chan * 2] << 8)) & 0x0FFF;
        scc->freq[chan] = 3579545.0 / (32.0 * (scc->period[chan] + 1));
        scc->volume[chan] = scc->reg[0xAA + chan] & 0x0F;
        scc->enabled[chan] = (scc->reg[0xAF] >> chan) & 1;
    }
}

static void decode_opll(msxdebug_opll *opll)
{
    int chan;
    for (chan = 0; chan < 9; chan++) {
        const int high = opll->reg[0x20 + chan];
        opll->fnum[chan] = opll->reg[0x10 + chan] | ((high & 1) << 8);
        opll->block[chan] = (high >> 1) & 7;
        opll->key_on[chan] = (high >> 4) & 1;
        opll->instrument[chan] = opll->reg[0x30 + chan] >> 4;
        opll->volume[chan] = opll->reg[0x30 + chan] & 0x0F;
        opll->freq[chan] = opll->fnum[chan] * 49716.0 / (double)(1 << (19 - opll->block[chan]));
    }
    opll->rhythm = (opll->reg[0x0E] >> 5) & 1;
}

int msxdebug_sound_get(msxdebug *dbg, msxdebug_sound *out)
{
    chips found;

    memset(out, 0, sizeof(*out));
    if (!dbg || !msxsession_is_running(msxdebug_session(dbg))) return 0;
    find_chips(&found);
    if (found.psg[0] && read_block(found.psg, out->psg.reg, 16)) {
        out->psg.present = 1;
        decode_psg(&out->psg);
    }
    if (found.scc[0] && read_block(found.scc, out->scc.reg, 256)) {
        out->scc.present = 1;
        snprintf(out->scc.name, sizeof(out->scc.name), "%s", found.scc);
        decode_scc(&out->scc);
    }
    if (found.opll[0] && read_block(found.opll, out->opll.reg, 0x40)) {
        out->opll.present = 1;
        snprintf(out->opll.name, sizeof(out->opll.name), "%s", found.opll);
        decode_opll(&out->opll);
    }
    if (found.y8950[0] && read_block(found.y8950, out->y8950.reg, 256)) {
        out->y8950.present = 1;
        snprintf(out->y8950.name, sizeof(out->y8950.name), "%s", found.y8950);
    }
    return out->psg.present || out->scc.present || out->opll.present || out->y8950.present;
}
