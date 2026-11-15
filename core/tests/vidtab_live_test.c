/*
 * vidtab_live_test -- the Video tab (core/debugger/vidtab.c) on a booted
 * C-BIOS MSX2:
 *
 *   - the Display view of a machine stopped by the debugger is exactly the
 *     frame the session published (msxsession_copy_frame), pixel for pixel;
 *   - the VDP is read live at the stop (mode, registers, palette, VRAM), the
 *     per-line log of the running machine reaches the tab, and the beam is
 *     known;
 *   - "Break on writes" on a name table entry: a program in RAM writing a
 *     different VRAM address does not stop the machine, one writing that
 *     address stops it at the OUT to the data port.
 *
 * Skips (77) when no machine can be started (no C-BIOS in the share tree).
 * The RAM program needs page 3 mapped by the BIOS; if that never happens the
 * break section prints a note and is skipped.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msx_host.hh"
#include "msxdebug.h"
#include "msxsession.h"
#include "msxvidtab.h"
#include "test_sleep.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "vidtab_live_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static int wait_stopped(msxdebug *dbg, int timeout_ms)
{
    int waited;
    for (waited = 0; waited < timeout_ms; waited += 20) {
        if (msxdebug_is_stopped(dbg)) return 1;
        sleep_ms(20);
    }
    return 0;
}

/* A program at $C000 that points the VRAM address latch at addr and writes one byte. */
static int load_vram_writer(msxdebug *dbg, unsigned addr)
{
    const uint8_t code[] = {
        0xF3,                                   /* DI */
        0x3E, (uint8_t)(addr & 0xFF),           /* LD A,low */
        0xD3, 0x99,                             /* OUT ($99),A */
        0x3E, (uint8_t)(((addr >> 8) & 0x3F) | 0x40),   /* LD A,high | write */
        0xD3, 0x99,                             /* OUT ($99),A */
        0x3E, 0xAA,                             /* LD A,$AA */
        0xD3, 0x98,                             /* OUT ($98),A */
        0x18, 0xFE                              /* JR $ */
    };
    uint8_t back[sizeof code];
    for (size_t idx = 0; idx < sizeof code; idx++) msxdebug_write(dbg, (uint16_t)(0xC000 + idx), code[idx]);
    if (msxdebug_read(dbg, 0xC000, back, (int)sizeof back) != (int)sizeof back) return 0;
    return memcmp(back, code, sizeof code) == 0;
}

/* With VIDTAB_DUMP_DIR set, pictures are written there as PPM files for a look. */
static void dump_picture(const char *name, const uint32_t *pix, int wide, int high)
{
    const char *dir = getenv("VIDTAB_DUMP_DIR");
    char path[512];
    FILE *outfile;
    if (!dir || !*dir) return;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    outfile = fopen(path, "wb");
    if (!outfile) return;
    fprintf(outfile, "P6\n%d %d\n255\n", wide, high);
    for (int idx = 0; idx < wide * high; idx++) {
        const unsigned char rgb[3] = { (unsigned char)(pix[idx] >> 16), (unsigned char)(pix[idx] >> 8), (unsigned char)pix[idx] };
        fwrite(rgb, 1, 3, outfile);
    }
    fclose(outfile);
}

/* Loads code at $C000 and runs it. Returns 1 when the machine stopped within wait_ms, 0 when it
 * ran on, -1 when the program would not load. */
static int run_code(msxdebug *dbg, const uint8_t *code, size_t length, int wait_ms)
{
    uint8_t back[64];
    if (msxdebug_is_stopped(dbg) == 0) {
        msxdebug_stop(dbg);
        if (!wait_stopped(dbg, 3000)) return -1;
    }
    for (size_t idx = 0; idx < length; idx++) msxdebug_write(dbg, (uint16_t)(0xC000 + idx), code[idx]);
    if (msxdebug_read(dbg, 0xC000, back, (int)length) != (int)length || memcmp(back, code, length) != 0) return -1;
    msxdebug_cpu_set(dbg, MSX_REG_SP, 0xF000);
    msxdebug_cpu_set(dbg, MSX_REG_PC, 0xC000);
    msxdebug_resume(dbg);
    return wait_stopped(dbg, wait_ms) ? 1 : 0;
}

int main(int argc, char **argv)
{
    msxsession *sess;
    msxsession_start_opts opts;
    msxdebug *dbg;
    msxvid_tab *tab;
    uint32_t *framebuf;
    uint64_t serial = 0;
    int waited_ms, wide = 0, high = 0;
    const uint32_t *pic;
    char buf[8192];

    sess = msxsession_new(NULL);
    if (!sess) { fprintf(stderr, "vidtab_live_test: msxsession_new failed\n"); return 1; }
    msxsession_default_opts(sess, &opts);
    opts.enable_fujinet = 0;
    opts.machine = argc > 1 ? argv[1] : MSXSESSION_MACHINE_MSX2;     /* another machine id to try by hand */
    if (msxsession_start(sess, &opts) != 0) {
        fprintf(stderr, "vidtab_live_test: cannot start a machine (%s): skipping\n", msxsession_last_error(sess));
        msxsession_free(sess);
        return 77;
    }
    framebuf = malloc((size_t)MSXSESSION_FB_WIDTH * MSXSESSION_FB_HEIGHT * sizeof(uint32_t));
    dbg = msxsession_debugger(sess);
    tab = msxvid_tab_new(dbg);
    if (!framebuf || !dbg || !tab) { fprintf(stderr, "vidtab_live_test: setup failed\n"); failures = 1; goto done; }
    msxvid_tab_set_viewport(tab, 640, 480);
    msxvid_tab_set_zoom(tab, 1);
    msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
    msxvid_tab_set_overlays(tab, 0);    /* sprite boxes and the like would change the picture */

    /* let the machine run until the tab has a captured frame and the log is going */
    for (waited_ms = 0; waited_ms < 15000; waited_ms += 50) {
        const unsigned flags = msxvid_tab_refresh(tab, 1);
        if (!(flags & MSXVID_TAB_OFF)) break;
        sleep_ms(50);
    }
    CHECK(msxvid_tab_picture(tab, &wide, &high) != NULL, "no picture while running: %s", msxvid_tab_why(tab));
    sleep_ms(500);
    msxvid_tab_refresh(tab, 1);
    msxvid_tab_text(tab, MSXVID_S_TIMING, buf, (int)sizeof buf);
    CHECK(strstr(buf, "Logged frame"), "timing section: %s", buf);
    CHECK(strstr(msxvid_tab_status(tab), "Running"), "running status: %s", msxvid_tab_status(tab));
    printf("vidtab_live_test: running status: %s\n", msxvid_tab_status(tab));

    /* stop it: the Display view is the published frame */
    msxdebug_attach(dbg);
    CHECK(wait_stopped(dbg, 3000), "attach did not stop the machine");
    msxsession_copy_frame(sess, framebuf, &serial);
    msxvid_tab_refresh(tab, 1);
    pic = msxvid_tab_picture(tab, &wide, &high);
    CHECK(pic && wide == 640 && high == 480, "stopped display picture %dx%d", wide, high);
    if (pic) {
        long diff = 0;
        for (int idx = 0; idx < 640 * 480; idx++)
            if ((pic[idx] & 0xFFFFFFu) != (framebuf[idx] & 0xFFFFFFu)) diff++;
        CHECK(diff == 0, "the Display view differs from the published frame in %ld pixels", diff);
    }
    /* the Decoded screen sits where the Display says it does: two points of the
     * display have the same colour in one exactly when they do in the other (the
     * sprite boxes are off; sprites are drawn over the Display but not over the
     * Decoded screen, so a few violations are tolerated) */
    {
        uint32_t *shot = malloc((size_t)640 * 480 * sizeof *shot);
        if (shot && pic) {
            memcpy(shot, pic, (size_t)640 * 480 * sizeof *shot);
            dump_picture("display", shot, 640, 480);
            msxvid_tab_set_view(tab, MSXVID_V_DECODED);
            msxvid_tab_set_source(tab, MSXVID_SRC_SCREEN);
            msxvid_tab_refresh(tab, 1);
            pic = msxvid_tab_picture(tab, &wide, &high);
            if (pic && (wide == 256 || wide == 512)) {
                long bad = 0, tried = 0, same = 0;
                dump_picture("decoded", pic, wide, high);
                {
                    uint32_t seed = 12345;
                    for (int round = 0; round < 6000; round++) {
                        int firstx, firsty, secondx, secondy, fax, fay, fbx, fby;
                        seed = seed * 1664525u + 1013904223u; firstx = (int)((seed >> 8) % (unsigned)wide);
                        seed = seed * 1664525u + 1013904223u; firsty = (int)((seed >> 8) % (unsigned)high);
                        seed = seed * 1664525u + 1013904223u; secondx = (int)((seed >> 8) % (unsigned)wide);
                        seed = seed * 1664525u + 1013904223u; secondy = (int)((seed >> 8) % (unsigned)high);
                        fax = 64 + firstx * (512 / wide); fbx = 64 + secondx * (512 / wide);
                        fay = 2 * (42 - 18 + firsty); fby = 2 * (42 - 18 + secondy);
                        if (fay >= 480 || fby >= 480) continue;
                        tried++;
                        if (((pic[firsty * wide + firstx] & 0xFFFFFF) == (pic[secondy * wide + secondx] & 0xFFFFFF)) !=
                            ((shot[fay * 640 + fax] & 0xFFFFFF) == (shot[fby * 640 + fbx] & 0xFFFFFF))) bad++;
                        if ((pic[firsty * wide + firstx] & 0xFFFFFF) == (shot[fay * 640 + fax] & 0xFFFFFF)) same++;
                    }
                }
                printf("vidtab_live_test: decoded vs display: %ld of %ld pairs disagree, %ld of %ld pixels identical\n", bad, tried, same, tried);
                CHECK(tried > 1000 && bad * 50 < tried, "the Decoded screen does not line up with the Display (%ld of %ld pairs)", bad, tried);
            } else {
                printf("vidtab_live_test: this mode has no Decoded screen (%s)\n", msxvid_tab_why(tab));
            }
            msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
            msxvid_tab_refresh(tab, 1);
        }
        free(shot);
    }
    printf("vidtab_live_test: stopped status: %s\n", msxvid_tab_status(tab));
    CHECK(strstr(msxvid_tab_status(tab), "Stopped"), "stopped status: %s", msxvid_tab_status(tab));

    /* the VDP is read live: a V9938, a mode, 28+9 registers, a palette, VRAM */
    {
        const msxvid_reg *regs;
        const msxvid_swatch *swat;
        int count = msxvid_tab_regs(tab, &regs);
        CHECK(count == 37 || count == 28, "register entries %d", count);
        CHECK(strstr(msxvid_tab_status(tab), "V99") != NULL || strstr(msxvid_tab_status(tab), "V9918") != NULL, "chip in status");
        CHECK(msxvid_tab_swatches(tab, &swat) == 16, "swatches");
        msxvid_tab_text(tab, MSXVID_S_VRAM, buf, (int)sizeof buf);
        CHECK(strncmp(buf, "00000", 5) == 0 && strlen(buf) > 200, "vram dump");
        msxvid_tab_text(tab, MSXVID_S_LINES, buf, (int)sizeof buf);
        CHECK(strstr(buf, "lines 0-") != NULL, "lines section: %s", buf);
        printf("vidtab_live_test: lines: %s", buf);
    }

    /* the beam: known at a stop */
    msxvid_tab_set_view(tab, MSXVID_V_BEAM);
    msxvid_tab_refresh(tab, 1);
    msxvid_tab_text(tab, MSXVID_S_TIMING, buf, (int)sizeof buf);
    printf("vidtab_live_test: timing at the stop:\n%s", buf);
    CHECK(strstr(buf, "Beam: line") != NULL, "the beam position is unknown at a stop (machine_info names?): %s", buf);
    CHECK(msxvid_tab_picture(tab, &wide, &high) != NULL, "beam view: %s", msxvid_tab_why(tab));

    /* every other view and source builds on a real C-BIOS screen */
    for (int view = 0; view < MSXVID_V_COUNT; view++) {
        msxvid_tab_set_view(tab, view);
        for (int source = 0; source < MSXVID_SRC_COUNT; source++) {
            msxvid_tab_set_source(tab, source);
            msxvid_tab_refresh(tab, 1);
            pic = msxvid_tab_picture(tab, &wide, &high);
            if (!pic)
                printf("vidtab_live_test: view %s source %s: no picture (%s)\n", msxvid_view_name(view), msxvid_source_name(source), msxvid_tab_why(tab));
            else {
                char name[64];
                snprintf(name, sizeof name, "%s_%s", msxvid_view_key(view), msxvid_source_key(source));
                dump_picture(name, pic, wide, high);
            }
        }
    }

    /* Break on writes on a name table entry */
    {
        const msxvid_pixel *pixp;
        unsigned target;
        int ready = 0, bpid;
        char why[160];
        int stop_addr = 0;

        msxvid_tab_set_view(tab, MSXVID_V_VALUES);
        msxvid_tab_refresh(tab, 1);
        if (!msxvid_tab_picture(tab, &wide, &high)) {
            printf("vidtab_live_test: no Values picture (%s): skipping Break on writes\n", msxvid_tab_why(tab));
            goto after_break;
        }
        msxvid_tab_hover(tab, 1, 1);
        pixp = msxvid_tab_pixel(tab);
        CHECK(pixp && pixp->nbreak == 1 && pixp->break_kind[0] == MSXVID_BK_VRAM, "values hover has no VRAM target");
        if (!pixp || pixp->nbreak != 1) goto after_break;
        target = pixp->break_lo[0];
        printf("vidtab_live_test: break target VRAM $%04X\n", target);
        CHECK(msxvid_tab_can_break(tab), "can_break");
        bpid = msxvid_tab_break_on_pixel(tab);
        CHECK(bpid > 0, "break_on_pixel returned %d", bpid);
        CHECK(msxvid_tab_break_on_pixel(tab) == bpid, "a second Break on writes should find the first");

        /* the BIOS maps RAM into page 3 a little after boot; retry the program load until it sticks */
        msxdebug_resume(dbg);
        for (waited_ms = 0; waited_ms < 8000 && !ready; waited_ms += 100) {
            sleep_ms(100);
            msxdebug_stop(dbg);
            if (!wait_stopped(dbg, 2000)) break;
            ready = load_vram_writer(dbg, (target + 100) & 0x3FFF);
            if (!ready) msxdebug_resume(dbg);
        }
        if (!ready) {
            printf("vidtab_live_test: page 3 never became RAM: skipping Break on writes\n");
            goto after_break;
        }
        /* a different address: runs on */
        msxdebug_cpu_set(dbg, MSX_REG_SP, 0xF000);
        msxdebug_cpu_set(dbg, MSX_REG_PC, 0xC000);
        msxdebug_resume(dbg);
        sleep_ms(500);
        CHECK(!msxdebug_is_stopped(dbg), "a write to another address stopped the machine");
        msxdebug_stop(dbg);
        CHECK(wait_stopped(dbg, 3000), "could not stop");
        CHECK(load_vram_writer(dbg, target & 0x3FFF), "second program did not load");
        msxdebug_cpu_set(dbg, MSX_REG_SP, 0xF000);
        msxdebug_cpu_set(dbg, MSX_REG_PC, 0xC000);
        msxdebug_resume(dbg);
        CHECK(wait_stopped(dbg, 3000), "the write to the watched address did not stop the machine");
        msxdebug_stop_reason(dbg, why, (int)sizeof why, &stop_addr);
        printf("vidtab_live_test: stop reason: %s\n", why);
        CHECK(strstr(why, "watchpoint") != NULL || strstr(why, "breakpoint") != NULL, "stop reason: %s", why);

        /* the other two targets: a register (OUT to the control port $99 with the register's
         * byte) and, on a V9938 or later, a palette entry (OUT to $9A). Each program writes
         * the wrong thing first and must not stop there. */
        {
            const uint8_t reg_wrong[] = { 0xF3, 0x3E, 0x04, 0xD3, 0x99, 0x3E, 0x88, 0xD3, 0x99, 0x18, 0xFE };  /* R#8 */
            const uint8_t reg_right[] = { 0xF3, 0x3E, 0x04, 0xD3, 0x99, 0x3E, 0x87, 0xD3, 0x99, 0x18, 0xFE };  /* R#7 */
            const uint8_t pal_write[] = { 0xF3, 0x3E, 0x07, 0xD3, 0x9A, 0x18, 0xFE };
            const msxvid_reg *regs;
            msxdebug_breakpoint_clear(dbg);
            msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
            msxvid_tab_refresh(tab, 1);
            msxvid_tab_hover(tab, 2, 2);
            pixp = msxvid_tab_pixel(tab);
            CHECK(pixp && pixp->kind == MSXVID_PK_BORDER && pixp->nbreak == 1 && pixp->break_kind[0] == MSXVID_BK_REGISTER &&
                  pixp->break_lo[0] == 7, "the border is made from R#7");
            bpid = msxvid_tab_break_on_pixel(tab);
            CHECK(bpid > 0, "break on the backdrop register: %d", bpid);
            CHECK(run_code(dbg, reg_wrong, sizeof reg_wrong, 400) == 0, "a write of another register stopped the machine");
            CHECK(run_code(dbg, reg_right, sizeof reg_right, 3000) == 1, "a write of R#7 did not stop the machine");
            msxdebug_stop_reason(dbg, why, (int)sizeof why, &stop_addr);
            printf("vidtab_live_test: register stop reason: %s\n", why);
            if (msxvid_tab_regs(tab, &regs) == 37) {
                msxdebug_breakpoint_clear(dbg);
                msxvid_tab_set_view(tab, MSXVID_V_DECODED);
                msxvid_tab_set_source(tab, MSXVID_SRC_PALETTE);
                msxvid_tab_refresh(tab, 1);
                msxvid_tab_hover(tab, 5, 5);
                pixp = msxvid_tab_pixel(tab);
                CHECK(pixp && pixp->kind == MSXVID_PK_PALETTE && pixp->nbreak == 1 && pixp->break_kind[0] == MSXVID_BK_PALETTE,
                      "a palette swatch is made from palette writes");
                CHECK(msxvid_tab_break_on_pixel(tab) > 0, "break on a palette entry");
                CHECK(run_code(dbg, pal_write, sizeof pal_write, 3000) == 1, "a palette write did not stop the machine");
                msxdebug_stop_reason(dbg, why, (int)sizeof why, &stop_addr);
                printf("vidtab_live_test: palette stop reason: %s\n", why);
            }
            msxdebug_breakpoint_clear(dbg);
        }

        /* a raster effect: a program that changes the backdrop (R#7) partway down every
         * frame. The per-line log of the running machine splits the frame into bands. */
        {
            static const uint8_t raster[] = {
                0xFB,                           /* EI */
                0x76,                           /* HALT: the vertical blank */
                0x0E, 0x0C,                     /* LD C,12 */
                0x06, 0x00,                     /* LD B,0 */
                0x10, 0xFE,                     /* DJNZ $ */
                0x0D,                           /* DEC C */
                0x20, 0xF9,                     /* JR NZ,outer */
                0x3E, 0x0A, 0xD3, 0x99, 0x3E, 0x87, 0xD3, 0x99,     /* R#7 = 10 */
                0x0E, 0x01, 0x06, 0x00, 0x10, 0xFE, 0x0D, 0x20, 0xF9,   /* a shorter wait */
                0x3E, 0x04, 0xD3, 0x99, 0x3E, 0x87, 0xD3, 0x99,     /* R#7 = 4 */
                0x18, 0xDB                      /* JR to the HALT */
            };
            uint8_t back[sizeof raster];
            const msxvid_band *bands;
            int count, saw_ten = 0;
            msxdebug_breakpoint_clear(dbg);
            for (size_t idx = 0; idx < sizeof raster; idx++) msxdebug_write(dbg, (uint16_t)(0xC000 + idx), raster[idx]);
            CHECK(msxdebug_read(dbg, 0xC000, back, (int)sizeof back) == (int)sizeof back && memcmp(back, raster, sizeof raster) == 0,
                  "raster program did not load");
            msxdebug_cpu_set(dbg, MSX_REG_SP, 0xF000);
            msxdebug_cpu_set(dbg, MSX_REG_PC, 0xC000);
            msxvid_tab_set_view(tab, MSXVID_V_DECODED);
            msxvid_tab_set_source(tab, MSXVID_SRC_SCREEN);
            msxdebug_resume(dbg);
            sleep_ms(1500);
            msxvid_tab_refresh(tab, 1);
            count = msxvid_tab_lines(tab, &bands);
            for (int idx = 0; idx < count; idx++) {
                printf("vidtab_live_test: raster band: %s\n", bands[idx].text);
                if (bands[idx].backdrop == 10) saw_ten = 1;
            }
            CHECK(count >= 3 && saw_ten, "the raster program should make 3 bands with a backdrop of 10: %d bands", count);
            msxvid_tab_text(tab, MSXVID_S_REGISTERS, buf, (int)sizeof buf);
            CHECK(strstr(buf, "writes, first at line") != NULL || strstr(buf, "write, first at line") != NULL, "registers text has no per-line write counts");
            CHECK(msxvid_tab_picture(tab, &wide, &high) != NULL, "split screen picture: %s", msxvid_tab_why(tab));
            dump_picture("raster_decoded", msxvid_tab_picture(tab, &wide, &high), wide, high);
            msxvid_tab_set_view(tab, MSXVID_V_DISPLAY);
            msxvid_tab_set_overlays(tab, MSXVID_OV_SPLITS);
            msxvid_tab_refresh(tab, 1);
            pic = msxvid_tab_picture(tab, &wide, &high);
            if (pic) dump_picture("raster_display", pic, wide, high);
            msxvid_tab_set_overlays(tab, 0);
        }
    }
after_break:

    /* the console's one-shot */
    {
        const int len = msxvid_tab_text_for(dbg, "timing", buf, (int)sizeof buf);
        CHECK(len > 0 && strstr(buf, "Standard"), "text_for: %s", buf);
        CHECK(msxvid_tab_text_for(dbg, NULL, buf, (int)sizeof buf) > 0 && strstr(buf, "R#0"), "text_for status");
    }

done:
    msxvid_tab_free(tab);
    if (dbg) msxdebug_detach(dbg);
    free(framebuf);
    msxsession_stop(sess);
    msxsession_free(sess);
    if (!failures) printf("vidtab_live_test: ok\n");
    return failures;
}
