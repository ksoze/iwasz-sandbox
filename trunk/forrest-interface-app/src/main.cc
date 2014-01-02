/****************************************************************************
 *                                                                          *
 *  Author : lukasz.iwaszkiewicz@gmail.com                                  *
 *  ~~~~~~~~                                                                *
 *  License : see COPYING file for details.                                 *
 *  ~~~~~~~~~                                                               *
 ****************************************************************************/

#include <iostream>
#include <thread>
#include <mutex>
#include <libusb.h>
#include <signal.h>
#include <gtk/gtk.h>
#include <string.h>
#include <boost/lexical_cast.hpp>

const int ISO_PACKET_SIZE = 64;
const int ENCODERS_NUMBER = 32;
bool doExit = false;

// Pointer to adjustment.
static GtkAdjustment *adjustment[ENCODERS_NUMBER];

// Handle to my device.
static libusb_device_handle *devh = NULL;

static void LIBUSB_CALL cb_xfr (libusb_transfer *xfr);

typedef uint8_t Buffer[ISO_PACKET_SIZE];
Buffer buf;
std::mutex bufferMutex;

void printDevs (libusb_device **devs)
{
        libusb_device *dev;
        int i = 0, j = 0;
        uint8_t path[8];

        while ((dev = devs[i++]) != NULL) {
                struct libusb_device_descriptor desc;
                int r = libusb_get_device_descriptor (dev, &desc);
                if (r < 0) {
                        fprintf (stderr, "failed to get device descriptor");
                        return;
                }

                printf ("%04x:%04x (bus %d, device %d)", desc.idVendor, desc.idProduct, libusb_get_bus_number (dev), libusb_get_device_address (dev));

                r = libusb_get_port_numbers (dev, path, sizeof(path));
                if (r > 0) {
                        printf (" path: %d", path[0]);
                        for (j = 1; j < r; j++)
                                printf (".%d", path[j]);
                }
                printf ("\n");
        }
}

void initUsb ()
{
        libusb_device **devs;
        ssize_t cnt;

        int r = libusb_init (NULL);

        if (r < 0) {
                std::cerr << "Error libusb_init!" << std::endl;
                return;
        }

        cnt = libusb_get_device_list (NULL, &devs);

        if (cnt < 0) {
                std::cerr << "Error retrieving usbList cnt=" << cnt << std::endl;
                libusb_exit (NULL);
                exit (1);
        }

        printDevs (devs);
        libusb_free_device_list (devs, 1);

        devh = libusb_open_device_with_vid_pid (NULL, 0x20a0, 0x41ff);

        if (!devh) {
                std::cerr << "Error finding USB device" << std::endl;
                libusb_exit (NULL);
                exit (1);
        }
        else {
                std::cerr << "USB device has been found." << std::endl;
        }

        int rc = libusb_claim_interface (devh, 0);

        if (rc < 0) {
                std::cerr << "Error claiming interface : " << libusb_error_name (rc) << std::endl;
                libusb_exit (NULL);
                exit (1);
        }
        else {
                std::cerr << "Interface claimed." << std::endl;
        }

        if ((rc = libusb_set_interface_alt_setting (devh, 0, 1)) != 0) {
                std::cerr << "Error libusb_set_interface_alt_setting rc = "  << rc << std::endl;
                libusb_exit (NULL);
                exit (1);
        }
        else {
                std::cerr << "libusb_set_interface_alt_setting OK" << std::endl;
        }

        /*--------------------------------------------------------------------------*/

        int num_iso_pack = 1; // ?!?!? CO to jest!
        libusb_transfer *xfr = libusb_alloc_transfer (num_iso_pack);

        if (!xfr) {
                std::cerr << "Error" << std::endl;
                libusb_exit (NULL);
                exit (1);
        }

//        libusb_fill_iso_transfer (xfr, devh, 0x81, buf, ISO_PACKET_SIZE, num_iso_pack, cb_xfr, NULL, 1000);
//        libusb_set_iso_packet_lengths (xfr, ISO_PACKET_SIZE / num_iso_pack);

        libusb_fill_interrupt_transfer(xfr, devh, 0x81, buf, sizeof (buf), cb_xfr, NULL, 100);

        int ret = libusb_submit_transfer (xfr);
        if (ret) {
                std::cerr << "libusb_submit_transfer error :" << std::endl;
                switch (ret) {
                case LIBUSB_ERROR_NO_DEVICE:
                        std::cerr << "LIBUSB_ERROR_NO_DEVICE" << std::endl;
                        break;
                case LIBUSB_ERROR_BUSY:
                        std::cerr << "LIBUSB_ERROR_BUSY" << std::endl;
                        break;
                case LIBUSB_ERROR_NOT_SUPPORTED:
                        std::cerr << "LIBUSB_ERROR_NOT_SUPPORTED" << std::endl;
                        break;
                default:
                        std::cerr << "Error nr : " << ret << std::endl;
                        break;
                }
        }
        else {
                std::cerr << "libusb_submit_transfer OK" << std::endl;
        }
}

void closeUsb ()
{
        if (!libusb_release_interface (devh, 0)) {
                std::cerr << "Interface released." << std::endl;
        }
        else {
                std::cerr << "Error releasing interface." << std::endl;
        }

        if (devh) {
                libusb_close(devh);
                std::cerr << "Device closed" << std::endl;
        }

        libusb_exit (NULL);
}

void usbThread ()
{
        initUsb ();

        while (true) {
                int rc = libusb_handle_events(NULL);

                if (rc != LIBUSB_SUCCESS) {
                        std::cerr << "Problem with handling event from the USB!" << std::endl;
                        break;
                }
        }
}

gboolean guiThread (gpointer user_data)
{
                {
                        std::lock_guard<std::mutex> guard (bufferMutex);

                        for (int i = 0; i < ENCODERS_NUMBER; ++i) {
                                int bufPos = i*2;
                                uint16_t val = buf[bufPos] | ((uint16_t)buf[bufPos + 1] << 8);
                                gtk_adjustment_set_value (adjustment[i], val);
                        }

//                        printf ("[");
//                        for (unsigned int i = 0; i < ISO_PACKET_SIZE; ++i) {
//                                printf("%02x", buf[i]);
//                        }
//                        printf("]%d %d\n", buf[16], buf[17]);
                }

//                for (int i = 0; i < ENCODERS_NUMBER; ++i) {
//                        gtk_adjustment_set_value (adjustment, buffer[0]);
//                }

        return G_SOURCE_CONTINUE;
}

static void LIBUSB_CALL cb_xfr (libusb_transfer *xfr)
{
        int i;
        // Error checkin on whole transfer.
        if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
                fprintf(stderr, "transfer status %d\n", xfr->status);
                libusb_free_transfer(xfr);
                exit(3);
        }

        // Error checking on every individual packet as suggested in the API docs.
        for (i = 0; i < xfr->num_iso_packets; i++) {
                struct libusb_iso_packet_descriptor *pack = &xfr->iso_packet_desc[i];

                if (pack->status != LIBUSB_TRANSFER_COMPLETED) {
                        fprintf(stderr, "Error: pack %u status %d\n", i, pack->status);
                        exit(5);
                }

//                uint8_t *buffer = libusb_get_iso_packet_buffer (xfr, 0);
                // Both x86 and STM32 are little endian.
//                uint16_t *angles = (uint16_t *)buffer;
//                static uint8_t prev = 0;

#if 0
                printf("pack%u length:%u, actual_length:%u\n", i, pack->length, pack->actual_length);

                printf ("[");
                for (unsigned int i = 0; i < pack->actual_length; ++i) {
                        printf("%02x", buffer[i]);
                }
                printf("]\n");
#endif
        }

#if 0
        printf("length:%u, actual_length:%u\n", xfr->length, xfr->actual_length);

        if (xfr->actual_length == ISO_PACKET_SIZE) {
                for (i = 0; i < xfr->actual_length; ++i) {
                        printf ("%02x", xfr->buffer[i]);
                        if ((i+1) % 16 == 0) {
                                printf (" ");
                        }
                }
        }
        else {
                for (i = 0; i < xfr->actual_length; ++i) {
                        printf ("%02x", xfr->buffer[i]);
                }
        }

        printf ("\n");

//        std::cerr << "Length received : " << xfr->actual_length << std::endl;
#endif

        if (xfr->actual_length == ISO_PACKET_SIZE) {
                std::lock_guard<std::mutex> guard(bufferMutex);
                memcpy(buf, xfr->buffer, ISO_PACKET_SIZE);
        }

        if (libusb_submit_transfer(xfr) < 0) {
                fprintf(stderr, "error re-submitting URB\n");
                exit(1);
        }
}

//static void sig_hdlr(int signum)
//{
//        switch (signum) {
//        case SIGINT:
//                std::cerr << "SIGINT !!!!" << std::endl;
//                doExit = true;
//                break;
//        }
//}
//
//static void print_hello (GtkWidget *widget, gpointer data)
//{
//        g_print ("Hello World\n");
//}

int main (int argc, char **argv)
{
        GtkBuilder *builder;
        GObject *window;
        GObject *button;

        gtk_init (&argc, &argv);

        /* Construct a GtkBuilder instance and load our UI description */
        builder = gtk_builder_new ();
        gtk_builder_add_from_file (builder, "forrest.ui", NULL);

        /* Connect signal handlers to the constructed widgets. */
        window = gtk_builder_get_object (builder, "window");
        g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), NULL);

//        button = gtk_builder_get_object (builder, "button1");
//        g_signal_connect (button, "clicked", G_CALLBACK (print_hello), NULL);
//
//        button = gtk_builder_get_object (builder, "button2");
//        g_signal_connect (button, "clicked", G_CALLBACK (print_hello), NULL);

        button = gtk_builder_get_object (builder, "quit");
        g_signal_connect (button, "clicked", G_CALLBACK (gtk_main_quit), NULL);

//        GtkScale *scale = GTK_SCALE (gtk_builder_get_object (builder, "scale1"));
        for (int i = 0; i < ENCODERS_NUMBER; ++i) {
                adjustment[i] = GTK_ADJUSTMENT (gtk_builder_get_object (builder, (std::string ("adjustment") + boost::lexical_cast <std::string> (i + 1)).c_str ()));
        }

        std::thread t (usbThread);
        t.detach ();

//        std::thread t2 (guiThread);
//        t2.detach ();

        g_idle_add (guiThread, NULL);
        gtk_main ();
        closeUsb ();



//        while (!doExit) {
//                int rc = libusb_handle_events(NULL);
//
//                if (rc != LIBUSB_SUCCESS)
//                        break;
//        }

        /*--------------------------------------------------------------------------*/

        return 0;
}