#include "ssa_geometry.h"
#include <stdio.h>
#include <string.h>

static void cb(int l, const char *f, va_list v, void *d) { (void)l; (void)f; (void)v; (void)d; }
typedef struct { int x0, y0, x1, y1, n; } BB;

static BB render(const SSA_GEOM *g, const char *script) {
    ASS_Library *lib = ass_library_init(); ass_set_message_cb(lib, cb, NULL);
    ASS_Renderer *r = ass_renderer_init(lib);
    ass_set_fonts(r, NULL, "DejaVu Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 0);
    ssa_geom_apply(r, g);
    ASS_Track *t = ass_read_memory(lib, (char *)script, strlen(script), NULL);
    int ch; ASS_Image *im = ass_render_frame(r, t, 1000, &ch);
    BB b = { 1 << 30, 1 << 30, -1, -1, 0 };
    for (; im; im = im->next)
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w; x++)
                if (im->bitmap[y * im->stride + x]) {
                    int X = im->dst_x + x, Y = im->dst_y + y;
                    if (X < b.x0) b.x0 = X;
                    if (Y < b.y0) b.y0 = Y;
                    if (X > b.x1) b.x1 = X;
                    if (Y > b.y1) b.y1 = Y;
                    b.n++;
                }
    ass_free_track(t); ass_renderer_done(r); ass_library_done(lib);
    return b;
}

#define HDR(px, py, sbas, outl) \
 "[Script Info]\nScriptType: v4.00+\nPlayResX: " #px "\nPlayResY: " #py "\nScaledBorderAndShadow: " sbas "\n\n" \
 "[V4+ Styles]\nFormat: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,Encoding\n" \
 "Style: Default,DejaVu Sans,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1," #outl ",0,2,10,10,30,1\n\n" \
 "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
#define EV(txt) "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,," txt "\n"

static int fails = 0;
static void check(const char *what, int got, int want) {
    if (got != want) { printf("      FAIL %-34s got %d, want %d\n", what, got, want); fails++; }
}

// scenario: canvas + box + coded size. Every check compares against the same script rendered
// on a frame that IS the video box (no bars), shifted by the box origin.
static void scenario(const char *name, int cw, int ch, int bx, int by, int bw, int bh, int rw, int rh) {
    printf("\n== %s   canvas %dx%d  box (%d,%d) %dx%d  coded %dx%d\n", name, cw, ch, bx, by, bw, bh, rw, rh);
    SSA_GEOM g   = ssa_geom_compute(cw, ch, bx, by, bw, bh, rw, rh);
    SSA_GEOM ref = ssa_geom_compute(g.content_w, g.content_h, 0, 0, g.content_w, g.content_h, rw, rh);
    printf("   -> margins t=%d b=%d l=%d r=%d  content %dx%d  storage %dx%d\n",
           g.margin_t, g.margin_b, g.margin_l, g.margin_r, g.content_w, g.content_h, g.storage_w, g.storage_h);

    struct { const char *n; const char *s; int dy_bottom; } T[] = {
        { "1920x1080  regular bottom", HDR(1920,1080,"yes",0) EV("HHHH"), 1 },
        { "1280x720   regular bottom", HDR(1280,720,"yes",0)  EV("HHHH"), 1 },
        { "384x288    regular bottom", HDR(384,288,"no",0)    EV("HHHH"), 1 },
        { "1920x1080  \\an7\\pos(0,0)",  HDR(1920,1080,"yes",0) EV("{\\an7\\pos(0,0)}HHHH"), 0 },
        { "1920x1080  \\an3\\pos(W,H)",  HDR(1920,1080,"yes",0) EV("{\\an3\\pos(1920,1080)}HHHH"), 0 },
        { "384x288 SBAS=no outline=6",   HDR(384,288,"no",6)    EV("HHHH"), 1 },
    };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) {
        BB a = render(&g, T[i].s), r = render(&ref, T[i].s);
        // Regular bottom-aligned text is deliberately placed against the FRAME bottom (use_margins),
        // so vs. a bars-free reference it shifts down by the total bar height; positioned text is
        // placed against the video box, so it shifts by the box origin only.
        int want_y0 = r.y0 + (T[i].dy_bottom ? g.margin_t + g.margin_b : g.margin_t);
        int want_y1 = r.y1 + (T[i].dy_bottom ? g.margin_t + g.margin_b : g.margin_t);
        printf("   %-28s ink %3dx%-3d @(%4d,%4d)   ref %3dx%-3d\n", T[i].n,
               a.x1 - a.x0 + 1, a.y1 - a.y0 + 1, a.x0, a.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);
        check("ink width  == ref", a.x1 - a.x0 + 1, r.x1 - r.x0 + 1);
        check("ink height == ref", a.y1 - a.y0 + 1, r.y1 - r.y0 + 1);
        check("x offset   == ref + box_x", a.x0, r.x0 + g.margin_l);
        check("y top      as expected", a.y0, want_y0);
        check("y bottom   as expected", a.y1, want_y1);
    }
}

int main(void) {
    // NOTE on "regular bottom" expectation: with use_margins the text sits against the FRAME
    // bottom, so relative to a video-only reference it moves down by (top bar + bottom bar).
    // For box-only frames (no bars) that shift is 0.
    scenario("portrait phone, 16:9 video, bars used", 1080, 2400, 0, 896, 1080, 608, 1920, 1080);
    scenario("landscape, video fills canvas",          1920, 1080, 0, 0, 1920, 1080, 1920, 1080);
    scenario("4:3 video on 16:9 TV (pillarbox only)",  1440, 1080, 0, 0, 1440, 1080, 1440, 1080);
    scenario("tablet portrait, 21:9 video",            1600, 2560, 0, 1015, 1600, 686, 2560, 1080);
    printf("\n-- degenerate / transient inputs (must not produce negative margins or crash) --\n");
    scenario("transient: landscape box in old portrait canvas", 1080, 2400, 0, 0, 2400, 1080, 1920, 1080);
    scenario("box not known yet (all zero)",           1080, 2400, 0, 0, 0, 0, 1920, 1080);
    scenario("canvas not known yet",                   0, 0, 0, 0, 0, 0, 0, 0);
    scenario("rotated video reported un-rotated",      1080, 2400, 0, 896, 1080, 608, 1080, 1920);
    printf("\n%s (%d failed checks)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails != 0;
}
