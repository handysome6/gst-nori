/*
 * GStreamer plugin entry point for the Nori Xvision camera plugin.
 * Registers the "norisrc" element.
 */

#include "gstnorisrc.h"

static gboolean
plugin_init (GstPlugin *plugin)
{
  return gst_element_register (plugin, "norisrc",
                               GST_RANK_NONE, GST_TYPE_NORI_SRC);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nori,
    "Nori Xvision USB camera source",
    plugin_init,
    VERSION,
    "LGPL",
    "gst-nori",
    "https://github.com/user/gst-nori"
)
