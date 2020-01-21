// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Samsung Electronics
 *               Igor Kotrasinski <i.kotrasinsk@samsung.com>
 *               Krzysztof Opasiak <k.opasiak@samsung.com>
 * Modified By
 * 	2020 dice14u - striped console and made it return a result for remote list
 */

#include <sys/types.h>
#include <libudev.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h>
#include <netdb.h>
#include <unistd.h>

#include <dirent.h>

#include <linux/usb/ch9.h>

#include <usbip/usbip_common.h>

#include "usbip_network.h"
#include "utils.h"

#define MAX_INTERFACES 10

struct usbip_external_list {
    char* product_name;
    char* path;
    char* busid;
    char* interfaces[MAX_INTERFACES];
    int num_interfaces;
    struct usbip_external_list* next;
};

void usbip_external_list_free(struct usbip_external_list* device) {
    int interface_count = lowest(device->num_interfaces, MAX_INTERFACES);
    for(int i = 0; i < interface_count; i++) {
        free(device->interfaces[i]);
    }
    free(device->product_name);
    free(device->busid);
    free(device->path);
    free(device);
};

static struct usbip_external_list* get_exported_devices(char *host, int sockfd)
{
    struct usbip_external_list* last = NULL;
    struct usbip_external_list* current = NULL;
    struct usbip_external_list* first = NULL;
	char product_name[100];
	char class_name[100];
	struct op_devlist_reply reply;
	uint16_t code = OP_REP_DEVLIST;
	struct usbip_usb_device udev;
	struct usbip_usb_interface uintf;
	unsigned int i;
	int rc, j;
	int status;

	rc = usbip_net_send_op_common(sockfd, OP_REQ_DEVLIST, 0);
	if (rc < 0) {
		dbg("usbip_net_send_op_common failed");
        return NULL;
	}

	rc = usbip_net_recv_op_common(sockfd, &code, &status);
	if (rc < 0) {
		err("Exported Device List Request failed - %s\n",
		    usbip_op_common_status_string(status));
        return NULL;
	}

	memset(&reply, 0, sizeof(reply));
	rc = usbip_net_recv(sockfd, &reply, sizeof(reply));
	if (rc < 0) {
		dbg("usbip_net_recv_op_devlist failed");
        return NULL;
	}
	PACK_OP_DEVLIST_REPLY(0, &reply);
	dbg("exportable devices: %d\n", reply.ndev);

	if (reply.ndev == 0) {
		info("no exportable devices found on %s", host);
        return NULL;
	}

	for (i = 0; i < reply.ndev; i++) {
		memset(&udev, 0, sizeof(udev));
		rc = usbip_net_recv(sockfd, &udev, sizeof(udev));
		if (rc < 0) {
			dbg("usbip_net_recv failed: usbip_usb_device[%d]", i);
            return NULL;
		}
		usbip_net_pack_usb_device(0, &udev);

		usbip_names_get_product(product_name, sizeof(product_name),
					udev.idVendor, udev.idProduct);

        current = (struct usbip_external_list*)malloc(sizeof(struct usbip_external_list));
        current->next = NULL;
        current->path = strdup(udev.path);
        current->busid = strdup(udev.busid);
        current->product_name = strdup(product_name);
        current->num_interfaces = lowest(udev.bNumInterfaces, MAX_INTERFACES);

        for (j = 0; j < udev.bNumInterfaces && j < 10; j++) {
            rc = usbip_net_recv(sockfd, &uintf, sizeof(uintf));
            if (rc < 0) {
                err("usbip_net_recv failed: usbip_usb_intf[%d]",
                        j);

                return NULL;
            }
            usbip_net_pack_usb_interface(0, &uintf);

            usbip_names_get_class(class_name, sizeof(class_name),
                    uintf.bInterfaceClass,
                    uintf.bInterfaceSubClass,
                    uintf.bInterfaceProtocol);
            current->interfaces[j] = strdup(class_name);
        }
        if (last != NULL) {
            last->next = current;
        }
        if (first == NULL) {
            first = current;
        }
        last = current;
	}

    return first;
}

struct usbip_external_list* usbip_external_devices(char *host)
{
    struct usbip_external_list* rc;
	int sockfd;

	sockfd = usbip_net_tcp_connect(host, usbip_port_string);
	if (sockfd < 0) {
		err("could not connect to %s:%s: %s", host,
		    usbip_port_string, gai_strerror(sockfd));
        return NULL;
	}
	dbg("connected to %s:%s", host, usbip_port_string);

	rc = get_exported_devices(host, sockfd);
	if (rc < 0) {
		err("failed to get device list from %s", host);
        return NULL;
	}

	close(sockfd);

    return rc;
}

static void print_device(const char *busid, const char *vendor,
			 const char *product, bool parsable)
{
	if (parsable)
		printf("busid=%s#usbid=%.4s:%.4s#", busid, vendor, product);
	else
		printf(" - busid %s (%.4s:%.4s)\n", busid, vendor, product);
}

static void print_product_name(char *product_name, bool parsable)
{
	if (!parsable)
		printf("   %s\n", product_name);
}

int list_devices(bool parsable)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev;
	const char *path;
	const char *idVendor;
	const char *idProduct;
	const char *bConfValue;
	const char *bNumIntfs;
	const char *busid;
	char product_name[128];
	int ret = -1;
	const char *devpath;

	/* Create libudev context. */
	udev = udev_new();

	/* Create libudev device enumeration. */
	enumerate = udev_enumerate_new(udev);

	/* Take only USB devices that are not hubs and do not have
	 * the bInterfaceNumber attribute, i.e. are not interfaces.
	 */
	udev_enumerate_add_match_subsystem(enumerate, "usb");
	udev_enumerate_add_nomatch_sysattr(enumerate, "bDeviceClass", "09");
	udev_enumerate_add_nomatch_sysattr(enumerate, "bInterfaceNumber", NULL);
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);

	/* Show information about each device. */
	udev_list_entry_foreach(dev_list_entry, devices) {
		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, path);

		/* Ignore devices attached to vhci_hcd */
		devpath = udev_device_get_devpath(dev);
		if (strstr(devpath, USBIP_VHCI_DRV_NAME)) {
			dbg("Skip the device %s already attached to %s\n",
			    devpath, USBIP_VHCI_DRV_NAME);
			continue;
		}

		/* Get device information. */
		idVendor = udev_device_get_sysattr_value(dev, "idVendor");
		idProduct = udev_device_get_sysattr_value(dev, "idProduct");
		bConfValue = udev_device_get_sysattr_value(dev,
				"bConfigurationValue");
		bNumIntfs = udev_device_get_sysattr_value(dev,
				"bNumInterfaces");
		busid = udev_device_get_sysname(dev);
		if (!idVendor || !idProduct || !bConfValue || !bNumIntfs) {
			err("problem getting device attributes: %s",
			    strerror(errno));
			goto err_out;
		}

		/* Get product name. */
		usbip_names_get_product(product_name, sizeof(product_name),
					strtol(idVendor, NULL, 16),
					strtol(idProduct, NULL, 16));

		/* Print information. */
		print_device(busid, idVendor, idProduct, parsable);
		print_product_name(product_name, parsable);

		printf("\n");

		udev_device_unref(dev);
	}

	ret = 0;

err_out:
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	return ret;
}

int list_gadget_devices(bool parsable)
{
	int ret = -1;
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev;
	const char *path;
	const char *driver;

	const struct usb_device_descriptor *d_desc;
	const char *descriptors;
	char product_name[128];

	uint16_t idVendor;
	char idVendor_buf[8];
	uint16_t idProduct;
	char idProduct_buf[8];
	const char *busid;

	udev = udev_new();
	enumerate = udev_enumerate_new(udev);

	udev_enumerate_add_match_subsystem(enumerate, "platform");

	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);

	udev_list_entry_foreach(dev_list_entry, devices) {
		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, path);

		driver = udev_device_get_driver(dev);
		/* We only have mechanism to enumerate gadgets bound to vudc */
		if (driver == NULL || strcmp(driver, USBIP_DEVICE_DRV_NAME))
			continue;

		/* Get device information. */
		descriptors = udev_device_get_sysattr_value(dev,
				VUDC_DEVICE_DESCR_FILE);

		if (!descriptors) {
			err("problem getting device attributes: %s",
			    strerror(errno));
			goto err_out;
		}

		d_desc = (const struct usb_device_descriptor *) descriptors;

		idVendor = le16toh(d_desc->idVendor);
		sprintf(idVendor_buf, "0x%4x", idVendor);
		idProduct = le16toh(d_desc->idProduct);
		sprintf(idProduct_buf, "0x%4x", idVendor);
		busid = udev_device_get_sysname(dev);

		/* Get product name. */
		usbip_names_get_product(product_name, sizeof(product_name),
					le16toh(idVendor),
					le16toh(idProduct));

		/* Print information. */
		print_device(busid, idVendor_buf, idProduct_buf, parsable);
		print_product_name(product_name, parsable);

		printf("\n");

		udev_device_unref(dev);
	}
	ret = 0;

err_out:
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	return ret;
}
