0. download mainline kernel 4.20.17 and unpack
1. replace files from ../linux-4.20.17/arch/arm/boot/dts/ with files from this folder
2. make ARCH=arm sunxi_defconfig
3. setup the kernel if needed: make ARCH=arm menuconfig
4. make the kernel: make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs
