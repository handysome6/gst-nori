/*
 * GStreamer Nori Xvision Camera Source Element
 *
 * A GstPushSrc-based live source that captures video from Nori Xvision
 * USB cameras via the Nori SDK.  Supports MJPEG and YUY2 output,
 * device enumeration, and runtime camera control properties.
 */

#include "gstnorisrc.h"
#include "nori_tag.h"

#include <gst/video/video.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (nori_src_debug);
#define GST_CAT_DEFAULT nori_src_debug

/* ================================================================
 *  SDK global init – reference-counted so multiple elements share it
 * ================================================================ */

static GMutex   sdk_lock;
static gint     sdk_refcount    = 0;
static guint32  sdk_device_count = 0;

static gboolean
nori_sdk_ref (void)
{
  gboolean ok = TRUE;
  g_mutex_lock (&sdk_lock);
  if (sdk_refcount == 0) {
    uint32_t n = 0;
    uint32_t ret = Nori_Xvision_Init (NORI_USB_DEVICE, &n);
    if (ret != NORI_OK) {
      GST_ERROR ("Nori_Xvision_Init failed: 0x%04x", ret);
      ok = FALSE;
    } else {
      sdk_device_count = n;
      GST_INFO ("Nori SDK initialised – %u device(s) found", n);
    }
  }
  if (ok)
    sdk_refcount++;
  g_mutex_unlock (&sdk_lock);
  return ok;
}

static void
nori_sdk_unref (void)
{
  g_mutex_lock (&sdk_lock);
  if (--sdk_refcount == 0) {
    Nori_Xvision_UnInit ();
    sdk_device_count = 0;
    GST_INFO ("Nori SDK uninitialised");
  }
  g_mutex_unlock (&sdk_lock);
}

static guint32
nori_sdk_num_devices (void)
{
  guint32 n;
  g_mutex_lock (&sdk_lock);
  n = sdk_device_count;
  g_mutex_unlock (&sdk_lock);
  return n;
}

/* ================================================================
 *  GEnum registrations
 * ================================================================ */

GType
gst_nori_trigger_mode_get_type (void)
{
  static gsize g_type = 0;
  if (g_once_init_enter (&g_type)) {
    static const GEnumValue values[] = {
      { GST_NORI_TRIGGER_NONE,     "Free-running (no trigger)", "none"     },
      { GST_NORI_TRIGGER_SOFTWARE, "Software trigger",          "software" },
      { GST_NORI_TRIGGER_HARDWARE, "Hardware trigger",          "hardware" },
      { GST_NORI_TRIGGER_COMMAND,  "Command trigger",           "command"  },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static ("GstNoriTriggerMode", values);
    g_once_init_leave (&g_type, t);
  }
  return (GType) g_type;
}

GType
gst_nori_mirror_flip_get_type (void)
{
  static gsize g_type = 0;
  if (g_once_init_enter (&g_type)) {
    static const GEnumValue values[] = {
      { GST_NORI_MF_NORMAL,      "Normal",        "normal"      },
      { GST_NORI_MF_MIRROR,      "Mirror",        "mirror"      },
      { GST_NORI_MF_FLIP,        "Flip",          "flip"        },
      { GST_NORI_MF_MIRROR_FLIP, "Mirror + Flip", "mirror-flip" },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static ("GstNoriMirrorFlip", values);
    g_once_init_leave (&g_type, t);
  }
  return (GType) g_type;
}

/* ================================================================
 *  Properties
 * ================================================================ */

enum
{
  PROP_0,
  PROP_DEVICE_INDEX,
  PROP_ROLE,
  PROP_TRIGGER_MODE,
  PROP_MIRROR_FLIP,
  PROP_AUTO_EXPOSURE,
  PROP_AUTO_WHITE_BALANCE,
  PROP_SENSOR_SHUTTER,
  PROP_SENSOR_GAIN,
};

/* ================================================================
 *  Pad template
 * ================================================================ */

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        "image/jpeg, "
          "width  = (int) [ 1, MAX ], "
          "height = (int) [ 1, MAX ], "
          "framerate = (fraction) [ 0/1, MAX ] "
        "; "
        "video/x-raw, "
          "format = (string) YUY2, "
          "width  = (int) [ 1, MAX ], "
          "height = (int) [ 1, MAX ], "
          "framerate = (fraction) [ 0/1, MAX ]"
    )
);

/* ================================================================
 *  Boilerplate
 * ================================================================ */

#define gst_nori_src_parent_class parent_class
G_DEFINE_TYPE (GstNoriSrc, gst_nori_src, GST_TYPE_PUSH_SRC);

/* forward declarations */
static void     gst_nori_src_set_property (GObject *obj, guint id,
                                           const GValue *val, GParamSpec *pspec);
static void     gst_nori_src_get_property (GObject *obj, guint id,
                                           GValue *val, GParamSpec *pspec);
static void     gst_nori_src_finalize     (GObject *obj);

static gboolean gst_nori_src_start        (GstBaseSrc *bsrc);
static gboolean gst_nori_src_stop         (GstBaseSrc *bsrc);
static GstCaps *gst_nori_src_get_caps     (GstBaseSrc *bsrc, GstCaps *filter);
static gboolean gst_nori_src_set_caps     (GstBaseSrc *bsrc, GstCaps *caps);
static GstCaps *gst_nori_src_fixate       (GstBaseSrc *bsrc, GstCaps *caps);
static gboolean gst_nori_src_unlock       (GstBaseSrc *bsrc);
static gboolean gst_nori_src_unlock_stop  (GstBaseSrc *bsrc);

static GstFlowReturn gst_nori_src_create  (GstPushSrc *psrc, GstBuffer **buf);

/* ================================================================
 *  class_init
 * ================================================================ */

static void
gst_nori_src_class_init (GstNoriSrcClass *klass)
{
  GObjectClass    *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_nori_src_set_property;
  gobject_class->get_property = gst_nori_src_get_property;
  gobject_class->finalize     = gst_nori_src_finalize;

  /* ---- properties ---- */

  g_object_class_install_property (gobject_class, PROP_DEVICE_INDEX,
      g_param_spec_uint ("device-index", "Device index",
          "Index of the Nori camera (0 = first). Ignored when 'role' is set.",
          0, 255, 0,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_ROLE,
      g_param_spec_string ("role", "Role tag",
          "If set, select the camera whose NORICAM tag (user-data block 255) "
          "matches this string. Takes precedence over 'device-index'. Use "
          "`nori-ctl tag set` to assign roles.",
          NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_TRIGGER_MODE,
      g_param_spec_enum ("trigger-mode", "Trigger mode",
          "Camera trigger mode",
          GST_TYPE_NORI_TRIGGER_MODE, GST_NORI_TRIGGER_NONE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MIRROR_FLIP,
      g_param_spec_enum ("mirror-flip", "Mirror / Flip",
          "Sensor mirror and flip setting",
          GST_TYPE_NORI_MIRROR_FLIP, GST_NORI_MF_NORMAL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_AUTO_EXPOSURE,
      g_param_spec_boolean ("auto-exposure", "Auto exposure",
          "Enable UVC auto exposure. When TRUE, the camera firmware drives both "
          "integration time and gain; sensor-shutter and sensor-gain are ignored. "
          "When FALSE, the UVC exposure/gain registers are reset to device "
          "defaults and sensor-shutter / sensor-gain take effect.", TRUE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_AUTO_WHITE_BALANCE,
      g_param_spec_boolean ("auto-white-balance", "Auto white balance",
          "Enable UVC auto white balance (V4L2_CID_AUTO_WHITE_BALANCE)", TRUE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_SENSOR_SHUTTER,
      g_param_spec_uint ("sensor-shutter", "Sensor shutter",
          "Sensor shutter / exposure time in microseconds. Only applied when "
          "auto-exposure=false; a warning is logged otherwise.",
          0, G_MAXUINT, 0,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_SENSOR_GAIN,
      g_param_spec_uint ("sensor-gain", "Sensor gain",
          "Sensor analogue gain multiplier. Only applied when "
          "auto-exposure=false; a warning is logged otherwise.",
          0, G_MAXUINT, 0,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /* ---- element metadata ---- */

  gst_element_class_set_static_metadata (element_class,
      "Nori Xvision Camera Source",
      "Source/Video/Hardware",
      "Capture video from Nori Xvision USB cameras via the Nori SDK",
      "gst-nori");

  gst_element_class_add_static_pad_template (element_class, &src_template);

  /* ---- virtual methods ---- */

  basesrc_class->start        = GST_DEBUG_FUNCPTR (gst_nori_src_start);
  basesrc_class->stop         = GST_DEBUG_FUNCPTR (gst_nori_src_stop);
  basesrc_class->get_caps     = GST_DEBUG_FUNCPTR (gst_nori_src_get_caps);
  basesrc_class->set_caps     = GST_DEBUG_FUNCPTR (gst_nori_src_set_caps);
  basesrc_class->fixate       = GST_DEBUG_FUNCPTR (gst_nori_src_fixate);
  basesrc_class->unlock       = GST_DEBUG_FUNCPTR (gst_nori_src_unlock);
  basesrc_class->unlock_stop  = GST_DEBUG_FUNCPTR (gst_nori_src_unlock_stop);

  pushsrc_class->create       = GST_DEBUG_FUNCPTR (gst_nori_src_create);

  GST_DEBUG_CATEGORY_INIT (nori_src_debug, "norisrc", 0,
      "Nori Xvision camera source");
}

/* ================================================================
 *  instance init
 * ================================================================ */

static void
gst_nori_src_init (GstNoriSrc *self)
{
  self->device_index      = 0;
  self->role              = NULL;
  self->trigger_mode      = GST_NORI_TRIGGER_NONE;
  self->mirror_flip       = GST_NORI_MF_NORMAL;
  self->auto_exposure     = TRUE;
  self->auto_white_balance = TRUE;
  self->sensor_shutter    = 0;
  self->sensor_gain       = 0;
  self->props_dirty       = 0;

  self->sdk_inited        = FALSE;
  self->video_started     = FALSE;
  self->flushing          = FALSE;

  self->uvc_exposure_default = 0;
  self->uvc_gain_default     = 0;
  self->uvc_defaults_valid   = FALSE;
  self->exposure_applied     = GST_NORI_EXP_UNKNOWN;

  self->video_infos       = NULL;
  self->n_video_infos     = 0;

  self->cur_format        = 0;
  self->cur_width         = 0;
  self->cur_height        = 0;
  self->cur_fps           = 0.0f;

  g_mutex_init (&self->apply_lock);

  /* This is a live source */
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_do_timestamp (GST_BASE_SRC (self), TRUE);
}

/* ================================================================
 *  finalize
 * ================================================================ */

static void
gst_nori_src_finalize (GObject *obj)
{
  GstNoriSrc *self = GST_NORI_SRC (obj);
  g_free (self->video_infos);
  self->video_infos = NULL;
  g_free (self->role);
  self->role = NULL;
  g_mutex_clear (&self->apply_lock);
  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

/* ================================================================
 *  Apply a single PU control, logging on failure
 * ================================================================ */

static void
nori_set_pu (GstNoriSrc *self, gint cid, gint val, const gchar *name)
{
  uint32_t ret = Nori_Xvision_SetProcessingUnitControl (self->device_index,
                                                        (int32_t) cid,
                                                        (int32_t) val);
  if (ret != NORI_OK)
    GST_WARNING_OBJECT (self, "Set %s = %d failed: 0x%04x", name, val, ret);
  else
    GST_DEBUG_OBJECT (self, "Set %s = %d OK", name, val);
}

/* ================================================================
 *  Exposure state machine
 *
 *  auto-exposure drives a two-state machine:
 *
 *    AUTO   : V4L2_CID_EXPOSURE_AUTO = 3 (aperture-priority). Firmware AE
 *             owns both integration time and gain. sensor-shutter /
 *             sensor-gain writes are ignored (with a warning).
 *    MANUAL : V4L2_CID_EXPOSURE_AUTO = 1. On entry, UVC exposure and gain
 *             registers are reset to the device's own defaults (captured
 *             once in start()), so every camera starts from an identical
 *             baseline before sensor-shutter / sensor-gain take over.
 *
 *  exposure_applied tracks the hardware state, independent of what the
 *  user has asked for, so transitions are idempotent and survive
 *  start/stop cycles.
 * ================================================================ */

static void
nori_apply_exposure (GstNoriSrc *self)
{
  guint32 d = self->props_dirty;
  GstNoriExposureState desired =
      self->auto_exposure ? GST_NORI_EXP_AUTO : GST_NORI_EXP_MANUAL;
  gboolean mode_changed        = (desired != self->exposure_applied);
  gboolean sensor_values_dirty = (d & (PROP_DIRTY_SHUTTER | PROP_DIRTY_SENSOR_GAIN)) != 0;

  if (mode_changed && desired == GST_NORI_EXP_AUTO) {
    nori_set_pu (self, V4L2_CID_EXPOSURE_AUTO, 3, "auto-exposure=on");
    self->exposure_applied = GST_NORI_EXP_AUTO;
    if (sensor_values_dirty)
      GST_WARNING_OBJECT (self,
          "sensor-shutter/sensor-gain set while auto-exposure=true; ignored");
    return;
  }

  if (mode_changed && desired == GST_NORI_EXP_MANUAL) {
    nori_set_pu (self, V4L2_CID_EXPOSURE_AUTO, 1, "auto-exposure=off");
    if (self->uvc_defaults_valid) {
      nori_set_pu (self, V4L2_CID_EXPOSURE_ABSOLUTE,
                   self->uvc_exposure_default, "uvc-exposure=default");
      nori_set_pu (self, V4L2_CID_GAIN,
                   self->uvc_gain_default, "uvc-gain=default");
    } else {
      GST_WARNING_OBJECT (self,
          "UVC defaults unavailable; cross-camera baseline may vary");
    }
    self->exposure_applied = GST_NORI_EXP_MANUAL;
    /* fall through to write sensor values */
  }

  if (self->exposure_applied == GST_NORI_EXP_MANUAL) {
    if (d & PROP_DIRTY_SHUTTER) {
      uint32_t ret = Nori_Xvision_SetSensorShutter (self->device_index,
                                                    self->sensor_shutter);
      if (ret != NORI_OK)
        GST_WARNING_OBJECT (self, "SetSensorShutter(%u) failed: 0x%04x",
                            self->sensor_shutter, ret);
    }
    if (d & PROP_DIRTY_SENSOR_GAIN) {
      uint32_t ret = Nori_Xvision_SetSensorGain (self->device_index,
                                                 self->sensor_gain);
      if (ret != NORI_OK)
        GST_WARNING_OBJECT (self, "SetSensorGain(%u) failed: 0x%04x",
                            self->sensor_gain, ret);
    }
  } else if (sensor_values_dirty) {
    GST_WARNING_OBJECT (self,
        "sensor-shutter/sensor-gain set while auto-exposure=true; ignored");
  }
}

/* ================================================================
 *  Apply all dirty properties to the running device
 * ================================================================ */

static void
nori_apply_controls (GstNoriSrc *self)
{
  guint32 d = self->props_dirty;
  if (!d)
    return;

  if (d & PROP_DIRTY_TRIGGER) {
    uint32_t ret = Nori_Xvision_SetTriggerMode (self->device_index,
                      (E_TRIGGER_MODE) self->trigger_mode);
    if (ret != NORI_OK)
      GST_WARNING_OBJECT (self, "SetTriggerMode failed: 0x%04x", ret);
  }
  if (d & PROP_DIRTY_MIRROR_FLIP) {
    uint32_t ret = Nori_Xvision_SetSensorMirrorFlip (self->device_index,
                      (E_SENSOR_MIRROR_FLIP) self->mirror_flip);
    if (ret != NORI_OK)
      GST_WARNING_OBJECT (self, "SetSensorMirrorFlip failed: 0x%04x", ret);
  }
  if (d & PROP_DIRTY_AUTO_WB)
    nori_set_pu (self, V4L2_CID_AUTO_WHITE_BALANCE,
                 self->auto_white_balance ? 1 : 0, "auto-white-balance");

  nori_apply_exposure (self);

  self->props_dirty = 0;
}

/* ================================================================
 *  Read a single PU control's current value, falling back to `cached`
 *  on SDK failure or before the SDK is initialised.
 * ================================================================ */

static gint
nori_get_pu (GstNoriSrc *self, gint cid, gint cached)
{
  if (!self->sdk_inited)
    return cached;
  int32_t cur = 0, flags = 0, step = 0, mn = 0, mx = 0, def = 0;
  uint32_t ret = Nori_Xvision_GetProcessingUnitControl (self->device_index,
                     (int32_t) cid, &cur, &flags, &step, &mn, &mx, &def);
  if (ret != NORI_OK) {
    GST_DEBUG_OBJECT (self, "GetProcessingUnitControl(cid=%d) failed: 0x%04x",
        cid, ret);
    return cached;
  }
  return (gint) cur;
}

/* ================================================================
 *  set_property / get_property
 * ================================================================ */

static void
gst_nori_src_set_property (GObject *obj, guint id,
                           const GValue *val, GParamSpec *pspec)
{
  GstNoriSrc *self = GST_NORI_SRC (obj);

  /* Lock for the entire write+apply sequence so we don't race set_caps
   * (state-change thread) or another set_property call. SDK calls inside
   * nori_apply_controls can take >1s, making the window wide. */
  g_mutex_lock (&self->apply_lock);

  switch (id) {
    case PROP_DEVICE_INDEX:
      self->device_index = g_value_get_uint (val);
      break;
    case PROP_ROLE:
      g_free (self->role);
      self->role = g_value_dup_string (val);
      if (self->role && self->role[0] == '\0') {
        g_free (self->role);
        self->role = NULL;
      }
      break;
    case PROP_TRIGGER_MODE:
      self->trigger_mode = (GstNoriTriggerMode) g_value_get_enum (val);
      self->props_dirty |= PROP_DIRTY_TRIGGER;
      break;
    case PROP_MIRROR_FLIP:
      self->mirror_flip = (GstNoriMirrorFlip) g_value_get_enum (val);
      self->props_dirty |= PROP_DIRTY_MIRROR_FLIP;
      break;
    case PROP_AUTO_EXPOSURE:
      self->auto_exposure = g_value_get_boolean (val);
      self->props_dirty |= PROP_DIRTY_AUTO_EXP;
      break;
    case PROP_AUTO_WHITE_BALANCE:
      self->auto_white_balance = g_value_get_boolean (val);
      self->props_dirty |= PROP_DIRTY_AUTO_WB;
      break;
    case PROP_SENSOR_SHUTTER:
      self->sensor_shutter = g_value_get_uint (val);
      self->props_dirty |= PROP_DIRTY_SHUTTER;
      break;
    case PROP_SENSOR_GAIN:
      self->sensor_gain = g_value_get_uint (val);
      self->props_dirty |= PROP_DIRTY_SENSOR_GAIN;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, id, pspec);
      break;
  }

  /* If the device is already streaming, apply immediately */
  if (self->video_started)
    nori_apply_controls (self);

  g_mutex_unlock (&self->apply_lock);
}

static void
gst_nori_src_get_property (GObject *obj, guint id,
                           GValue *val, GParamSpec *pspec)
{
  GstNoriSrc *self = GST_NORI_SRC (obj);

  switch (id) {
    case PROP_DEVICE_INDEX:
      g_value_set_uint (val, self->device_index);
      break;
    case PROP_ROLE:
      g_value_set_string (val, self->role);
      break;
    case PROP_TRIGGER_MODE: {
      E_TRIGGER_MODE m = (E_TRIGGER_MODE) self->trigger_mode;
      if (self->sdk_inited)
        Nori_Xvision_GetTriggerMode (self->device_index, &m);
      g_value_set_enum (val, (gint) m);
      break;
    }
    case PROP_MIRROR_FLIP: {
      E_SENSOR_MIRROR_FLIP m = (E_SENSOR_MIRROR_FLIP) self->mirror_flip;
      if (self->sdk_inited)
        Nori_Xvision_GetSensorMirrorFlip (self->device_index, &m);
      g_value_set_enum (val, (gint) m);
      break;
    }
    case PROP_AUTO_EXPOSURE: {
      /* Setter writes 3 (aperture-priority) for auto, 1 (manual) otherwise;
       * reverse here so anything but explicit manual reads as auto. */
      gint cur = nori_get_pu (self, V4L2_CID_EXPOSURE_AUTO,
                              self->auto_exposure ? 3 : 1);
      g_value_set_boolean (val, cur != 1);
      break;
    }
    case PROP_AUTO_WHITE_BALANCE: {
      gint cur = nori_get_pu (self, V4L2_CID_AUTO_WHITE_BALANCE,
                              self->auto_white_balance ? 1 : 0);
      g_value_set_boolean (val, cur != 0);
      break;
    }
    case PROP_SENSOR_SHUTTER: {
      uint32_t v = self->sensor_shutter;
      if (self->sdk_inited)
        Nori_Xvision_GetSensorShutter (self->device_index, &v);
      g_value_set_uint (val, v);
      break;
    }
    case PROP_SENSOR_GAIN: {
      uint32_t v = self->sensor_gain, mn = 0, mx = 0, step = 0;
      if (self->sdk_inited)
        Nori_Xvision_GetSensorGain (self->device_index, &v, &mn, &mx, &step);
      g_value_set_uint (val, v);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, id, pspec);
      break;
  }
}

/* ================================================================
 *  start  –  init SDK, enumerate device formats
 * ================================================================ */

static gboolean
gst_nori_src_start (GstBaseSrc *bsrc)
{
  GstNoriSrc *self = GST_NORI_SRC (bsrc);

  if (!nori_sdk_ref ()) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ,
        ("Failed to initialise Nori SDK"), (NULL));
    return FALSE;
  }
  self->sdk_inited = TRUE;

  /* If 'role' is set, resolve it to a device index by scanning tags. */
  if (self->role != NULL) {
    guint32 n = nori_sdk_num_devices ();
    gboolean found = FALSE;
    size_t role_len = strlen (self->role);
    for (guint32 i = 0; i < n; i++) {
      BYTE buf[NORI_TAG_BLOCK_SIZE];
      if (Nori_Xvision_GetUserData (i, NORI_TAG_BLOCK_DEFAULT, buf) != NORI_OK)
        continue;
      NoriTagData t;
      nori_tag_decode (buf, &t);
      if (t.status == NORI_TAG_VALID
          && t.role_len == role_len
          && memcmp (t.role, self->role, role_len) == 0) {
        self->device_index = i;
        found = TRUE;
        GST_INFO_OBJECT (self, "role='%s' matched device[%u]", self->role, i);
        break;
      }
    }
    if (!found) {
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
          ("No camera with role tag '%s' found among %u device(s). "
           "Use `nori-ctl list` to see tags and `nori-ctl tag set` to assign one.",
           self->role, n), (NULL));
      nori_sdk_unref ();
      self->sdk_inited = FALSE;
      return FALSE;
    }
  }

  if (self->device_index >= nori_sdk_num_devices ()) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("Device index %u out of range (%u device(s) found)",
         self->device_index, nori_sdk_num_devices ()), (NULL));
    nori_sdk_unref ();
    self->sdk_inited = FALSE;
    return FALSE;
  }

  /* Log device info */
  {
    DEVICE_INFO di;
    if (Nori_Xvision_GetDeviceInfo (self->device_index, &di) == NORI_OK) {
      GST_INFO_OBJECT (self, "Device[%u]: %s  S/N: %s  (vid=0x%04x pid=0x%04x)",
          self->device_index, di.iProduct, di.iSerialNumber,
          di.idVendor, di.idProduct);
    }
  }

  /* Enumerate supported video formats */
  {
    uint32_t n = 0;
    uint32_t ret = Nori_Xvision_GetDeviceVideoInfoSize (self->device_index, &n);
    if (ret != NORI_OK || n == 0) {
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
          ("Cannot enumerate video formats (ret=0x%04x, n=%u)", ret, n), (NULL));
      nori_sdk_unref ();
      self->sdk_inited = FALSE;
      return FALSE;
    }

    g_free (self->video_infos);
    self->video_infos   = g_new0 (VIDEO_INFO, n);
    self->n_video_infos = n;

    for (uint32_t i = 0; i < n; i++) {
      Nori_Xvision_GetDeviceVideoInfo (self->device_index, i,
                                       &self->video_infos[i]);
      VIDEO_INFO *vi = &self->video_infos[i];
      GST_DEBUG_OBJECT (self, "  format[%u]: %c%c%c%c %ux%u @ %.1f fps", i,
          (char)(vi->u_Format >>  0), (char)(vi->u_Format >>  8),
          (char)(vi->u_Format >> 16), (char)(vi->u_Format >> 24),
          vi->u_Width, vi->u_Height, vi->f_Fps);
    }
  }

  /* Capture this device's UVC exposure/gain defaults. Used when entering
   * manual exposure mode so every camera starts from an identical baseline
   * before sensor-shutter / sensor-gain drive the exposure. */
  {
    int32_t _c, _f, _s, _mn, _mx, edef = 0, gdef = 0;
    uint32_t re = Nori_Xvision_GetProcessingUnitControl (self->device_index,
        V4L2_CID_EXPOSURE_ABSOLUTE, &_c, &_f, &_s, &_mn, &_mx, &edef);
    uint32_t rg = Nori_Xvision_GetProcessingUnitControl (self->device_index,
        V4L2_CID_GAIN,              &_c, &_f, &_s, &_mn, &_mx, &gdef);
    self->uvc_defaults_valid   = (re == NORI_OK && rg == NORI_OK);
    self->uvc_exposure_default = (re == NORI_OK) ? edef : 0;
    self->uvc_gain_default     = (rg == NORI_OK) ? gdef : 0;
    if (self->uvc_defaults_valid) {
      GST_INFO_OBJECT (self, "UVC defaults: exposure=%d gain=%d",
          self->uvc_exposure_default, self->uvc_gain_default);
    } else {
      GST_WARNING_OBJECT (self,
          "Could not read UVC defaults (exposure=0x%04x gain=0x%04x)", re, rg);
    }
  }

  self->exposure_applied = GST_NORI_EXP_UNKNOWN;
  self->flushing = FALSE;
  return TRUE;
}

/* ================================================================
 *  stop  –  tear down video, release SDK
 * ================================================================ */

static gboolean
gst_nori_src_stop (GstBaseSrc *bsrc)
{
  GstNoriSrc *self = GST_NORI_SRC (bsrc);

  if (self->video_started) {
    Nori_Xvision_VideoStop (self->device_index);
    Nori_Xvision_DeviceVideoUnInit (self->device_index);
    self->video_started = FALSE;
    GST_DEBUG_OBJECT (self, "Video stopped");
  }

  g_free (self->video_infos);
  self->video_infos   = NULL;
  self->n_video_infos = 0;

  if (self->sdk_inited) {
    nori_sdk_unref ();
    self->sdk_inited = FALSE;
  }

  /* Invalidate per-device baseline so a restart (possibly on a different
   * camera) re-reads defaults and runs a fresh AE transition. */
  self->uvc_defaults_valid = FALSE;
  self->exposure_applied   = GST_NORI_EXP_UNKNOWN;

  return TRUE;
}

/* ================================================================
 *  get_caps  –  return device-specific caps when available
 * ================================================================ */

static GstCaps *
gst_nori_src_get_caps (GstBaseSrc *bsrc, GstCaps *filter)
{
  GstNoriSrc *self = GST_NORI_SRC (bsrc);

  if (!self->video_infos || self->n_video_infos == 0) {
    /* Not started yet – return the pad template */
    GstCaps *tmpl = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (bsrc));
    if (filter) {
      GstCaps *filtered = gst_caps_intersect_full (filter, tmpl,
                               GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (tmpl);
      return filtered;
    }
    return tmpl;
  }

  /* Build caps from the device's advertised formats */
  GstCaps *caps = gst_caps_new_empty ();

  for (guint i = 0; i < self->n_video_infos; i++) {
    VIDEO_INFO *vi = &self->video_infos[i];
    GstStructure *s = NULL;

    if (vi->u_Format == VIDEO_MEDIA_TYPE_MJPG) {
      s = gst_structure_new ("image/jpeg",
              "width",     G_TYPE_INT,      (gint) vi->u_Width,
              "height",    G_TYPE_INT,      (gint) vi->u_Height,
              "framerate", GST_TYPE_FRACTION,
                           (gint) vi->f_Fps, 1,
              NULL);
    } else if (vi->u_Format == VIDEO_MEDIA_TYPE_YUYV) {
      s = gst_structure_new ("video/x-raw",
              "format",    G_TYPE_STRING,   "YUY2",
              "width",     G_TYPE_INT,      (gint) vi->u_Width,
              "height",    G_TYPE_INT,      (gint) vi->u_Height,
              "framerate", GST_TYPE_FRACTION,
                           (gint) vi->f_Fps, 1,
              NULL);
    } else {
      GST_DEBUG_OBJECT (self, "Skipping unknown format 0x%08x", vi->u_Format);
      continue;
    }

    gst_caps_append_structure (caps, s);
  }

  if (filter) {
    GstCaps *filtered = gst_caps_intersect_full (filter, caps,
                             GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    return filtered;
  }
  return caps;
}

/* ================================================================
 *  fixate  –  prefer MJPEG, highest resolution, 30 fps
 * ================================================================ */

static GstCaps *
gst_nori_src_fixate (GstBaseSrc *bsrc, GstCaps *caps)
{
  GstStructure *s = gst_caps_get_structure (caps, 0);

  gst_structure_fixate_field_nearest_int (s, "width",  1920);
  gst_structure_fixate_field_nearest_int (s, "height", 1080);
  gst_structure_fixate_field_nearest_fraction (s, "framerate", 30, 1);

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);
  return caps;
}

/* ================================================================
 *  set_caps  –  configure device and start streaming
 * ================================================================ */

static gboolean
gst_nori_src_set_caps (GstBaseSrc *bsrc, GstCaps *caps)
{
  GstNoriSrc *self = GST_NORI_SRC (bsrc);
  GstStructure *s  = gst_caps_get_structure (caps, 0);
  const gchar  *media = gst_structure_get_name (s);

  gint w = 0, h = 0, fps_n = 0, fps_d = 1;
  gst_structure_get_int (s, "width",  &w);
  gst_structure_get_int (s, "height", &h);
  gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d);

  guint32 sdk_fmt;
  if (g_str_equal (media, "image/jpeg")) {
    sdk_fmt = VIDEO_MEDIA_TYPE_MJPG;
  } else if (g_str_equal (media, "video/x-raw")) {
    sdk_fmt = VIDEO_MEDIA_TYPE_YUYV;
  } else {
    GST_ERROR_OBJECT (self, "Unsupported media type: %s", media);
    return FALSE;
  }

  /* If already streaming with the same params, nothing to do */
  if (self->video_started &&
      self->cur_format == sdk_fmt &&
      self->cur_width  == (guint) w &&
      self->cur_height == (guint) h) {
    return TRUE;
  }

  /* Stop any prior stream */
  if (self->video_started) {
    Nori_Xvision_VideoStop (self->device_index);
    Nori_Xvision_DeviceVideoUnInit (self->device_index);
    self->video_started = FALSE;
  }

  /* Configure and start */
  VIDEO_INFO vi;
  memset (&vi, 0, sizeof (vi));
  vi.u_Format = sdk_fmt;
  vi.u_Width  = (uint32_t) w;
  vi.u_Height = (uint32_t) h;
  vi.f_Fps    = (fps_d > 0) ? (float) fps_n / (float) fps_d : 30.0f;

  uint32_t ret = Nori_Xvision_DeviceVideoInit (self->device_index, vi);
  if (ret != NORI_OK) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("DeviceVideoInit failed: 0x%04x", ret), (NULL));
    return FALSE;
  }

  /* Trigger mode is latched by the SDK at VideoStart, and the camera firmware
   * persists the mode across close/reopen cycles. Always enforce the requested
   * mode here — otherwise a prior HARDWARE setting will keep a new pipeline
   * (even one explicitly asking for NONE) blocked waiting for trigger pulses. */
  {
    uint32_t tret = Nori_Xvision_SetTriggerMode (self->device_index,
                        (E_TRIGGER_MODE) self->trigger_mode);
    if (tret != NORI_OK)
      GST_WARNING_OBJECT (self, "SetTriggerMode(%d) failed: 0x%04x",
          self->trigger_mode, tret);
    self->props_dirty &= ~PROP_DIRTY_TRIGGER;
  }

  ret = Nori_Xvision_VideoStart (self->device_index);
  if (ret != NORI_OK) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ,
        ("VideoStart failed: 0x%04x", ret), (NULL));
    Nori_Xvision_DeviceVideoUnInit (self->device_index);
    return FALSE;
  }

  self->video_started = TRUE;
  self->cur_format    = sdk_fmt;
  self->cur_width     = (guint) w;
  self->cur_height    = (guint) h;
  self->cur_fps       = vi.f_Fps;

  GST_INFO_OBJECT (self, "Streaming: %c%c%c%c %ux%u @ %.1f fps",
      (char)(sdk_fmt >>  0), (char)(sdk_fmt >>  8),
      (char)(sdk_fmt >> 16), (char)(sdk_fmt >> 24),
      w, h, vi.f_Fps);

  /* Force the exposure state machine to run at least once on first start,
   * so the UVC baseline is enforced even if the user left auto-exposure at
   * its default. exposure_applied=UNKNOWN (set in start()/stop()) guarantees
   * nori_apply_exposure treats this as a mode transition.
   *
   * Held under apply_lock so a concurrent set_property cannot race the
   * mode transition — SDK calls inside apply can take >1s. */
  g_mutex_lock (&self->apply_lock);
  self->props_dirty |= PROP_DIRTY_AUTO_EXP;
  nori_apply_controls (self);
  g_mutex_unlock (&self->apply_lock);

  return TRUE;
}

/* ================================================================
 *  unlock / unlock_stop  –  interrupt blocking GetFrameBuff
 * ================================================================ */

static gboolean
gst_nori_src_unlock (GstBaseSrc *bsrc)
{
  GST_NORI_SRC (bsrc)->flushing = TRUE;
  return TRUE;
}

static gboolean
gst_nori_src_unlock_stop (GstBaseSrc *bsrc)
{
  GST_NORI_SRC (bsrc)->flushing = FALSE;
  return TRUE;
}

/* ================================================================
 *  create  –  grab one frame, wrap in a GstBuffer
 * ================================================================ */

static GstFlowReturn
gst_nori_src_create (GstPushSrc *psrc, GstBuffer **buf)
{
  GstNoriSrc *self = GST_NORI_SRC (psrc);

  if (!self->video_started) {
    GST_ERROR_OBJECT (self, "create() called but video not started");
    return GST_FLOW_ERROR;
  }

  FRAME_BUFFER_DATA *frame = NULL;

  /* Poll with a moderate timeout so we can respect unlock (flushing) */
  while (!self->flushing) {
    frame = Nori_Xvision_GetFrameBuff (self->device_index, true, 500);
    if (frame)
      break;
    /* NULL means timeout – loop and check flushing flag */
  }

  if (self->flushing) {
    if (frame)
      Nori_Xvision_FreeFrameBuff (self->device_index, frame);
    return GST_FLOW_FLUSHING;
  }

  if (!frame) {
    GST_ERROR_OBJECT (self, "GetFrameBuff returned NULL unexpectedly");
    return GST_FLOW_ERROR;
  }

  /* Allocate a GstBuffer and copy frame data */
  guint size = frame->buff_Length;
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, size, NULL);

  GstMapInfo map;
  if (!gst_buffer_map (buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref (buffer);
    Nori_Xvision_FreeFrameBuff (self->device_index, frame);
    GST_ERROR_OBJECT (self, "Failed to map output buffer");
    return GST_FLOW_ERROR;
  }
  memcpy (map.data, frame->pBufAddr, size);
  gst_buffer_unmap (buffer, &map);

  /* Set duration based on negotiated framerate */
  if (self->cur_fps > 0.0f) {
    GST_BUFFER_DURATION (buffer) =
        gst_util_uint64_scale_int (GST_SECOND, 1, (gint) self->cur_fps);
  }

  /* Return frame buffer to the SDK pool */
  Nori_Xvision_FreeFrameBuff (self->device_index, frame);

  *buf = buffer;
  return GST_FLOW_OK;
}
