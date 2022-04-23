/* File: ffmpeg_decode.c
Modified from 
https://github.com/gavv/snippets/blob/master/decode_play/ffmpeg_decode.cpp

with the following changes:
- change from cpp to c
- degenerate to a function which output a vox file
- output format is changed to AV_SAMPLE_FMT_S16 from AV_SAMPLE_FMT_FLT
- change the deprecated API
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

int ffmpeg_decode (char *infile, char *outfile)
{

FILE *out_fp;

out_fp = fopen (outfile, "w");
if (out_fp == NULL) {
  fprintf (stderr, "Cannot open file \"%s\"\n", outfile);
  exit (1);
  }

int out_channels = 2, out_samples = 512, sample_rate = 44100;

int max_buffer_size =
        av_samples_get_buffer_size(
            NULL, out_channels, out_samples, AV_SAMPLE_FMT_S16, 1);

// register supported formats and codecs
// av_register_all(); deprecated

// allocate empty format context
// provides methods for reading input packets
AVFormatContext* fmt_ctx = avformat_alloc_context();
assert(fmt_ctx);

// determine input file type and initialize format context
if (avformat_open_input(&fmt_ctx, infile, NULL, NULL) != 0) {
  fprintf(stderr, "error: avformat_open_input()\n");
  exit(1);
  }

// determine supported codecs for input file streams and add them to format context
if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
  fprintf(stderr, "error: avformat_find_stream_info()\n");
  exit(1);
  }

// av_dump_format(fmt_ctx, 0, infile, 0);

// find audio stream in format context
size_t stream = 0;
for (; stream < fmt_ctx->nb_streams; stream++) {
  if (fmt_ctx->streams[stream]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
    break;
    }
  }
if (stream == fmt_ctx->nb_streams) {
  fprintf(stderr, "error: no audio stream found\n");
  exit(1);
  }

// find decoder for audio stream
// AVCodec* codec = avcodec_find_decoder(codec_ctx->codec_id);
AVCodec* codec = avcodec_find_decoder(fmt_ctx->streams[stream]->codecpar->codec_id);
if (!codec) {
  fprintf(stderr, "error: avcodec_find_decoder()\n");
  exit(1);
  }

AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
assert(codec_ctx);

// Fill the codecCtx with the parameters of the codec used in the read file.
int err;
if ((err = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream]->codecpar)) != 0) {
    // Something went wrong. Cleaning up...
  avcodec_close(codec_ctx);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);
  fprintf(stderr, "Error in avcodec_parameters_to_context() returns %d\n", err);
  exit (1);
  }

// initialize codec context with decoder we've found
if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
  fprintf(stderr, "error: avcodec_open2()\n");
  exit(1);
  }

// initialize converter from input audio stream to output stream
// provides methods for converting decoded packets to output stream
SwrContext* swr_ctx =
   swr_alloc_set_opts(NULL,
                           AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT, // output
                           AV_SAMPLE_FMT_S16,                    // output
                           sample_rate,                          // output
                           codec_ctx->channel_layout,  // input
                           codec_ctx->sample_fmt,      // input
                           codec_ctx->sample_rate,     // input
                           0,
                           NULL);
if (!swr_ctx) {
  fprintf(stderr, "error: swr_alloc_set_opts()\n");
  exit(1);
  }
swr_init(swr_ctx);

// create empty packet for input stream
AVPacket packet;
av_init_packet(&packet);
packet.data = NULL;
packet.size = 0;

// allocate empty frame for decoding
AVFrame* frame = av_frame_alloc();
assert(frame);

// allocate buffer for output stream
uint8_t* buffer = (uint8_t*)av_malloc(max_buffer_size);
assert(buffer);

// read packet from input audio file
while (av_read_frame(fmt_ctx, &packet) >= 0) {
  // skip non-audio packets
  if (packet.stream_index != stream) {
    continue;
    }

int ret = avcodec_send_packet(codec_ctx, &packet);
if (ret < 0) {
  fprintf (stderr, "ERROR: avcodec_send_packet() returns %d\n", ret);
  exit (1);
  }

while (avcodec_receive_frame(codec_ctx, frame) == 0) {
  // convert input frame to output buffer
  int got_samples = swr_convert(
    swr_ctx,
    &buffer, out_samples,
    (const uint8_t **)frame->data, frame->nb_samples);

  if (got_samples < 0) {
    fprintf(stderr, "error: swr_convert()\n");
    exit(1);
    }

  while (got_samples > 0) {
    int buffer_size =
      av_samples_get_buffer_size(
        NULL, out_channels, got_samples, AV_SAMPLE_FMT_S16, 1);

    assert(buffer_size <= max_buffer_size);

    // write output buffer to stdout
    if (fwrite(buffer, buffer_size, 1, out_fp) != 1) {
      fprintf(stderr, "error: fwrite()\n");
      exit(1);
      }

    // process samples buffered inside swr context
    // in and in_count are set to 0 to flush the last few samples out at the end
    got_samples = swr_convert(swr_ctx, &buffer, out_samples, NULL, 0);
    if (got_samples < 0) {
      fprintf(stderr, "error: swr_convert()\n");
      exit(1);
      }
    } // while (got_samples > 0)

  // free packet created by decoder
  // av_free_packet(&packet); deprecated
  av_packet_unref(&packet);
  } // while (av_read_frame(fmt_ctx, &packet) >= 0)
}

av_free(buffer);
av_frame_free(&frame);

swr_free(&swr_ctx);

avcodec_close(codec_ctx);
avformat_close_input(&fmt_ctx);

fclose (out_fp);

return 0;
} // ffmpeg_decode()

/*
int main (int argc, char **argv)
{
ffmpeg_decode (argv[1], argv[2]);
}
*/
