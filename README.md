# pi_radio
Raspberry Pi Internet Ratio

an internet radio to play RTHK developed on Rasbberry Pi.  It uses the following libraries : curl, mpg123, asound

* pi_rthk.c : the original version just supporting mp3 streaming (using RTHK as test case)

* pi_radio.c : the enhanced version supporting HTTP Live Streaming (HLS) on slices of MPEG-2 Transport Stream
