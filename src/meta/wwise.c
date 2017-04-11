#include "meta.h"
#include "../util.h"
#include "../coding/coding.h"


/* Wwise uses a custom RIFF/RIFX header, non-standard enough that it's parsed it here.
 * There is some repetition from other metas, but not enough to bother me.
 *
 * Some info: https://www.audiokinetic.com/en/library/edge/
 */
typedef enum { PCM, IMA, VORBIS, DSP, XMA2, XWMA, AAC, HEVAG, ATRAC9 } wwise_codec;
typedef struct {
    int big_endian;
    size_t file_size;

    /* chunks references */
    off_t fmt_offset;
    size_t fmt_size;
    off_t data_offset;
    size_t data_size;

    /* standard fmt stuff */
    wwise_codec codec;
    int format;
    int channels;
    int sample_rate;
    int block_align;
    int average_bps;
    int bits_per_sample;
    size_t extra_size;

    int32_t num_samples;
    int loop_flag;
    int32_t loop_start_sample;
    int32_t loop_end_sample;
} wwise_header;


/* Wwise - Audiokinetic Wwise (Wave Works Interactive Sound Engine) middleware */
VGMSTREAM * init_vgmstream_wwise(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    wwise_header ww;
    off_t start_offset, first_offset = 0xc;
    int32_t (*read_32bit)(off_t,STREAMFILE*) = NULL;
    int16_t (*read_16bit)(off_t,STREAMFILE*) = NULL;

    /* basic checks */
    /* .wem (Wwise Encoded Media) is "newer Wwise", used after the 2011.2 SDK (~july)
     * .wav (ex. Shadowrun X360) and .ogg (ex. KOF XII X360), .xma (ex. Tron Evolution X360) are used in older Wwise */
    if (!check_extensions(streamFile,"wem,wav,lwav,ogg,logg,xma")) goto fail;

    if ((read_32bitBE(0x00,streamFile) != 0x52494646) &&    /* "RIFF" (LE) */
        (read_32bitBE(0x00,streamFile) != 0x52494658))      /* "RIFX" (BE) */
        goto fail;
    if ((read_32bitBE(0x08,streamFile) != 0x57415645) &&    /* "WAVE" */
        (read_32bitBE(0x08,streamFile) != 0x58574D41))      /* "XWMA" */
        goto fail;

    memset(&ww,0,sizeof(wwise_header));

    ww.big_endian = read_32bitBE(0x00,streamFile) == 0x52494658;/* RIFX */
    if (ww.big_endian) { /* Wwise honors machine's endianness (PC=RIFF, X360=RIFX --unlike XMA) */
        read_32bit = read_32bitBE;
        read_16bit = read_16bitBE;
    } else {
        read_32bit = read_32bitLE;
        read_16bit = read_16bitLE;
    }

    ww.file_size = streamFile->get_size(streamFile);

#if 0
    /* sometimes uses a RIFF size that doesn't count chunks/sizes, has LE size in RIFX, or is just wrong...? */
    if (4+4+read_32bit(0x04,streamFile) != ww.file_size) {
        VGM_LOG("WWISE: bad riff size (real=0x%x vs riff=0x%x)\n", 4+4+read_32bit(0x04,streamFile), ww.file_size);
        goto fail;
    }
#endif


    /* parse format (roughly spec-compliant but some massaging is needed) */
    {
        off_t loop_offset;
        size_t loop_size;

        /* find basic chunks */
        if (!find_chunk(streamFile, 0x666d7420,first_offset,0, &ww.fmt_offset,&ww.fmt_size, ww.big_endian, 0)) goto fail; /*"fmt "*/
        if (!find_chunk(streamFile, 0x64617461,first_offset,0, &ww.data_offset,&ww.data_size, ww.big_endian, 0)) goto fail; /*"data"*/

        /* base fmt */
        if (ww.fmt_size < 0x12) goto fail;
        ww.format           = (uint16_t)read_16bit(ww.fmt_offset+0x00,streamFile);

        if (ww.format == 0x0165) { /* XMA2WAVEFORMAT (always "fmt"+"XMA2", unlike .xma that may only have "XMA2") */
            off_t xma2_offset;
            if (!find_chunk(streamFile, 0x584D4132,first_offset,0, &xma2_offset,NULL, ww.big_endian, 0)) goto fail;
            xma2_parse_xma2_chunk(streamFile, xma2_offset,&ww.channels,&ww.sample_rate, &ww.loop_flag, &ww.num_samples, &ww.loop_start_sample, &ww.loop_end_sample);
        } else { /* WAVEFORMATEX */
            ww.channels         = read_16bit(ww.fmt_offset+0x02,streamFile);
            ww.sample_rate      = read_32bit(ww.fmt_offset+0x04,streamFile);
            ww.average_bps      = read_32bit(ww.fmt_offset+0x08,streamFile);/* bytes per sec */
            ww.block_align      = (uint16_t)read_16bit(ww.fmt_offset+0x0c,streamFile);
            ww.bits_per_sample   = (uint16_t)read_16bit(ww.fmt_offset+0x0e,streamFile);
            if (ww.fmt_size > 0x10 && ww.format != 0x0165 && ww.format != 0x0166) /* ignore XMAWAVEFORMAT */
                ww.extra_size   = (uint16_t)read_16bit(ww.fmt_offset+0x10,streamFile);
            /* channel bitmask, see AkSpeakerConfig.h (ex. 1ch uses FRONT_CENTER 0x4, 2ch FRONT_LEFT 0x1 | FRONT_RIGHT 0x2, etc) */
            //if (ww.extra_size >= 6)
            //    ww.channel_config = read_32bit(ww.fmt_offset+0x14,streamFile);
        }

        /* find loop info */
        if (ww.format == 0x0166) { /* XMA2WAVEFORMATEX */
            xma2_parse_fmt_chunk_extra(streamFile, ww.fmt_offset, &ww.loop_flag, &ww.num_samples, &ww.loop_start_sample, &ww.loop_end_sample, ww.big_endian);
        }
        else if (find_chunk(streamFile, 0x736D706C,first_offset,0, &loop_offset,&loop_size, ww.big_endian, 0)) { /*"smpl". common */
            if (loop_size >= 0x34
                    && read_32bit(loop_offset+0x1c, streamFile)==1        /*loop count*/
                    && read_32bit(loop_offset+0x24+4, streamFile)==0) {
                ww.loop_flag = 1;
                ww.loop_start_sample = read_32bit(loop_offset+0x24+0x8, streamFile);
                ww.loop_end_sample   = read_32bit(loop_offset+0x24+0xc,streamFile);
                //todo fix repeat looping
            }
        }
        else if (find_chunk(streamFile, 0x4C495354,first_offset,0, &loop_offset,&loop_size, ww.big_endian, 0)) { /*"LIST", common */
            //todo parse "adtl" (does it ever contain loop info in Wwise?)
        }

        /* other Wwise specific: */
        //"JUNK": optional padding so that raw data starts in an offset multiple of 0x10 (0-size JUNK exists too)
        //"akd ": unknown (IMA/PCM; "audiokinetic data"?)
    }

    /* format to codec */
    switch(ww.format) {
        case 0x0001: ww.codec = PCM; break; /* older Wwise */
        case 0x0002: ww.codec = IMA; break; /* newer Wwise (conflicts with MSADPCM, probably means "platform's ADPCM") */
        //case 0x0011: ww.codec = IMA; break; /* older Wwise (used?) */
        case 0x0069: ww.codec = IMA; break; /* older Wwise (Spiderman Web of Shadows X360) */
        case 0x0161: ww.codec = XWMA; break;
        case 0x0162: ww.codec = XWMA; break;
        case 0x0165: ww.codec = XMA2; break; /* always with the "XMA2" chunk, Wwise doesn't use XMA1 */
        case 0x0166: ww.codec = XMA2; break;
        case 0xAAC0: ww.codec = AAC; break;
        case 0xFFF0: ww.codec = DSP; break;
        case 0xFFFB: ww.codec = HEVAG; break;
        case 0xFFFC: ww.codec = ATRAC9; break;
        case 0xFFFE: ww.codec = PCM; break; /* newer Wwise ("PCM for Wwise Authoring") (conflicts with WAVEFORMATEXTENSIBLE) */
        case 0xFFFF: ww.codec = VORBIS; break;
        default:
            VGM_LOG("WWISE: unknown codec 0x%x \n", ww.format);
            goto fail;
    }
    /* fix for newer Wwise DSP with coefs: Epic Mickey 2 (Wii), Batman Arkham Origins Blackgate (3DS) */
    if (ww.format == 0x0002 && ww.extra_size == 0x0c + ww.channels * 0x2e) {
        ww.codec = DSP;
    }

    /* this happens in some IMA files (ex. Bayonetta 2 sfx), maybe they are split and and meant to be joined in memory? */
    if (ww.data_size > ww.file_size) {
        VGM_LOG("WWISE: bad data size (real=0x%x > riff=0x%x)\n", ww.data_size, ww.file_size);
        if (ww.codec == IMA)
            ww.data_size = ww.file_size - ww.data_offset;
        else
            goto fail;
    }


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(ww.channels,ww.loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->sample_rate = ww.sample_rate;
    vgmstream->loop_start_sample = ww.loop_start_sample;
    vgmstream->loop_end_sample = ww.loop_end_sample;
    vgmstream->meta_type = meta_WWISE_RIFF;

    start_offset = ww.data_offset;

    switch(ww.codec) {
        case PCM: /* common */
            /* normally riff.c has priority but it's needed when .wem is used */
            if (ww.bits_per_sample != 16) goto fail;

            vgmstream->coding_type = (ww.big_endian ? coding_PCM16BE : coding_PCM16LE);
            vgmstream->layout_type = ww.channels > 1 ? layout_interleave : layout_none;
            vgmstream->interleave_block_size = 0x02;

            vgmstream->num_samples = pcm_bytes_to_samples(ww.data_size, ww.channels, ww.bits_per_sample);
            break;

        case IMA: /* common */
            /* slightly modified MS IMA with interleaved sub-blocks and LE/BE header */

            /* Wwise uses common codecs (ex. 0x0002 MSADPCM) so this parser should go AFTER riff.c avoid misdetection */

            if (ww.bits_per_sample != 4) goto fail;
            vgmstream->coding_type = coding_WWISE_IMA;
            vgmstream->layout_type = layout_none;
            vgmstream->interleave_block_size = ww.block_align;
            vgmstream->codec_endian = ww.big_endian;

            vgmstream->num_samples = ms_ima_bytes_to_samples(ww.data_size, ww.block_align, ww.channels);
            break;

#ifdef VGM_USE_VORBIS
        case VORBIS: {  /* common */
            /* Wwise uses custom Vorbis, which changed over time (config must be detected to pass to the decoder).
             * Original research by hcs in ww2ogg (https://github.com/hcs64/ww2ogg) */
#if 0
            off_t vorb_offset;
            size_t vorb_size;
            wwise_setup_type setup_type;
            wwise_header_type header_type;
            wwise_packet_type packet_type;
            int blocksize_0_exp = 0, blocksize_1_exp = 0;

            if (ww.block_align != 0 || ww.bits_per_sample != 0) goto fail; /* always 0 for Worbis */

            /* autodetect format */
            if (find_chunk(streamFile, 0x766F7262,first_offset,0, &vorb_offset,&vorb_size, ww.big_endian, 0)) { /*"vorb"*/
                /* older Wwise (~2011) */
                switch (vorb_size) {
                    case 0x28:
                    case 0x2A:
                    case 0x2C:
                    case 0x32:
                    case 0x34:
                    default: goto fail;
                }
            }
            else {
                /* newer Wwise (~2012+) */
                if (ww.extra_size != 0x30) goto fail; //todo some 0x2a exist

                //todo mod packets true on certain configs and always goes with 6/2 headers?

                setup_type  = AOTUV603_CODEBOOKS;
                header_type = TYPE_2;
                packet_type = STANDARD;

                /* 0x12 (2): unk (00,10,18) not related to seek table*/
                /* 0x14 (4): channel config */
                vgmstream->num_samples = read_32bit(ww.fmt_offset + 0x18, streamFile);
                /* 0x20 (4): config, 0xcb/0xd9/etc */
                /* 0x24 (4): ? samples? */
                /* 0x28 (4): seek table size (setup packet offset within data) */
                /* 0x2c (4): setup size (first audio packet offset within data) */
                /* 0x30 (2): ? */
                /* 0x32 (2): ? */
                /* 0x34 (4): ? */
                /* 0x38 (4): ? */
                /* 0x3c (4): uid */ //todo same as external crc32?
                blocksize_0_exp = read_8bit(ww.fmt_offset + 0x40, streamFile);
                blocksize_1_exp = read_8bit(ww.fmt_offset + 0x41, streamFile);

                goto fail;
            }

            vgmstream->codec_data = init_wwise_vorbis_codec_data(streamFile, start_offset, ww.channels, ww.sample_rate, blocksize_0_exp,blocksize_1_exp,
                    setup_type,header_type,packet_type, ww.big_endian);
            if (!vgmstream->codec_data) goto fail;
            vgmstream->coding_type = coding_wwise_vorbis;
            vgmstream->layout_type = layout_none;
            vgmstream->codec_endian = ww.big_endian;
            break;
#endif
            VGM_LOG("WWISE: VORBIS found (unsupported)\n");
            goto fail;
        }
#endif

        case DSP: {     /* Wii/3DS/WiiU */
            off_t wiih_offset;
            size_t wiih_size;
            int i;

            if (ww.bits_per_sample != 4) goto fail;

            vgmstream->coding_type = coding_NGC_DSP;
            vgmstream->layout_type = layout_interleave;
            vgmstream->interleave_block_size = 0x8; /* ww.block_align = 0x8 in older Wwise, samples per block in newer Wwise */

            /* find coef position */
            if (find_chunk(streamFile, 0x57696948,first_offset,0, &wiih_offset,&wiih_size, ww.big_endian, 0)) { /*"WiiH"*/ /* older Wwise */
                vgmstream->num_samples = dsp_bytes_to_samples(ww.data_size, ww.channels);
                if (wiih_size != 0x2e * ww.channels) goto fail;
            }
            else if (ww.extra_size == 0x0c + ww.channels * 0x2e) { /* newer Wwise */
                vgmstream->num_samples = read_32bit(ww.fmt_offset + 0x18, streamFile);
                wiih_offset = ww.fmt_offset + 0x1c;
                wiih_size = 0x2e * ww.channels;
            }
            else {
                goto fail;
            }

            /* get coefs and default history */
            dsp_read_coefs(vgmstream,streamFile,wiih_offset, 0x2e, ww.big_endian);
            for (i=0; i < ww.channels; i++) {
                vgmstream->ch[i].adpcm_history1_16 = read_16bitBE(wiih_offset + i * 0x2e + 0x24,streamFile);
                vgmstream->ch[i].adpcm_history2_16 = read_16bitBE(wiih_offset + i * 0x2e + 0x26,streamFile);
            }

            break;
        }

#ifdef VGM_USE_FFMPEG
        case XMA2: {    /* X360/XBone */
            uint8_t buf[0x100];
            int bytes;
            off_t xma2_offset;
            size_t xma2_size;

            if (!ww.big_endian) goto fail; /* must be Wwise (real XMA are LE and parsed elsewhere) */

            if (find_chunk(streamFile, 0x584D4132,first_offset,0, &xma2_offset,&xma2_size, ww.big_endian, 0)) { /*"XMA2"*/ /* older Wwise */
                bytes = ffmpeg_make_riff_xma2_from_xma2_chunk(buf,0x100, xma2_offset, xma2_size, ww.data_size, streamFile);
            } else { /* newer Wwise */
                bytes = ffmpeg_make_riff_xma_from_fmt(buf,0x100, ww.fmt_offset, ww.fmt_size, ww.data_size, streamFile, ww.big_endian);
            }
            if (bytes <= 0) goto fail;

            vgmstream->codec_data = init_ffmpeg_header_offset(streamFile, buf,bytes, ww.data_offset,ww.data_size);
            if ( !vgmstream->codec_data ) goto fail;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            vgmstream->num_samples = ww.num_samples; /* set while parsing XMAWAVEFORMATs */

            /* "XMAc": rare Wwise extension, XMA2 physical loop regions (loop_start_b, loop_end_b, loop_subframe_data) */
            VGM_ASSERT(find_chunk(streamFile, 0x584D4163,first_offset,0, NULL,NULL, ww.big_endian, 0), "WWISE: XMAc chunk found\n");
            /* other chunks: "seek", regular XMA2 seek table */

            break;
        }

        case XWMA: {    /* X360 */
            ffmpeg_codec_data *ffmpeg_data = NULL;
            uint8_t buf[0x100];
            int bytes;

            if (!ww.big_endian) goto fail; /* must be from Wwise X360 (PC LE XWMA is parsed elsewhere) */

            bytes = ffmpeg_make_riff_xwma(buf,0x100, ww.format, ww.data_size, vgmstream->channels, vgmstream->sample_rate, ww.average_bps, ww.block_align);
            if (bytes <= 0) goto fail;

            ffmpeg_data = init_ffmpeg_header_offset(streamFile, buf,bytes, ww.data_offset,ww.data_size);
            if ( !ffmpeg_data ) goto fail;
            vgmstream->codec_data = ffmpeg_data;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            /* manually find total samples, why don't they put this in the header is beyond me */
            if (ww.format == 0x0162) { /* WMAPRO */
                xma_sample_data msd;
                memset(&msd,0,sizeof(xma_sample_data));

                msd.channels = ww.channels;
                msd.data_offset = ww.data_offset;
                msd.data_size = ww.data_size;
                wmapro_get_samples(&msd, streamFile,  ww.block_align, ww.sample_rate,0x0000);

                vgmstream->num_samples = msd.num_samples;
            } else { /* WMAv2 */
                vgmstream->num_samples = ffmpeg_data->totalSamples; //todo inaccurate approximation using the avg_bps
            }

            break;
        }

        case AAC: {     /* iOS/Mac */
            ffmpeg_codec_data * ffmpeg_data = NULL;
            if (ww.block_align != 0 || ww.bits_per_sample != 0) goto fail;

            /* extra: size 0x12, unknown values */

            ffmpeg_data = init_ffmpeg_offset(streamFile, ww.data_offset,ww.data_size);
            if (!ffmpeg_data) goto fail;
            vgmstream->codec_data = ffmpeg_data;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            vgmstream->num_samples = ffmpeg_data->totalSamples;
            break;
        }
#endif
        case HEVAG:     /* PSV */
            /* changed values, another bizarre Wwise quirk */
            //ww.block_align /* unknown (1ch=2, 2ch=4) */
            //ww.bits_per_sample; /* probably interleave (0x10) */
            //if (ww.bits_per_sample != 4) goto fail;

            if (ww.big_endian) goto fail;

            /* extra_data: size 0x06, @0x00: samples per block (0x1c), @0x04: channel config */

            vgmstream->coding_type = coding_HEVAG;
            vgmstream->layout_type = layout_interleave;
            vgmstream->interleave_block_size = 0x10;

            vgmstream->num_samples = ps_bytes_to_samples(ww.data_size, ww.channels);
            break;

        case ATRAC9:    /* PSV/PS4 */
            VGM_LOG("WWISE: ATRAC9 found (unsupported)\n");
            goto fail;

        default:
            goto fail;
    }


    if ( !vgmstream_open_stream(vgmstream,streamFile,start_offset) )
        goto fail;
    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
