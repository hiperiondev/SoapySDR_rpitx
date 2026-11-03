/**
 * @file rpi.hpp
 * @brief Raspberry Pi SoC address discovery from the device tree.
 *
 * @copyright Copyright (C) 2018 Evariste COURJAUD F5OEO
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DEF_RPI
#define DEF_RPI

#include <stdint.h>

/**
 * @brief Root of the device tree exported by the kernel.
 *
 * Can be overridden at build time (the host-side tests point it to a generated tree).
 */
#ifndef RPITX_DT_ROOT
#define RPITX_DT_ROOT "/proc/device-tree"
#endif

/**
 * @brief Returns the ARM physical base address of the SoC peripherals.
 *
 * Read from the "ranges" property of the soc node: 0x20000000 on BCM2835, 0x3F000000 on BCM2836/BCM2837
 * and 0xFE000000 on BCM2711 (low-peripheral mode).
 *
 * @return Peripheral base address, or 0 when the device tree cannot be read.
 */
unsigned bcm_host_get_peripheral_address(void);

/**
 * @brief Returns the bus alias of the SDRAM as seen by the VideoCore.
 * @return 0x40000000 (BCM2835) or 0xC0000000 (later SoCs), or 0 when the device tree cannot be read.
 */
unsigned bcm_host_get_sdram_address(void);

/**
 * @brief Returns the DMA channels reserved for the Linux DMA engine driver.
 *
 * The channel masks of every DMA controller node of the soc node ("dma@..." or "dma-controller@...") are
 * combined; the property is "dma-channel-mask", or "brcm,dma-channel-mask" on older device trees. Bit n set
 * means that the kernel may use channel n.
 *
 * @param known Receives true when at least one mask was found.
 * @return Channel bit mask (0 when unknown).
 */
uint32_t bcm_host_get_kernel_dma_mask(bool *known);

/**
 * @brief Returns the highest legacy DMA channel of the SoC.
 *
 * On BCM2711 channels 11-14 are DMA4 engines with a different register and control-block layout.
 *
 * @return 10 on BCM2711, 14 on BCM2835/6/7.
 */
int bcm_host_get_max_legacy_dma_channel(void);

#endif
