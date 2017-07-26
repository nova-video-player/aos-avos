/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _AV_H
#define _AV_H

#include "global.h"
#include "types.h"
#include "rect.h"
#include "h264_sps.h"

#include <unistd.h>

// the first values MUST be defined by hand because 
// we use them in stuff like bookmarks or MTPLIB which are
// stored on the HDD and therefore the values
// must be persistent accross SW updates!
// please also do not put #ifdefs here
typedef enum {
	TYPE_NONE 		= 0,
	TYPE_AUD		= 1,
	TYPE_VID		= 2,
	TYPE_PIC		= 3,
	TYPE_DIR		= 4,
	TYPE_LST		= 5,
	TYPE_TXT		= 6,
	TYPE_UNKNOWN		= 7,
	TYPE_MPN		= 8,
	TYPE_PDF		= 9,
	TYPE_PLS		= 10,
	TYPE_HTML		= 11,
	TYPE_APPL		= 12,
	TYPE_SUBT		= 13,
	TYPE_LYR		= 14,
	TYPE_SWF		= 15,
	// dead		 	= 16,
	TYPE_DOCUMENT		= 17,
	TYPE_EMX		= 18,
	TYPE_APK		= 19,
	// add more "real" file types here

	TYPE_OPEN_FOLDER	= 100,
	TYPE_UPDATE,
	TYPE_ROOT,
	TYPE_PURCHASE,
	TYPE_ABSTRACT_ROOT,
	TYPE_FM_RADIO,
	// add more "virtual" file types here

	// should be last
	TYPE_ANY
} AV_TYPE;

// please do not put #ifdefs here
typedef enum {
	ETYPE_NONE 		= 0,
	ETYPE_MP3		= 1,
	ETYPE_WMA		= 2,
	ETYPE_PCM		= 3,
	ETYPE_AAC		= 4,
	ETYPE_M3U		= 5,
	ETYPE_PLA		= 6,
	ETYPE_JPG		= 7,
	ETYPE_BMP		= 8,
	ETYPE_PNG		= 9,
	ETYPE_AVI		= 10,
	ETYPE_ASF		= 11,
	ETYPE_MPG		= 12,
	ETYPE_DPD		= 13,
	ETYPE_AVU		= 14,
	ETYPE_MP4		= 15,
	ETYPE_MPEG_PS		= 16,
	ETYPE_RCV		= 17,
	ETYPE_MPEG_TS		= 18,
	ETYPE_MPEG_RAW		= 19,
	ETYPE_RCA		= 20,
	ETYPE_AC3		= 21,
	ETYPE_PLS		= 22,
	ETYPE_H264_RAW		= 23,
	ETYPE_YUV		= 24,
	ETYPE_AMV		= 25,
	ETYPE_MPEG_TS_LIVE	= 26,
	ETYPE_MMS		= 27,
	ETYPE_MPG4_RAW		= 28,
	ETYPE_FLV		= 29,
	ETYPE_RM		= 30,
	ETYPE_GIF		= 31,
	ETYPE_MMSH		= 32,
	ETYPE_IMG_RAW		= 33,
	ETYPE_RCRV		= 34,
	ETYPE_ICO		= 35,
	ETYPE_RTSP		= 36,
	ETYPE_SPEK		= 37,
	ETYPE_AMR		= 38,
	ETYPE_ASX		= 39,
	ETYPE_FLAC		= 40,
	ETYPE_OGG		= 41,
	ETYPE_SDP		= 42,
	ETYPE_DOC		= 43,
	ETYPE_XLS		= 44,
	ETYPE_PPT		= 45,
	ETYPE_XPL		= 46,
	ETYPE_XML      		= 47,
	ETYPE_MKV      		= 48,
	// do not use		= 49!
	ETYPE_DTS      		= 50,
	ETYPE_WAVPACK  		= 51,
	ETYPE_TTA      		= 52,
	ETYPE_MKA      		= 53,
	ETYPE_MJPG    		= 54,
	ETYPE_WTV    		= 55,
	ETYPE_OGV    		= 56,
	// add more "real" etypes here
	
	ETYPE_ARTIST		= 100,
	ETYPE_U_ARTIST,
	ETYPE_ALBUM,
	ETYPE_U_ALBUM,
	ETYPE_TITLE,
	ETYPE_GENRE,
	ETYPE_DATE,
	ETYPE_PLAYLIST,
	ETYPE_RADIO,
	ETYPE_FAVORITES,
	ETYPE_BROWSE_HD,
	ETYPE_PHOTO,
	ETYPE_NETWORK,
	ETYPE_WORKGROUP,
	ETYPE_HOST,
	ETYPE_SHARE,
	ETYPE_RATING,
	ETYPE_PODCAST,
	ETYPE_RECORD,
	ETYPE_MEDIA,

	// add more "virtual" etypes here
	ETYPE_PURCHASE,
	ETYPE_ABSTRACT_ROOT,

	// UPNP stuff
	ETYPE_UPNP,		// upnp
	ETYPE_UPNPCD,		// upnp content directory
	
	ETYPE_CARD,		// SD/MMC card reader

	ETYPE_SEARCH,
	ETYPE_HOME_SHORTCUT,

	ETYPE_SWISSCOM_STORE,	// online swisscom store

	// should be last
	ETYPE_ANY
	
} AV_ETYPE;

// *************************************************************************
// WAVE format codes
// Comes from Windows Platform SDK\Include\MMReg.h
// See also http://msdn.microsoft.com/library/default.asp?url=/library/en-us/dnwmt/html/registeredfourcccodesandwaveformats.asp
#define WAVE_FORMAT_UNKNOWN			0x0000
#define WAVE_FORMAT_PCM        			0x0001
#define WAVE_FORMAT_ALAW			0x0006 /* Microsoft Corporation */
#define WAVE_FORMAT_MULAW			0x0007 /* Microsoft Corporation */
#define WAVE_FORMAT_IMA				0x0011
#define WAVE_FORMAT_MPEG			0x0050 /* Microsoft Corporation */
#define WAVE_FORMAT_MPEGLAYER3 			0x0055 /* ISO/MPEG Layer3 Format Tag */
#define WAVE_FORMAT_AAC 			0x00FF /* AAC */
#define WAVE_FORMAT_MSAUDIO2 			0x0161 /* Microsoft Corporation WMA v9 */
#define WAVE_FORMAT_MSAUDIO3 			0x0162 /* Microsoft Corporation WMA multi channel = WMA_PRO */
#define WAVE_FORMAT_AC3 			0x2000 /* AC3 in WAV/AVI files */


// tags define by AVOS, not M$!!!:
#define WAVE_FORMAT_ALAC 			0xA1AC /* Apple lossless*/
#define WAVE_FORMAT_NELLY_MOSER			0x4242 /* flash */
#define WAVE_FORMAT_FLV_ADPCM			0x4343 /* flash */
#define WAVE_FORMAT_IMA_QT			0x4444 /* QT/MOV/MP4 */
#define WAVE_FORMAT_TRUEHD			0x4646 /* TrueHD */
#define WAVE_FORMAT_EAC3			0x4747 /* EAC3 */
#define WAVE_FORMAT_DTS_HD			0x4848 /* DTS HD */

#define WAVE_FORMAT_LAVC			0x5454 /* catchall codec for lavc */

// lavc defined formats
#define WAVE_FORMAT_PCM_BLURAY			0x5401 /* PCM/BLURAY */

#define WAVE_FORMAT_COOK			0x2004 /* real audio cook */

// not used by AVOS (for now)
#define WAVE_FORMAT_MS_ADPCM			0x0002 /* Microsoft Corporation */
#define WAVE_FORMAT_IEEE_FLOAT			0x0003 /* Microsoft Corporation */
#define WAVE_FORMAT_VSELP			0x0004 /* Compaq Computer Corp. */
#define WAVE_FORMAT_IBM_CVSD			0x0005 /* IBM Corporation */
#define WAVE_FORMAT_MS_DTS 			0x0008 /* Microsoft Corporation */
#define WAVE_FORMAT_DRM				0x0009 /* Microsoft Corporation */
#define WAVE_FORMAT_MSAUDIO_SPEECH		0x000A /* Microsoft Corporation */
#define WAVE_FORMAT_OKI_ADPCM			0x0010 /* OKI */
#define WAVE_FORMAT_DVI_ADPCM			0x0011 /* Intel Corporation */
#define WAVE_FORMAT_MEDIASPACE_ADPCM		0x0012 /* Videologic */
#define WAVE_FORMAT_SIERRA_ADPCM		0x0013 /* Sierra Semiconductor Corp */
#define WAVE_FORMAT_G723_ADPCM			0x0014 /* Antex Electronics Corporation */
#define WAVE_FORMAT_DIGISTD			0x0015 /* DSP Solutions, Inc. */
#define WAVE_FORMAT_DIGIFIX			0x0016 /* DSP Solutions, Inc. */
#define WAVE_FORMAT_DIALOGIC_OKI_ADPCM		0x0017 /* Dialogic Corporation */
#define WAVE_FORMAT_MEDIAVISION_ADPCM		0x0018 /* Media Vision, Inc. */
#define WAVE_FORMAT_CU_CODEC			0x0019 /* Hewlett-Packard Company */
#define WAVE_FORMAT_YAMAHA_ADPCM		0x0020 /* Yamaha Corporation of America */
#define WAVE_FORMAT_SONARC			0x0021 /* Speech Compression */
#define WAVE_FORMAT_DSPGROUP_TRUESPEECH		0x0022 /* DSP Group, Inc */
#define WAVE_FORMAT_ECHOSC1			0x0023 /* Echo Speech Corporation */
#define WAVE_FORMAT_AUDIOFILE_AF36		0x0024 /* Virtual Music, Inc. */
#define WAVE_FORMAT_APTX			0x0025 /* Audio Processing Technology */
#define WAVE_FORMAT_AUDIOFILE_AF10		0x0026 /* Virtual Music, Inc. */
#define WAVE_FORMAT_PROSODY_1612		0x0027 /* Aculab plc */
#define WAVE_FORMAT_LRC				0x0028 /* Merging Technologies S.A. */
#define WAVE_FORMAT_DOLBY_AC2			0x0030 /* AC2 */
#define WAVE_FORMAT_GSM610			0x0031 /* Microsoft Corporation */
#define WAVE_FORMAT_MSNAUDIO			0x0032 /* Microsoft Corporation */
#define WAVE_FORMAT_ANTEX_ADPCME		0x0033 /* Antex Electronics Corporation */
#define WAVE_FORMAT_CONTROL_RES_VQLPC		0x0034 /* Control Resources Limited */
#define WAVE_FORMAT_DIGIREAL			0x0035 /* DSP Solutions, Inc. */
#define WAVE_FORMAT_DIGIADPCM			0x0036 /* DSP Solutions, Inc. */
#define WAVE_FORMAT_CONTROL_RES_CR10		0x0037 /* Control Resources Limited */
#define WAVE_FORMAT_NMS_VBXADPCM		0x0038 /* Natural MicroSystems */
#define WAVE_FORMAT_CS_IMAADPCM			0x0039 /* Crystal Semiconductor IMA ADPCM */
#define WAVE_FORMAT_ECHOSC3			0x003A /* Echo Speech Corporation */
#define WAVE_FORMAT_ROCKWELL_ADPCM		0x003B /* Rockwell International */
#define WAVE_FORMAT_ROCKWELL_DIGITALK		0x003C /* Rockwell International */
#define WAVE_FORMAT_XEBEC			0x003D /* Xebec Multimedia Solutions Limited */
#define WAVE_FORMAT_G721_ADPCM			0x0040 /* Antex Electronics Corporation */
#define WAVE_FORMAT_G728_CELP			0x0041 /* Antex Electronics Corporation */
#define WAVE_FORMAT_MSG723			0x0042 /* Microsoft Corporation */
#define WAVE_FORMAT_RT24			0x0052 /* InSoft, Inc. */
#define WAVE_FORMAT_PAC				0x0053 /* InSoft, Inc. */
#define WAVE_FORMAT_LUCENT_G723			0x0059 /* Lucent Technologies */
#define WAVE_FORMAT_CIRRUS			0x0060 /* Cirrus Logic */
#define WAVE_FORMAT_ESPCM			0x0061 /* ESS Technology */
#define WAVE_FORMAT_VOXWARE			0x0062 /* Voxware Inc */
#define WAVE_FORMAT_CANOPUS_ATRAC		0x0063 /* Canopus, co., Ltd. */
#define WAVE_FORMAT_G726_ADPCM			0x0064 /* APICOM */
#define WAVE_FORMAT_G722_ADPCM			0x0065 /* APICOM */
#define WAVE_FORMAT_DSAT_DISPLAY		0x0067 /* Microsoft Corporation */
#define WAVE_FORMAT_VOXWARE_BYTE_ALIGNED	0x0069 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_AC8			0x0070 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_AC10		0x0071 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_AC16		0x0072 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_AC20		0x0073 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_RT24		0x0074 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_RT29		0x0075 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_RT29HW		0x0076 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_VR12		0x0077 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_VR18		0x0078 /* Voxware Inc */
#define WAVE_FORMAT_VOXWARE_TQ40		0x0079 /* Voxware Inc */
#define WAVE_FORMAT_SOFTSOUND			0x0080 /* Softsound, Ltd. */
#define WAVE_FORMAT_VOXWARE_TQ60		0x0081 /* Voxware Inc */
#define WAVE_FORMAT_MSRT24			0x0082 /* Microsoft Corporation */
#define WAVE_FORMAT_G729A			0x0083 /* AT&T Labs, Inc. */
#define WAVE_FORMAT_MVI_MVI2			0x0084 /* Motion Pixels */
#define WAVE_FORMAT_DF_G726			0x0085 /* DataFusion Systems (Pty) (Ltd) */
#define WAVE_FORMAT_DF_GSM610			0x0086 /* DataFusion Systems (Pty) (Ltd) */
#define WAVE_FORMAT_ISIAUDIO			0x0088 /* Iterated Systems, Inc. */
#define WAVE_FORMAT_ONLIVE			0x0089 /* OnLive! Technologies, Inc. */
#define WAVE_FORMAT_SBC24			0x0091 /* Siemens Business Communications Sys */
#define WAVE_FORMAT_DOLBY_AC3_SPDIF		0x0092 /* Sonic Foundry */
#define WAVE_FORMAT_MEDIASONIC_G723		0x0093 /* MediaSonic */
#define WAVE_FORMAT_PROSODY_8KBPS		0x0094 /* Aculab plc */
#define WAVE_FORMAT_ZYXEL_ADPCM			0x0097 /* ZyXEL Communications, Inc. */
#define WAVE_FORMAT_PHILIPS_LPCBB		0x0098 /* Philips Speech Processing */
#define WAVE_FORMAT_PACKED 			0x0099 /* Studer Professional Audio AG */
#define WAVE_FORMAT_MALDEN_PHONYTALK 		0x00A0 /* Malden Electronics Ltd. */
#define WAVE_FORMAT_RHETOREX_ADPCM 		0x0100 /* Rhetorex Inc. */
#define WAVE_FORMAT_IRAT 			0x0101 /* BeCubed Software Inc. */
#define WAVE_FORMAT_VIVO_G723 			0x0111 /* Vivo Software */
#define WAVE_FORMAT_VIVO_SIREN 			0x0112 /* Vivo Software */
#define WAVE_FORMAT_DIGITAL_G723 		0x0123 /* Digital Equipment Corporation */
#define WAVE_FORMAT_SANYO_LD_ADPCM 		0x0125 /* Sanyo Electric Co., Ltd. */
#define WAVE_FORMAT_SIPROLAB_ACEPLNET 		0x0130 /* Sipro Lab Telecom Inc. */
#define WAVE_FORMAT_SIPROLAB_ACELP4800 		0x0131 /* Sipro Lab Telecom Inc. */
#define WAVE_FORMAT_SIPROLAB_ACELP8V3 		0x0132 /* Sipro Lab Telecom Inc. */
#define WAVE_FORMAT_SIPROLAB_G729 		0x0133 /* Sipro Lab Telecom Inc. */
#define WAVE_FORMAT_SIPROLAB_G729A 		0x0134 /* Sipro Lab Telecom Inc. */
#define WAVE_FORMAT_SIPROLAB_KELVIN 		0x0135 /* Sipro Lab Telecom Inc. */
#define WAVE_FORMAT_VOICEAGE_AMR		0x0136 /* VoiceAge AMR */
#define WAVE_FORMAT_G726ADPCM 			0x0140 /* Dictaphone Corporation */
#define WAVE_FORMAT_QUALCOMM_PUREVOICE 		0x0150 /* Qualcomm, Inc. */
#define WAVE_FORMAT_QUALCOMM_HALFRATE 		0x0151 /* Qualcomm, Inc. */
#define WAVE_FORMAT_TUBGSM 			0x0155 /* Ring Zero Systems, Inc. */
#define WAVE_FORMAT_MSAUDIO1 			0x0160 /* Microsoft Corporation WMA */
#define WAVE_FORMAT_MSAUDIO_LOSSLESS		0x0163 /* Microsoft Corporation WMA multi channel */
#define WAVE_FORMAT_UNISYS_NAP_ADPCM 		0x0170 /* Unisys Corp. */
#define WAVE_FORMAT_UNISYS_NAP_ULAW 		0x0171 /* Unisys Corp. */
#define WAVE_FORMAT_UNISYS_NAP_ALAW 		0x0172 /* Unisys Corp. */
#define WAVE_FORMAT_UNISYS_NAP_16K 		0x0173 /* Unisys Corp. */
#define WAVE_FORMAT_CREATIVE_ADPCM 		0x0200 /* Creative Labs, Inc */
#define WAVE_FORMAT_CREATIVE_FASTSPEECH8 	0x0202 /* Creative Labs, Inc */
#define WAVE_FORMAT_CREATIVE_FASTSPEECH10 	0x0203 /* Creative Labs, Inc */
#define WAVE_FORMAT_UHER_ADPCM 			0x0210 /* UHER informatic GmbH */
#define WAVE_FORMAT_QUARTERDECK 		0x0220 /* Quarterdeck Corporation */
#define WAVE_FORMAT_ILINK_VC 			0x0230 /* I-link Worldwide */
#define WAVE_FORMAT_RAW_SPORT 			0x0240 /* Aureal Semiconductor */
#define WAVE_FORMAT_ESST_AC3 			0x0241 /* ESS Technology, Inc. */
#define WAVE_FORMAT_IPI_HSX 			0x0250 /* Interactive Products, Inc. */
#define WAVE_FORMAT_IPI_RPELP 			0x0251 /* Interactive Products, Inc. */
#define WAVE_FORMAT_CS2 			0x0260 /* Consistent Software */
#define WAVE_FORMAT_SONY_SCX 			0x0270 /* Sony Corp. */
#define WAVE_FORMAT_FM_TOWNS_SND 		0x0300 /* Fujitsu Corp. */
#define WAVE_FORMAT_BTV_DIGITAL 		0x0400 /* Brooktree Corporation */
#define WAVE_FORMAT_QDESIGN_MUSIC 		0x0450 /* QDesign Corporation */
#define WAVE_FORMAT_ON2_AVC_AUDIO 		0x0500 /* On2 VP7 On2 Technologies AVC Audio */
#define WAVE_FORMAT_VME_VMPCM 			0x0680 /* AT&T Labs, Inc. */
#define WAVE_FORMAT_TPC 			0x0681 /* AT&T Labs, Inc. */
#define WAVE_FORMAT_OLIGSM 			0x1000 /* Ing C. Olivetti & C., S.p.A. */
#define WAVE_FORMAT_OLIADPCM 			0x1001 /* Ing C. Olivetti & C., S.p.A. */
#define WAVE_FORMAT_OLICELP 			0x1002 /* Ing C. Olivetti & C., S.p.A. */
#define WAVE_FORMAT_OLISBC 			0x1003 /* Ing C. Olivetti & C., S.p.A. */
#define WAVE_FORMAT_OLIOPR 			0x1004 /* Ing C. Olivetti & C., S.p.A. */
#define WAVE_FORMAT_LH_CODEC 			0x1100 /* Lernout & Hauspie */
#define WAVE_FORMAT_NORRIS 			0x1400 /* Norris Communications, Inc. */
#define WAVE_FORMAT_SOUNDSPACE_MUSICOMPRESS	0x1500 /* AT&T Labs, Inc. */
#define WAVE_FORMAT_AAC_LATM			0x1602 /* AAC in LATM */

#define WAVE_FORMAT_DTS	 			0x2001 /* DTS */
#define WAVE_FORMAT_14_4			0x2002 /* real audio 14_4 */
#define WAVE_FORMAT_28_8			0x2003 /* real audio 28_8 */
#define WAVE_FORMAT_COOK 			0x2004 /* real audio cook */
#define WAVE_FORMAT_DNET			0x2005 /* real audio AC3  */
#define WAVE_FORMAT_WAVPACK			0x5756
#define WAVE_FORMAT_OGG1			0x674F /* Ogg Vorbis 1  */
#define WAVE_FORMAT_OGG2			0x6750 /* Ogg Vorbis 2  */
#define WAVE_FORMAT_OGG3			0x6751 /* Ogg Vorbis 3  */
#define WAVE_FORMAT_OGG1_PLUS			0x676F /* Ogg Vorbis 1+ */
#define WAVE_FORMAT_OGG2_PLUS			0x6770 /* Ogg Vorbis 2+ */
#define WAVE_FORMAT_OGG3_PLUS			0x6771 /* Ogg Vorbis 3+ */
#define WAVE_FORMAT_FAAD_AAC			0x706D /* FAAD-AAC??? */
#define WAVE_FORMAT_TTA				0x77A1
#define WAVE_FORMAT_GSM_AMR_CBR			0x7A21 /* GSM-AMR (CBR, no SID) */
#define WAVE_FORMAT_GSM_AMR_VBR			0x7A22 /* GSM-AMR (VBR, including SID) */
#define WAVE_FORMAT_VOICEAGE_AMR_WB		0xA104 /* VoiceAge AMR WB VoiceAge Corporation */
#define WAVE_FORMAT_FLAC			0xF1AC /* Free Lossless Audio Codec FLAC */
#define WAVE_FORMAT_OPUS			0x6780 /* Codec Opus */

// Video fourCC codes:
#define mmioFOURCC( ch0, ch1, ch2, ch3 ) \
  ( (long)(unsigned char)(ch0) | ( (long)(unsigned char)(ch1) << 8 ) | \
  ( (long)(unsigned char)(ch2) << 16 ) | ( (long)(unsigned char)(ch3) << 24 ) )
// Video fourCC codes:
#define VIDEO_FOURCC_NONE		(0x0000)

// generic MPEG-4
#define VIDEO_FOURCC_MP4V		mmioFOURCC('M', 'P', '4', 'V')     

#define VIDEO_FOURCC_4GMC		mmioFOURCC('4', 'G', 'M', 'C')     

// microsoft MPEG4
#define VIDEO_FOURCC_M4S2		mmioFOURCC('M', '4', 'S', '2')     

// DIVX
#define VIDEO_FOURCC_DIVX		mmioFOURCC('D', 'I', 'V', 'X')     
#define VIDEO_FOURCC_DX50		mmioFOURCC('D', 'X', '5', '0')     

// XVID
#define VIDEO_FOURCC_XVID		mmioFOURCC('X', 'V', 'I', 'D')     

// WMV
#define VIDEO_FOURCC_WMV1		mmioFOURCC('W', 'M', 'V', '1')     // WMV v7
#define VIDEO_FOURCC_WMV2		mmioFOURCC('W', 'M', 'V', '2')     // WMV v8
#define VIDEO_FOURCC_WMV3		mmioFOURCC('W', 'M', 'V', '3')     // WMV v9
#define VIDEO_FOURCC_WMVP		mmioFOURCC('W', 'M', 'V', 'P')     // WMV v?

#define VIDEO_FOURCC_WM3B		mmioFOURCC('W', 'M', '3', 'B')     // WMV v9 BETA

#define VIDEO_FOURCC_WVC1		mmioFOURCC('W', 'V', 'C', '1')     // WMV VC1
#define VIDEO_FOURCC_WMVA		mmioFOURCC('W', 'M', 'V', 'A')     // obsolete pre VC1

#define VIDEO_FOURCC_MSS2		mmioFOURCC('M', 'S', 'S', '2')     // WMV 9 Screen

#define VIDEO_FOURCC_WVP2		mmioFOURCC('W', 'V', 'P', '2')     // VC1 image

// OpenDivX 
#define VIDEO_FOURCC_MP4S		mmioFOURCC('M', 'P', '4', 'S')
#define VIDEO_FOURCC_DIV1		mmioFOURCC('D', 'I', 'V', '1')

// (old) DivX codecs 
#define VIDEO_FOURCC_DIV2		mmioFOURCC('D', 'I', 'V', '2')
#define VIDEO_FOURCC_DIV3		mmioFOURCC('D', 'I', 'V', '3')
#define VIDEO_FOURCC_DIV4		mmioFOURCC('D', 'I', 'V', '4')
#define VIDEO_FOURCC_DIV5		mmioFOURCC('D', 'I', 'V', '5')
#define VIDEO_FOURCC_DIV6		mmioFOURCC('D', 'I', 'V', '6')
#define VIDEO_FOURCC_MP41		mmioFOURCC('M', 'P', '4', '1')
#define VIDEO_FOURCC_MP43		mmioFOURCC('M', 'P', '4', '3')

// old MS Mpeg-4 codecs 
#define VIDEO_FOURCC_MP42		mmioFOURCC('M', 'P', '4', '2')
#define VIDEO_FOURCC_MPG4		mmioFOURCC('M', 'P', 'G', '4')

#define VIDEO_FOURCC_MPG1		mmioFOURCC('M', 'P', 'G', '1')
#define VIDEO_FOURCC_MPG2		mmioFOURCC('M', 'P', 'G', '2')

#define VIDEO_FOURCC_H264		mmioFOURCC('H', '2', '6', '4')
#define VIDEO_FOURCC_DAVC		mmioFOURCC('D', 'A', 'V', 'C')
#define VIDEO_FOURCC_AVC1		mmioFOURCC('A', 'V', 'C', '1')
#define VIDEO_FOURCC_X264		mmioFOURCC('X', '2', '6', '4')

#define VIDEO_FOURCC_H265		mmioFOURCC('H', '2', '6', '5')
#define VIDEO_FOURCC_X265		mmioFOURCC('X', '2', '6', '5')
#define VIDEO_FOURCC_HEVC		mmioFOURCC('H', 'E', 'V', 'C')

#define VIDEO_FOURCC_SVQ1		mmioFOURCC('S', 'V', 'Q', '1')
#define VIDEO_FOURCC_SVQ3		mmioFOURCC('S', 'V', 'Q', '3')
#define VIDEO_FOURCC_CVID		mmioFOURCC('C', 'V', 'I', 'D')

#define VIDEO_FOURCC_MJPG		mmioFOURCC('M', 'J', 'P', 'G')

#define VIDEO_FOURCC_ARCHOS		mmioFOURCC('R', 'C', 'H', 'S')
#define VIDEO_FOURCC_YUV		mmioFOURCC('Y', 'U', 'V', ' ')

#define VIDEO_FOURCC_SPARK		mmioFOURCC('S', 'P', 'R', 'K')

#define VIDEO_FOURCC_DVR		mmioFOURCC('D', 'V', 'R', ' ')     

// codec we blacklist = not support:

#define VIDEO_FOURCC_DIB		mmioFOURCC('D', 'I', 'B', ' ')
#define VIDEO_FOURCC_IV32		mmioFOURCC('I', 'V', '3', '2')
#define VIDEO_FOURCC_IV50		mmioFOURCC('I', 'V', '5', '0')
#define VIDEO_FOURCC_H263		mmioFOURCC('H', '2', '6', '3')
#define VIDEO_FOURCC_H263PLUS		mmioFOURCC('2', '6', '3', '+')
#define VIDEO_FOURCC_VP6F		mmioFOURCC('V', 'P', '6', 'F')
#define VIDEO_FOURCC_VP70		mmioFOURCC('V', 'P', '7', '0')
#define VIDEO_FOURCC_VP80		mmioFOURCC('V', 'P', '8', '0')
#define VIDEO_FOURCC_VP90		mmioFOURCC('V', 'P', '9', '0')
#define VIDEO_FOURCC_SCREEN		mmioFOURCC('S', 'C', 'R', 'N')
#define VIDEO_FOURCC_RV10		mmioFOURCC('R', 'V', '1', '0')
#define VIDEO_FOURCC_RV20		mmioFOURCC('R', 'V', '2', '0')
#define VIDEO_FOURCC_RVTR_RV20		mmioFOURCC('R', 'V', 'T', 'R')
#define VIDEO_FOURCC_RV30		mmioFOURCC('R', 'V', '3', '0')
#define VIDEO_FOURCC_RVTR_RV30		mmioFOURCC('R', 'V', 'T', '2')
#define VIDEO_FOURCC_RV40		mmioFOURCC('R', 'V', '4', '0')
#define VIDEO_FOURCC_RV41		mmioFOURCC('R', 'V', '4', '1')
#define VIDEO_FOURCC_RV4X		mmioFOURCC('R', 'V', '4', 'X')

#define VIDEO_FOURCC_MSVC		mmioFOURCC('M', 'S', 'V', 'C')
#define VIDEO_FOURCC_CRAM		mmioFOURCC('C', 'R', 'A', 'M')
#define VIDEO_FOURCC_WHAM		mmioFOURCC('W', 'H', 'A', 'M')
#define VIDEO_FOURCC_TSCC		mmioFOURCC('T', 'S', 'C', 'C')

#define VIDEO_FOURCC_THEO		mmioFOURCC('T', 'H', 'E', 'O')

#define VIDEO_FOURCC_LAVC		mmioFOURCC('L', 'A', 'V', 'C')

#define VIDEO_MAX_WIDTH  7680
#define VIDEO_MAX_HEIGHT 4320

// (internal) Video formats:
enum {
	VIDEO_FORMAT_UNKNOWN = 0,
	VIDEO_FORMAT_MPG4,
	VIDEO_FORMAT_MPG4GMC,
	VIDEO_FORMAT_WMV1,
	VIDEO_FORMAT_WMV2,
	VIDEO_FORMAT_WMV3,
	VIDEO_FORMAT_WMV3B,
	VIDEO_FORMAT_VC1,
	VIDEO_FORMAT_MPEG,
	VIDEO_FORMAT_H264,
	VIDEO_FORMAT_MSMP43,
	VIDEO_FORMAT_MSMP42,
	VIDEO_FORMAT_MSMP41,
	VIDEO_FORMAT_ARCHOS,
	VIDEO_FORMAT_MJPG,
	VIDEO_FORMAT_YUV,
	VIDEO_FORMAT_SPARK,
	VIDEO_FORMAT_RV10,
	VIDEO_FORMAT_RV20,
	VIDEO_FORMAT_RV30,
	VIDEO_FORMAT_RV40,
	VIDEO_FORMAT_VP6,
	VIDEO_FORMAT_VP7,
	VIDEO_FORMAT_VP8,
	VIDEO_FORMAT_VP9,
	VIDEO_FORMAT_F2M,
	VIDEO_FORMAT_H263,
	VIDEO_FORMAT_VC1IMAGE,
	VIDEO_FORMAT_THEORA,
	VIDEO_FORMAT_HEVC,
	VIDEO_FORMAT_LAVC,
};

// video sub formats
enum {
	VIDEO_SUBFMT_NONE = 0,
	VIDEO_SUBFMT_MPEG1,
	VIDEO_SUBFMT_MPEG2
};

enum {
	AUDIO_PCM_LE = 0,
	AUDIO_PCM_BE
};

enum {
	ADPCM_IMA = 0,
	ADPCM_AMV
};

enum {
	AUDIO_AAC_RAW = 0,
	AUDIO_AAC_ADTS
};

// subtitle formats
enum {
	SUB_FORMAT_NONE = 0,
	SUB_FORMAT_DVD_GFX,
	SUB_FORMAT_DVD,
	SUB_FORMAT_DVBT,
	SUB_FORMAT_TEXT,
	SUB_FORMAT_XSUB,
	SUB_FORMAT_SSA,
	SUB_FORMAT_ASS,
	SUB_FORMAT_EXT,		// external subs from separate files
};

#define AUDIO_STREAM_MAX	6
#define VIDEO_STREAM_MAX	6
#define SUB_STREAM_MAX		32
#define AV_NAME_LEN		32

enum {
	CRYPT_NONE = 0,
	CRYPT_DRM,
};

enum {
	VIDEO_PROGRESSIVE = 0,
	VIDEO_INTERLACED,
	VIDEO_INTERLACED_ONE_FIELD,
};

#define AV_COMMON_PROPS	\
	int 	valid;	\
	int 	crypted;\
	int 	crypt_type;\
	int	compressed;			/* e.g. MKV coding.compressed */ \
	int	comp_algo;			/* e.g  MKV coding.comp_algo */ \
	int 	not_allowed; 			/* e.g. no plugin/codec licence*/ \
	int 	not_allowed_reason;		/* opaque reason */ \
	int 	stream;\
	int 	PID;\
	int 	duration;\
	int 	bytesPerSec;\
	uint32_t scale;\
	uint32_t rate;\
	int 	maxbuf;\
	int 	frames;\
	int 	format;\
	int 	subfmt;\
	int	codec_id;\
	char	codec_name[AV_NAME_LEN + 1];\
	int     priority;			/* 0 is highest */\
	int 	profile;			/* e.g. H264/VC1 profile */\
	char	profile_name[AV_NAME_LEN + 1];	/* verbose profile name  */\
	int 	level;				/* e.g. H264 level       */\
	char	level_name[AV_NAME_LEN + 1];	/* verbose level name    */\
	int	vbr;\
	int	extraDataSize;\
	unsigned char extraData[4096];\
	int	extraDataSize2;\
	unsigned char *extraData2;\
	char	name[AV_NAME_LEN + 1];\
	void	*priv;				/* codec private data */\
	\
	int 	end;			/* codec end of data*/\
	int 	leave_open;		/* codec can be left open when changing track*/\
	int	needs_header;		/* we have to send a "setup" header before the data*/\
	int	header_sent;		/* we have sent the "setup" header*/\
	int	extra_sent;		/* we have sent the extradata inline*/\
	int	no_extra;		/* do not send extradata inline */\
	
typedef struct _audio_props {
	AV_COMMON_PROPS
	
	int 	samplesPerSec;
	int 	samplesPerBlock;	// for IMA
	int	channels;
	int	channelMask;
	int	blockAlign;
	int	bitsPerSample;
	int	blockSize;		// for IMA
	int	byteOrder;
	void	*ctx;			// for hacks!
	int	sourceSamples;		// original sample rate of source
	int	sourceChannels;		// original channels of source
	int	sourceBitsPerSample;
	int	bytesPerFrame;
	int	request_channels;	// requested number of output channels (downmix)
					// 0 == no downmix
	UINT64  codec_delay;
	UINT64  seek_preroll;
} AUDIO_PROPERTIES;

typedef struct _video_props {
	AV_COMMON_PROPS
		
	UINT32 	fourcc;
	int 	width;
	int 	height;
	int 	padded_width;
	int 	padded_height;
	int	colorspace;
	int	aspect_n;
	int	aspect_d;
	int 	rotation;
	int 	msPerFrame;
	int 	framesPerSec;
	int	interlaced;
	int	stereo_mode;
	int	flush_frames;	// hoy many frames to decode to flush the codec
	int	delay_frames;	// how many frames does the codec hold "locked"
	
	// for PTS/DTS issues:
	int	reorder_pts;
	int	never_reorder_pts;
	
	// for H264
	H264_SPS sps;	
	int	avcc;
	int	hvcc;
	int	nal_unit_size;
	
	// for MPG4
	int	vol;
	int	sprite_usage;
	
	// for RV10|20|30|40
	int 	slice_sof;	// 1: start of frame, 0: end of frame	
	
	// for braindead formats
	int	flip_h;
	int	flip_v;

} VIDEO_PROPERTIES;

typedef struct _sub_props {
	AV_COMMON_PROPS

	char	path[MAX_NAME_LEN + 1];
	int	gfx;
	int	ext;
} SUB_PROPERTIES;

void show_audio_props( AUDIO_PROPERTIES *audio );
void show_video_props( VIDEO_PROPERTIES *video );
void show_subtitle_props( SUB_PROPERTIES *sub  );

typedef struct _av_props {
	
	int 		force_notify;
	int		as;
	int		as_max;
	AUDIO_PROPERTIES audio[AUDIO_STREAM_MAX];

	int		vs;
	int		vs_max;
	VIDEO_PROPERTIES video[VIDEO_STREAM_MAX];

	int		subs;
	int		subs_max;
	SUB_PROPERTIES sub[SUB_STREAM_MAX];

} AV_PROPERTIES;

#define av_init_props( x ) \
{ \
	int i; \
	(x)->audio     = &(x)->av.audio[0]; \
	(x)->av.as     = 0; \
	(x)->av.as_max = 0; \
	(x)->video     = &(x)->av.video[0]; \
	(x)->av.vs     = 0; \
	(x)->av.vs_max = 0; \
	for( i = 0; i < VIDEO_STREAM_MAX; i ++ ) { \
		(x)->av.video[i].aspect_n = 1;\
		(x)->av.video[i].aspect_d = 1;\
	} \
	(x)->subtitle    = &(x)->av.sub[0]; \
	(x)->av.subs     = 0; \
	(x)->av.subs_max = 0; \
}

#define av_copy_props( dst, src ) \
{ \
	memcpy( (dst), (src), sizeof( *(dst) ) ); \
	(dst)->audio     = &(dst)->av.audio[0]; \
	(dst)->video     = &(dst)->av.video[0]; \
	(dst)->subtitle  = &(dst)->av.sub[0]; \
}

#define linestep_from_width(width) (32 * (((width)  + 15 ) / 16))
#define linestep_from_width32(width) (64 * (((width)  + 31 ) / 32))
#define pad_height(height)         (16 * (((height) + 15 ) / 16))

enum {
	I_VOP = 0,
	P_VOP,
	B_VOP,
	IDR_VOP,
	BR_VOP,		// this is a B frame that is used as a reference frame, so it cannot be dropped!
	NOCODE_VOP	// this is actually not a valid frame, it's just there to keep the timestamp
} FRAME_TYPE;

enum {
	FRAME_DROPPABLE = 1,
} FRAME_FLAGS;

static inline char frame_type( int type )
{
	if( type < I_VOP || type > NOCODE_VOP )
		return '?';
	return "IPBRbN"[type];
}

enum AV_COLORSPACE {
	AV_IMAGE_YUV_422 = 0,
	AV_IMAGE_NV12,
	AV_IMAGE_YV12,
	AV_IMAGE_RGB_16,
	AV_IMAGE_RGB_16_AUTO_ALPHA, // only supported by LCD_DrawImage
	AV_IMAGE_RGBA_32,
	AV_IMAGE_ARGB_32,
	AV_IMAGE_GRAYSCALE,
	AV_IMAGE_8BIT_INDEXED,
	AV_IMAGE_BGRA_32,           // only supported by LCD_DrawImage 
	AV_IMAGE_RGB_24,
	AV_IMAGE_RGBX_32,
	AV_IMAGE_QCOM_NV12_TILED,
	AV_IMAGE_EXYNOS_NV12_TILED,
	AV_IMAGE_TEGRA3_YV12,
	AV_IMAGE_NV21,
	AV_IMAGE_HW,
};

#define FRAME_COMMON	\
	/* these fields must remain unchanged during the image lifetime*/\
	unsigned char	*buffer;	/* internal buffer if needed */\
	unsigned char	*data[3];	/* pointer to the start of the image. Must be 32 bytes aligned*/\
	int		size;		/* size available in buffer (in bytes)*/\
	int		mem_type;	/* whether DMA or normal allocated */\
	\
	/* these fields could possibly change*/\
	int		width;		/* width in pixels*/\
	int		height;		/* height in pixels*/\
	int		linestep[3];	/* stride in bytes. Must be a multiple of 32*/\
	\
	RECT		window;		/* region of interest for resize or draw operations*/\
	int		colorspace;	\
	\
	int		bpp[3];		/* bytes per pixel */ \
	unsigned short	*palette;	/* palette for 8bit indexed */ \
	void		*priv;		/* frame private data */ \
	void		*dec;		/* video decoder that produces this frame */
	
typedef struct image_struct {
	FRAME_COMMON
} IMAGE;

typedef struct vfr_str {
	// These fields must be synchronized with IMAGE
	FRAME_COMMON
	// End of common fields

	int		format;
	int		error;
	int		valid;
	int		type;
	int 		flags;
	
	int		time;
	int		blit_time;
	int		audio_skip;
	int		video_skip;

	int		user_ID;
	int		pts;
	int		cpn;
	int		dpn;

	int		interlaced;
	int		top_field_first;
	int 		deinterlace;

	int		index;		// for 4vl
	struct vfr_str	*next;		// for queueing
	int		locked;
	int		epoch;

	int		duration;	// frame duration in ms
	int		aspect_n;
	int		aspect_d;
	int		data_size[3];
	unsigned char	*phys[3];
	void		*handle[3];
	const void	*android_handle;
	int		map_fd[3];

	int		ofs_x;
	int 		ofs_y;  

	int 		decode_time;

	void		(*destroy)(struct vfr_str *f);

} VIDEO_FRAME;

typedef struct {
	unsigned char 	*data;
	int		size;
	int		fakeSize;

	int		format;
	int 		error;	
	
	int		channels;
	int		samplesPerSec;
	int		bits;
	int		time;
} AUDIO_FRAME;

void show_audio_props   ( AUDIO_PROPERTIES *audio );
void show_video_props   ( VIDEO_PROPERTIES *video );
void show_subtitle_props( SUB_PROPERTIES   *sub );
void show_av_props      ( AV_PROPERTIES    *av );
void show_short_av_props( AV_PROPERTIES    *av );

const char *av_get_type_name( int type );
int av_get_type_from_name( const char *name );
const char *av_get_etype_name( int etype );
int av_get_etype_from_name( const char *name );
int av_is_HD( int width, int height );

int video_get_format_from_fourcc( UINT32 fourcc, int *subfmt );
const char *video_get_fourcc_name( VIDEO_PROPERTIES *video );
const char *video_get_format_name( VIDEO_PROPERTIES *video );
const char *audio_get_format_name( AUDIO_PROPERTIES *audio );
const char   *sub_get_format_name( SUB_PROPERTIES   *sub );
const char *audio_get_channel_layout_name( int channels );

void av_dump_video_frame( VIDEO_FRAME *frame );

void av__reduce( int *a, int *b );
int  av__gcd   ( int  a, int  b );

void video_frame_copy_props( VIDEO_FRAME *dst, VIDEO_FRAME *src );

#endif
