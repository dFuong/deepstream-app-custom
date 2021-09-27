#include <stdio.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <glib.h>
#include <math.h>
#include <gmodule.h>
#include <string.h>
#include <sys/time.h>
#include "gstnvdsmeta.h"
#include "gst-nvmessage.h"
#include "nvdsmeta.h"
#include <cuda_runtime_api.h>
#include <gst/rtsp-server/rtsp-server.h>


#define MAX_DISPLAY_LEN 64

#define PGIE_CLASS_ID_VEHICLE 0
#define PGIE_CLASS_ID_PERSON 2

#define OSD_PROCESS_MODE 0
#define OSD_DISPLAY_TEXT 1

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 40000
#define GST_CAPS_FEATURES_NVMM "memory:NVMM"
#define TILED_OUTPUT_WIDTH 1280
#define TILED_OUTPUT_HEIGHT 720
#define GPU_ID 0

gint frame_number = 0;
gchar pgie_classes_str[4][32] = { "Vehicle", "TwoWheeler", "Person", "Roadsign"};

/* osd_sink_pad_buffer_probe  will extract metadata received on OSD sink pad
 * and update params for drawing rectangle, object information etc. */

static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
                           gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *) info->data;
    guint num_rects = 0;
    NvDsObjectMeta *obj_meta = NULL;
    guint vehicle_count = 0;
    guint person_count = 0;
    NvDsMetaList * l_frame = NULL;
    NvDsMetaList * l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
         l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
        int offset = 0;
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL;
             l_obj = l_obj->next) {
            obj_meta = (NvDsObjectMeta *) (l_obj->data);
            if (obj_meta->class_id == PGIE_CLASS_ID_VEHICLE) {
                vehicle_count++;
                num_rects++;
            }
            if (obj_meta->class_id == PGIE_CLASS_ID_PERSON) {
                person_count++;
                num_rects++;
            }
        }
        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        NvOSD_TextParams *txt_params  = &display_meta->text_params[0];
        display_meta->num_labels = 1;
        txt_params->display_text = g_malloc0 (MAX_DISPLAY_LEN);
        offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Person = %d ", person_count);
        offset = snprintf(txt_params->display_text + offset , MAX_DISPLAY_LEN, "Vehicle = %d ", vehicle_count);

        /* Now set the offsets where the string should appear */
        txt_params->x_offset = 10;
        txt_params->y_offset = 12;

        /* Font , font-color and font-size */
        txt_params->font_params.font_name = "Serif";
        txt_params->font_params.font_size = 10;
        txt_params->font_params.font_color.red = 1.0;
        txt_params->font_params.font_color.green = 1.0;
        txt_params->font_params.font_color.blue = 1.0;
        txt_params->font_params.font_color.alpha = 1.0;

        /* Text background color */
        txt_params->set_bg_clr = 1;
        txt_params->text_bg_clr.red = 0.0;
        txt_params->text_bg_clr.green = 0.0;
        txt_params->text_bg_clr.blue = 0.0;
        txt_params->text_bg_clr.alpha = 1.0;

        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }

    g_print ("Frame Number = %d Number of objects = %d "
             "Vehicle Count = %d Person Count = %d\n",
             frame_number, num_rects, vehicle_count, person_count);
    frame_number++;
    return GST_PAD_PROBE_OK;
}

static gboolean
bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_EOS:
            g_print ("End of stream\n");
            g_main_loop_quit (loop);
            break;
        case GST_MESSAGE_ERROR:{
            gchar *debug;
            GError *error;
            gst_message_parse_error (msg, &error, &debug);
            g_printerr ("ERROR from element %s: %s\n",
                        GST_OBJECT_NAME (msg->src), error->message);
            if (debug)
                g_printerr ("Error details: %s\n", debug);
            g_free (debug);
            g_error_free (error);
            g_main_loop_quit (loop);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

static void
cb_newpad (GstElement * decodebin, GstPad * decoder_src_pad, gpointer data)
{
  g_print ("In cb_newpad\n");
  GstCaps *caps = gst_pad_get_current_caps (decoder_src_pad);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);
  GstElement *source_bin = (GstElement *) data;
  GstCapsFeatures *features = gst_caps_get_features (caps, 0);

  /* Need to check if the pad created by the decodebin is for video and not
   * audio. */
  if (!strncmp (name, "video", 5)) {
    /* Link the decodebin pad only if decodebin has picked nvidia
     * decoder plugin nvdec_*. We do this by checking if the pad caps contain
     * NVMM memory features. */
    if (gst_caps_features_contains (features, GST_CAPS_FEATURES_NVMM)) {
      /* Get the source bin ghost pad */
      GstPad *bin_ghost_pad = gst_element_get_static_pad (source_bin, "src");
      if (!gst_ghost_pad_set_target (GST_GHOST_PAD (bin_ghost_pad),
              decoder_src_pad)) {
        g_printerr ("Failed to link decoder src pad to source bin ghost pad\n");
      }
      gst_object_unref (bin_ghost_pad);
    } else {
      g_printerr ("Error: Decodebin did not pick nvidia decoder plugin.\n");
    }
  }
}

static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
  g_print ("Decodebin child added: %s\n", name);
  if (g_strrstr (name, "decodebin") == name) {
    g_signal_connect (G_OBJECT (object), "child-added",
        G_CALLBACK (decodebin_child_added), user_data);
  }
}

static GstElement *
create_source_bin (guint index, gchar * uri)
{
  GstElement *bin = NULL, *uri_decode_bin = NULL;
  gchar bin_name[16] = { };

  g_snprintf (bin_name, 15, "source-bin-%02d", index);
  /* Create a source GstBin to abstract this bin's content from the rest of the
   * pipeline */
  bin = gst_bin_new (bin_name);

  /* Source element for reading from the uri.
   * We will use decodebin and let it figure out the container format of the
   * stream and the codec and plug the appropriate demux and decode plugins. */
  uri_decode_bin = gst_element_factory_make ("uridecodebin", "uri-decode-bin");

  if (!bin || !uri_decode_bin) {
    g_printerr ("One element in source bin could not be created.\n");
    return NULL;
  }

  /* We set the input uri to the source element */
  g_object_set (G_OBJECT (uri_decode_bin), "uri", uri, NULL);

  /* Connect to the "pad-added" signal of the decodebin which generates a
   * callback once a new pad for raw data has beed created by the decodebin */
  g_signal_connect (G_OBJECT (uri_decode_bin), "pad-added",
      G_CALLBACK (cb_newpad), bin);
  g_signal_connect (G_OBJECT (uri_decode_bin), "child-added",
      G_CALLBACK (decodebin_child_added), bin);

  gst_bin_add (GST_BIN (bin), uri_decode_bin);

  /* We need to create a ghost pad for the source bin which will act as a proxy
   * for the video decoder src pad. The ghost pad will not have a target right
   * now. Once the decode bin creates the video decoder and generates the
   * cb_newpad callback, we will set the ghost pad target to the video decoder
   * src pad. */
  if (!gst_element_add_pad (bin, gst_ghost_pad_new_no_target ("src",
              GST_PAD_SRC))) {
    g_printerr ("Failed to add ghost pad in source bin\n");
    return NULL;
  }

  return bin;
}

static GstRTSPServer *server;
static gboolean
start_rtsp_streaming (guint rtsp_port_num, guint updsink_port_num,
                      guint64 udp_buffer_size)
{
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;
    char udpsrc_pipeline[512];

    char port_num_Str[64] = { 0 };
    char *encoder_name;

    if (udp_buffer_size == 0)
        udp_buffer_size = 512 * 1024;

    sprintf (udpsrc_pipeline,
             "( udpsrc name=pay0 port=%d buffer-size=%lu caps=\"application/x-rtp, media=video, "
             "clock-rate=90000, encoding-name=H265, payload=96 \" )",
             updsink_port_num, udp_buffer_size);

    sprintf (port_num_Str, "%d", rtsp_port_num);

    server = gst_rtsp_server_new ();
    g_object_set (server, "service", port_num_Str, NULL);

    mounts = gst_rtsp_server_get_mount_points (server);

    factory = gst_rtsp_media_factory_new ();
    gst_rtsp_media_factory_set_launch (factory, udpsrc_pipeline);

    gst_rtsp_mount_points_add_factory (mounts, "/ds-test", factory);

    g_object_unref (mounts);

    gst_rtsp_server_attach (server, NULL);

    g_print
            ("\n *** DeepStream: Launched RTSP Streaming at rtsp://192.168.1.23:%d/ds-test ***\n\n",
             rtsp_port_num);

    return TRUE;
}

static GstRTSPFilterResult
client_filter (GstRTSPServer * server, GstRTSPClient * client,
               gpointer user_data)
{
    return GST_RTSP_FILTER_REMOVE;
}

static void
destroy_sink_bin ()
{
    GstRTSPMountPoints *mounts;
    GstRTSPSessionPool *pool;

    mounts = gst_rtsp_server_get_mount_points (server);
    gst_rtsp_mount_points_remove_factory (mounts, "/ds-test");
    g_object_unref (mounts);
    gst_rtsp_server_client_filter (server, client_filter, NULL);
    pool = gst_rtsp_server_get_session_pool (server);
    gst_rtsp_session_pool_cleanup (pool);
    g_object_unref (pool);
}

int
main (int argc, char *argv[])
{
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;
    char udpsrc_pipeline[512];

    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, 
         *streammux = NULL, *sink = NULL, *pgie = NULL, *nvvidconv = NULL,
            *nvosd = NULL, *rtppay = NULL, *encoder = NULL, *tiler=NULL;

    GstElement *transform = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;
    GstPad *osd_sink_pad = NULL;
    GstElement *nvvidconv1 = NULL, *filter = NULL, *qtmux = NULL;
    GstCaps *caps = NULL;
    guint num_sources,i;
    guint tiler_rows, tiler_columns;
    guint pgie_batch_size;
    GstPad *tiler_src_pad = NULL;

    int current_device = -1;
    cudaGetDevice(&current_device);
    struct cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, current_device);
    /* Check input arguments */
    if (argc < 2) {
        g_printerr ("Usage: %s <uri1> [uri2] ... [uriN] \n", argv[0]);
        return -1;
    }
    num_sources=argc - 1;
    /* Standard GStreamer initialization */
    gst_init (&argc, &argv);
    loop = g_main_loop_new (NULL, FALSE);

    /* Create gstreamer elements */
    /* Create Pipeline element that will form a connection of other elements */
    pipeline = gst_pipeline_new ("dstest-pipeline");


    /* Create nvstreammux instance to form batches from one or more sources. */
    streammux = gst_element_factory_make ("nvstreammux", "streammuxer");

    if (!pipeline || !streammux) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    gst_bin_add (GST_BIN (pipeline), streammux);
    for (i = 0; i < num_sources; i++) {
        GstPad *sinkpad, *srcpad;
        gchar pad_name[16] = { };
        GstElement *source_bin = create_source_bin (i, argv[i + 1]);

        if (!source_bin) {
        g_printerr ("Failed to create source bin. Exiting.\n");
        return -1;
        }

        gst_bin_add (GST_BIN (pipeline), source_bin);

        g_snprintf (pad_name, 15, "sink_%u", i);
        sinkpad = gst_element_get_request_pad (streammux, pad_name);
        if (!sinkpad) {
        g_printerr ("Streammux request sink pad failed. Exiting.\n");
        return -1;
        }

        srcpad = gst_element_get_static_pad (source_bin, "src");
        if (!srcpad) {
        g_printerr ("Failed to get src pad of source bin. Exiting.\n");
        return -1;
        }

        if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr ("Failed to link source bin to stream muxer. Exiting.\n");
        return -1;
        }

        gst_object_unref (srcpad);
        gst_object_unref (sinkpad);
    }
    
    /* Use nvinfer to run inferencing on decoder's output,
     * behaviour of inferencing is set through config file */
    pgie = gst_element_factory_make ("nvinfer", "primary-nvinference-engine");

    /* Use convertor to convert from NV12 to RGBA as required by nvosd */
    nvvidconv = gst_element_factory_make ("nvvideoconvert", "nvvideo-converter");

    /* Create OSD to draw on the converted RGBA buffer */
    nvosd = gst_element_factory_make ("nvdsosd", "nv-onscreendisplay");

    nvvidconv1 = gst_element_factory_make ("nvvideoconvert", "convertor_postosd");


    qtmux = gst_element_factory_make ("qtmux", "muxer");

    filter = gst_element_factory_make ("capsfilter", "filter");
    caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=I420");
    g_object_set (G_OBJECT (filter), "caps", caps, NULL);
    gst_caps_unref (caps);

    tiler = gst_element_factory_make ("nvmultistreamtiler", "nvtiler");

    if (!nvvidconv1 ||!nvvidconv || !tiler || !qtmux || !filter || !caps) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;}
    /* Finally render the osd output */
    if(prop.integrated) {
        transform = gst_element_factory_make ("nvegltransform", "nvegl-transform");
    }
//sink = gst_element_factory_make ("fakesink", "nvvideo-renderer");

    guint udp_port  = 5400;
    encoder = gst_element_factory_make ("nvv4l2h265enc","encoder");
    rtppay = gst_element_factory_make ("rtph265pay", "rtp-payer");
    sink = gst_element_factory_make ("udpsink", "udp-sink");

    if ( !sink || !rtppay || !encoder) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    if(!transform && prop.integrated) {
        g_printerr ("One tegra element could not be created. Exiting.\n");
        return -1;
    }

    /* we set the input filename to the source element */

    g_object_set (G_OBJECT (streammux), "batch-size", 1, NULL);
    g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
                  MUXER_OUTPUT_HEIGHT,
                  "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);

    // tiler_rows = (guint) (1);
    // tiler_columns = (guint) (1);
    tiler_rows = (guint) sqrt (num_sources);
    tiler_columns = (guint) ceil (1.0 * num_sources / tiler_rows);

    g_object_set (G_OBJECT (tiler), "rows", tiler_rows, "columns", tiler_columns,
                  "width", TILED_OUTPUT_WIDTH, "height", TILED_OUTPUT_HEIGHT, NULL);
    
    g_object_set (G_OBJECT (nvosd), "process-mode", OSD_PROCESS_MODE,
      "display-text", OSD_DISPLAY_TEXT, NULL);

    /* Set all the necessary properties of the nvinfer element,
     * the necessary ones are : */
    g_object_set (G_OBJECT (pgie),
                  "config-file-path", "dstest1_pgie_config.txt", NULL);

    /* we add a message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

    /* Set up the pipeline */
    /* we add all elements into the pipeline */
    if(prop.integrated) {
        gst_bin_add_many (GST_BIN (pipeline),
                           streammux, pgie,tiler,
                          nvvidconv, nvosd, transform, sink, NULL);
    }
    else {
        gst_bin_add_many (GST_BIN (pipeline),
                          streammux, pgie,tiler, nvvidconv, nvosd, nvvidconv1,
                          filter, encoder, rtppay, sink, NULL);
    }


    if(prop.integrated) {
        if (!gst_element_link_many (streammux, pgie,tiler,
                                    nvvidconv, nvosd, transform, sink, NULL)) {
            g_printerr ("Elements could not be linked: 2. Exiting.\n");
            return -1;
        }
    }
    else {
        if (!gst_element_link_many (streammux, pgie,tiler, nvvidconv, nvosd, nvvidconv1,
                                    filter, encoder, rtppay, sink,NULL)) {
            g_printerr ("Elements could not be linked: 3. Exiting.\n");
            return -1;
        }
    }

    /*server init
     * */
    g_object_set (G_OBJECT (encoder), "preset-level", 1, NULL);
    g_object_set (G_OBJECT (encoder), "insert-sps-pps", 1, NULL);
    g_object_set (G_OBJECT (encoder), "bufapi-version", 1, NULL);
    g_object_set (G_OBJECT (sink), "host", "224.224.255.255", "port",
                  udp_port, "async", FALSE, "sync", 1, NULL);
    start_rtsp_streaming (8554/*rtsp_port*/, udp_port, 0);
    /* Lets add probe to get informed of the meta data generated, we add probe to
     * the sink pad of the osd element, since by that time, the buffer would have
     * had got all the metadata. */
    osd_sink_pad = gst_element_get_static_pad (nvosd, "sink");
    if (!osd_sink_pad)
        g_print ("Unable to get sink pad\n");
    else
        gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                           osd_sink_pad_buffer_probe, NULL, NULL);
    gst_object_unref (osd_sink_pad);

    /* Set the pipeline to "playing" state */
    g_print ("Now playing: %s\n", argv[1]);
    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    /* Wait till pipeline encounters an error or EOS */
    g_print ("Running...\n");
    g_main_loop_run (loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");
    gst_element_set_state (pipeline, GST_STATE_NULL);
    g_print ("Deleting pipeline\n");
    destroy_sink_bin();
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);
    g_main_loop_unref (loop);
    return 0;
}