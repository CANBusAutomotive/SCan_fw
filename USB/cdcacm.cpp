#include "usbd.h"
#include "cdc.h"
#include "cdcacm.h"
#include "Buffer.h"
#include "Pins.h"
#include "stm32.h"
#include <algorithm>
#include <cstring>


static usbd_device *usbd_dev;


extern "C" void OTG_FS_IRQHandler()
{	usbd_poll(usbd_dev);	}


static const struct usb_device_descriptor dev = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_CDC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x0483,
	.idProduct = 0x5740,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

// This notification endpoint isn't implemented. According to CDC spec its
// optional, but its absence causes a NULL pointer dereference in Linux
// cdc_acm driver.
static const struct usb_endpoint_descriptor comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = Usb::epCdcNotify,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 16,
	.bInterval = 255,
	.extra = nullptr,
	.extralen = 0
}};

static const struct usb_endpoint_descriptor data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = Usb::epOutCdc,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
	.extra = nullptr,
	.extralen = 0
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = Usb::epInCdc,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
	.extra = nullptr,
	.extralen = 0
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 0,
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = 0,
		.bSubordinateInterface0 = 1,
	 },
};

static const struct usb_interface_descriptor comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 0,

	.endpoint = comm_endp,

	.extra = &cdcacm_functional_descriptors,
	.extralen = sizeof(cdcacm_functional_descriptors),
}};

static const struct usb_interface_descriptor data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = data_endp,
	.extra = nullptr,
	.extralen = 0
}};

static const struct usb_interface ifaces[] = {{
	.cur_altsetting = 0,
	.num_altsetting = 1,
	.iface_assoc = nullptr,
	.altsetting = comm_iface,
}, {
	.cur_altsetting = 0,
	.num_altsetting = 1,
	.iface_assoc = nullptr,
	.altsetting = data_iface,
}};

static const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 0x32,

	.interface = ifaces,
};

static const char * const usb_strings[] = {
	"Black Sphere Technologies",
	"CDC-ACM Demo",
	"DEMO",
};

// Buffer to be used for control requests.
static uint8_t usbd_control_buffer[128];


// current line coding (only for GET_LINE_CODING support)
static usb_cdc_line_coding line_coding;

static enum usbd_request_return_codes cdcacm_control_request(usbd_device *usbd_dev, struct usb_setup_data *req, uint8_t **buf,
		uint16_t *len, void (**complete)(usbd_device *usbd_dev, struct usb_setup_data *req))
{
	(void)complete;
	(void)buf;
	(void)usbd_dev;

	switch (req->bRequest)
	{
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE:
	{
		// This Linux cdc_acm driver requires this to be implemented
		// even though it's optional in the CDC spec, and we don't
		// advertise it in the ACM functional descriptor.
		struct notify_serial_state : usb_cdc_notification {
			uint16_t value;
		} __attribute__((packed)) notif;
		static_assert (sizeof(notif) == 10, "incorrect USB_CDC_NOTIFY_SERIAL_STATE size");

		// We echo signals back to host as notification.
		notif.bmRequestType = 0xA1;
		notif.bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
		notif.wValue = 0;
		notif.wIndex = 0;
		notif.wLength = 2;
		notif.value = req->wValue & 3;
		usbd_ep_write_packet(usbd_dev, Usb::epCdcNotify, &notif, 10);
		return USBD_REQ_HANDLED;
	}

	case USB_CDC_REQ_SET_LINE_CODING:
		if (*len < sizeof(usb_cdc_line_coding))
			return USBD_REQ_NOTSUPP;
		memcpy(&line_coding, *buf, sizeof(line_coding));
		return USBD_REQ_HANDLED;

	case USB_CDC_REQ_GET_LINE_CODING:
		*buf = (uint8_t*)&line_coding;
		*len = sizeof(line_coding);
		return USBD_REQ_HANDLED;

	}
	return USBD_REQ_NOTSUPP;
}



static CircularBuffer<char, 1024> txBuf;
static CircularBuffer<char, 256> rxBuf;



static void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
	(void)ep;

	char buf[64], *ptr = buf;
	int len = usbd_ep_read_packet(usbd_dev, Usb::epOutCdc, buf, sizeof(buf));
	while (len--)
		rxBuf.Put(*ptr++);
}

static void cdcacm_data_tx_cb(usbd_device *usbd_dev, uint8_t ep)
{
	(void)ep;
	static char buf[64];
	static uint32_t buf_ptr = 0;
//	static bool mutex = false;
	static bool need_zlp = false;

//	if (mutex) return;
//	mutex = true;
	NVIC_DisableIRQ(OTG_FS_IRQn);

	for ( ; buf_ptr < sizeof(buf) && txBuf.Avail(); ++buf_ptr)
		buf[buf_ptr] = txBuf.Get();

	need_zlp = (buf_ptr == sizeof(buf));		// need to send empty packet

	if (buf_ptr > 0 || need_zlp)
	{
		if (usbd_ep_write_packet(usbd_dev, Usb::epInCdc, buf, buf_ptr))
		{
			// write ok
			if (buf_ptr == 0)
				need_zlp = false;
			buf_ptr = 0;
		}
	}
	NVIC_EnableIRQ(OTG_FS_IRQn);
//	mutex = false;
}


static void cdcacm_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
	(void)wValue;

	usbd_ep_setup(usbd_dev, Usb::epOutCdc, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_data_rx_cb);
	usbd_ep_setup(usbd_dev, Usb::epInCdc, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_data_tx_cb);
	usbd_ep_setup(usbd_dev, Usb::epCdcNotify, USB_ENDPOINT_ATTR_INTERRUPT, 16, nullptr);

	usbd_register_control_callback(
				usbd_dev,
				USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
				USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
				cdcacm_control_request);
}


void Usb::init()
{
	PinUsbDP::Mode(ALT_OUTPUT);
	PinUsbDM::Mode(ALT_OUTPUT);
	PinUsbVbus::Mode(OUTPUT); PinUsbVbus::On();

	usbd_dev = usbd_init(&stm32f107_usb_driver, &dev, &config, usb_strings, 3, usbd_control_buffer, sizeof(usbd_control_buffer));
	usbd_register_set_config_callback(usbd_dev, cdcacm_set_config);

	NVIC_EnableIRQ (OTG_FS_IRQn);

}

static bool connected;
bool Usb::checkConnect()
{
	bool vbus = PinUsbConnect::Signalled();
	if (vbus && !connected)
	{
		connected = true;
		usbd_disconnect(usbd_dev, false);
	}
	else if (!vbus && connected)
	{
		connected = false;
		usbd_disconnect(usbd_dev, true);
	}
	return connected;
}


bool Usb::send(const void * data, uint32_t dataLen)
{
	auto p = (const char *) data;
	while (dataLen--)
		txBuf.Put(*p++);

	if (usbd_dev && connected)
	{
		cdcacm_data_tx_cb (usbd_dev, Usb::epInCdc);
		return true;
	}
	return false;
}

uint32_t Usb::receive(const void *data, uint32_t bufLen)
{
	uint32_t dataLen = rxBuf.Avail();
	dataLen = std::min(dataLen, bufLen);

	auto p = (char *) data;
	for (auto len = dataLen; len; len--)
		*p++ = rxBuf.Get();

	return dataLen;
}
