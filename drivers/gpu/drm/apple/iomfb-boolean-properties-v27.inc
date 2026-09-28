/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* D569: IORegistryEntry::setProperty(char const *, bool) host metadata. */
struct dcp_boolean_property_v27 {
	struct list_head node;
	char name[64];
	bool value;
};

static bool trampoline_set_boolean_property_v27(struct apple_dcp *dcp, int tag,
						void *out, void *in)
{
	const u8 *request = in;
	struct dcp_boolean_property_v27 *property;
	char name[64];

	trace_iomfb_callback(dcp, tag, __func__);
	/* Native marshalling terminates the fixed-width name at byte63. */
	memcpy(name, request, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	put_unaligned_le32(0, out);
	/* All IOMFB callbacks for this DCP run on its single RTKit worker. */
	list_for_each_entry(property, &dcp->iomfb_boolean_properties, node)
		if (!strcmp(property->name, name))
			goto update;
	property = kzalloc(sizeof(*property), GFP_KERNEL);
	if (!property)
		return true;
	memcpy(property->name, name, sizeof(name));
	list_add_tail(&property->node, &dcp->iomfb_boolean_properties);
update:
	property->value = request[64] & 1;
	dev_dbg(dcp->dev, "boolean property %s=%u\n", name, property->value);
	put_unaligned_le32(1, out);
	return true;
}

/* Registered before RTKit resources: teardown stops its worker first. */
void iomfb_boolean_properties_cleanup_v27(void *data)
{
	struct apple_dcp *dcp = data;
	struct dcp_boolean_property_v27 *property, *next;

	list_for_each_entry_safe(property, next, &dcp->iomfb_boolean_properties, node) {
		list_del(&property->node);
		kfree(property);
	}
}
