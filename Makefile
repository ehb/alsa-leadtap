# SPDX-License-Identifier: LGPL-2.1-or-later
# alsa-leadtap
#
# Needs the ALSA headers (alsa/asoundlib.h, alsa/pcm_external.h), not the library, for
# the plugin: it is loaded into a process that already has libasound. The selftest
# links libasound. Cross build example (headers copied from a target-compatible tree):
#   make plugin CC=aarch64-linux-gnu-gcc ALSA_INC=/path/to/dir-containing-alsa
ALSA_INC ?= /usr/include
BUILD ?= build
CFLAGS ?= -O2 -g -Wall -Wextra
SO = libasound_module_pcm_leadtap.so
FAKE = libasound_module_pcm_fakeslave.so

all: plugin

plugin: $(BUILD)/$(SO)

$(BUILD)/$(SO): src/pcm_leadtap.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -pthread -I$(ALSA_INC) -DPIC src/pcm_leadtap.c -o $@

$(BUILD)/$(FAKE): test/fake_slave.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -I$(ALSA_INC) -DPIC test/fake_slave.c -o $@

$(BUILD)/selftest: test/selftest.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(ALSA_INC) test/selftest.c -o $@ -lasound -lm

# Runs on a machine with alsa-lib; no audio hardware needed.
test: $(BUILD)/$(SO) $(BUILD)/$(FAKE) $(BUILD)/selftest
	$(BUILD)/selftest $(CURDIR)/$(BUILD)/$(SO) $(CURDIR)/$(BUILD)/$(FAKE)

clean:
	$(RM) -r $(BUILD)

.PHONY: all plugin test clean
