import os

dts_path = '/home/brandon/Coding/photonicat2/openwrt/tmp/rk3576-photonicat-2.dts'
patch_path = '/home/brandon/Coding/photonicat2/openwrt/target/linux/rockchip/patches-6.12/999-arm64-dts-rockchip-rk3576-photonicat-2.patch'

with open(dts_path, 'r') as f:
    dts_content = f.read()

lines = dts_content.splitlines()
num_lines = len(lines)

patch_content = f'''From: Brandon Cleary <cleary.brandon@gmail.com>
Date: Fri, 28 Feb 2026 12:00:00 +0000
Subject: [PATCH] rockchip: add support for Ariaboard Photonicat 2

Add support for the Ariaboard Photonicat 2, a dual-GbE router board based
on the Rockchip RK3576 SoC (4x Cortex-A72 + 4x Cortex-A53).

Specifications:
- Rockchip RK3576 SoC with RK806 PMIC
- 4GB/8GB LPDDR5 RAM, 32GB/64GB/128GB eMMC
- 2x Gigabit Ethernet (RTL8211F PHY, RGMII)
- M.2 Key E (PCIe Gen2 x1) for WiFi (Qualcomm WCN7850)
- M.2 Key M (PCIe Gen1 x1) for NVMe SSD
- USB 3.0 host port
- M.2 Key B for 5G modem (Quectel RM520N)
- microSD card slot
- HDMI output
- Serial console on UART0 at 1500000 baud

Installation: Write the sysupgrade image to a microSD card using dd.
The board boots from SD card when inserted.

Signed-off-by: Brandon Cleary <cleary.brandon@gmail.com>
---
 arch/arm64/boot/dts/rockchip/Makefile              |   1 +
 .../arm64/boot/dts/rockchip/rk3576-photonicat-2.dts | {num_lines} ++++++++++++++++++++
 2 files changed, {num_lines + 1} insertions(+)
 create mode 100644 arch/arm64/boot/dts/rockchip/rk3576-photonicat-2.dts

--- a/arch/arm64/boot/dts/rockchip/Makefile
+++ b/arch/arm64/boot/dts/rockchip/Makefile
@@ -127,6 +127,7 @@ dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3568-wo
 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3568-wolfvision-pf5-display-vz.dtbo
 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3568-wolfvision-pf5-io-expander.dtbo
 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3576-nanopi-r76s.dtb
+dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3576-photonicat-2.dtb
 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3576-rock-4d.dtb
 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3582-radxa-e52c.dtb
 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3588-armsom-sige7.dtb
--- /dev/null
+++ b/arch/arm64/boot/dts/rockchip/rk3576-photonicat-2.dts
@@ -0,0 +1,{num_lines} @@
'''

for line in lines:
    patch_content += '+' + line + '\n'

with open(patch_path, 'w') as f:
    f.write(patch_content)
