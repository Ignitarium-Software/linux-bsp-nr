# SPDX-License-Identifier: GPL-2.0
#
# X5H Gen5 Audio Pipeline — split into two modules:
#   x5h-audio-hw.ko   — hardware platform driver (pipeline, DMA, buffers)
#   x5h-audio-alsa.ko — ASoC interface driver (component, DAI, PCM)
#

x5h-audio-hw-objs := x5h_audio.o x5h_hw_platform.o
x5h-audio-alsa-objs := x5h_alsa.o

obj-$(CONFIG_SND_SOC_X5H_AUDIO) += x5h-audio-hw.o
obj-$(CONFIG_SND_SOC_X5H_AUDIO) += x5h-audio-alsa.o
