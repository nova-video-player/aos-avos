/*
 * subtitle_pgs.c
 *
 * External PGS (.sup) subtitle format support.
 *
 * A .sup file is a flat sequence of PGS segments, each prefixed on-disk
 * with a 13-byte envelope:
 *
 *     2 bytes   'P','G' magic
 *     4 bytes   PTS, 90kHz clock, big-endian
 *     4 bytes   DTS, 90kHz clock, big-endian (unused here)
 *     1 byte    segment_type  (0x14 PDS, 0x15 ODS, 0x16 PCS, 0x17 WDS, 0x80 END)
 *     2 bytes   segment_size, big-endian
 *     N bytes   payload (segment_size bytes)
 *
 * One "Display Set" = one on-screen subtitle update = a PCS, optionally a
 * WDS, one or more PDS/ODS, terminated by exactly one zero-length END
 * segment. codec_ffsub.c's ffmpeg PGS decoder (AV_CODEC_ID_HDMV_PGS_SUBTITLE)
 * expects one Display Set's segments concatenated WITHOUT the 10-byte
 * 'PG'+PTS+DTS envelope — just type(1)+size(2)+payload repeated, exactly
 * like MKV's S_HDMV/PGS demuxed packets.
 *
 * This parser doesn't decode bitmaps itself — same division of labor as
 * subtitle_idx.c/VobSub: parse_SUP() just indexes every Display Set by its
 * file offset and PTS into a sub_line list, and get_gfx_SUP() re-reads and
 * re-packs the raw segment bytes for a given offset on demand. The actual
 * bitmap decode happens in codec_ffsub.c, already wired for SUB_FORMAT_PGS.
 */

#include "global.h"
#include "debug.h"
#include "subtitle_format.h"
#include "astdlib.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define DBG if(Debug[DBG_SUB])

#define PGS_SEG_PDS 0x14
#define PGS_SEG_ODS 0x15
#define PGS_SEG_PCS 0x16
#define PGS_SEG_WDS 0x17
#define PGS_SEG_END 0x80

// ---------------------------------------------------------------------------
// detect_SUP
//
// Reads the first segment envelope: requires the 'P','G' magic and a
// segment_type byte that's one of the five documented PGS types. Checking
// the type (not just the magic) avoids false-positives on arbitrary binary
// files that happen to start with 0x50 0x47.
// ---------------------------------------------------------------------------
static int detect_SUP( FILE *file )
{
	fseek( file, 0, SEEK_SET );

	unsigned char hdr[13];
	if ( fread( hdr, 1, sizeof(hdr), file ) != sizeof(hdr) ) {
DBG serprintf( "SUP: not SUP (too short)\n" );
		return 1;
	}

	if ( hdr[0] != 'P' || hdr[1] != 'G' ) {
DBG serprintf( "SUP: not SUP (no PG magic)\n" );
		return 1;
	}

	unsigned char seg_type = hdr[10];
	if ( seg_type != PGS_SEG_PDS && seg_type != PGS_SEG_ODS &&
	     seg_type != PGS_SEG_PCS && seg_type != PGS_SEG_WDS &&
	     seg_type != PGS_SEG_END ) {
DBG serprintf( "SUP: not SUP (bad segment type 0x%02X)\n", seg_type );
		return 1;
	}

DBG serprintf( "SUP: found!\n" );
	return 0;
}

// ---------------------------------------------------------------------------
// parse_SUP
//
// Walks the whole file once, recording one sub_line per Display Set:
//   pos   = file offset of the Display Set's first segment (its PCS)
//   start = that PCS segment's PTS, converted from 90kHz ticks to ms
//   end   = start + 100 (dummy — same convention as parse_IDX; the real
//           on-screen duration comes from codec_ffsub's decode of the
//           Display Set itself, including empty Display Sets that signal
//           "clear the screen now")
//
// No bitmap data is touched here — only get_gfx_SUP() below reads payload.
// ---------------------------------------------------------------------------
static uni_sub *parse_SUP( subt_orig *spex, int clean_tags )
{
	(void)clean_tags; // PGS bitmaps carry no text to strip tags from

	if ( !spex || !spex->filename ) {
DBG serprintf( "SUP: invalid params\n" );
		return NULL;
	}

	FILE *fd = fopen( spex->filename, "rb" );
	if ( !fd ) {
DBG serprintf( "SUP: cannot open %s\n", spex->filename );
		return NULL;
	}

	uni_sub *sub = acalloc( 1, sizeof( uni_sub ) );
	if ( !sub ) {
		fclose( fd );
		return NULL;
	}
	sub->is_pgs = 1;
	// Load-bearing: subtitle_free_converted() only calls format->close()
	// (which closes sub->sup_fd) when this is set — see close_SUP() below.
	sub->format = spex->format;

	long   ds_pos = -1;
	int    ds_pts_ms = 0;
	int    in_ds = 0;
	unsigned char hdr[13];

	for ( ;; ) {
		long seg_pos = ftell( fd );
		if ( fread( hdr, 1, sizeof(hdr), fd ) != sizeof(hdr) ) {
			break; // EOF (or trailing partial garbage) — stop cleanly
		}
		if ( hdr[0] != 'P' || hdr[1] != 'G' ) {
DBG serprintf( "SUP: desync at offset %ld, stopping\n", seg_pos );
			break;
		}

		uint32_t pts90 = ((uint32_t)hdr[2] << 24) | ((uint32_t)hdr[3] << 16) |
		                 ((uint32_t)hdr[4] << 8)  |  (uint32_t)hdr[5];
		unsigned char seg_type = hdr[10];
		int seg_size = (hdr[11] << 8) | hdr[12];

		if ( seg_type == PGS_SEG_PCS && !in_ds ) {
			ds_pos    = seg_pos;
			ds_pts_ms = (int)(pts90 / 90); // 90kHz ticks -> ms
			in_ds     = 1;
		}

		if ( seg_size > 0 && fseek( fd, seg_size, SEEK_CUR ) != 0 ) {
			break; // truncated file
		}

		if ( seg_type == PGS_SEG_END && in_ds ) {
			sub_line *node = acalloc( 1, sizeof( sub_line ) );
			node->start = ds_pts_ms;
			node->end   = ds_pts_ms + 100; // dummy, see comment above
			node->pos   = (uint32_t)ds_pos;
			if ( sub->first == 0 ) {
				sub->first = node;
				sub->last  = node;
			} else {
				sub->last->next = node;
				node->prev       = sub->last;
				sub->last        = node;
			}
			in_ds = 0;
		}
	}
	fclose( fd );

	if ( !sub->first ) {
DBG serprintf( "SUP: no display sets found in %s\n", spex->filename );
		afree( sub );
		return NULL;
	}

DBG serprintf( "SUP: parsed %s\n", spex->filename );
	return sub;
}

// ---------------------------------------------------------------------------
// get_gfx_SUP
//
// Re-reads the Display Set starting at file offset `pos`, stripping the
// 10-byte 'PG'+PTS+DTS envelope from each segment and keeping only
// type(1)+size(2)+payload — the exact byte layout codec_ffsub.c's
// avcodec_decode_subtitle2()/AV_CODEC_ID_HDMV_PGS_SUBTITLE expects.
//
// Unlike VobSub's MPEG-PES scan (which has to hunt for packet boundaries),
// PGS segments carry their own length, so this is a precise seek + read —
// no speculative chunk buffering needed.
// ---------------------------------------------------------------------------
static int get_gfx_SUP( uni_sub *sub, uint32_t pos, uint8_t *data, int *size )
{
	int out_max = *size;
	*size = 0;

	if ( !sub->sup_fd ) {
		if ( !sub->spex || !sub->spex->filename ) return 1;
		sub->sup_fd = fopen( sub->spex->filename, "rb" );
		if ( !sub->sup_fd ) return 1;
	}

	if ( fseek( sub->sup_fd, (long)pos, SEEK_SET ) != 0 ) return 1;

	int out_len = 0;
	unsigned char hdr[13];

	while ( fread( hdr, 1, sizeof(hdr), sub->sup_fd ) == sizeof(hdr) ) {
		if ( hdr[0] != 'P' || hdr[1] != 'G' ) break; // desync, stop

		unsigned char seg_type = hdr[10];
		int seg_size = (hdr[11] << 8) | hdr[12];

		if ( out_len + 3 + seg_size > out_max ) {
DBG serprintf( "SUP: gfx buffer too small (%d) for display set at %u\n", out_max, pos );
			break;
		}

		// type(1) + size(2), big-endian — envelope-free, decoder-ready
		data[out_len++] = hdr[10];
		data[out_len++] = hdr[11];
		data[out_len++] = hdr[12];

		if ( seg_size > 0 ) {
			if ( (int)fread( data + out_len, 1, seg_size, sub->sup_fd ) != seg_size ) break;
			out_len += seg_size;
		}

		if ( seg_type == PGS_SEG_END ) break; // display set complete
	}

	if ( out_len == 0 ) return 1;

	*size = out_len;
	return 0;
}

static int close_SUP( uni_sub *sub )
{
	if ( sub->sup_fd ) {
DBG serprintf( "close_SUP\n" );
		fclose( sub->sup_fd );
		sub->sup_fd = NULL;
	}
	return 0;
}

static struct SUBTITLE_FORMAT SUP = {
	"PGS",
	detect_SUP,
	NULL,		// no info() — one .sup file is one track, no language header
	parse_SUP,
	get_gfx_SUP,
	close_SUP,
	NULL,		// no feed() — bitmap track, uses pos-indexed get_gfx like VobSub
};

SUBTITLE_REGISTER_FORMAT( SUP );
