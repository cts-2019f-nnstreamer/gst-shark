#include <gst/gst.h>
// #include <ncurses.h>
#include <stdlib.h>
// #include <time.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "gstliveprofiler.h"
#include "gstliveunit.h"
#include "visualizeutil.h"
#include "gstctf.h"

// #define _DEBUG_TRUE

// Plugin Data
Packet *packet;
int log_idx = 0;
int metadata_writed = 0;

gboolean
is_filter (GstElement * element)
{
  GList *pads;
  GList *iterator;
  GstPad *pPad;
  gint sink = 0;
  gint src = 0;

  pads = GST_ELEMENT_PADS (element);
  iterator = g_list_first (pads);
  while (iterator != NULL) {
    pPad = iterator->data;
    if (gst_pad_get_direction (pPad) == GST_PAD_SRC)
      src++;
    if (gst_pad_get_direction (pPad) == GST_PAD_SINK)
      sink++;
    if (src > 1 || sink > 1)
      return FALSE;

    iterator = g_list_next (iterator);
  }
  if (src == 1 && sink == 1)
    return TRUE;
  else
    return FALSE;
}

static gboolean
is_queue (GstElement * element)
{
  static GstElementFactory *qfactory = NULL;
  GstElementFactory *efactory;

  g_return_val_if_fail (element, FALSE);

  if (NULL == qfactory) {
    qfactory = gst_element_factory_find ("queue");
  }

  efactory = gst_element_get_factory (element);

  return efactory == qfactory;
}

void
generate_meta_data_pad (gpointer key, gpointer value, gpointer user_data)
{
  gchar *pad_name = (gchar *) key;
  PadUnit *data = (PadUnit *) value;
  gchar *element_name = (gchar *) user_data;

  data->elem_idx = log_idx;
  char text[50];
  sprintf (text, "%d %s-%s", log_idx, element_name, pad_name);
  do_print_log ("log_metadata", text);
  log_idx++;
}

void
generate_meta_data (gpointer key, gpointer value, gpointer user_data)
{
  gchar *name = (gchar *) key;
  ElementUnit *data = (ElementUnit *) value;

  data->elem_idx = log_idx;

  char text[50];
  sprintf (text, "%d %s", log_idx++, name);
  do_print_log ("log_metadata", text);

  g_hash_table_foreach (data->pad, (GHFunc) generate_meta_data_pad, name);
}

/*
 * Adds children of element recursively to the Hashtable.
 * Write log metadata file when LOG_ENABLE is true
 */
void
add_children_recursively (GstElement * element, GHashTable * table)
{
  GList *children, *iter;
  ElementUnit *eUnit;
  PadUnit *pUnit;

  if (GST_IS_BIN (element)) {
    children = GST_BIN_CHILDREN (element);
    iter = g_list_first (children);

    while (iter != NULL) {
      add_children_recursively ((GstElement *) iter->data, table);
      iter = g_list_next (iter);
    }
  } else {
    eUnit = element_unit_new ();
    eUnit->element = element;
    eUnit->pad = g_hash_table_new (g_str_hash, g_str_equal);
    eUnit->is_filter = is_filter (element);
    eUnit->is_queue = is_queue (element);

    children = GST_ELEMENT_PADS (element);
    iter = g_list_first (children);

    while (iter != NULL) {
      pUnit = pad_unit_new ();
      pUnit->element = iter->data;
      g_hash_table_insert (eUnit->pad, GST_OBJECT_NAME (iter->data), pUnit);

      iter = g_list_next (iter);
    }
    g_hash_table_insert (table, GST_OBJECT_NAME (element), eUnit);
  }
}

gboolean
gst_liveprofiler_init (void)
{
  gint cpu_num;
  gfloat *cpu_load;
  GHashTable *elements;
  GHashTable *connections;

  if ((cpu_num = sysconf (_SC_NPROCESSORS_CONF)) == -1) {
    GST_WARNING ("Failed to get numbers of cpus");
    cpu_num = 1;
  }

  cpu_load = g_malloc0 (sizeof (gfloat) * cpu_num);
  elements = g_hash_table_new (g_str_hash, g_str_equal);
  connections = g_hash_table_new (g_str_hash, g_str_equal);

  packet = g_malloc0 (sizeof (Packet));
  packet_set_cpu_num (packet, cpu_num);
  packet_set_cpu_load (packet, cpu_load);
  packet_set_elements (packet, elements);
  packet_set_connections (packet, connections);

#ifndef _DEBUG_TRUE
  pthread_t thread;
  pthread_create (&thread, NULL, curses_loop, packet);
  pthread_detach (thread);
#endif

  return TRUE;
}

void
update_cpuusage_event (guint32 cpunum, gfloat * cpuload)
{
  gint cpu_num = packet->cpu_num;
  gfloat *cpu_load = packet->cpu_load;

  memcpy (cpu_load, cpuload, cpu_num * sizeof (gfloat));
}

void
update_proctime (ElementUnit * element, ElementUnit * peerElement, guint64 ts)
{
  guint64 delta;
  if (element->is_filter) {
    // element pushed buffer
    delta = time_pop_value (element->time_log, ts)
        avg_update_value (element->proctime, delta);
  }
  if (peerElement->is_filter) {
    // element pulled buffer
    time_push_value (peerElement->time_log, ts);
  }
//   if (peerElement->is_filter) {
//     peerElement->time = ts;
//   }

//   if (element->is_filter) {
//     if (ts - element->time > 0) {
//       avg_update_value (element->proctime, ts - element->time);
//     }
//   }
}

void
update_datatrate (PadUnit * pad, PadUnit * peerPad, guint64 ts)
{
  // guint64 prev_ts = pad->time;
  // gdouble rate;

  // if (ts > prev_ts) {
  //   rate = 1e9 / (ts - prev_ts);
  //   pad->datarate = rate;
  //   peerPad->datarate = rate;
  // }
  // pad->time = ts;
  // peerPad->time = ts;

  guint length = g_queue_get_length (pad->time_log);
  gdouble datarate;
  guint64 *pTime;

  if (length == 0) {
    pTime = g_malloc (sizeof (gdouble));
    *pTime = ts;
  } else {
    if (length > 10) {
      pTime = (guint64 *) g_queue_pop_tail (pad->time_log);
    } else {
      pTime = (guint64 *) g_queue_peek_tail (pad->time_log);
    }

    datarate = 1e9 * length / (ts - *pTime);
    pad->datarate = datarate;
    peerPad->datarate = datarate;

    pTime = g_malloc (sizeof (gdouble));
    *pTime = ts;
  }
  g_queue_push_head (pad->time_log, pTime);
}

void
update_buffer_size (PadUnit * pad, PadUnit * peerPad, guint64 size)
{
  avg_update_value (pad->buffer_size, size);
  avg_update_value (peerPad->buffer_size, size);
}

void
update_queue_level (ElementUnit * element)
{
  GstElement *pElement = element->element;
  guint32 size_buffers;
  guint32 max_size_buffers;
  if (!element->is_queue) {
    return;
  }

  g_object_get (pElement, "current-level-buffers", &size_buffers,
      "max-size-buffers", &max_size_buffers, NULL);

  element->queue_level = size_buffers;
  element->max_queue_level = max_size_buffers;
}

void
element_push_buffer_pre (gchar * elementname, gchar * padname, guint64 ts,
    GstBuffer * buffer)
{
  GHashTable *elements = packet->elements;
  ElementUnit *pElement, *pPeerElement;
  PadUnit *pPad, *pPeerPad;
#ifdef _DEBUG_TRUE
  printf ("[%ld]%s-%s pre\n", ts, elementname, padname);
#endif

  pElement = g_hash_table_lookup (elements, elementname);
  g_return_if_fail (pElement);
  pPad = g_hash_table_lookup (pElement->pad, padname);
  g_return_if_fail (pPad);
  pPeerPad = pad_unit_peer (elements, pPad);
  g_return_if_fail (pPeerPad);
  pPeerElement = pad_unit_parent (elements, pPeerPad);
  g_return_if_fail (pPeerElement);

  update_proctime (pElement, pPeerElement, ts);
  update_datatrate (pPad, pPeerPad, ts);
  update_buffer_size (pPad, pPeerPad, gst_buffer_get_size (buffer));
  update_queue_level (pElement);
}

void
element_push_buffer_post (gchar * elementname, gchar * padname, guint64 ts)
{
#ifdef _DEBUG_TRUE
  printf ("[%ld]%s-%s post\n", ts, elementname, padname);
#endif
}

void
element_push_buffer_list_pre (gchar * elementname, gchar * padname, guint64 ts)
{
#ifdef _DEBUG_TRUE
  printf ("[%ld]%s-%s pre list\n", ts, elementname, padname);
#endif
}

void
element_push_buffer_list_post (gchar * elementname, gchar * padname, guint64 ts)
{
#ifdef _DEBUG_TRUE
  printf ("[%ld]%s-%s post list\n", ts, elementname, padname);
#endif
}

void
element_pull_range_pre (gchar * elementname, gchar * padname, guint64 ts)
{
#ifdef _DEBUG_TRUE
  printf ("[%ld]%s-%s pre pull\n", ts, elementname, padname);
#endif
}

void
element_pull_range_post (gchar * elementname, gchar * padname, guint64 ts)
{
#ifdef _DEBUG_TRUE
  printf ("[%ld]%s-%s post pull\n", ts, elementname, padname);
#endif
}

void
update_pipeline_init (GstPipeline * element)
{
  add_children_recursively ((GstElement *) element, packet->elements);
  if (g_getenv ("LOG_ENABLED") && !metadata_writed) {
    g_hash_table_foreach (packet->elements, (GHFunc) generate_meta_data, NULL);
    metadata_writed = log_idx;
  }
}
