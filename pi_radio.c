/*
File: pi_radio.c
Description: an internet radio on Raspberry Pi
Modification history
2021-10-13  copy from pi_rthk and add logic to play m3u8 playlist streaming mpeg ts 
*/

/* the following is the MIME and filename extension mapping used in this program
m3u8 application/vnd.apple.mpegurl
m3u  audio/mpegurl
ts   video/mp2t
aac  audio/aac
mp3  audio/mpeg
*/

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <libgen.h>

#include <curl/curl.h>
#include <mpg123.h>
#include <alsa/asoundlib.h>

int ffmpeg_decode (char *, char *);

#define LOG_FILENAME "/tmp/pi_radio.log"
#define VOX_FILENAME "/tmp/pi_radio.vox"
#define TS_FILENAME  "/tmp/pi_radio.ts"
#define M3U8_FILENAME  "/tmp/pi_radio.m3u8"
// ALSA on Pi only support 44100 ?!?
#define VOX_SAMPLING_RATE 44100

char refresh_url[1000];
char last_url[1000];

mpg123_handle *mh = NULL;
snd_pcm_t *playback_handle;
CURL *http_handle;
CURLM *multi_handle;
int channels;

char content_type[2000];
FILE *curl_output_fp;
char playlist_url[10][2000];  // maximum 10 playlist each 2000 char long
int media_sequence_fetched;
int media_sequence_played;

FILE *log_fp;

// ==============================================================

void pi_radio_log (char * format, ... )
{
time_t t1;
struct tm *tm1;
time( &t1 );
tm1 = localtime( &t1 );
fprintf ((log_fp ? log_fp : stderr), "%04d-%02d-%02d %02d:%02d:%02d ", tm1->tm_year+1900, tm1->tm_mon+1, tm1->tm_mday, tm1->tm_hour, tm1->tm_min, tm1->tm_sec);

va_list args;
va_start (args, format);

vfprintf ((log_fp ? log_fp : stderr), format, args);
va_end (args);
if (log_fp)
  fflush (log_fp);
}

// ==============================================================

int parse_m3u8_file (char *m3u8_filename)
/* parse an m3u8 file and return the following
0 : fail
1 : if return one URL (result stored in playlisturl[0])
>1: if it contains multiple ts files 
*/
{
FILE *fp;
char line[2000];
int url_count = 0;
fp = fopen (m3u8_filename, "r");
while (fgets(line, 2000, fp) != NULL) {
  pi_radio_log ("m3u8 contains: %s", line);
  if (memcmp (line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
    sscanf (line+22, "%d", &media_sequence_fetched);
    continue;
    }
  if (line [0] == '#')
    continue;
  sscanf (line, "%s", playlist_url[url_count]);
  url_count++;
  if (media_sequence_fetched == 0)
    break;
  } // while
return url_count;
}

void str_trim (char *s)
// triming the trailing white spaces by modifying the original string
{
int m = strlen (s);
int i;
for (i=m-1; i>=0; i--) {
  if (isspace(s[i]))
    s[i] = '\0';
  else
    break;
  }
} // str_trim();

void str_toupper (char *s)
// converting the string to upper case by modify ihe original string
{
int m = strlen (s);
if (m == 0)
  return;
int i;
for (i=0; i<m; i++)
  s[i] = toupper (s[i]);
} // str_toupper ()

// ==============================================================

size_t curl_write_callback_handler (char *ptr, size_t size, size_t nmemb, void *userdata)
{

size_t decoded_bytes;
long int rate;
int encoding;
int err = MPG123_OK;
int frames;

pi_radio_log ("Inside %s() with %d bytes\n", __func__, nmemb);

pi_radio_log ("calling mpg123_feed()\n");
err = mpg123_feed (mh, ptr, nmemb); // size is always 1 in curl
if (err != MPG123_OK) {
  pi_radio_log ("ERROR: mpg123_feed fails (%s)", mpg123_plain_strerror(err));
  return 0; // return 0 means error to curl
  }

off_t frame_offset;
unsigned char *audio;
do {
  pi_radio_log ("calling mpg123_decode_frame()\n");
  err = mpg123_decode_frame (mh, &frame_offset, &audio, &decoded_bytes);
  switch (err) {
    case MPG123_NEW_FORMAT:
      pi_radio_log ("mpg123_decode_frame returns MPG123_NEW_FORMAT\n");
      pi_radio_log ("calling mpg123_getformat()\n");
      if (MPG123_OK != mpg123_getformat(mh, &rate, &channels, &encoding)) {
        pi_radio_log ("ERROR: mpg123_getformat fails\n");
        return 0; // return 0 means error to curl
        }
      pi_radio_log ("rate = %ld channels = %d encoding = %d (%s)\n", rate, channels, encoding,
        (encoding == MPG123_ENC_SIGNED_16 ? "MPG123_ENC_SIGNED_16" : " "));
      pi_radio_log ("calling mpg123_format()\n");
      if (MPG123_OK != mpg123_format (mh, rate, channels, encoding)) {
        pi_radio_log ("mpg123_format fails\n");
        return 0; // return 0 means error to curl
        }
      pi_radio_log ("calling snd_pcm_set_params\n");
      if ((err = snd_pcm_set_params(playback_handle,
             SND_PCM_FORMAT_S16_LE,
             SND_PCM_ACCESS_RW_INTERLEAVED,
             channels,
             (unsigned int) rate,
             0, /* disallow resampling */
             5000000)) < 0) {   /* 5sec */
        pi_radio_log("ERROR: snd_pcm_set_params() fails: %s\n", snd_strerror(err));
        return 0; // return 0 means error to curl
        }
       break;
     case MPG123_NEED_MORE:
       pi_radio_log ("mpg123_decode_frame returns MPG123_NEED_MORE with decoded_bytes = %d\n", decoded_bytes);
       break;
     case MPG123_OK :
       pi_radio_log ("mpg123_decode_frame() returns MPG124_OK with decoded_bytes = %d\n", decoded_bytes);
       if (decoded_bytes > 0) {
         frames = decoded_bytes / 2 / channels; /* 2 == 16(sample size) / 8(bits per byte) */
         pi_radio_log ("calling snd_pcm_writei()\n");
         err = snd_pcm_writei (playback_handle, audio, frames);
         if (err != frames) {
           pi_radio_log ("ERROR: snd_pcm_writei() fails (%s)\n", snd_strerror (err));
           return 0; // return 0 means error to curl
           }
         }
       break;
     default: 
       pi_radio_log("ERROR: mpg123_read fails... %s", mpg123_plain_strerror(err));
       break;
     } // switch
   } while (decoded_bytes > 0);

return nmemb;
} // curl_write_callback_handler()

// ==============================================================

static size_t curl_header_callback (char *buffer, size_t size, size_t nitems, void *userdata)
{
size_t numbytes = size * nitems;
char b[1000];
memcpy (b, buffer, (numbytes>999) ? 999 : numbytes);
b[numbytes]= '\0';
str_trim (b);
pi_radio_log ("HTTP HEADER : [%s]\n", b);
str_toupper (b);

if (memcmp (b, "CONTENT-TYPE:", 13) == 0) {
  sscanf (b+13, "%s", content_type);
  if (strcmp (content_type, "APPLICATION/VND.APPLE.MPEGURL") == 0) {
    pi_radio_log ("Content-Type (%s) is m3u8\n", content_type);
    curl_easy_setopt (http_handle, CURLOPT_WRITEFUNCTION, fwrite);
    curl_output_fp = fopen (M3U8_FILENAME, "w");
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, curl_output_fp);
    }
  else if (strcmp (content_type, "VIDEO/MP2T") == 0) {
    pi_radio_log ("Content-Type (%s) is TS stream\n", content_type);
    curl_easy_setopt (http_handle, CURLOPT_WRITEFUNCTION, fwrite);
    curl_output_fp = fopen (TS_FILENAME, "w");
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, curl_output_fp);
    }
  else if (strcmp (content_type, "AUDIO/X-MPEGURL") == 0) {
    pi_radio_log ("Content-Type (%s) is m3u\n", content_type);
    curl_easy_setopt (http_handle, CURLOPT_WRITEFUNCTION, fwrite);
    curl_output_fp = fopen ("tmp.m3u", "w");
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, curl_output_fp);
    }
  else if (strcmp (content_type, "AUDIO/MPEG") == 0) {
    pi_radio_log ("Content-Type (%s) is MP3\n", content_type);
    curl_easy_setopt (http_handle, CURLOPT_WRITEFUNCTION, curl_write_callback_handler);
    }
  else if (strcmp (content_type, "AUDIO/AAC") == 0) {
    pi_radio_log ("Content-Type (%s) is not supported\n", content_type);
    return 0; // error exit
    }
  }
return numbytes;
} // curl_header_callback ()

// ==============================================================

int pi_aplay (char *vox_filename, int rate)
{
int err;
FILE *vox_fp;
vox_fp = fopen (vox_filename, "r");
if (vox_fp == NULL) {
  pi_radio_log ("ERROR: cannot open %s\n", vox_filename);
  return 0;
  }

while (1) {
  char buffer[409600];
  int n = fread (buffer, 1, 409600, vox_fp);
  if (n == 0)
    break;
  int frames = n / 4;
  pi_radio_log ("calling snd_pcm_writei() with %d frames\n", frames);
    err = snd_pcm_writei (playback_handle, buffer, frames);
  if (err != frames) {
    pi_radio_log ("ERROR: snd_pcm_writei() failed (%s)\n", snd_strerror (err));
    return 0;
    }
  } // while (1)
fclose (vox_fp);
} // pi_aplay

// ==============================================================

void radio_clean_up()
{
pi_radio_log ("Calling mpg123_delete()\n");
mpg123_delete (mh);
pi_radio_log ("Calling snd_pcm_drop()\n");
snd_pcm_drop(playback_handle);
pi_radio_log ("Calling snd_pcm_close()\n");
snd_pcm_close (playback_handle);
pi_radio_log ("Calling curl_multi_remove_handle()\n");
curl_multi_remove_handle(multi_handle, http_handle);
pi_radio_log ("Calling curl_easy_cleanup()\n");
curl_easy_cleanup(http_handle);
pi_radio_log ("Calling curl_multi_cleanup()\n");
curl_multi_cleanup(multi_handle);
pi_radio_log ("Calling curl_global_cleanup()\n");
curl_global_cleanup();
fclose (log_fp);
}

// ==============================================================

void pi_radio_atexit ()
{
  pi_radio_log ("Inside %s\n", __func__);
  radio_clean_up();
}

// ==============================================================

void default_signal_handler(int signal_number)
{
  pi_radio_log ("Inside %s\n", __func__);
  exit(0);  // will call pi_radio_exit()
}

// ==============================================================

int start_curl (char *url)
{
strcpy (last_url, url);
int still_running = 1; /* keep number of running handles */

pi_radio_log ("start_curl() starts with url = %s\n", url);

curl_easy_setopt (http_handle, CURLOPT_URL, url);

// curl_easy_setopt(http_handle, CURLOPT_NOSIGNAL, 1L);

curl_multi_add_handle(multi_handle, http_handle);

do {
  CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
  if(!mc)
/* since the version of libcurl in Raspberry Pi is quite old */
#if LIBCURL_VERSION_NUM >= 0x076600
    mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
#else
    mc = curl_multi_wait (multi_handle, NULL, 0, 1000, NULL);
#endif

  if (mc) {
    pi_radio_log("ERROR: curl_multi_poll() or curl_multi_wait() failed, code %d.\n", (int)mc);
    break;
    }

  } while (still_running);

curl_multi_remove_handle(multi_handle, http_handle);

if (curl_output_fp != NULL)
  fclose (curl_output_fp);

pi_radio_log ("end of start_curl()\n");

} // start_curl()

/*********************************/
int main(int argc, char **argv)
{
if (argc != 2) {
  fprintf (stderr, "Usage: %s radio_url\n", basename(argv[0]));
  fprintf (stderr, "\nThe following URL's have been tested okay\n");
  fprintf (stderr, "\nMETRO 104\n");
  fprintf (stderr, "https://metroradio-lh.akamaihd.net/i/104_h@349798/master.m3u8\n");
  fprintf (stderr, "http://metroradio-lh.akamaihd.net/i/104_h@349798/index_48_a-p.m3u8?sd=10&rebase=on\n");
  fprintf (stderr, "\nMETRO 997\n");
  fprintf (stderr, "https://metroradio-lh.akamaihd.net/i/997_h@349799/master.m3u8\n");
  fprintf (stderr, "\nMETRO 1044\n");
  fprintf (stderr, "https://metroradio-lh.akamaihd.net/i/1044_h@349800/master.m3u8\n");
  fprintf (stderr, "\nRTHK\n");
  fprintf (stderr, "http://stm.rthk.hk/radio1  https://www.rthk.hk/live1.m3u\n");
  fprintf (stderr, "http://stm.rthk.hk/radio2\n");
  fprintf (stderr, "http://stm.rthk.hk/radio3\n");
  fprintf (stderr, "http://stm.rthk.hk/radio4\n");
  fprintf (stderr, "http://stm.rthk.hk/radio5\n");
  fprintf (stderr, "http://stm.rthk.hk/radiopth\n");
  fprintf (stderr, "\nSmooth Jazz CD 101.9 New York\n");
  fprintf (stderr, "http://us3.internet-radio.com:8485/\n");
  fprintf (stderr, "\nPlease use quotes to embed the whole URL lest it contains the ampersand character\n");
  return 1;
  }

log_fp = fopen (LOG_FILENAME, "w");
if (log_fp == NULL) {
  fprintf (stderr, "ERROR: Cannot open \"" LOG_FILENAME "\". Program exits\n");
  exit (1);
  }
fprintf (stderr, "open \"" LOG_FILENAME "\" to read the log\n");

pi_radio_log ("%s starts\n", argv[0]);
atexit(pi_radio_atexit); // for abnormal exit

pi_radio_log ("Calling signal() to set signal handler\n");
signal(SIGINT, default_signal_handler); // for Ctrl-C
signal(SIGSTOP, default_signal_handler); // for Ctrl-C

int err;

pi_radio_log ("Calling snd_pcm_open()\n");
if ((err = snd_pcm_open (&playback_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
  pi_radio_log ("ERROR: snd_pcm_open() fails (%s)\n", snd_strerror (err));
  return 1;
  }

curl_version_info_data *d = curl_version_info (CURLVERSION_NOW);
pi_radio_log ("curl version %s\n", d->version);

pi_radio_log ("mpg123 version %d\n", MPG123_API_VERSION);

pi_radio_log ("ALSA version %s\n", snd_asoundlib_version());

#if MPG123_API_VERSION < 46

pi_radio_log ("Calling mpg123_init()\n");
err = mpg123_init();
if (err != MPG123_OK) {
  pi_radio_log ("ERROR: mpg123_init() fails... %s\n", mpg123_plain_strerror(err));
  return 0;
  }
#endif

pi_radio_log ("Calling mpg123_new()\n");
mh = mpg123_new(NULL, &err);
if (mh  == NULL) {
  pi_radio_log("ERROR: mpg123_new() fails... %s", mpg123_plain_strerror(err));
  return 0;
  }

pi_radio_log ("Calling mpg123_open_feed()\n");
err = mpg123_open_feed (mh);
if (err != MPG123_OK) {
  pi_radio_log("ERROR: mpg123_open_feed() fails... %s", mpg123_plain_strerror(err));
  return 0;
  }

pi_radio_log ("Calling curl_global_init()\n");
curl_global_init(CURL_GLOBAL_DEFAULT);

http_handle = curl_easy_init();

curl_easy_setopt (http_handle, CURLOPT_HEADERFUNCTION, curl_header_callback);

// curl_easy_setopt(http_handle, CURLOPT_VERBOSE, 1L);

// RTHK does not like a null user-agent in libcurl
curl_easy_setopt(http_handle, CURLOPT_USERAGENT, "curl/7.64.0");
 
multi_handle = curl_multi_init();

// if the Content-Type is audio/mpeg, the following function will not return
int curl_rtn = start_curl (argv[1]);

if (strcmp (content_type, "AUDIO/X-MPEGURL") == 0) {
  pi_radio_log ("got an m3u file and therefore need to parse the data\n");
  if (parse_m3u8_file("tmp.m3u") == 1) {
    pi_radio_log ("only one URL is returned and assume it is a MP3 (for the RTHK case)\n");
    start_curl (playlist_url[0]); // will not return if the Content-Type is audio/mpeg
    }
  else {
    pi_radio_log ("ERROR: expect only one URL\n");
    exit (1);
    }
  } // content_type == "AUDIO/X-MPEGURL"


if (strcmp (content_type, "APPLICATION/VND.APPLE.MPEGURL") != 0) {
  pi_radio_log ("ERROR: content_type (%s) is not \"APPLICATION/VND.APPLE.MPEGURL\"\n", content_type);
  exit (1);
  }

  pi_radio_log ("going to call snd_pcm_set_params()\n");
  if ((err = snd_pcm_set_params(playback_handle,
         SND_PCM_FORMAT_S16_LE,
         SND_PCM_ACCESS_RW_INTERLEAVED,
         2,
         VOX_SAMPLING_RATE,
         0, /* disallow resampling */
         100000000)) < 0) {   // 10 sec
    pi_radio_log("ERROR: snd_pcm_set_params() fails: %s\n", snd_strerror(err));
    exit (1);
    }

  snd_pcm_sw_params_t *sw_params;
  snd_pcm_sw_params_malloc (&sw_params);
  snd_pcm_sw_params_current (playback_handle, sw_params);
  snd_pcm_sw_params_set_start_threshold(playback_handle, sw_params, 0);

  pi_radio_log ("got a m3u8 file and therefore need to parse the data\n");
  int num_url = parse_m3u8_file(M3U8_FILENAME);
  if (num_url == 1) {
    pi_radio_log ("num_url == 1 and need to collect the next playlist\n");
    start_curl (playlist_url[0]); 
    if (strcmp (content_type, "APPLICATION/VND.APPLE.MPEGURL") != 0) {
      pi_radio_log ("ERROR: the next playlist is not an m3u8 file\n");
      exit (1);
      }
    num_url = parse_m3u8_file(M3U8_FILENAME);
    }
  if (num_url <=1 ) {
    pi_radio_log ("ERROR: num_url (%d) is not greater than 1\n");
    exit (1);
    }
  pi_radio_log ("parse_m3u8_file returns %d URL with media sequence = %d\n", num_url, media_sequence_fetched);
  strcpy (refresh_url, last_url);
  pi_radio_log ("set refresh_url to \"%s\"\n", refresh_url);

  int i;
  for (i=0; i<num_url; i++) {
    pi_radio_log ("handing url[%d] \"%s\"\n", i, playlist_url[i]);
    start_curl (playlist_url[i]); 
    pi_radio_log ("calling ffmpeg_decode()\n");
    ffmpeg_decode (TS_FILENAME, VOX_FILENAME);
    pi_radio_log ("calling pi_aplay()\n");
    pi_aplay (VOX_FILENAME, VOX_SAMPLING_RATE);
    media_sequence_played = media_sequence_fetched + i;
    pi_radio_log ("updating media_sequence_played to %d\n", media_sequence_played);
    }

  while (1) {
  start_curl (refresh_url);
  if (strcmp (content_type, "APPLICATION/VND.APPLE.MPEGURL") != 0) {
    pi_radio_log ("ERROR: the next playlist is not an m3u8 file\n");
    exit (1);
    }
  num_url = parse_m3u8_file(M3U8_FILENAME);
  if (num_url <=1) {
    pi_radio_log ("ERROR: num_url (%d) is not greater than 1\n");
    exit (1);
    }
  pi_radio_log ("parse_m3u8_file returns %d URL with media sequence = %d\n", num_url, media_sequence_fetched);
  if ((media_sequence_fetched + num_url - 1) > media_sequence_played) {
    pi_radio_log ("new media sequence received\n");
  for (i=0; i<num_url; i++) {
    if ((media_sequence_fetched + i) > media_sequence_played) {
      pi_radio_log ("handing url[%d] \"%s\"\n", i, playlist_url[i]);
      start_curl (playlist_url[i]); 
      pi_radio_log ("calling ffmpeg_decode()\n");
      ffmpeg_decode (TS_FILENAME, VOX_FILENAME);
      pi_radio_log ("calling pi_aplay()\n");
      pi_aplay (VOX_FILENAME, VOX_SAMPLING_RATE);
      media_sequence_played = media_sequence_fetched + i;
      pi_radio_log ("updating media_sequence_played to %d\n", media_sequence_played);
      }
    } // for
    }
  else {
    pi_radio_log ("sleep 4 seconds\n");
    sleep (4);
    }
  } // while (1)

pi_radio_log ("going to call snd_pcm_drain()\n");
snd_pcm_drain(playback_handle);
pi_radio_log ("Program exits\n");
return 0;
} // main


