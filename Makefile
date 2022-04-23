all: pi_rthk pi_radio

pi_radio: pi_radio.c ffmpeg_decode.c
	gcc -o $@ -lavformat -lavcodec -lavutil -lswscale -lswresample -lcurl -lmpg123 -lasound $^

pi_rthk: pi_rthk.c
	gcc -lcurl -lmpg123 -lasound -o pi_rthk pi_rthk.c