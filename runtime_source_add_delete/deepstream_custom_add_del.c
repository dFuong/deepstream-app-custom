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



#define MAX_DISPLAY_LEN 64

#define PGIE_CLASS_ID_VEHICLE 0
#define PGIE_CLASS_ID_PERSON 2
#define SET_GPU_ID(object, gpu_id) g_object_set (G_OBJECT (object), "gpu-id", gpu_id, NULL);
#define SET_MEMORY(object, mem_id) g_object_set (G_OBJECT (object), "nvbuf-memory-type", mem_id, NULL);
#define SINK_ELEMENT "nveglglessink"

GMainLoop *loop = NULL;
/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

#define TILED_OUTPUT_WIDTH 1280
#define TILED_OUTPUT_HEIGHT 720
#define GPU_ID 0
#define MAX_NUM_SOURCES 4


#define CONFIG_GPU_ID "gpu-id"


gint g_num_sources = 0;
gint g_source_id_list[MAX_NUM_SOURCES];
gboolean g_eos_list[MAX_NUM_SOURCES];
gboolean g_source_enabled[MAX_NUM_SOURCES];
GstElement **g_source_bin_list = NULL;
GMutex eos_lock;

/* Assuming Resnet 10 model packaged in DS SDK */
// gchar pgie_classes_str[4][32] = { "Vehicle", "TwoWheeler", "Person", "Roadsign"};

GstElement *pipeline = NULL, *streammux = NULL, *sink = NULL, 
    *nvvideoconvert = NULL, *queue1 = NULL, *tiler = NULL;

// gchar *uri[100];
gchar uri[500];


static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
  g_print ("decodebin child added %s\n", name);
  if (g_strrstr (name, "decodebin") == name) {
    g_signal_connect (G_OBJECT (object), "child-added",
        G_CALLBACK (decodebin_child_added), user_data);
  }
  if (g_strrstr (name, "nvv4l2decoder") == name) {
#ifdef PLATFORM_TEGRA
    g_object_set (object, "enable-max-performance", TRUE, NULL);
    g_object_set (object, "bufapi-version", TRUE, NULL);
    g_object_set (object, "drop-frame-interval", 0, NULL);
    g_object_set (object, "num-extra-surfaces", 0, NULL);
#else
    g_object_set (object, "gpu-id", GPU_ID, NULL);
#endif
  }
}


static void
cb_newpad (GstElement * decodebin, GstPad * pad, gpointer data)
{
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  g_print ("decodebin new pad %s\n", name);
  if (!strncmp (name, "video", 5)) {
    gint source_id = (*(gint *) data);
    gchar pad_name[16] = { 0 };
    GstPad *sinkpad = NULL;
    g_snprintf (pad_name, 15, "sink_%u", source_id);
    sinkpad = gst_element_get_request_pad (streammux, pad_name);
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {
      g_print ("Failed to link decodebin to pipeline\n");
    } else {
      g_print ("Decodebin linked to pipeline\n");
    }
    gst_object_unref (sinkpad);
  }
}

static GstElement *
create_uridecode_bin (guint index, gchar * filename)
{
  GstElement *bin = NULL;
  gchar bin_name[16] = { };

  g_print ("creating uridecodebin for [%s]\n", filename);
  g_source_id_list[index] = index;
  g_snprintf (bin_name, 15, "source-bin-%02d", index);
  bin = gst_element_factory_make ("uridecodebin", bin_name);
  g_object_set (G_OBJECT (bin), "uri", filename, NULL);
  g_signal_connect (G_OBJECT (bin), "pad-added",
      G_CALLBACK (cb_newpad), &g_source_id_list[index]);
  g_signal_connect (G_OBJECT (bin), "child-added",
      G_CALLBACK (decodebin_child_added), &g_source_id_list[index]);
  g_source_enabled[index] = TRUE;

  return bin;
}

static void
stop_release_source (gint source_id)
{
  GstStateChangeReturn state_return;
  gchar pad_name[16];
  GstPad *sinkpad = NULL;
  state_return =
      gst_element_set_state (g_source_bin_list[source_id], GST_STATE_NULL);
  switch (state_return) {
    case GST_STATE_CHANGE_SUCCESS:
      g_print ("STATE CHANGE SUCCESS\n\n");
      g_snprintf (pad_name, 15, "sink_%u", source_id);
      sinkpad = gst_element_get_static_pad (streammux, pad_name);
      gst_pad_send_event (sinkpad, gst_event_new_flush_stop (FALSE));
      gst_element_release_request_pad (streammux, sinkpad);
      g_print ("STATE CHANGE SUCCESS %p\n\n", sinkpad);
      gst_object_unref (sinkpad);
      gst_bin_remove (GST_BIN (pipeline), g_source_bin_list[source_id]);
      source_id--;
      g_num_sources--;
      break;
    case GST_STATE_CHANGE_FAILURE:
      g_print ("STATE CHANGE FAILURE\n\n");
      break;
    case GST_STATE_CHANGE_ASYNC:
      g_print ("STATE CHANGE ASYNC\n\n");
      state_return =
          gst_element_get_state (g_source_bin_list[source_id], NULL, NULL,
          GST_CLOCK_TIME_NONE);
      g_snprintf (pad_name, 15, "sink_%u", source_id);
      sinkpad = gst_element_get_static_pad (streammux, pad_name);
      gst_pad_send_event (sinkpad, gst_event_new_flush_stop (FALSE));
      gst_element_release_request_pad (streammux, sinkpad);
      g_print ("STATE CHANGE ASYNC %p\n\n", sinkpad);
      gst_object_unref (sinkpad);
      gst_bin_remove (GST_BIN (pipeline), g_source_bin_list[source_id]);
      source_id--;
      g_num_sources--;
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      g_print ("STATE CHANGE NO PREROLL\n\n");
      break;
    default:
      break;
  }


}

static gboolean
delete_sources ()
{
  // gint source_id;
  g_mutex_lock (&eos_lock);
  for (gint source_id = 0; source_id < MAX_NUM_SOURCES; source_id++) {
    if (g_eos_list[source_id] == TRUE && g_source_enabled[source_id] == TRUE) {
      g_source_enabled[source_id] = FALSE;
      stop_release_source (source_id);
    }
  }
  g_mutex_unlock (&eos_lock);

  if (g_num_sources == 0) {
    g_main_loop_quit (loop);
    g_print ("All sources Stopped quitting\n");
    return FALSE;
  }

  g_print("\nEnter the ID-Deleted: ");
  gchar temp[2];
  scanf("%[^\n]%*c",temp);
  gint source_id = (gint)*temp -48;

  g_source_enabled[source_id]=FALSE;

  /*Release the source*/
  g_print("Calling stop %d \n",source_id);
  stop_release_source(source_id);

  if (g_num_sources == 0) {
    g_main_loop_quit (loop);
    g_print ("All sources Stopped quitting\n");
    return FALSE;
  }

  return TRUE;
}

static gboolean
add_sources ()
{
  gint source_id = g_num_sources; 
  GstElement *source_bin;
  GstStateChangeReturn state_return;

  g_print("\nEnter the Source_ID: ");
  gchar temp[2];
  scanf("%[^\n]%*c",temp);
  // fflush(stdin);
  source_id = (gint)*temp -48;

  g_source_enabled[source_id]=TRUE;

  g_print("\nEnter the Source_Uri: ");
  // fgets(uri,1000,stdin);
  scanf("%[^\n]%*c",uri);
  g_print("\nSource_ID = %d --- Source_URI = %s \n\n",source_id,uri);

  g_print ("Calling Start %d \n", source_id);
  source_bin = create_uridecode_bin (source_id, uri);
  if (!source_bin) {
    g_printerr ("Failed to create source bin. Exiting.\n");
    return -1;
  }
  g_source_bin_list[source_id] = source_bin;
  gst_bin_add (GST_BIN (pipeline), source_bin);
  state_return =
      gst_element_set_state (g_source_bin_list[source_id], GST_STATE_PLAYING);
  switch (state_return) {
    case GST_STATE_CHANGE_SUCCESS:
      g_print ("STATE CHANGE SUCCESS\n\n");
      source_id++;
      break;
    case GST_STATE_CHANGE_FAILURE:
      g_print ("STATE CHANGE FAILURE\n\n");
      break;
    case GST_STATE_CHANGE_ASYNC:
      g_print ("STATE CHANGE ASYNC\n\n");
      state_return =
          gst_element_get_state (g_source_bin_list[source_id], NULL, NULL,
          GST_CLOCK_TIME_NONE);
      source_id++;
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      g_print ("STATE CHANGE NO PREROLL\n\n");
      break;
    default:
      break;
  }
  g_num_sources++;

  return TRUE;
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
    case GST_MESSAGE_WARNING:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_warning (msg, &error, &debug);
      g_printerr ("WARNING from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), error->message);
      g_free (debug);
      g_printerr ("Warning: %s\n", error->message);
      g_error_free (error);
      break;
    }
    case GST_MESSAGE_ERROR:
    {
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
    case GST_MESSAGE_ELEMENT:
    {
      if (gst_nvmessage_is_stream_eos (msg)) {
        guint stream_id;
        if (gst_nvmessage_parse_stream_eos (msg, &stream_id)) {
          g_print ("Got EOS from stream %d\n", stream_id);
          g_mutex_lock (&eos_lock);
          g_eos_list[stream_id] = TRUE;
          g_mutex_unlock (&eos_lock);
        }
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}



int main (int argc, char *argv[])
{
  GstBus *bus = NULL;
  guint bus_watch_id;
  guint i, num_sources;
  guint tiler_rows, tiler_columns;
  guint pgie_batch_size;

#ifdef PLATFORM_TEGRA
  GstElement *nvtransform;
#endif

  num_sources = 1;

  /* Standard GStreamer initialization */
  gst_init (&argc, &argv);
  loop = g_main_loop_new (NULL, FALSE);

  g_mutex_init (&eos_lock);
  /* Create gstreamer elements */
  /* Create Pipeline element that will form a connection of other elements */
  pipeline = gst_pipeline_new ("dstest-pipeline");

  /* Use nvinfer to run inferencing on decoder's output,
   * behaviour of inferencing is set through config file */
  streammux = gst_element_factory_make ("nvstreammux", 'stream-muxer');
  g_object_set (G_OBJECT (streammux), "batched-push-timeout", 25000, NULL);
  g_object_set (G_OBJECT (streammux), "batch-size", 30, NULL);
  SET_GPU_ID (streammux, GPU_ID);

  if (!pipeline || !streammux) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }
  gst_bin_add (GST_BIN (pipeline), streammux);
  g_object_set (G_OBJECT (streammux), "live-source", 1, NULL);


  g_source_bin_list = g_malloc0 (sizeof (GstElement *) * MAX_NUM_SOURCES);

  /* Inital create the input source  */
  g_print("\nEnter the Source_ID: ");
  gchar temp[2];
  scanf("%[^\n]%*c",temp);
  gint source_id = (gint)*temp -48;
  g_print("\nEnter the Source_Uri: ");
  scanf("%[^\n]%*c",uri);
 
  g_print("\nSource_ID = %d --- Source_URI = %s \n\n",source_id,uri);

  GstElement *source_bin = create_uridecode_bin (source_id,uri);
  if (!source_bin) {
    g_printerr ("Failed to create source bin. Exiting.\n");
    return -1;
  }
  g_source_bin_list[source_id] = source_bin;
  gst_bin_add (GST_BIN (pipeline), source_bin);
  

  g_num_sources = num_sources;


  /* Use nvtiler to stitch o/p from upstream components */
  tiler = gst_element_factory_make ("nvmultistreamtiler", "nvtiler");

  /* Use convertor to convert from NV12 to RGBA as required by nvosd */
  nvvideoconvert =
      gst_element_factory_make ("nvvideoconvert", "converter");
#ifdef PLATFORM_TEGRA
  nvtransform = gst_element_factory_make ("nvegltransform", "nvegltransform");
#endif


  queue1=gst_element_factory_make("queue","queue1");

  /* Finally render the osd output */
  sink = gst_element_factory_make (SINK_ELEMENT, "nveglglessink");

  if ( !tiler || !nvvideoconvert 
      || !sink || !queue1
#ifdef PLATFORM_TEGRA
      || !nvtransform
#endif
      ) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

  g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
      MUXER_OUTPUT_HEIGHT, NULL);


  tiler_rows = (guint) sqrt (num_sources);
  tiler_columns = (guint) ceil (1.0 * num_sources / tiler_rows);
  /* we set the osd properties here */
  g_object_set (G_OBJECT (tiler), "rows", tiler_rows, "columns", tiler_columns,
      "width", TILED_OUTPUT_WIDTH, "height", TILED_OUTPUT_HEIGHT, NULL);
  SET_GPU_ID (tiler, GPU_ID);
  SET_GPU_ID (nvvideoconvert, GPU_ID);
  // SET_GPU_ID (nvosd, GPU_ID);
#ifndef PLATFORM_TEGRA
  SET_GPU_ID (sink, GPU_ID);
#endif

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  /* Set up the pipeline */
  /* we add all elements into the pipeline */
  gst_bin_add_many (GST_BIN (pipeline), 
      tiler, nvvideoconvert, queue1, sink, NULL);

#ifdef PLATFORM_TEGRA
  gst_bin_add (GST_BIN (pipeline), nvtransform);
#endif

  /* we link the elements together */
  /* file-source -> h264-parser -> nvh264-decoder ->
   * nvinfer -> nvvideoconvert -> nvosd -> video-renderer */
#ifdef PLATFORM_TEGRA
  if (!gst_element_link_many (streammux,  
          tiler, nvvideoconvert,queue1, nvtransform, sink, NULL)) {
    g_printerr ("Elements could not be linked. Exiting.\n");
    return -1;
  }
#else
  if (!gst_element_link_many (streammux,
          tiler, nvvideoconvert, queue1, sink, NULL)) {
    g_printerr ("Elements could not be linked. Exiting.\n");
    return -1;
  }
#endif

  g_object_set (G_OBJECT (sink), "sync", FALSE, "qos", FALSE, NULL);

  gst_element_set_state (pipeline, GST_STATE_PAUSED);

  /* Set the pipeline to "playing" state */
  // g_print ("Now playing: %s\n", argv[1]);
  if (gst_element_set_state (pipeline,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Failed to set pipeline to playing. Exiting.\n");
    return -1;
  }
  //GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-playing");

  /* Wait till pipeline encounters an error or EOS */
  g_print ("Running...\n");
  // g_timeout_add_seconds (10, add_sources, (gpointer) g_source_bin_list);
  while(1){
    g_print("\nSelect Initially: Add source < a > | Delete source < d >: \n\n");
    gchar input[2];
    scanf("%[^\n]%*c",input);
    if (input[0] == 'a'){
      bool add=add_sources();
      if (!add){
        break;
      }
    }
    else if(input[0]=='d'){
      bool del=delete_sources();
      if (!del){
        break;
      }
    }
  }
  
  g_main_loop_run (loop);


  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
  g_free (g_source_bin_list);
  g_free (uri);
  g_mutex_clear (&eos_lock);
  return 0;
}
