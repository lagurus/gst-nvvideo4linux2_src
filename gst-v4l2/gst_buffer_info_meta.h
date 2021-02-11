/*
    thanks to 
    https://github.com/jackersson/gst-python-hacks
    http://lifestyletransfer.com/how-to-add-metadata-to-gstreamer-buffer-in-python/
    https://github.com/EricssonResearch/openwebrtc-gst-plugins/tree/master/gst-libs/gst/sctp
*/

 
#ifndef __GST_BUFFER_INFO_META_H__
#define __GST_BUFFER_INFO_META_H__
 
#include <gst/gst.h>
 
#include "ext/videodev2.h"
#include "../v4l2_nv_extensions.h"
 
G_BEGIN_DECLS

// Api Type
// 1-st field of GstMetaInfo
#define GST_BUFFER_INFO_META_API_TYPE (gst_buffer_info_meta_api_get_type())
#define GST_BUFFER_INFO_META_INFO     (gst_buffer_info_meta_get_info())
 
typedef struct _GstBufferInfoMeta  GstBufferInfoMeta;
typedef struct _GstBufferInfo      GstBufferInfo;

#define D_USE_META_STATIC   0

/**
 * Holds the motion vector parameters for one complete frame.
 */
typedef struct metadata_MV_ {
    /** Size of the pMVInfo buffer, in bytes. */
    guint32 bufSize;
    /** Pointer to the buffer containing the motion vectors. */
    #ifdef D_USE_META_STATIC
        int        m_nInfoCount;
        MVInfo  rec_mv_info[12000];
    #else
        MVInfo  *pMVInfo;
    #endif

} metadata_MV;

struct _GstBufferInfo {
    
    metadata_MV   m_enc_mv_metadata;
};


struct _GstBufferInfoMeta {

    // Required as it is base structure for metadata
    // https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/gstreamer-GstMeta.html
    GstMeta meta;  

    // Custom fields
    GstBufferInfo info;
};  

GST_EXPORT void AllocateMyMetaData( GstBufferInfo *pBufferInfo, v4l2_ctrl_videoenc_outputbuf_metadata_MV *p_meta_MV, int nInfoCount );

GType gst_buffer_info_meta_api_get_type(void);
 
GST_EXPORT const GstMetaInfo * gst_buffer_info_meta_get_info(void);
 
GST_EXPORT GstBufferInfoMeta* gst_buffer_add_buffer_info_meta(GstBuffer *buffer, GstBufferInfo*);

GST_EXPORT gboolean gst_buffer_remove_buffer_info_meta(GstBuffer *buffer);

// ---------------------------------------------------------------------------------------------------------------------------------------------------
 
G_END_DECLS
 
#endif /* __GST_BUFFER_INFO_META_H__ */