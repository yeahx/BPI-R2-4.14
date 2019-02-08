OVERLAY_SRC=rpi-lora-spi-overlay.dts
OVERLAY_DST=rpi-lora-spi.dtbo

all:
	dtc -I dts -O dtb -@ -o $(OVERLAY_DST) $(OVERLAY_SRC)

test:
	dtc -I fs /proc/device-tree

