/*
 * Copyright (c) 2016, CESAR.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 *
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <glib.h>
#include <json-c/json.h>

#include "phy_driver_private.h"
#include "nrf24l01_io.h"
#include "manager.h"

static struct phy_driver *driver = &nrf24l01;

static int nrf24_fd = -1;
static guint listen_idle = 0;

static void watch_rxtx_destroy(gpointer user_data)
{
	/* Close pipe & free resources */
}

static gboolean watch_rxtx_cb(gpointer user_data)
{
	int cli_fd = GPOINTER_TO_INT(user_data);

	/* FIXME: recv & send & detect disconnection */
	fprintf(stdout, "client id: %d\n", cli_fd);

	return TRUE;
}

static void watch_accept_destroy(gpointer user_data)
{
}

static gboolean watch_accept_cb(gpointer user_data)
{
	int cli_fd;

	cli_fd = driver->accept(nrf24_fd);
	if (cli_fd < 0)
		return TRUE;

	/* FIXME: remove client sources before exiting */
	g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
			watch_rxtx_cb, GINT_TO_POINTER(cli_fd),
			watch_rxtx_destroy);

	return TRUE;
}

static int radio_init(const char *spi)
{
	int retval, fd;

	/* FIXME: pass 'spi' to driver */
	retval= driver->probe();
	if (retval < 0)
		return retval;

	/* nRF24 radio initialization */
	fd = driver->open(NULL);
	if (fd < 0) {
		driver->remove();
		return fd;
	}

	/* Blocking operation: waiting for available nRF24L01 channel */
	retval = driver->listen(fd);
	if (retval < 0) {
		driver->close(fd);
		driver->remove();
		return retval;
	}

	nrf24_fd = fd;

	listen_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
				      watch_accept_cb, NULL,
				      watch_accept_destroy);

	return 0;
}

static void radio_stop(void)
{

	if (listen_idle) {
		g_source_remove(listen_idle);
		listen_idle = 0;
	}

	/* nRF24: Stop listening pipe0 */
	if (nrf24_fd >= 0)
		driver->close(nrf24_fd);

	/* Stop nRF24 radio */
	driver->remove();
	nrf24_fd = -1;
}

static gboolean nrf_data_watch(GIOChannel *io, GIOCondition cond,
						gpointer user_data)
{
	char buffer[1024];
	GIOStatus status;
	GError *gerr = NULL;
	gsize rbytes;

	/*
	 * Manages TCP data from spiproxyd(nRF proxy). All traffic(raw
	 * data) should be transferred using unix socket to knotd.
	 */

	if (cond & (G_IO_HUP | G_IO_ERR))
		return FALSE;

	memset(buffer, 0, sizeof(buffer));

	/* Incoming data through TCP socket */
	status = g_io_channel_read_chars(io, buffer, sizeof(buffer),
						 &rbytes, &gerr);
	if (status == G_IO_STATUS_ERROR) {
		printf("read(): %s\n", gerr->message);
		g_error_free(gerr);
		return FALSE;
	}

	if (rbytes == 0)
		return FALSE;

	/*
	 * Decode based on nRF PIPE information and forward
	 * the data through a unix socket to knotd.
	 */
	printf("read(): %lu bytes\n", rbytes);

	return TRUE;
}

static int tcp_init(const char *host, int port)
{
	GIOChannel *io;
	GIOCondition cond = G_IO_IN | G_IO_ERR | G_IO_HUP;
	struct hostent *hostent;		/* Host information */
	struct in_addr h_addr;			/* Internet address */
	struct sockaddr_in server;		/* nRF proxy: spiproxyd */
	int err, sock;

	hostent = gethostbyname(host);
	if (hostent == NULL) {
		err = errno;
		fprintf(stderr, "gethostbyname(): %s(%d)\n",
						strerror(err), err);
		return -err;
	}

	h_addr.s_addr = *((unsigned long *) hostent-> h_addr_list[0]);

	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		err = errno;
		fprintf(stderr, "socket(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = h_addr.s_addr;
	server.sin_port = htons(port);

	err = connect(sock, (struct sockaddr *) &server, sizeof(server));
	if (err < 0) {
		err = errno;
		fprintf(stderr, "connect(): %s(%d)\n", strerror(err), err);
		close(sock);
		return -err;
	}

	fprintf(stdout, "nRF Proxy address: %s\n", inet_ntoa(h_addr));

	io = g_io_channel_unix_new(sock);
	g_io_channel_set_close_on_unref(io, TRUE);

	/* Ending 'NULL' for binary data */
	g_io_channel_set_encoding(io, NULL, NULL);
	g_io_channel_set_buffered(io, FALSE);

	/* TCP handler: incoming data from spiproxyd (nRF proxy) */
	g_io_add_watch(io, cond, nrf_data_watch, NULL);

	/* Keep only one reference: watch */
	g_io_channel_unref(io);

	return 0;
}

static char *load_config(const char *file)
{
	char *buffer = NULL;
	int length;
	FILE *fl = fopen(file, "r");

	if (fl == NULL) {
		fprintf(stderr, "No such file available: %s\n", file);
		return NULL;
	}

	fseek(fl, 0, SEEK_END);
	length = ftell(fl);
	fseek(fl, 0, SEEK_SET);

	buffer = (char *) malloc((length+1)*sizeof(char));
	if (buffer) {
		fread(buffer, length, 1, fl);
		buffer[length] = '\0';
	}
	fclose(fl);

	return buffer;
}

static uint8_t set_tx_input(int tx_pwr)
{
	switch (tx_pwr) {

	case 0:
		return NRF24_PWR_0DBM;

	case -6:
		return NRF24_PWR_6DBM;

	case -12:
		return NRF24_PWR_12DBM;

	case -18:
		return NRF24_PWR_18DBM;
	}

	return NRF24_PWR_0DBM;
}

/*
 * TODO: Get "host", "spi" and "port"
 * parameters when/if implemented
 * in the json configuration file
 */
static int parse_config(const char *config, const char **host, int *port,
			const char **spi, uint8_t *channel, uint8_t *tx_pwr)
{
	json_object *jobj, *obj_radio, *obj_tmp;

	int tx_pwr_dBm, channel_json_input;

	int err = -EINVAL;

	jobj = json_tokener_parse(config);
	if (jobj == NULL)
		return -EINVAL;

	if (!json_object_object_get_ex(jobj, "radio", &obj_radio))
		goto done;

	if (json_object_object_get_ex(obj_radio,  "channel", &obj_tmp)) {
		channel_json_input = json_object_get_int(obj_tmp);

		/*
		 * Validate channel values
		 * channel range 0 - 125 valid values
		 */
		if (channel_json_input > 125 || channel_json_input < 0) {
			fprintf(stderr, "Invalid channel value: %d\n", channel_json_input);
			channel_json_input = NRF24_CH_MIN;
		}
		*channel = channel_json_input;
	}

	if (json_object_object_get_ex(obj_radio,  "powerRate", &obj_tmp)) {
		tx_pwr_dBm = json_object_get_int(obj_tmp);

		/*
		 * Sets tx_pwr
		 * write Power Amplifier(PA) control in
		 * TX mode: 0 <= v <= 3 valid values
		 * and respectively to: -18dBm,
		 * -12dBm, -6 dBm and 0dBm
		 */
		*tx_pwr = set_tx_input(tx_pwr_dBm);
	}

	/* Success */
	err = 0;

done:
	/* Free mem used in json parse: */
	json_object_put(jobj);
	return err;
}

int manager_start(const char *file, const char *host, int port,
			const char *spi, uint8_t channel, uint8_t tx_pwr)
{
	char *json_str;

	json_str = load_config(file);

	if (json_str == NULL)
		printf("Invalid file\nCommand line settings will be set\n");

	else {

		if (parse_config(json_str, &host, &port, &spi, &channel, &tx_pwr) < 0)
			printf("Invalid file parameters\n");
	}

	if (host == NULL)
		return radio_init(spi);
	/*
	 * TCP development mode: Linux connected to RPi(phynrfd radio
	 * proxy). Connect to phynrfd routing all traffic over TCP.
	 */
	return tcp_init(host, port);
}

void manager_stop(void)
{
	radio_stop();
}