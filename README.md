sunxi cedar kernel module - targeting mainline kernel - linux-4.11.y and higher


Module compilation - arm7v or aarch64 e.g.:
* make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnueabihf- KDIR="<kernel_dir>" -f Makefile.linux
* make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR="<kernel_dir>" -f Makefile.linux

###**UPD**

The driver has been modified for linux mainline kernel 4.19 and successfully tested.

#####*Modifications*
* reserved memory CMA below 256M is used for **dma_alloc_coherent** (see the device tree file) instead of ION allocator
* mmap has been modified to support MACC and DMA memory
* interrupt status code 0xB (AVC (h264 encoder)) has been added for Allwinner A20
* video engine has been added to the device tree

`ve: video-engine@01c0e000 {
      compatible = "allwinner,sunxi-cedar-ve";
      reg = <0x01c0e000 0x1000>,
	    <0x01c00000 0x10>,
	    <0x01c20000 0x800>;
      memory-region = <&cma_pool>;      
      syscon = <&syscon>;         
      clocks = <&ccu CLK_AHB_VE>, <&ccu CLK_VE>,
			<&ccu CLK_DRAM_VE>;
	  clock-names = "ahb", "mod", "ram";
	  resets = <&ccu RST_VE>;
	  interrupts = <GIC_SPI 53 IRQ_TYPE_LEVEL_HIGH>;
  };
`

Don't forget to put this code into your **dts** file
`
&ve {
	status = "okay";
};
`

Sources from other repositories and updated.
