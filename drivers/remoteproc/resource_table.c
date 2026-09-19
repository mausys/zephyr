/*
 * Copyright (c) 2020 STMicroelectronics
 * Copyright (c) 2024 Maurer Systems GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * In addition to the standard ELF segments, most remote processors would
 * also include a special section which we call "the resource table".
 *
 * The resource table contains system resources that the remote processor
 * requires before it should be powered on, such as allocation of physically
 * contiguous memory, or iommu mapping of certain on-chip peripherals.

 * In addition to system resources, the resource table may also contain
 * resource entries that publish the existence of supported features
 * or configurations by the remote processor, such as trace buffers and
 * supported virtio devices (and their configurations).

 * Dependencies:
 *   to be compliant with Linux kernel OS the resource table must be linked in a
 *   specific section named ".resource_table".

 * Related documentation:
 *   https://www.kernel.org/doc/Documentation/remoteproc.txt
 *   https://github.com/OpenAMP/open-amp/wiki/OpenAMP-Life-Cycle-Management
 */

#include <zephyr/kernel.h>

#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/drivers/remoteproc.h>
#include <zephyr/logging/log.h>

#include <openamp/remoteproc.h>
#include <openamp/virtio.h>

LOG_MODULE_REGISTER(rsc_table, CONFIG_RPROC_LOG_LEVEL);

extern char ram_console_buf[];

#define VRING_ALIGNMENT       16 /* fixed to match with Linux constraint */
#define RPMSG_IPU_C0_FEATURES 1

#define RESOURCE_TABLE_ATTR                                                                        \
__attribute__((__section__(DT_PROP(VDEV_NODE, resource_table)))))
#define __resource Z_GENERIC_SECTION(.resource_table)

#define RESOURCE_TABLE_NODE DT_CHOSEN(openamp_resource_table)

#define NUM_VDEVS DT_PROP_LEN(RESOURCE_TABLE_NODE, vdevs)



		
#define SUMMAND_CARVEOUT(node_vring) \
		IF_ENABLED(DT_NODE_HAS_PROP(node_vring, memory_region), (+ 1))

#define SUMMAND_USER_DEFINED_CARVEOUT(node_rsc_table) \
	COND_CODE_1(DT_NODE_HAS_PROP(node_rsc_table, carveouts), (DT_PROP_LEN(node_rsc_table, carveouts)), (0))	
	
#define ADD_VDEV_VRING_CARVEOUT(node_vring, prop, idx_vring, dummy, ...)  \
	SUMMAND_CARVEOUT(DT_PHANDLE_BY_IDX(node_vring, prop, idx_vring))

/*
 * Can't use DT_FOREACH_PROP_ELEM twice, so use DT_FOREACH_PROP_ELEM_VARGS
 * and add a dummy
 * */
#define COUNT_VDEV_VRINGS_CARVEOUTS(node_vring) \
	DT_FOREACH_PROP_ELEM_VARGS(node_vring, vrings,     \
					   ADD_VDEV_VRING_CARVEOUT, 0)	


#define COUNT_VDEV_CARVEOUTS(node_vdev, prop, idx_vdev)  \
		SUMMAND_CARVEOUT(DT_PHANDLE_BY_IDX(node_vdev, prop, idx_vdev)) \
		COUNT_VDEV_VRINGS_CARVEOUTS(DT_PHANDLE_BY_IDX(node_vdev, prop, idx_vdev))				   


#define COUNT_CARVEOUTS (SUMMAND_USER_DEFINED_CARVEOUT(RESOURCE_TABLE_NODE) DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, vdevs, COUNT_VDEV_CARVEOUTS))


#define VDEV_INDEX(idx) vdev_##idx


#define VRING_ENTRY(node_id, prop, idx, ...)                                                       \
	.vrings[idx] = (struct fw_rsc_vdev_vring){                                                 \
		.da = DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, device_address),                  \
		.align = VRING_ALIGNMENT,                                                          \
		.num = DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, num_buffers),                    \
	},

#define VDEV_ENTRY(node_id, prop, idx)                                                             \
	.VDEV_INDEX(idx) = {                                                                       \
		.vdev =                                                                            \
			(struct fw_rsc_vdev) {                                                      \
				.type = RSC_VDEV,                                                  \
				.id = DT_PROP_BY_PHANDLE_IDX(node_id, prop, idx, device_id),       \
				.dfeatures = RPMSG_IPU_C0_FEATURES,                                \
				.gfeatures = 0,                                                    \
				.config_len = 0,                                                   \
				.status = 0,                                                       \
				.num_of_vrings = DT_PROP_LEN(                                      \
					DT_PHANDLE_BY_IDX(node_id, prop, idx), vrings),            \
			},                                                                         \
		DT_FOREACH_PROP_ELEM_VARGS(DT_PHANDLE_BY_IDX(node_id, prop, idx), vrings,          \
					   VRING_ENTRY)},


#define VDEV_FIELD(node_id, prop, idx)                                                             \
	struct {                                                                                   \
		struct fw_rsc_vdev vdev;                                                           \
		struct fw_rsc_vdev_vring                                                           \
			vrings[DT_PROP_LEN(DT_PHANDLE_BY_IDX(node_id, prop, idx), vrings)];        \
	} vdev_##idx;

	
#define CARVEOUT_ENTRY(reg, carveout_name)    \
		(struct fw_rsc_carveout) {  \
		.type = RSC_CARVEOUT,  \
		.da = DT_REG_ADDR(reg),      \
		.pa = DT_REG_ADDR(reg),     \
		.len = DT_REG_SIZE(reg),   \
		.flags = 0,  \
		.name = carveout_name }, \
		

#define COND_CARVEOUT_ENTRY(node, carveout_name)    \
	IF_ENABLED(                          \
		DT_NODE_HAS_PROP(node, memory_region),         \
		(CARVEOUT_ENTRY(DT_PROP(node, memory_region), carveout_name)))
	
#define VDEV_VRING_CARVEOUT_NAME(idx_vdev, idx_vring)  \ 
	STRINGIFY(DT_CAT4(vdev, idx_vdev, vring, idx_vring))
	
#define VDEV_BUFFER_CARVEOUT_NAME(idx_vdev)  \ 
	STRINGIFY(DT_CAT3(vdev, idx_vdev, buffer))


#define VDEV_VRING_CARVEOUT_ENTRY(node_vring, prop, idx_vring, idx_vdev, ...)  \
	COND_CARVEOUT_ENTRY(DT_PHANDLE_BY_IDX(node_vring, prop, idx_vring), VDEV_VRING_CARVEOUT_NAME(idx_vdev, idx_vring))

#define VDEV_BUFFER_CARVEOUT_ENTRY(node_vdev, prop, idx_vdev)  \
	COND_CARVEOUT_ENTRY(DT_PHANDLE_BY_IDX(node_vdev, prop, idx_vdev), VDEV_BUFFER_CARVEOUT_NAME(idx_vdev))

#define VDEV_CARVEOUT_ENTRIES(node_vdev, prop, idx_vdev)         \
	VDEV_BUFFER_CARVEOUT_ENTRY(node_vdev, prop, idx_vdev) \
	DT_FOREACH_PROP_ELEM_VARGS(DT_PHANDLE_BY_IDX(node_vdev, prop, idx_vdev), vrings,     \
					   VDEV_VRING_CARVEOUT_ENTRY, idx_vdev)

#define USER_DEFINED_CARVEOUTS(node_id, prop, idx)   CARVEOUT_ENTRY(DT_PHANDLE_BY_IDX(node_id, prop, idx), DT_PROP_BY_PHANDLE_IDX(node_id, carveouts, idx, zephyr_memory_region))

#define FOREACH_CARVEOUT(fn)
#define CARVEOUT_ENTRIES FOREACH_CARVEOUT(CARVEOUT_ENTRY_ITER)

enum rsc_table_entries {
#if COUNT_CARVEOUTS > 0
	RSC_TABLE_CARVEOUT_ENTRIES = COUNT_CARVEOUTS - 1,
#endif
#if NUM_VDEVS > 0
	RSC_TABLE_VDEV_ENTRY = COUNT_CARVEOUTS + NUM_VDEVS - 1,
#endif
#if defined(CONFIG_RAM_CONSOLE)
	RSC_TABLE_TRACE_ENTRY,
#endif
	RSC_TABLE_NUM_ENTRIES
};


#define VDEV_OFFSET(node_id, prop, idx)     offsetof(struct fw_resource_table, vdev_##idx),
#define CARVEOUT_OFFSET(node_id, prop, idx) offsetof(struct fw_resource_table, carveouts[idx]),

struct fw_resource_table {
	struct resource_table hdr;
	uint32_t offset[RSC_TABLE_NUM_ENTRIES];
#if COUNT_CARVEOUTS > 0
	struct fw_rsc_carveout carveouts[COUNT_CARVEOUTS];
#endif
	DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, vdevs, VDEV_FIELD)
#if defined(CONFIG_RAM_CONSOLE)
	struct fw_rsc_trace cm_trace;
#endif
} METAL_PACKED_END __aligned(8) resource_table __resource = {
	.hdr =
		{
			.ver = 1,
			.num = RSC_TABLE_NUM_ENTRIES,
		},

	.offset =
		{
			DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, carveouts, CARVEOUT_OFFSET)
				DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, vdevs, VDEV_OFFSET)

#if defined(CONFIG_RAM_CONSOLE)
					offsetof(struct fw_resource_table, cm_trace),
#endif
		},

#if COUNT_CARVEOUTS > 0
	.carveouts = {
			     DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, vdevs, VDEV_CARVEOUT_ENTRIES)
				DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, carveouts, USER_DEFINED_CARVEOUTS)
			},
#endif
	DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, vdevs, VDEV_ENTRY)

#if defined(CONFIG_RAM_CONSOLE)
		.cm_trace =
		{
			.type = RSC_TRACE,
			.da = (uint32_t)ram_console_buf,
			.len = CONFIG_RAM_CONSOLE_BUFFER_SIZE,
			.name = "Zephyr_log",
		},
#endif
};



#if NUM_VDEVS > 0
#define VDEV_LUT_ENTRY(node_id, prop, idx)                                                         \
	{.index = DT_REG_ADDR(DT_PHANDLE_BY_IDX(node_id, prop, idx)),                              \
	 &resource_table.VDEV_INDEX(idx).vdev},

static struct {
	unsigned int index;
	struct fw_rsc_vdev *vdev;
} vdev_lut[] = {DT_FOREACH_PROP_ELEM(RESOURCE_TABLE_NODE, vdevs, VDEV_LUT_ENTRY)};
#endif

void *z_rproc_rsc_table(size_t *size)
{
	if (size) {
		*size = sizeof(resource_table);
	}

	return &resource_table;
}

int z_rproc_init_carveout_region(struct metal_io_region *region, const char *name)
{
#if COUNT_CARVEOUTS > 0
	static metal_phys_addr_t carveout_pa[COUNT_CARVEOUTS];
	struct fw_resource_table *rsc_table = &resource_table;

	for (int i = 0; i < COUNT_CARVEOUTS; i++) {
		if (strcmp(rsc_table->carveouts[i].name, name) == 0) {
			const struct fw_rsc_carveout *carveout = &rsc_table->carveouts[i];
			carveout_pa[i] = carveout->pa;
			metal_io_init(region, (void *)carveout->da, &carveout_pa[i], carveout->len, -1,
				      carveout->flags, NULL);
			return 0;
		}
	}
#endif
	return -ENOENT;
}

struct fw_rsc_carveout *z_rproc_get_carveout_by_index(unsigned int idx)
{
#if COUNT_CARVEOUTS > 0
	struct fw_resource_table *rsc_table = &resource_table;

	if (idx >= COUNT_CARVEOUTS) {
		return NULL;
	}

	return &rsc_table->carveouts[idx];
#else
	return NULL;
#endif
}

struct fw_rsc_vdev *z_rproc_get_vdev(unsigned int idx)
{
#if NUM_VDEVS > 0
	for (int i = 0; i < NUM_VDEVS; i++) {
		if (vdev_lut[i].index == idx) {
			return vdev_lut[i].vdev;
		}
	}
#endif
	return NULL;
}
