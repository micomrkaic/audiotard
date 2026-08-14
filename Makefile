# This file is part of audiotard.
# Copyright (C) 2026  Mico
# audiotard is free software, released under the GNU GPL v3 or later;
# see the COPYING file or <https://www.gnu.org/licenses/>.

CC      = cc
CFLAGS  = -std=c17 -O2 -Wall -Wextra -Wpedantic
LDLIBS  = -lm
CORE    = src/chain.c src/engine.c src/audio_io.c src/effects.c
HDRS    = src/engine.h src/audio_io.h src/effects.h src/chain.h

all: audiotard test_engine

audiotard: src/main.c src/abx.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ src/main.c src/abx.c $(CORE) $(LDLIBS)

test_engine: src/test_engine.c src/engine.c src/engine.h
	$(CC) $(CFLAGS) -o $@ src/test_engine.c src/engine.c $(LDLIBS)

# GUI: needs libgtk-3-dev. GTK headers are not -Wpedantic-clean, so the
# GUI translation unit builds without it; the core keeps full warnings.
gui: audiotard-gui
audiotard-gui: src/gui.c src/playback.c src/fft.c $(CORE) $(HDRS) src/playback.h src/fft.h
	$(CC) -std=c17 -O2 -Wall -Wextra $$(pkg-config --cflags gtk+-3.0 alsa) \
	    -o $@ src/gui.c src/playback.c src/fft.c $(CORE) $$(pkg-config --libs gtk+-3.0 alsa) -lpthread $(LDLIBS)

.PHONY: all gui check clean
check: test_engine
	./test_engine

clean:
	rm -f audiotard test_engine audiotard-gui
