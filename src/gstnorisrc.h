#ifndef __GST_NORI_SRC_H__
#define __GST_NORI_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

/* SDK header (requires C++ due to <chrono> include in Nori_Public.h) */
#include <Nori_Xvision_API/Nori_Xvision_API.h>

G_BEGIN_DECLS

#define GST_TYPE_NORI_SRC            (gst_nori_src_get_type())
#define GST_NORI_SRC(obj)           (G_TYPE_CHECK_INSTANCE_CAST((obj),  GST_TYPE_NORI_SRC, GstNoriSrc))
#define GST_NORI_SRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),   GST_TYPE_NORI_SRC, GstNoriSrcClass))
#define GST_IS_NORI_SRC(obj)        (G_TYPE_CHECK_INSTANCE_TYPE((obj),  GST_TYPE_NORI_SRC))
#define GST_IS_NORI_SRC_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE((klass),   GST_TYPE_NORI_SRC))

typedef struct _GstNoriSrc      GstNoriSrc;
typedef struct _GstNoriSrcClass GstNoriSrcClass;

/* ---- Trigger-mode enum (mirrors E_TRIGGER_MODE) ---- */
typedef enum {
  GST_NORI_TRIGGER_NONE     = 0,
  GST_NORI_TRIGGER_SOFTWARE = 1,
  GST_NORI_TRIGGER_HARDWARE = 2,
  GST_NORI_TRIGGER_COMMAND  = 3,
} GstNoriTriggerMode;

GType gst_nori_trigger_mode_get_type (void);
#define GST_TYPE_NORI_TRIGGER_MODE (gst_nori_trigger_mode_get_type())

/* ---- Mirror / flip enum (mirrors E_SENSOR_MIRROR_FLIP) ---- */
typedef enum {
  GST_NORI_MF_NORMAL      = 0,
  GST_NORI_MF_MIRROR      = 1,
  GST_NORI_MF_FLIP        = 2,
  GST_NORI_MF_MIRROR_FLIP = 3,
} GstNoriMirrorFlip;

GType gst_nori_mirror_flip_get_type (void);
#define GST_TYPE_NORI_MIRROR_FLIP (gst_nori_mirror_flip_get_type())

/* ---- Dirty-property bitmask ---- */
#define PROP_DIRTY_TRIGGER       (1u <<  0)
#define PROP_DIRTY_MIRROR_FLIP   (1u <<  1)
#define PROP_DIRTY_AUTO_EXP      (1u <<  2)
#define PROP_DIRTY_AUTO_WB       (1u <<  3)
#define PROP_DIRTY_SHUTTER       (1u <<  4)
#define PROP_DIRTY_SENSOR_GAIN   (1u <<  5)

/* Hardware-applied exposure state. Tracks what the device is *currently in*,
 * not what the user asked for, so transitions are idempotent. */
typedef enum {
  GST_NORI_EXP_UNKNOWN = 0,
  GST_NORI_EXP_AUTO,
  GST_NORI_EXP_MANUAL,
} GstNoriExposureState;

struct _GstNoriSrc
{
  GstPushSrc parent;

  /* --- GObject properties --- */
  guint                device_index;
  gchar               *role;            /* user-assigned tag label; NULL = match by device_index */
  GstNoriTriggerMode   trigger_mode;
  GstNoriMirrorFlip    mirror_flip;
  gboolean             auto_exposure;
  gboolean             auto_white_balance;
  guint                sensor_shutter;   /* microseconds, used only when !auto_exposure */
  guint                sensor_gain;      /* multiplier,   used only when !auto_exposure */

  /* Tracks which properties were explicitly set by the user */
  guint32              props_dirty;

  /* Serialises nori_apply_controls / set_property / set_caps. SDK control
   * calls can take >1s, long enough for the state-change thread (set_caps)
   * and the property-setter thread to race through the exposure state
   * machine concurrently and double-apply it. */
  GMutex               apply_lock;

  /* --- Runtime state --- */
  gboolean             sdk_inited;
  gboolean             video_started;
  volatile gboolean    flushing;

  /* Per-device UVC defaults, read once in start(). Used on AUTO->MANUAL
   * transition to force a deterministic baseline so sensor-shutter/gain
   * are the only knobs driving exposure. */
  gint32               uvc_exposure_default;
  gint32               uvc_gain_default;
  gboolean             uvc_defaults_valid;

  /* Hardware-applied AE state, for transition detection */
  GstNoriExposureState exposure_applied;

  /* Enumerated device capabilities (filled in start()) */
  VIDEO_INFO          *video_infos;
  guint                n_video_infos;

  /* Currently negotiated format */
  guint32              cur_format;   /* SDK fourcc */
  guint                cur_width;
  guint                cur_height;
  gfloat               cur_fps;
};

struct _GstNoriSrcClass
{
  GstPushSrcClass parent_class;
};

GType gst_nori_src_get_type (void);

G_END_DECLS

#endif /* __GST_NORI_SRC_H__ */
