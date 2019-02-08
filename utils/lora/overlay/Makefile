OVERLAY_SRC=bpi-r2-lora-spi-overlay.dts
OVERLAY_DST=bpi-r2-lora-spi.dtbo

all:
	dtc -I dts -O dtb -@ -o $(OVERLAY_DST) $(OVERLAY_SRC)

test:
	dtc -I fs /proc/device-tree

