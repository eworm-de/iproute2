// SPDX-License-Identifier: GPL-2.0+

#include <stdio.h>
#include <getopt.h>
#include <errno.h>
#include <sys/time.h>
#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/vdpa.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_net.h>
#include <linux/netlink.h>
#include <libmnl/libmnl.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_config.h>
#include "mnl_utils.h"
#include <rt_names.h>

#include "version.h"
#include "json_print.h"
#include "utils.h"

#define VDPA_OPT_MGMTDEV_HANDLE		BIT(0)
#define VDPA_OPT_VDEV_MGMTDEV_HANDLE	BIT(1)
#define VDPA_OPT_VDEV_NAME		BIT(2)
#define VDPA_OPT_VDEV_HANDLE		BIT(3)
#define VDPA_OPT_VDEV_MAC		BIT(4)
#define VDPA_OPT_VDEV_MTU		BIT(5)
#define VDPA_OPT_MAX_VQP		BIT(6)
#define VDPA_OPT_QUEUE_INDEX		BIT(7)
#define VDPA_OPT_VDEV_FEATURES		BIT(8)

struct vdpa_opts {
	uint64_t present; /* flags of present items */
	char *mdev_bus_name;
	char *mdev_name;
	const char *vdev_name;
	unsigned int device_id;
	char mac[ETH_ALEN];
	uint16_t mtu;
	uint16_t max_vqp;
	uint32_t queue_idx;
	uint64_t device_features;
};

struct vdpa {
	struct mnlu_gen_socket nlg;
	struct vdpa_opts opts;
	bool json_output;
	struct indent_mem *indent;
};

static void pr_out_section_start(struct vdpa *vdpa, const char *name)
{
	open_json_object(NULL);
	open_json_object(name);
}

static void pr_out_section_end(struct vdpa *vdpa)
{
	close_json_object();
	close_json_object();
}

static void pr_out_array_start(struct vdpa *vdpa, const char *name)
{
	if (!vdpa->json_output) {
		print_nl();
		inc_indent(vdpa->indent);
		print_indent(vdpa->indent);
	}
	open_json_array(PRINT_ANY, name);
}

static void pr_out_array_end(struct vdpa *vdpa)
{
	close_json_array(PRINT_JSON, NULL);
	if (!vdpa->json_output)
		dec_indent(vdpa->indent);
}

static const enum mnl_attr_data_type vdpa_policy[VDPA_ATTR_MAX + 1] = {
	[VDPA_ATTR_MGMTDEV_BUS_NAME] = MNL_TYPE_NUL_STRING,
	[VDPA_ATTR_MGMTDEV_DEV_NAME] = MNL_TYPE_NUL_STRING,
	[VDPA_ATTR_DEV_NAME] = MNL_TYPE_STRING,
	[VDPA_ATTR_DEV_ID] = MNL_TYPE_U32,
	[VDPA_ATTR_DEV_VENDOR_ID] = MNL_TYPE_U32,
	[VDPA_ATTR_DEV_MAX_VQS] = MNL_TYPE_U32,
	[VDPA_ATTR_DEV_MAX_VQ_SIZE] = MNL_TYPE_U16,
	[VDPA_ATTR_DEV_NEGOTIATED_FEATURES] = MNL_TYPE_U64,
	[VDPA_ATTR_DEV_MGMTDEV_MAX_VQS] = MNL_TYPE_U32,
	[VDPA_ATTR_DEV_SUPPORTED_FEATURES] = MNL_TYPE_U64,
};

static int attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type;

	if (mnl_attr_type_valid(attr, VDPA_ATTR_MAX) < 0)
		return MNL_CB_OK;

	type = mnl_attr_get_type(attr);
	if (mnl_attr_validate(attr, vdpa_policy[type]) < 0)
		return MNL_CB_ERROR;

	tb[type] = attr;
	return MNL_CB_OK;
}

static int vdpa_argv_handle(struct vdpa *vdpa, int argc, char **argv,
			    char **p_mdev_bus_name,
			    char **p_mdev_name)
{
	unsigned int slashcount;
	char *str;

	if (argc <= 0 || *argv == NULL) {
		fprintf(stderr,
			"vdpa identification (\"mgmtdev_bus_name/mgmtdev_name\") expected\n");
		return -EINVAL;
	}
	str = *argv;
	slashcount = get_str_char_count(str, '/');
	if (slashcount > 1) {
		fprintf(stderr,
			"Wrong vdpa mgmtdev identification string format\n");
		fprintf(stderr, "Expected \"mgmtdev_bus_name/mgmtdev_name\"\n");
		fprintf(stderr, "Expected \"mgmtdev_name\"\n");
		return -EINVAL;
	}
	switch (slashcount) {
	case 0:
		*p_mdev_bus_name = NULL;
		*p_mdev_name = str;
		return 0;
	case 1:
		str_split_by_char(str, p_mdev_bus_name, p_mdev_name, '/');
		return 0;
	default:
		return -EINVAL;
	}
}

static int vdpa_argv_str(struct vdpa *vdpa, int argc, char **argv,
			 const char **p_str)
{
	if (argc <= 0 || *argv == NULL) {
		fprintf(stderr, "String parameter expected\n");
		return -EINVAL;
	}
	*p_str = *argv;
	return 0;
}

static int vdpa_argv_mac(struct vdpa *vdpa, int argc, char **argv, char *mac)
{
	int alen;

	if (argc <= 0 || *argv == NULL) {
		fprintf(stderr, "String parameter expected\n");
		return -EINVAL;
	}

	alen = ll_addr_a2n(mac, ETH_ALEN, *argv);
	if (alen < 0)
		return -EINVAL;
	return 0;
}

static int vdpa_argv_u16(struct vdpa *vdpa, int argc, char **argv,
			 uint16_t *result)
{
	if (argc <= 0 || *argv == NULL) {
		fprintf(stderr, "number expected\n");
		return -EINVAL;
	}

	return get_u16(result, *argv, 10);
}

static int vdpa_argv_u32(struct vdpa *vdpa, int argc, char **argv,
			 uint32_t *result)
{
	if (argc <= 0 || !*argv) {
		fprintf(stderr, "number expected\n");
		return -EINVAL;
	}

	return get_u32(result, *argv, 10);
}

static int vdpa_argv_u64_hex(struct vdpa *vdpa, int argc, char **argv,
			     uint64_t *result)
{
	if (argc <= 0 || !*argv) {
		fprintf(stderr, "number expected\n");
		return -EINVAL;
	}

	return get_u64((__u64 *)result, *argv, 16);
}

struct vdpa_args_metadata {
	uint64_t o_flag;
	const char *err_msg;
};

static const struct vdpa_args_metadata vdpa_args_required[] = {
	{VDPA_OPT_VDEV_MGMTDEV_HANDLE, "management device handle not set."},
	{VDPA_OPT_VDEV_NAME, "device name is not set."},
	{VDPA_OPT_VDEV_HANDLE, "device name is not set."},
	{VDPA_OPT_QUEUE_INDEX, "queue index is not set."},
};

static int vdpa_args_finding_required_validate(uint64_t o_required,
					       uint64_t o_found)
{
	uint64_t o_flag;
	int i;

	for (i = 0; i < ARRAY_SIZE(vdpa_args_required); i++) {
		o_flag = vdpa_args_required[i].o_flag;
		if ((o_required & o_flag) && !(o_found & o_flag)) {
			fprintf(stderr, "%s\n", vdpa_args_required[i].err_msg);
			return -EINVAL;
		}
	}
	if (o_required & ~o_found) {
		fprintf(stderr,
			"BUG: unknown argument required but not found\n");
		return -EINVAL;
	}
	return 0;
}

static void vdpa_opts_put(struct nlmsghdr *nlh, struct vdpa *vdpa)
{
	struct vdpa_opts *opts = &vdpa->opts;

	if ((opts->present & VDPA_OPT_MGMTDEV_HANDLE) ||
	    (opts->present & VDPA_OPT_VDEV_MGMTDEV_HANDLE)) {
		if (opts->mdev_bus_name)
			mnl_attr_put_strz(nlh, VDPA_ATTR_MGMTDEV_BUS_NAME,
					  opts->mdev_bus_name);
		mnl_attr_put_strz(nlh, VDPA_ATTR_MGMTDEV_DEV_NAME,
				  opts->mdev_name);
	}
	if ((opts->present & VDPA_OPT_VDEV_NAME) ||
	    (opts->present & VDPA_OPT_VDEV_HANDLE))
		mnl_attr_put_strz(nlh, VDPA_ATTR_DEV_NAME, opts->vdev_name);
	if (opts->present & VDPA_OPT_VDEV_MAC)
		mnl_attr_put(nlh, VDPA_ATTR_DEV_NET_CFG_MACADDR,
			     sizeof(opts->mac), opts->mac);
	if (opts->present & VDPA_OPT_VDEV_MTU)
		mnl_attr_put_u16(nlh, VDPA_ATTR_DEV_NET_CFG_MTU, opts->mtu);
	if (opts->present & VDPA_OPT_MAX_VQP)
		mnl_attr_put_u16(nlh, VDPA_ATTR_DEV_NET_CFG_MAX_VQP, opts->max_vqp);
	if (opts->present & VDPA_OPT_QUEUE_INDEX)
		mnl_attr_put_u32(nlh, VDPA_ATTR_DEV_QUEUE_INDEX, opts->queue_idx);
	if (opts->present & VDPA_OPT_VDEV_FEATURES) {
		mnl_attr_put_u64(nlh, VDPA_ATTR_DEV_FEATURES,
				opts->device_features);
	}
}

static int vdpa_argv_parse(struct vdpa *vdpa, int argc, char **argv,
			   uint64_t o_required, uint64_t o_optional)
{
	uint64_t o_all = o_required | o_optional;
	struct vdpa_opts *opts = &vdpa->opts;
	uint64_t o_found = 0;
	int err;

	if (o_required & VDPA_OPT_MGMTDEV_HANDLE) {
		err = vdpa_argv_handle(vdpa, argc, argv, &opts->mdev_bus_name,
				       &opts->mdev_name);
		if (err)
			return err;

		NEXT_ARG_FWD();
		o_found |= VDPA_OPT_MGMTDEV_HANDLE;
	} else if (o_required & VDPA_OPT_VDEV_HANDLE) {
		err = vdpa_argv_str(vdpa, argc, argv, &opts->vdev_name);
		if (err)
			return err;

		NEXT_ARG_FWD();
		o_found |= VDPA_OPT_VDEV_HANDLE;
	}

	while (NEXT_ARG_OK()) {
		if ((matches(*argv, "name") == 0) &&
		    (o_all & VDPA_OPT_VDEV_NAME)) {
			const char *namestr;

			NEXT_ARG_FWD();
			err = vdpa_argv_str(vdpa, argc, argv, &namestr);
			if (err)
				return err;
			opts->vdev_name = namestr;
			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_VDEV_NAME;
		} else if ((matches(*argv, "mgmtdev")  == 0) &&
			   (o_all & VDPA_OPT_VDEV_MGMTDEV_HANDLE)) {
			NEXT_ARG_FWD();
			err = vdpa_argv_handle(vdpa, argc, argv,
					       &opts->mdev_bus_name,
					       &opts->mdev_name);
			if (err)
				return err;

			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_VDEV_MGMTDEV_HANDLE;
		} else if ((strcmp(*argv, "mac") == 0) &&
			   (o_all & VDPA_OPT_VDEV_MAC)) {
			NEXT_ARG_FWD();
			err = vdpa_argv_mac(vdpa, argc, argv, opts->mac);
			if (err)
				return err;

			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_VDEV_MAC;
		} else if ((strcmp(*argv, "mtu") == 0) &&
			   (o_all & VDPA_OPT_VDEV_MTU)) {
			NEXT_ARG_FWD();
			err = vdpa_argv_u16(vdpa, argc, argv, &opts->mtu);
			if (err)
				return err;

			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_VDEV_MTU;
		} else if ((matches(*argv, "max_vqp")  == 0) && (o_optional & VDPA_OPT_MAX_VQP)) {
			NEXT_ARG_FWD();
			err = vdpa_argv_u16(vdpa, argc, argv, &opts->max_vqp);
			if (err)
				return err;

			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_MAX_VQP;
		} else if (!strcmp(*argv, "qidx") &&
			   (o_optional & VDPA_OPT_QUEUE_INDEX)) {
			NEXT_ARG_FWD();
			err = vdpa_argv_u32(vdpa, argc, argv, &opts->queue_idx);
			if (err)
				return err;

			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_QUEUE_INDEX;
		} else if (!strcmp(*argv, "device_features") &&
			   (o_optional & VDPA_OPT_VDEV_FEATURES)) {
			NEXT_ARG_FWD();
			err = vdpa_argv_u64_hex(vdpa, argc, argv,
						&opts->device_features);
			if (err)
				return err;

			NEXT_ARG_FWD();
			o_found |= VDPA_OPT_VDEV_FEATURES;
		} else {
			fprintf(stderr, "Unknown option \"%s\"\n", *argv);
			return -EINVAL;
		}
	}

	opts->present = o_found;

	return vdpa_args_finding_required_validate(o_required, o_found);
}

static int vdpa_argv_parse_put(struct nlmsghdr *nlh, struct vdpa *vdpa,
			       int argc, char **argv,
			       uint64_t o_required, uint64_t o_optional)
{
	int err;

	err = vdpa_argv_parse(vdpa, argc, argv, o_required, o_optional);
	if (err)
		return err;
	vdpa_opts_put(nlh, vdpa);
	return 0;
}

static void cmd_mgmtdev_help(void)
{
	fprintf(stderr, "Usage: vdpa mgmtdev show [ DEV ]\n");
}

static void pr_out_handle_start(struct vdpa *vdpa, struct nlattr **tb)
{
	const char *mdev_bus_name = NULL;
	const char *mdev_name;
	SPRINT_BUF(buf);

	mdev_name = mnl_attr_get_str(tb[VDPA_ATTR_MGMTDEV_DEV_NAME]);
	if (tb[VDPA_ATTR_MGMTDEV_BUS_NAME]) {
		mdev_bus_name = mnl_attr_get_str(tb[VDPA_ATTR_MGMTDEV_BUS_NAME]);
		sprintf(buf, "%s/%s", mdev_bus_name, mdev_name);
	} else {
		sprintf(buf, "%s", mdev_name);
	}

	if (vdpa->json_output)
		open_json_object(buf);
	else
		printf("%s: ", buf);
}

static void pr_out_handle_end(struct vdpa *vdpa)
{
	if (vdpa->json_output)
		close_json_object();
	else
		print_nl();
}

static void __pr_out_vdev_handle_start(struct vdpa *vdpa, const char *vdev_name)
{
	SPRINT_BUF(buf);

	sprintf(buf, "%s", vdev_name);
	if (vdpa->json_output)
		open_json_object(buf);
	else
		printf("%s: ", buf);
}

static void pr_out_vdev_handle_start(struct vdpa *vdpa, struct nlattr **tb)
{
	const char *vdev_name;

	vdev_name = mnl_attr_get_str(tb[VDPA_ATTR_DEV_NAME]);
	__pr_out_vdev_handle_start(vdpa, vdev_name);
}

static void pr_out_vdev_handle_end(struct vdpa *vdpa)
{
	if (vdpa->json_output)
		close_json_object();
	else
		print_nl();
}

static struct str_num_map class_map[] = {
	{ .str = "net", .num = VIRTIO_ID_NET },
	{ .str = "block", .num = VIRTIO_ID_BLOCK },
	{ .str = NULL, },
};

static const char *parse_class(int num)
{
	const char *class;

	class = str_map_lookup_uint(class_map, num);
	return class ? class : "< unknown class >";
}

static const char * const net_feature_strs[64] = {
	[VIRTIO_NET_F_CSUM] = "CSUM",
	[VIRTIO_NET_F_GUEST_CSUM] = "GUEST_CSUM",
	[VIRTIO_NET_F_CTRL_GUEST_OFFLOADS] = "CTRL_GUEST_OFFLOADS",
	[VIRTIO_NET_F_MTU] = "MTU",
	[VIRTIO_NET_F_MAC] = "MAC",
	[VIRTIO_NET_F_GUEST_TSO4] = "GUEST_TSO4",
	[VIRTIO_NET_F_GUEST_TSO6] = "GUEST_TSO6",
	[VIRTIO_NET_F_GUEST_ECN] = "GUEST_ECN",
	[VIRTIO_NET_F_GUEST_UFO] = "GUEST_UFO",
	[VIRTIO_NET_F_HOST_TSO4] = "HOST_TSO4",
	[VIRTIO_NET_F_HOST_TSO6] = "HOST_TSO6",
	[VIRTIO_NET_F_HOST_ECN] = "HOST_ECN",
	[VIRTIO_NET_F_HOST_UFO] = "HOST_UFO",
	[VIRTIO_NET_F_MRG_RXBUF] = "MRG_RXBUF",
	[VIRTIO_NET_F_STATUS] = "STATUS",
	[VIRTIO_NET_F_CTRL_VQ] = "CTRL_VQ",
	[VIRTIO_NET_F_CTRL_RX] = "CTRL_RX",
	[VIRTIO_NET_F_CTRL_VLAN] = "CTRL_VLAN",
	[VIRTIO_NET_F_CTRL_RX_EXTRA] = "CTRL_RX_EXTRA",
	[VIRTIO_NET_F_GUEST_ANNOUNCE] = "GUEST_ANNOUNCE",
	[VIRTIO_NET_F_MQ] = "MQ",
	[VIRTIO_F_NOTIFY_ON_EMPTY] = "NOTIFY_ON_EMPTY",
	[VIRTIO_NET_F_CTRL_MAC_ADDR] = "CTRL_MAC_ADDR",
	[VIRTIO_F_ANY_LAYOUT] = "ANY_LAYOUT",
	[VIRTIO_NET_F_RSC_EXT] = "RSC_EXT",
	[VIRTIO_NET_F_HASH_REPORT] = "HASH_REPORT",
	[VIRTIO_NET_F_RSS] = "RSS",
	[VIRTIO_NET_F_STANDBY] = "STANDBY",
	[VIRTIO_NET_F_SPEED_DUPLEX] = "SPEED_DUPLEX",
};

#define VIRTIO_F_IN_ORDER 35
#define VIRTIO_F_NOTIFICATION_DATA 38
#define VDPA_EXT_FEATURES_SZ (VIRTIO_TRANSPORT_F_END - \
			      VIRTIO_TRANSPORT_F_START + 1)

static const char * const ext_feature_strs[VDPA_EXT_FEATURES_SZ] = {
	[VIRTIO_RING_F_INDIRECT_DESC - VIRTIO_TRANSPORT_F_START] = "RING_INDIRECT_DESC",
	[VIRTIO_RING_F_EVENT_IDX - VIRTIO_TRANSPORT_F_START] = "RING_EVENT_IDX",
	[VIRTIO_F_VERSION_1 - VIRTIO_TRANSPORT_F_START] = "VERSION_1",
	[VIRTIO_F_ACCESS_PLATFORM - VIRTIO_TRANSPORT_F_START] = "ACCESS_PLATFORM",
	[VIRTIO_F_RING_PACKED - VIRTIO_TRANSPORT_F_START] = "RING_PACKED",
	[VIRTIO_F_IN_ORDER - VIRTIO_TRANSPORT_F_START] = "IN_ORDER",
	[VIRTIO_F_ORDER_PLATFORM - VIRTIO_TRANSPORT_F_START] = "ORDER_PLATFORM",
	[VIRTIO_F_SR_IOV - VIRTIO_TRANSPORT_F_START] = "SR_IOV",
	[VIRTIO_F_NOTIFICATION_DATA - VIRTIO_TRANSPORT_F_START] = "NOTIFICATION_DATA",
};

static const char * const *dev_to_feature_str[] = {
	[VIRTIO_ID_NET] = net_feature_strs,
};

#define NUM_FEATURE_BITS 64

static void print_features(struct vdpa *vdpa, uint64_t features, bool mgmtdevf,
			   uint16_t dev_id)
{
	const char * const *feature_strs = NULL;
	const char *s;
	int i;

	if (dev_id < ARRAY_SIZE(dev_to_feature_str))
		feature_strs = dev_to_feature_str[dev_id];

	if (mgmtdevf)
		pr_out_array_start(vdpa, "dev_features");
	else
		pr_out_array_start(vdpa, "negotiated_features");

	for (i = 0; i < NUM_FEATURE_BITS; i++) {
		if (!(features & (1ULL << i)))
			continue;

		if (i < VIRTIO_TRANSPORT_F_START || i > VIRTIO_TRANSPORT_F_END)
			s = feature_strs ? feature_strs[i] : NULL;
		else
			s = ext_feature_strs[i - VIRTIO_TRANSPORT_F_START];

		if (!s)
			print_uint(PRINT_ANY, NULL, " bit_%d", i);
		else
			print_string(PRINT_ANY, NULL, " %s", s);
	}

	pr_out_array_end(vdpa);
}

static void pr_out_mgmtdev_show(struct vdpa *vdpa, const struct nlmsghdr *nlh,
				struct nlattr **tb)
{
	uint64_t classes = 0;
	const char *class;
	unsigned int i;

	pr_out_handle_start(vdpa, tb);

	if (tb[VDPA_ATTR_MGMTDEV_SUPPORTED_CLASSES]) {
		classes = mnl_attr_get_u64(tb[VDPA_ATTR_MGMTDEV_SUPPORTED_CLASSES]);
		pr_out_array_start(vdpa, "supported_classes");

		for (i = 1; i < 64; i++) {
			if ((classes & (1ULL << i)) == 0)
				continue;

			class = parse_class(i);
			print_string(PRINT_ANY, NULL, " %s", class);
		}
		pr_out_array_end(vdpa);
	}

	if (tb[VDPA_ATTR_DEV_MGMTDEV_MAX_VQS]) {
		uint32_t num_vqs;

		print_nl();
		num_vqs = mnl_attr_get_u32(tb[VDPA_ATTR_DEV_MGMTDEV_MAX_VQS]);
		print_uint(PRINT_ANY, "max_supported_vqs", "  max_supported_vqs %d", num_vqs);
	}

	if (tb[VDPA_ATTR_DEV_SUPPORTED_FEATURES]) {
		uint64_t features;

		features  = mnl_attr_get_u64(tb[VDPA_ATTR_DEV_SUPPORTED_FEATURES]);
		if (classes & BIT(VIRTIO_ID_NET))
			print_features(vdpa, features, true, VIRTIO_ID_NET);
		else
			print_features(vdpa, features, true, 0);
	}

	pr_out_handle_end(vdpa);
}

static int cmd_mgmtdev_show_cb(const struct nlmsghdr *nlh, void *data)
{
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *tb[VDPA_ATTR_MAX + 1] = {};
	struct vdpa *vdpa = data;

	mnl_attr_parse(nlh, sizeof(*genl), attr_cb, tb);

	if (!tb[VDPA_ATTR_MGMTDEV_DEV_NAME])
		return MNL_CB_ERROR;

	pr_out_mgmtdev_show(vdpa, nlh, tb);

	return MNL_CB_OK;
}

static int cmd_mgmtdev_show(struct vdpa *vdpa, int argc, char **argv)
{
	uint16_t flags = NLM_F_REQUEST | NLM_F_ACK;
	struct nlmsghdr *nlh;
	int err;

	if (argc == 0)
		flags |= NLM_F_DUMP;

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_MGMTDEV_GET,
					  flags);
	if (argc > 0) {
		err = vdpa_argv_parse_put(nlh, vdpa, argc, argv,
					  VDPA_OPT_MGMTDEV_HANDLE, 0);
		if (err)
			return err;
	}

	pr_out_section_start(vdpa, "mgmtdev");
	err = mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, cmd_mgmtdev_show_cb, vdpa);
	pr_out_section_end(vdpa);
	return err;
}

static int cmd_mgmtdev(struct vdpa *vdpa, int argc, char **argv)
{
	if (!argc || matches(*argv, "help") == 0) {
		cmd_mgmtdev_help();
		return 0;
	} else if (matches(*argv, "show") == 0 ||
		   matches(*argv, "list") == 0) {
		return cmd_mgmtdev_show(vdpa, argc - 1, argv + 1);
	}
	fprintf(stderr, "Command \"%s\" not found\n", *argv);
	return -ENOENT;
}

static void cmd_dev_help(void)
{
	fprintf(stderr, "Usage: vdpa dev show [ DEV ]\n");
	fprintf(stderr, "       vdpa dev add name NAME mgmtdev MANAGEMENTDEV [ device_features DEVICE_FEATURES]\n");
	fprintf(stderr, "                                                    [ mac MACADDR ] [ mtu MTU ]\n");
	fprintf(stderr, "                                                    [ max_vqp MAX_VQ_PAIRS ]\n");
	fprintf(stderr, "       vdpa dev del DEV\n");
	fprintf(stderr, "Usage: vdpa dev config COMMAND [ OPTIONS ]\n");
	fprintf(stderr, "Usage: vdpa dev vstats COMMAND\n");
}

static const char *device_type_name(uint32_t type)
{
	switch (type) {
	case 0x1: return "network";
	case 0x2: return "block";
	default: return "<unknown type>";
	}
}

static void pr_out_dev(struct vdpa *vdpa, struct nlattr **tb)
{
	const char *mdev_name = mnl_attr_get_str(tb[VDPA_ATTR_MGMTDEV_DEV_NAME]);
	uint32_t device_id = mnl_attr_get_u32(tb[VDPA_ATTR_DEV_ID]);
	const char *mdev_bus_name = NULL;
	char mgmtdev_buf[128];

	if (tb[VDPA_ATTR_MGMTDEV_BUS_NAME])
		mdev_bus_name = mnl_attr_get_str(tb[VDPA_ATTR_MGMTDEV_BUS_NAME]);

	if (mdev_bus_name)
		sprintf(mgmtdev_buf, "%s/%s", mdev_bus_name, mdev_name);
	else
		sprintf(mgmtdev_buf, "%s", mdev_name);
	pr_out_vdev_handle_start(vdpa, tb);
	print_string(PRINT_ANY, "type", "type %s", device_type_name(device_id));
	print_string(PRINT_ANY, "mgmtdev", " mgmtdev %s", mgmtdev_buf);

	if (tb[VDPA_ATTR_DEV_VENDOR_ID])
		print_uint(PRINT_ANY, "vendor_id", " vendor_id %u",
			   mnl_attr_get_u32(tb[VDPA_ATTR_DEV_VENDOR_ID]));
	if (tb[VDPA_ATTR_DEV_MAX_VQS])
		print_uint(PRINT_ANY, "max_vqs", " max_vqs %u",
			   mnl_attr_get_u32(tb[VDPA_ATTR_DEV_MAX_VQS]));
	if (tb[VDPA_ATTR_DEV_MAX_VQ_SIZE])
		print_uint(PRINT_ANY, "max_vq_size", " max_vq_size %u",
			   mnl_attr_get_u16(tb[VDPA_ATTR_DEV_MAX_VQ_SIZE]));
	pr_out_vdev_handle_end(vdpa);
}

static int cmd_dev_show_cb(const struct nlmsghdr *nlh, void *data)
{
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *tb[VDPA_ATTR_MAX + 1] = {};
	struct vdpa *vdpa = data;

	mnl_attr_parse(nlh, sizeof(*genl), attr_cb, tb);
	if (!tb[VDPA_ATTR_MGMTDEV_DEV_NAME] ||
	    !tb[VDPA_ATTR_DEV_NAME] || !tb[VDPA_ATTR_DEV_ID])
		return MNL_CB_ERROR;
	pr_out_dev(vdpa, tb);
	return MNL_CB_OK;
}

static int cmd_dev_show(struct vdpa *vdpa, int argc, char **argv)
{
	uint16_t flags = NLM_F_REQUEST | NLM_F_ACK;
	struct nlmsghdr *nlh;
	int err;

	if (argc <= 0)
		flags |= NLM_F_DUMP;

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_DEV_GET, flags);
	if (argc > 0) {
		err = vdpa_argv_parse_put(nlh, vdpa, argc, argv,
					  VDPA_OPT_VDEV_HANDLE, 0);
		if (err)
			return err;
	}

	pr_out_section_start(vdpa, "dev");
	err = mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, cmd_dev_show_cb, vdpa);
	pr_out_section_end(vdpa);
	return err;
}

static int cmd_dev_add(struct vdpa *vdpa, int argc, char **argv)
{
	struct nlmsghdr *nlh;
	int err;

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_DEV_NEW,
					  NLM_F_REQUEST | NLM_F_ACK);
	err = vdpa_argv_parse_put(nlh, vdpa, argc, argv,
				  VDPA_OPT_VDEV_MGMTDEV_HANDLE | VDPA_OPT_VDEV_NAME,
				  VDPA_OPT_VDEV_MAC | VDPA_OPT_VDEV_MTU |
				  VDPA_OPT_MAX_VQP | VDPA_OPT_VDEV_FEATURES);
	if (err)
		return err;

	return mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, NULL, NULL);
}

static int cmd_dev_del(struct vdpa *vdpa,  int argc, char **argv)
{
	struct nlmsghdr *nlh;
	int err;

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_DEV_DEL,
					  NLM_F_REQUEST | NLM_F_ACK);
	err = vdpa_argv_parse_put(nlh, vdpa, argc, argv, VDPA_OPT_VDEV_HANDLE,
				  0);
	if (err)
		return err;

	return mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, NULL, NULL);
}

static int cmd_dev_set(struct vdpa *vdpa, int argc, char **argv)
{
	struct nlmsghdr *nlh;
	int err;

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_DEV_ATTR_SET,
					  NLM_F_REQUEST | NLM_F_ACK);
	err = vdpa_argv_parse_put(nlh, vdpa, argc, argv,
				  VDPA_OPT_VDEV_NAME,
				  VDPA_OPT_VDEV_MAC);
	if (err)
		return err;

	return mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, NULL, NULL);
}

static void pr_out_dev_net_config(struct vdpa *vdpa, struct nlattr **tb)
{
	SPRINT_BUF(macaddr);
	uint64_t val_u64;
	uint16_t val_u16;

	if (tb[VDPA_ATTR_DEV_NET_CFG_MACADDR]) {
		const unsigned char *data;
		uint16_t len;

		len = mnl_attr_get_payload_len(tb[VDPA_ATTR_DEV_NET_CFG_MACADDR]);
		data = mnl_attr_get_payload(tb[VDPA_ATTR_DEV_NET_CFG_MACADDR]);

		print_string(PRINT_ANY, "mac", "mac %s ",
			     ll_addr_n2a(data, len, 0, macaddr, sizeof(macaddr)));
	}
	if (tb[VDPA_ATTR_DEV_NET_STATUS]) {
		val_u16 = mnl_attr_get_u16(tb[VDPA_ATTR_DEV_NET_STATUS]);
		print_string(PRINT_ANY, "link ", "link %s ",
			     (val_u16 & VIRTIO_NET_S_LINK_UP) ? "up" : "down");
		print_bool(PRINT_ANY, "link_announce ", "link_announce %s ",
			     (val_u16 & VIRTIO_NET_S_ANNOUNCE) ? true : false);
	}
	if (tb[VDPA_ATTR_DEV_NET_CFG_MAX_VQP]) {
		val_u16 = mnl_attr_get_u16(tb[VDPA_ATTR_DEV_NET_CFG_MAX_VQP]);
		print_uint(PRINT_ANY, "max_vq_pairs", "max_vq_pairs %d ",
			     val_u16);
	}
	if (tb[VDPA_ATTR_DEV_NET_CFG_MTU]) {
		val_u16 = mnl_attr_get_u16(tb[VDPA_ATTR_DEV_NET_CFG_MTU]);
		print_uint(PRINT_ANY, "mtu", "mtu %d ", val_u16);
	}
	if (tb[VDPA_ATTR_DEV_NEGOTIATED_FEATURES]) {
		uint16_t dev_id = 0;

		if (tb[VDPA_ATTR_DEV_ID])
			dev_id = mnl_attr_get_u32(tb[VDPA_ATTR_DEV_ID]);

		val_u64 = mnl_attr_get_u64(tb[VDPA_ATTR_DEV_NEGOTIATED_FEATURES]);
		print_features(vdpa, val_u64, false, dev_id);
	}
}

static void pr_out_dev_config(struct vdpa *vdpa, struct nlattr **tb)
{
	uint32_t device_id = mnl_attr_get_u32(tb[VDPA_ATTR_DEV_ID]);

	pr_out_vdev_handle_start(vdpa, tb);
	switch (device_id) {
	case VIRTIO_ID_NET:
		pr_out_dev_net_config(vdpa, tb);
		break;
	default:
		break;
	}
	pr_out_vdev_handle_end(vdpa);
}

static int cmd_dev_config_show_cb(const struct nlmsghdr *nlh, void *data)
{
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *tb[VDPA_ATTR_MAX + 1] = {};
	struct vdpa *vdpa = data;

	mnl_attr_parse(nlh, sizeof(*genl), attr_cb, tb);
	if (!tb[VDPA_ATTR_DEV_NAME] || !tb[VDPA_ATTR_DEV_ID])
		return MNL_CB_ERROR;
	pr_out_dev_config(vdpa, tb);
	return MNL_CB_OK;
}

static int cmd_dev_config_show(struct vdpa *vdpa, int argc, char **argv)
{
	uint16_t flags = NLM_F_REQUEST | NLM_F_ACK;
	struct nlmsghdr *nlh;
	int err;

	if (argc <= 0)
		flags |= NLM_F_DUMP;

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_DEV_CONFIG_GET,
					  flags);
	if (argc > 0) {
		err = vdpa_argv_parse_put(nlh, vdpa, argc, argv,
					  VDPA_OPT_VDEV_HANDLE, 0);
		if (err)
			return err;
	}

	pr_out_section_start(vdpa, "config");
	err = mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, cmd_dev_config_show_cb, vdpa);
	pr_out_section_end(vdpa);
	return err;
}

static void cmd_dev_config_help(void)
{
	fprintf(stderr, "Usage: vdpa dev config show [ DEV ]\n");
}

static int cmd_dev_config(struct vdpa *vdpa, int argc, char **argv)
{
	if (!argc)
		return cmd_dev_config_show(vdpa, argc - 1, argv + 1);

	if (matches(*argv, "help") == 0) {
		cmd_dev_config_help();
		return 0;
	} else if (matches(*argv, "show") == 0) {
		return cmd_dev_config_show(vdpa, argc - 1, argv + 1);
	}
	fprintf(stderr, "Command \"%s\" not found\n", *argv);
	return -ENOENT;
}

#define MAX_KEY_LEN 200
/* 5 bytes for format */
#define MAX_FMT_LEN (MAX_KEY_LEN + 5 + 1)

static void print_queue_type(struct nlattr *attr, uint16_t max_vqp, uint64_t features)
{
	bool is_ctrl = false;
	uint16_t qidx = 0;

	qidx = mnl_attr_get_u16(attr);
	is_ctrl = features & BIT(VIRTIO_NET_F_CTRL_VQ) && qidx == 2 * max_vqp;
	if (!is_ctrl) {
		if (qidx & 1)
			print_string(PRINT_ANY, "queue_type", "queue_type %s ",
				     "tx");
		else
			print_string(PRINT_ANY, "queue_type", "queue_type %s ",
				     "rx");
	} else {
		print_string(PRINT_ANY, "queue_type", "queue_type %s ",
			     "control_vq");
	}
}

static void pr_out_dev_net_vstats(const struct nlmsghdr *nlh)
{
	const char *name = NULL;
	uint64_t features = 0;
	char fmt[MAX_FMT_LEN];
	uint16_t max_vqp = 0;
	struct nlattr *attr;
	uint64_t v64;

	mnl_attr_for_each(attr, nlh, sizeof(struct genlmsghdr)) {
		switch (attr->nla_type) {
		case VDPA_ATTR_DEV_NET_CFG_MAX_VQP:
			max_vqp = mnl_attr_get_u16(attr);
			break;
		case VDPA_ATTR_DEV_NEGOTIATED_FEATURES:
			features = mnl_attr_get_u64(attr);
			break;
		case VDPA_ATTR_DEV_QUEUE_INDEX:
			print_queue_type(attr, max_vqp, features);
			break;
		case VDPA_ATTR_DEV_VENDOR_ATTR_NAME:
			name = mnl_attr_get_str(attr);
			if (strlen(name) > MAX_KEY_LEN)
				return;

			strcpy(fmt, name);
			strcat(fmt, " %lu ");
			break;
		case VDPA_ATTR_DEV_VENDOR_ATTR_VALUE:
			v64 = mnl_attr_get_u64(attr);
			print_u64(PRINT_ANY, name, fmt, v64);
			break;
		}
	}
}

static void pr_out_dev_vstats(struct vdpa *vdpa, struct nlattr **tb, const struct nlmsghdr *nlh)
{
	uint32_t device_id = mnl_attr_get_u32(tb[VDPA_ATTR_DEV_ID]);

	pr_out_vdev_handle_start(vdpa, tb);
	switch (device_id) {
	case VIRTIO_ID_NET:
		pr_out_dev_net_vstats(nlh);
		break;
	default:
		break;
	}
	pr_out_vdev_handle_end(vdpa);
}

static int cmd_dev_vstats_show_cb(const struct nlmsghdr *nlh, void *data)
{
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlh);
	struct nlattr *tb[VDPA_ATTR_MAX + 1] = {};
	struct vdpa *vdpa = data;

	mnl_attr_parse(nlh, sizeof(*genl), attr_cb, tb);
	if (!tb[VDPA_ATTR_DEV_NAME] || !tb[VDPA_ATTR_DEV_ID])
		return MNL_CB_ERROR;
	pr_out_dev_vstats(vdpa, tb, nlh);
	return MNL_CB_OK;
}

static void cmd_dev_vstats_help(void)
{
	fprintf(stderr, "Usage: vdpa dev vstats show DEV [qidx QUEUE_INDEX]\n");
}

static int cmd_dev_vstats_show(struct vdpa *vdpa, int argc, char **argv)
{
	uint16_t flags = NLM_F_REQUEST | NLM_F_ACK;
	struct nlmsghdr *nlh;
	int err;

	if (argc != 1 && argc != 3) {
		cmd_dev_vstats_help();
		return -EINVAL;
	}

	nlh = mnlu_gen_socket_cmd_prepare(&vdpa->nlg, VDPA_CMD_DEV_VSTATS_GET,
					  flags);

	err = vdpa_argv_parse_put(nlh, vdpa, argc, argv,
				  VDPA_OPT_VDEV_HANDLE, VDPA_OPT_QUEUE_INDEX);
	if (err)
		return err;

	pr_out_section_start(vdpa, "vstats");
	err = mnlu_gen_socket_sndrcv(&vdpa->nlg, nlh, cmd_dev_vstats_show_cb, vdpa);
	pr_out_section_end(vdpa);
	return err;
}

static int cmd_dev_vstats(struct vdpa *vdpa, int argc, char **argv)
{
	if (argc < 1) {
		cmd_dev_vstats_help();
		return -EINVAL;
	}

	if (!strcmp(*argv, "help")) {
		cmd_dev_vstats_help();
		return 0;
	} else if (!strcmp(*argv, "show")) {
		return cmd_dev_vstats_show(vdpa, argc - 1, argv + 1);
	}
	fprintf(stderr, "Command \"%s\" not found\n", *argv);
	return -ENOENT;
}

static int cmd_dev(struct vdpa *vdpa, int argc, char **argv)
{
	if (!argc)
		return cmd_dev_show(vdpa, argc - 1, argv + 1);

	if (matches(*argv, "help") == 0) {
		cmd_dev_help();
		return 0;
	} else if (matches(*argv, "show") == 0 ||
		   matches(*argv, "list") == 0) {
		return cmd_dev_show(vdpa, argc - 1, argv + 1);
	} else if (matches(*argv, "add") == 0) {
		return cmd_dev_add(vdpa, argc - 1, argv + 1);
	} else if (matches(*argv, "del") == 0) {
		return cmd_dev_del(vdpa, argc - 1, argv + 1);
	} else if (matches(*argv, "config") == 0) {
		return cmd_dev_config(vdpa, argc - 1, argv + 1);
	} else if (!strcmp(*argv, "vstats")) {
		return cmd_dev_vstats(vdpa, argc - 1, argv + 1);
	} else if (!strcmp(*argv, "set")) {
		return cmd_dev_set(vdpa, argc - 1, argv + 1);
	}
	fprintf(stderr, "Command \"%s\" not found\n", *argv);
	return -ENOENT;
}

static void help(void)
{
	fprintf(stderr,
		"Usage: vdpa [ OPTIONS ] OBJECT { COMMAND | help }\n"
		"where  OBJECT := { mgmtdev | dev }\n"
		"       OPTIONS := { -V[ersion] | -n[o-nice-names] | -j[son] | -p[retty] }\n");
}

static int vdpa_cmd(struct vdpa *vdpa, int argc, char **argv)
{
	if (!argc || matches(*argv, "help") == 0) {
		help();
		return 0;
	} else if (matches(*argv, "mgmtdev") == 0) {
		return cmd_mgmtdev(vdpa, argc - 1, argv + 1);
	} else if (matches(*argv, "dev") == 0) {
		return cmd_dev(vdpa, argc - 1, argv + 1);
	}
	fprintf(stderr, "Object \"%s\" not found\n", *argv);
	return -ENOENT;
}

static int vdpa_init(struct vdpa *vdpa)
{
	int err;

	err = mnlu_gen_socket_open(&vdpa->nlg, VDPA_GENL_NAME,
				   VDPA_GENL_VERSION);
	if (err) {
		fprintf(stderr, "Failed to connect to vdpa Netlink\n");
		return -errno;
	}
	new_json_obj_plain(vdpa->json_output);
	return 0;
}

static void vdpa_fini(struct vdpa *vdpa)
{
	delete_json_obj_plain();
	mnlu_gen_socket_close(&vdpa->nlg);
}

static struct vdpa *vdpa_alloc(void)
{
	struct vdpa *vdpa = calloc(1, sizeof(struct vdpa));

	if (!vdpa)
		return NULL;

	vdpa->indent = alloc_indent_mem();
	if (!vdpa->indent)
		goto indent_err;

	return vdpa;

indent_err:
	free(vdpa);
	return NULL;
}

static void vdpa_free(struct vdpa *vdpa)
{
	free_indent_mem(vdpa->indent);
	free(vdpa);
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "Version",		no_argument,	NULL, 'V' },
		{ "json",		no_argument,	NULL, 'j' },
		{ "pretty",		no_argument,	NULL, 'p' },
		{ "help",		no_argument,	NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct vdpa *vdpa;
	int opt;
	int err;
	int ret;

	vdpa = vdpa_alloc();
	if (!vdpa) {
		fprintf(stderr, "Failed to allocate memory for vdpa\n");
		return EXIT_FAILURE;
	}

	while ((opt = getopt_long(argc, argv, "Vjpsh", long_options, NULL)) >= 0) {
		switch (opt) {
		case 'V':
			printf("vdpa utility, iproute2-%s\n", version);
			ret = EXIT_SUCCESS;
			goto vdpa_free;
		case 'j':
			vdpa->json_output = true;
			break;
		case 'p':
			pretty = true;
			break;
		case 'h':
			help();
			ret = EXIT_SUCCESS;
			goto vdpa_free;
		default:
			fprintf(stderr, "Unknown option.\n");
			help();
			ret = EXIT_FAILURE;
			goto vdpa_free;
		}
	}

	argc -= optind;
	argv += optind;

	err = vdpa_init(vdpa);
	if (err) {
		ret = EXIT_FAILURE;
		goto vdpa_free;
	}

	err = vdpa_cmd(vdpa, argc, argv);
	if (err) {
		ret = EXIT_FAILURE;
		goto vdpa_fini;
	}

	ret = EXIT_SUCCESS;

vdpa_fini:
	vdpa_fini(vdpa);
vdpa_free:
	vdpa_free(vdpa);
	return ret;
}
